/* SPDX-License-Identifier: Apache-2.0 */
/* Board bundle 20: the first CPU operating-point step of
 * docs/designs/cpu-top-opp.md. On the returning CPU0 image of bundle 15, with
 * cores 6 and 7 powered off, the actual CPU clock driver raises CPU-B from
 * 1,014 to 1,404 MHz and lowers it to 1,014 MHz again. Both points need the
 * inherited 800 mV on speed bin vf0400, so no PMIC register is written.
 *
 * The client task owns the thermal driver, the thermal policy and the CPU
 * clock driver. It reads one full set of sensors, inspects the three CPU
 * domains read-only (DCDC5 and DCDC3 snapshots, the PLL, mux, pattern and LFM
 * words), admits the step only at the NORMAL thermal level on bin vf0400
 * with DCDC3 and DCDC5 at 800 mV, CPU-B at 1,014 MHz and clear pattern and
 * LFM enables, and otherwise returns with a REFUSED line before any write. It
 * runs the two transitions with 200 us bursts,
 * requires all three domains' PLL and mux words and both rails to read as
 * before, reads a second sensor set, disables the thermal
 * converter, parks the PMIC service and returns. */
#include "power.h"
#include "platform.h"
#include "a733_range.h"
#include "a733_mmio.h"
#include "a733_adc_native.h"
#include "a733_thermal.h"
#include "a733_thermal_native.h"
#include "a733_cpu_clock.h"

#define CPU_OWNER 4u
#define THERMAL_OWNER 5u
#define STEP_MHZ 1404u   /* Highest CPU-B point at 800 mV in vf0400 (sun60iw2p1-cpu-vf.dtsi:1126). */
#define BASE_MHZ 1014u   /* CPU-B rate that the boot firmware leaves. */
#define STEP_BIN 4u      /* Column vf0400: SID code 0x04, port 3-3's fuse word 0x496. */
#define STEP_UV 800000u  /* DCDC3 and DCDC5 at selector 0x1e (bit 7 set: 0x9e). */
#define REQUEST_US 120000u
#define BURST_US 200u

static struct a733_adc_native binding;
static struct a733_clock_token thermal_token;
static struct a733_thermal thermal;
static struct a733_thermal_config thermal_config;
static struct a733_thermal_ops thermal_ops;
static struct a733_thermal_policy policy;
static struct a733_cpu_clock cpu;
static TaskHandle_t owner;
static unsigned sets;

/* Thermal callbacks: the common register mutex through the ADC native
 * binding, as the returning thermal image (apps/thermal-returning/main.c)
 * binds them. Neither analog interrupt may be pending or enabled. */
static int now_us(void *unused, uint64_t *out)
{ (void)unused; return a733_counter_clock_now(&bb_power.counter, out); }
static int quiet(void)
{
    return !(a733_mmio_read32(0x03400000) & (1u << 31)) &&
           !((a733_mmio_read32(0x03400108) | a733_mmio_read32(0x03400208) | a733_mmio_read32(0x03400308)) & 0xc0);
}
static int current(void) { return binding.operations.current(&binding) == 1; }
static int enter(void *unused, uint64_t end)
{
    (void)unused;
    const int held = binding.operations.enter(&binding, end);
    return held == 1 ? (quiet() ? 1 : -1) : held;
}
static void leave(void *unused) { (void)unused; binding.operations.leave(&binding); }
static void retain(void *unused) { (void)unused; binding.operations.retain(&binding); }
static int accessible(void)
{
    return current() && quiet() && thermal.held &&
           !(atomic_load_explicit(&bb_power.locks.retained, memory_order_acquire) & 1) &&
           xSemaphoreGetMutexHolder(bb_power.locks.mutex[A733_PLATFORM_REGISTER_LOCK]) == owner;
}
static int thermal_read(void *unused, uintptr_t a, uint32_t *out)
{ (void)unused; return accessible() && a733_thermal_native_read(NULL, a, out) == 1; }
static int thermal_write(void *unused, uintptr_t a, uint32_t value)
{ (void)unused; return accessible() && a733_thermal_native_write(NULL, a, value) == 1; }
static int unused_admit(void *v, const struct a733_adc_config *c, const struct a733_clock_token *t)
{ (void)v; (void)c; (void)t; return 0; }
static int admit(void *unused, const struct a733_thermal_config *c, const struct a733_clock_token *t)
{
    (void)unused;
    return current() && quiet() && c == &thermal.config && c->owner == thermal_config.owner &&
           c->handoff == thermal_config.handoff && c->startup_deadline_us == thermal_config.startup_deadline_us &&
           c->apb1 == thermal_config.apb1 && c->apb1_hz == thermal_config.apb1_hz &&
           c->configure_adc_clock == thermal_config.configure_adc_clock && !c->alarm_mask && !c->critical_mask &&
           t->id == thermal_token.id && t->owner == thermal_token.owner && t->shared == thermal_token.shared &&
           t->exclusive == thermal_token.exclusive;
}
static int notify(void *unused, enum a733_thermal_level level)
{ (void)unused; (void)level; return 1; }

/* CPU clock callbacks: Device-nGnRnE accesses of the words that
 * a733_cpu_clock_address allows, with DSB SY after each mux write, as the OS
 * adapter (apps/os/cpu.c) makes them. */
static int cpu_now(void *unused, uint64_t *out)
{ (void)unused; return a733_counter_clock_now(&bb_power.counter, out); }
static int cpu_read(void *unused, uintptr_t a, uint32_t *v)
{
    (void)unused;
    if (!a733_cpu_clock_address(a, 0))
        return 0;
    *v = a733_mmio_read32(a);
    return 1;
}
static int cpu_write(void *unused, uintptr_t a, uint32_t value)
{
    (void)unused;
    if (!a733_cpu_clock_address(a, 1))
        return 0;
    a733_mmio_write32(a, value);
    if ((a & 0xfff) == 0x01c)
        a733_mmio_dsb_sy();
    return 1;
}

static _Noreturn void fail(const char *what)
{
    struct bb_line l;
    bb_line_start(&l, "STEP");
    bb_line_dec(&l, "state", cpu.state);
    bb_line_dec(&l, "cluster", cpu.cluster);
    bb_line_dec(&l, "failed", cpu.failed);
    bb_line_dec(&l, "parked", (uint64_t)a733_cpu_clock_parked(&cpu));
    bb_line_dec(&l, "thermal_stage", thermal.stage);
    bb_line_dec(&l, "thermal_fault", thermal.fault);
    bb_line_end(&l);
    bb_power_print(&l);
    bb_power_fail(what);
}
#define REQUIRE(v, what) do { if (!(v)) fail(what); } while (0)
static void thermal_stop(void);
/* A refusal before any clock or PMIC write: the converter is disabled and
 * the PMIC service parked, so the board returns in its inherited state.
 * Each refusal is a controlled outcome of the bundle. */
static _Noreturn void refuse(const char *what)
{
    thermal_stop();
    bb_power_park();
    platform_puts("REFUSED board bundle 20: ");
    platform_puts(what);
    platform_puts("\n");
    platform_exit(1);
}

/* One full set of the five sensors, then one policy update. Returns the
 * level in force. */
static enum a733_thermal_level sensor_set(void)
{
    struct a733_thermal_sample set = {0};
    while (set.valid != 31) {
        struct a733_thermal_sample sample;
        const enum a733_thermal_result r = a733_thermal_read(&thermal, bb_power.deadline, &sample);
        REQUIRE(r == A733_THERMAL_BUSY || r == A733_THERMAL_AGAIN || r == A733_THERMAL_SAMPLE, "thermal read");
        REQUIRE(!thermal.fault, "thermal read uncertainty");
        if (r == A733_THERMAL_SAMPLE) {
            REQUIRE(!sample.alarm && !sample.alarm_off && !sample.critical, "unexpected thermal events");
            for (unsigned i = 0; i < A733_THERMAL_SENSORS; ++i)
                if (sample.valid & (1u << i)) {
                    set.code[i] = sample.code[i];
                    set.millicelsius[i] = sample.millicelsius[i];
                }
            set.valid |= sample.valid;
            set.observed_us = sample.observed_us;
            set.sequence = sample.sequence;
        }
        if (set.valid != 31)
            bb_power_pause("thermal set deadline");
    }
    const enum a733_thermal_level level =
        a733_thermal_policy_update(&policy, bb_power_now(), A733_THERMAL_SAMPLE, &set);
    struct bb_line l;
    bb_line_start(&l, "TEMP");
    bb_line_dec(&l, "set", sets++);
    bb_line_dec(&l, "level", level);
    static const char *const names[A733_THERMAL_SENSORS] = {"cpub_mc", "ddr_mc", "npu_mc", "cpul_mc", "gpu_mc"};
    for (unsigned i = 0; i < A733_THERMAL_SENSORS; ++i)
        bb_line_dec(&l, names[i], (uint64_t)(int64_t)set.millicelsius[i]);
    bb_line_end(&l);
    bb_power_print(&l);
    return level;
}

static void thermal_start(void)
{
    owner = xTaskGetCurrentTaskHandle();
    REQUIRE(a733_adc_native_init(&binding, &bb_power.locks, owner, unused_admit, NULL), "thermal binding");
    REQUIRE(current() && quiet(), "analog interrupts quiescent");
    const uint32_t apb1 = a733_mmio_read32(0x02002518) & 0x0300001f;
    const unsigned parent = apb1 >> 24, div = (apb1 & 31) + 1;
    const uint32_t hz = parent == 0 ? 24000000u : parent == 3 ? 600000000u : 0;
    REQUIRE(hz && !(hz % div), "APB1 selection");
    a733_clock_mask_t shared = A733_CLOCK_OSC | A733_CLOCK_REF | A733_CLOCK_FABRIC;
    if (parent == 3)
        shared |= A733_CLOCK_PERI0;
    REQUIRE(a733_clock_acquire(&bb_power.clocks, THERMAL_OWNER, shared, A733_CLOCK_THS | A733_CLOCK_GPADC24M,
                               bb_power.deadline, &thermal_token) == A733_CLOCK_OK, "exclusive analog lease");
    const uint32_t source = a733_mmio_read32(0x02002fc0);
    thermal_config = (struct a733_thermal_config){.owner = THERMAL_OWNER, .handoff = 1,
        .startup_deadline_us = bb_power.deadline, .apb1 = apb1, .apb1_hz = hz / div,
        .configure_adc_clock = !(source & (1u << 31))};
    thermal_ops = (struct a733_thermal_ops){now_us, admit, enter, leave, retain, thermal_read, thermal_write};
    REQUIRE(a733_thermal_init(&thermal, &thermal_config, &thermal_ops, NULL, &bb_power.clocks, &thermal_token),
            "thermal init");
    enum a733_thermal_result r;
    while ((r = a733_thermal_poll(&thermal)) != A733_THERMAL_READY) {
        REQUIRE((r == A733_THERMAL_PENDING || r == A733_THERMAL_BUSY) && !thermal.fault, "thermal startup");
        bb_power_pause("thermal startup deadline");
    }
    REQUIRE(a733_thermal_policy_init(&policy, bb_power_now(), notify, NULL), "thermal policy");
}

static void thermal_stop(void)
{
    enum a733_thermal_result r;
    while ((r = a733_thermal_stop(&thermal, bb_power.deadline)) != A733_THERMAL_STOPPED) {
        REQUIRE(r == A733_THERMAL_BUSY && !thermal.fault, "thermal configured disable");
        bb_power_pause("thermal disable deadline");
    }
}

/* The configured words of one domain, its pattern and LFM words and its
 * supply reading, after an inspection or a transition. */
static void words(const char *label, enum a733_cpu_domain d)
{
    struct a733_cpu_clock_report r;
    REQUIRE(a733_cpu_clock_report(cpu_read, NULL, &r), "clock report");
    const struct a733_cpu_clock_domain *x = &r.domain[d];
    struct bb_line l;
    bb_line_start(&l, label);
    bb_line_hex(&l, "pll", x->pll);
    bb_line_hex(&l, "mux", x->select);
    bb_line_dec(&l, "mhz", x->decoded ? x->hz / 1000000u : 0);
    bb_line_hex(&l, "pat0", cpu.words[d][0]);
    bb_line_hex(&l, "pat1", cpu.words[d][1]);
    bb_line_hex(&l, "lfm", cpu.words[d][2]);
    bb_line_end(&l);
    bb_power_print(&l);
}

/* A request deadline that never passes the session's. */
static uint64_t request_deadline(void)
{
    const uint64_t end = bb_power_now() + REQUEST_US;
    return end < bb_power.deadline ? end : bb_power.deadline;
}

static enum a733_cpu_clock_status finish(enum a733_cpu_clock_status r)
{
    while (r == A733_CPU_CLOCK_PENDING) {
        vTaskDelay(1);
        r = a733_cpu_clock_burst(&cpu, BURST_US);
    }
    return r;
}

static void inspect(enum a733_cpu_domain d, const char *label)
{
    REQUIRE(a733_cpu_clock_inspect(&cpu, d, request_deadline()) == A733_CPU_CLOCK_PENDING,
            "inspection start");
    REQUIRE(finish(A733_CPU_CLOCK_PENDING) == A733_CPU_CLOCK_COMPLETE, "inspection");
    words(label, d);
}

static void transition(uint32_t mhz, const char *label)
{
    const uint64_t start = bb_power_now();
    REQUIRE(a733_cpu_clock_request(&cpu, A733_CPU_BIG, mhz, request_deadline()) == A733_CPU_CLOCK_PENDING,
            "transition admission");
    REQUIRE(finish(A733_CPU_CLOCK_PENDING) == A733_CPU_CLOCK_COMPLETE, "transition");
    /* The transition kept DCDC3's voltage, so it made no selector write. */
    REQUIRE(cpu.from_uv == STEP_UV && cpu.to_uv == STEP_UV, "voltage write planned");
    const uint64_t end = bb_power_now();
    words(label, A733_CPU_BIG);
    struct bb_line l;
    bb_line_start(&l, "STEP");
    bb_line_dec(&l, "cpu_b_mhz", cpu.applied_mhz[A733_CPU_BIG]);
    bb_line_dec(&l, "dcdc3_uv", cpu.rail_uv[1]);
    bb_line_dec(&l, "parked_max_us", cpu.parked_max_us);
    bb_line_dec(&l, "elapsed_us", end - start);
    bb_line_dec(&l, "changes_left", cpu.changes);
    bb_line_end(&l);
    bb_power_print(&l);
}

static void client(void *unused)
{
    (void)unused;
    platform_puts("BEGIN board bundle 20: CPU-B 1014 -> 1404 -> 1014 MHz at 800 mV, cores 6 and 7 off\n");
    bb_power_wait_ready();
    uint64_t settle;
    if (!a733_deadline_after(bb_power_now(), A733_POWER_SUPPLY_SETTLE_US, &settle) || settle >= bb_power.deadline)
        bb_power_fail("settle deadline");
    while (bb_power_now() < settle)
        bb_power_pause("settle deadline");
    thermal_start();
    if (sensor_set() != A733_THERMAL_NORMAL)
        refuse("thermal level");

    /* The grant names CPU-B up to 1,404 MHz and two changes; the session is
     * this image, bound to port 3-3 by the approval record beside it. */
    const struct a733_cpu_clock_config config = {CPU_OWNER, {1, 1u << A733_CPU_BIG, {0, STEP_MHZ}, 2}};
    static const struct a733_cpu_clock_ops ops = {cpu_now, cpu_read, cpu_write};
    REQUIRE(a733_cpu_clock_init(&cpu, &config, &ops, NULL, &bb_power.clocks, &bb_power.rails, &bb_power.pmic,
                                &policy), "CPU clock init");
    inspect(A733_CPU_LITTLE, "CPU-L before");
    inspect(A733_CPU_DSU, "DSU before");
    inspect(A733_CPU_BIG, "CPU-B before");
    const struct a733_cpu_clock_report before = cpu.report;
    struct bb_line l;
    bb_line_start(&l, "SUPPLY");
    bb_line_dec(&l, "dcdc5_uv", cpu.rail_uv[0]);
    bb_line_dec(&l, "dcdc3_uv", cpu.rail_uv[1]);
    bb_line_hex(&l, "sid", cpu.report.sid_word);
    bb_line_dec(&l, "bin", cpu.report.binned ? cpu.report.bin : 99u);
    bb_line_dec(&l, "dcxo_hz", cpu.report.dcxo_hz);
    bb_line_end(&l);
    bb_power_print(&l);

    /* Admission of this step: the approval record's board state. */
    const struct a733_cpu_clock_domain *b = &cpu.report.domain[A733_CPU_BIG];
    if (!cpu.report.binned || cpu.report.bin != STEP_BIN)
        refuse("speed bin");
    if (cpu.rail_uv[1] != STEP_UV || cpu.rail_uv[0] != STEP_UV)
        refuse("CPU supplies");
    if (!b->decoded || b->hz != BASE_MHZ * 1000000ull)
        refuse("CPU-B rate");
    for (unsigned d = 0; d < A733_CPU_DOMAINS; ++d)
        if ((cpu.words[d][0] & (3u << 29)) || (cpu.words[d][1] >> 31) || (cpu.words[d][2] >> 31))
            refuse("pattern or LFM enabled");

    transition(STEP_MHZ, "CPU-B raised");
    transition(BASE_MHZ, "CPU-B restored");
    inspect(A733_CPU_BIG, "CPU-B after");
    REQUIRE(cpu.rail_uv[1] == STEP_UV, "DCDC3 changed");
    inspect(A733_CPU_LITTLE, "CPU-L after");
    inspect(A733_CPU_DSU, "DSU after");
    REQUIRE(cpu.rail_uv[0] == STEP_UV, "DCDC5 changed");
    for (unsigned d = 0; d < A733_CPU_DOMAINS; ++d)
        REQUIRE(cpu.report.domain[d].pll == before.domain[d].pll &&
                cpu.report.domain[d].select == before.domain[d].select, "clock words changed");
    REQUIRE(sensor_set() == A733_THERMAL_NORMAL, "thermal level after the step");
    thermal_stop();
    bb_power_park();
    platform_puts("COMPLETE board bundle 20: CPU-B 1404 MHz and back to 1014 MHz at 800 mV, no PMIC write\n");
    platform_exit(0);
}

void vApplicationStackOverflowHook(TaskHandle_t handle, char *name)
{
    (void)handle;
    platform_panic(name, 0);
}

void kernel_main(void)
{
    platform_init();
    if (!bb_power_snapshot_init(client, "cpu-opp-step"))
        platform_panic("bundle 20 power initialization", 0);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
