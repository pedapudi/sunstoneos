/* SPDX-License-Identifier: Apache-2.0 */
/* Board bundles 20 to 27: the CPU operating-point steps of
 * docs/designs/cpu-top-opp.md on the returning CPU0 image of bundle 15. Cores
 * 6 and 7 stay powered off. OPP_BUNDLE selects the step's plan:
 *
 *   20  step 1: CPU-B 1014 -> 1404 -> 1014 MHz at 800 mV;
 *   21  step 3: DCDC3 800 -> 1050 -> 800 mV with CPU-B held at 1014 MHz;
 *   22  step 2: CPU-L to 1196 MHz, then the DSU to 780 MHz, at 800 mV;
 *   23  step 4: CPU-B to 2002 MHz at 1050 mV, the DSU to 910 MHz, then the
 *       DSU to 598 MHz and CPU-B back to 1014 MHz at 800 mV;
 *   24  step 5: CPU-L to 1794 MHz at 1000 mV, then the DSU to 1274 MHz;
 *   25  step 6, first image: CPU-L 1794 MHz at 1000 mV and the DSU 1274 MHz,
 *       then DCDC3 1050 mV with CPU-B held at 1014 MHz; the rails stay for
 *       the ROM return;
 *   26  step 6, read-back before the workload: DCDC5 1000 mV and DCDC3
 *       1050 mV must have survived the ROM return, and the three domains
 *       must read the boot PLL and mux words; no write;
 *   27  step 6, read-back after the workload: temperatures, rails and the
 *       boot clock words, which the workload's own CPU-B lowering and the
 *       ROM return leave; no write.
 *
 * The FEL ROM's clock setup writes the CPU-L and DSU PLLs only: on each
 * return CPU-L and the DSU read 494 MHz again, but CPU-B keeps the PLL word
 * the last image left (board-session-cpu-top-opp-7, bundle 26). Every plan
 * is admitted only with CPU-B at 1014 MHz and ends with it there, and a
 * DCDC3 write runs with the CPU-B clock lease held and CPU-B at 1014 MHz.
 *
 * The client task owns the thermal driver, the thermal policy and the CPU
 * clock driver. It reads one full sensor set and inspects the three CPU
 * domains read-only (DCDC5 and DCDC3 snapshots; PLL, mux, pattern and LFM
 * words). It admits the step only at the NORMAL thermal level with every
 * sensor below OPP_LIMIT_MC, on bin vf0400, with DCDC3 and DCDC5 at the
 * plan's entry voltages (800 mV unless it names others), CPU-B at 1014 MHz, CPU-L and the DSU on decodable PLL rates and every
 * pattern and LFM enable clear; otherwise it returns with a REFUSED line
 * before any write. Each clock change goes through the driver in 200 us
 * bursts and is followed by a check of the rail voltage it planned. A sensor
 * set follows each change that raises a rate or a voltage; a sensor at or
 * above OPP_LIMIT_MC, or a level other than NORMAL, stops the step: what it
 * raised is lowered again (the DSU to 598 MHz, the clusters to 1014 MHz,
 * DCDC3 to 800 mV) and the image returns with a STOPPED line. CPU0's rate is
 * measured with its cycle counter against CNTPCT around each CPU-L change. No
 * step reads or depends on the memory bus clock. At the end the three domains
 * are inspected again and must read the plan's rates and rails; a last sensor
 * set follows, the converter is disabled and the PMIC service parked. */
#include "power.h"
#include "platform.h"
#include "a733_range.h"
#include "a733_mmio.h"
#include "a733_adc_native.h"
#include "a733_thermal.h"
#include "a733_thermal_native.h"
#include "a733_cpu_clock.h"
#include "a733_cpu_pmu.h"

#ifndef OPP_BUNDLE
#define OPP_BUNDLE 20
#endif
#define CPU_OWNER 4u
#define THERMAL_OWNER 5u
#define VOLT_OWNER 6u
#define BASE_MHZ 1014u  /* CPU-B rate that the boot firmware leaves. */
#define STEP_BIN 4u     /* Column vf0400: SID code 0x04, port 3-3's fuse word 0x496. */
#define BOOT_MV 800u    /* DCDC3 and DCDC5 at selector 0x1e (bit 7 set: 0x9e). */
/* Thermal stop: the board kernel's first passive trip of both CPU zones
 * (sun60iw2p1.dtsi cpul_threshold and cpub_threshold, 70 C). */
#define OPP_LIMIT_MC 70000
#define REQUEST_US 120000u
#define BURST_US 200u
#define VOLT_SETTLE_US 1000u /* The vendor's wait for any change of up to 250 mV. */
#define RATE_TICKS 24000u    /* 1 ms of the 24 MHz counter. */
#define UNWIND_CHANGES 3u    /* DSU, CPU-B and CPU-L lowerings of a thermal stop. */

enum { OP_CLOCK, OP_VOLT, OP_RATE };
/* OP_CLOCK: one domain to mhz, with mv the planned voltage of its rail.
 * OP_VOLT: DCDC3 to mv with CPU-B held. OP_RATE: measure CPU0. */
struct op { uint8_t kind, domain; uint16_t mhz, mv; };
struct plan {
    const char *text;              /* BEGIN and COMPLETE text. */
    uint32_t clusters, max_mhz[2]; /* The grant; a CPU-L grant also admits the DSU. */
    unsigned count;
    struct op ops[6];
    uint16_t entry_mv[2];          /* DCDC5 and DCDC3 at admission; 0: 800 mV. */
};
#if OPP_BUNDLE == 20
static const struct plan plan = {"CPU-B 1404 MHz and back to 1014 MHz at 800 mV, no PMIC write",
    1u << A733_CPU_BIG, {0, 1404}, 2,
    {{OP_CLOCK, A733_CPU_BIG, 1404, 800}, {OP_CLOCK, A733_CPU_BIG, 1014, 800}}, {0, 0}};
#elif OPP_BUNDLE == 21
static const struct plan plan = {"DCDC3 1050 mV and back to 800 mV, CPU-B held at 1014 MHz",
    0, {0, 0}, 2, {{OP_VOLT, A733_CPU_BIG, 0, 1050}, {OP_VOLT, A733_CPU_BIG, 0, 800}}, {0, 0}};
#elif OPP_BUNDLE == 22
static const struct plan plan = {"CPU-L 1196 MHz and the DSU 780 MHz at 800 mV",
    1u << A733_CPU_LITTLE, {1196, 0}, 4,
    {{OP_RATE, 0, 0, 0}, {OP_CLOCK, A733_CPU_LITTLE, 1196, 800}, {OP_RATE, 0, 0, 0},
     {OP_CLOCK, A733_CPU_DSU, 780, 800}}, {0, 0}};
#elif OPP_BUNDLE == 23
static const struct plan plan = {"CPU-B 2002 MHz at 1050 mV with the DSU at 910 MHz, then CPU-B 1014 MHz at 800 mV",
    (1u << A733_CPU_LITTLE) | (1u << A733_CPU_BIG), {1196, 2002}, 4,
    {{OP_CLOCK, A733_CPU_BIG, 2002, 1050}, {OP_CLOCK, A733_CPU_DSU, 910, 800},
     {OP_CLOCK, A733_CPU_DSU, 598, 800}, {OP_CLOCK, A733_CPU_BIG, 1014, 800}}, {0, 0}};
#elif OPP_BUNDLE == 24
static const struct plan plan = {"CPU-L 1794 MHz at 1000 mV and the DSU 1274 MHz",
    1u << A733_CPU_LITTLE, {1794, 0}, 4,
    {{OP_RATE, 0, 0, 0}, {OP_CLOCK, A733_CPU_LITTLE, 1794, 1000}, {OP_RATE, 0, 0, 0},
     {OP_CLOCK, A733_CPU_DSU, 1274, 1000}}, {0, 0}};
#elif OPP_BUNDLE == 25
/* Step 6, before the workload: DCDC5 to 1000 mV through CPU-L's top point
 * and the DSU's, then DCDC3 to 1050 mV with CPU-B held at 1014 MHz. The ROM
 * return resets CPU-L and the DSU and keeps the rails, which bundle 26 reads
 * back; CPU-B, which the ROM does not reset, never leaves 1014 MHz here. */
static const struct plan plan = {"CPU-L 1794 MHz and DCDC5 1000 mV, the DSU 1274 MHz, DCDC3 1050 mV with CPU-B at 1014 MHz",
    1u << A733_CPU_LITTLE, {1794, 0}, 3,
    {{OP_CLOCK, A733_CPU_LITTLE, 1794, 1000}, {OP_CLOCK, A733_CPU_DSU, 1274, 1000},
     {OP_VOLT, A733_CPU_BIG, 0, 1050}}, {0, 0}};
#elif OPP_BUNDLE == 26 || OPP_BUNDLE == 27
/* Step 6 read-backs with no clock or PMIC write: DCDC5 1000 mV and DCDC3
 * 1050 mV kept across the ROM return, and the three domains at their boot
 * words, before the workload (26) and after it (27). */
static const struct plan plan = {OPP_BUNDLE == 26 ? "DCDC5 1000 mV and DCDC3 1050 mV kept across the ROM return, read only"
                                                  : "temperatures, rails and boot clock words after the top-point workload, read only",
    0, {0, 0}, 0, {{OP_RATE, 0, 0, 0}}, {1000, 1050}};
#else
#error OPP_BUNDLE names no step
#endif
#define STR2(x) #x
#define STR(x) STR2(x)
#define BUNDLE "board bundle " STR(OPP_BUNDLE)

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

static int hottest_mc;

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
/* The end of a controlled outcome: the converter is disabled and the PMIC
 * service parked, so the board returns in a recorded state. */
static _Noreturn void controlled(const char *word, const char *what)
{
    thermal_stop();
    bb_power_park();
    platform_puts(word);
    platform_puts(" " BUNDLE ": ");
    platform_puts(what);
    platform_puts("\n");
    platform_exit(1);
}
/* A refusal before any clock or PMIC write leaves the inherited state. */
static _Noreturn void refuse(const char *what) { controlled("REFUSED", what); }

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
    hottest_mc = set.millicelsius[0];
    for (unsigned i = 1; i < A733_THERMAL_SENSORS; ++i)
        if (set.millicelsius[i] > hottest_mc)
            hottest_mc = set.millicelsius[i];
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
/* The configured words of one domain and its pattern and LFM words. */
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

static const char *const before_label[A733_CPU_DOMAINS] = {"CPU-L before", "CPU-B before", "DSU before"};
static const char *const set_label[A733_CPU_DOMAINS] = {"CPU-L set", "CPU-B set", "DSU set"};
static const char *const after_label[A733_CPU_DOMAINS] = {"CPU-L after", "CPU-B after", "DSU after"};

static void inspect(enum a733_cpu_domain d, const char *label)
{
    REQUIRE(a733_cpu_clock_inspect(&cpu, d, request_deadline()) == A733_CPU_CLOCK_PENDING, "inspection start");
    REQUIRE(finish(A733_CPU_CLOCK_PENDING) == A733_CPU_CLOCK_COMPLETE, "inspection");
    words(label, d);
}

static unsigned supply(enum a733_cpu_domain d) { return d == A733_CPU_BIG; }
static uint32_t rail_mv[2] = {BOOT_MV, BOOT_MV}; /* Planned DCDC5 and DCDC3 voltages. */
static uint32_t now_mhz[A733_CPU_DOMAINS];        /* Rates the plan has set so far. */
static unsigned raised;                            /* Domains raised above entry; bit 3: DCDC3. */
static struct a733_cpu_clock_report entry;         /* The report at admission. */

/* One driver transition, then the rail voltage it planned. */
static void clock_change(enum a733_cpu_domain d, uint32_t mhz, uint32_t mv)
{
    const uint64_t start = bb_power_now();
    REQUIRE(a733_cpu_clock_request(&cpu, d, mhz, request_deadline()) == A733_CPU_CLOCK_PENDING,
            "transition admission");
    REQUIRE(finish(A733_CPU_CLOCK_PENDING) == A733_CPU_CLOCK_COMPLETE, "transition");
    const uint64_t end = bb_power_now();
    if (d != A733_CPU_DSU)
        rail_mv[supply(d)] = mv;
    REQUIRE(cpu.rail_uv[supply(d)] == rail_mv[supply(d)] * 1000u, "rail voltage other than planned");
    if (mhz > now_mhz[d])
        raised |= 1u << d;
    now_mhz[d] = mhz;
    words(set_label[d], d);
    struct bb_line l;
    bb_line_start(&l, "STEP");
    bb_line_dec(&l, "domain", d);
    bb_line_dec(&l, "mhz", cpu.applied_mhz[d]);
    bb_line_dec(&l, "dcdc5_uv", cpu.rail_uv[0]);
    bb_line_dec(&l, "dcdc3_uv", cpu.rail_uv[1]);
    bb_line_dec(&l, "parked_max_us", cpu.parked_max_us);
    bb_line_dec(&l, "elapsed_us", end - start);
    bb_line_end(&l);
    bb_power_print(&l);
}

/* One DCDC3 request under the step's exclusive lease: a snapshot, or a
 * selector write that the provider reads back. */
static void dcdc3_request(const struct a733_rail_token *t, int write, uint8_t expected, uint8_t target,
                          struct a733_pmic_snapshot *out)
{
    struct a733_pmic_request q = {0};
    enum a733_pmic_status r;
    while ((r = write ? a733_pmic_submit_write(&bb_power.pmic, VOLT_OWNER, t, A733_RAIL_DCDC3, expected, target,
                                               bb_power.deadline, &q)
                      : a733_pmic_submit(&bb_power.pmic, VOLT_OWNER, t, A733_RAIL_DCDC3, bb_power.deadline, &q)) ==
           A733_PMIC_BUSY)
        bb_power_pause("PMIC submit deadline");
    REQUIRE(r == A733_PMIC_PENDING, "PMIC submit");
    while ((r = a733_pmic_result(&bb_power.pmic, &q, out)) == A733_PMIC_PENDING || r == A733_PMIC_BUSY)
        bb_power_pause("PMIC result deadline");
    REQUIRE(r == A733_PMIC_COMPLETE && out->rail == A733_RAIL_DCDC3, "PMIC result");
    while ((r = a733_pmic_reap(&bb_power.pmic, &q)) == A733_PMIC_BUSY)
        bb_power_pause("PMIC reap deadline");
    REQUIRE(r == A733_PMIC_COMPLETE, "PMIC reap");
}

/* DCDC3 to mv with CPU-B's clock held exclusively, so its rate stays at
 * 1014 MHz, which both 800 and 1050 mV cover; then the vendor's 1 ms. */
static void dcdc3_change(uint32_t mv)
{
    struct a733_clock_token hold = {0};
    struct a733_rail_token rail = {0};
    REQUIRE(a733_clock_acquire(&bb_power.clocks, VOLT_OWNER, 0, A733_CLOCK_CPU_B, request_deadline(), &hold) ==
            A733_CLOCK_OK, "CPU-B clock lease");
    enum a733_rail_result rr = a733_rail_acquire(&bb_power.rails, VOLT_OWNER, 0, A733_RAIL_BIT(A733_RAIL_DCDC3),
                                                 request_deadline(), &rail);
    REQUIRE(rr == A733_RAIL_PENDING || rr == A733_RAIL_READY, "DCDC3 lease");
    while ((rr = a733_rail_poll(&bb_power.rails, &rail)) != A733_RAIL_READY) {
        REQUIRE(rr == A733_RAIL_PENDING || rr == A733_RAIL_BUSY, "DCDC3 lease poll");
        bb_power_pause("DCDC3 lease deadline");
    }
    /* Admission found CPU-B at 1014 MHz, which every DCDC3 point covers, and
     * the clock lease held here keeps it there; no plan with a DCDC3 write
     * changes CPU-B. */
    REQUIRE(now_mhz[A733_CPU_BIG] == BASE_MHZ, "CPU-B not at 1014 MHz for the DCDC3 write");
    struct a733_pmic_snapshot s;
    dcdc3_request(&rail, 0, 0, 0, &s);
    REQUIRE(s.enabled && s.decoded && s.programmed_uv == rail_mv[1] * 1000u, "DCDC3 before the write");
    dcdc3_request(&rail, 1, (uint8_t)(s.selector & 127u), (uint8_t)((mv - 500u) / 10u), &s);
    REQUIRE(s.enabled && s.decoded && s.programmed_uv == mv * 1000u, "DCDC3 readback");
    const uint64_t settled = bb_power_now() + VOLT_SETTLE_US;
    while (bb_power_now() < settled)
        bb_power_pause("DCDC3 settle deadline");
    dcdc3_request(&rail, 0, 0, 0, &s);
    REQUIRE(s.enabled && s.decoded && s.programmed_uv == mv * 1000u, "DCDC3 after the settle");
    while ((rr = a733_rail_release(&bb_power.rails, &rail, request_deadline())) != A733_RAIL_READY) {
        REQUIRE(rr == A733_RAIL_PENDING || rr == A733_RAIL_BUSY, "DCDC3 release");
        bb_power_pause("DCDC3 release deadline");
    }
    enum a733_clock_result cr;
    while ((cr = a733_clock_release(&bb_power.clocks, &hold, request_deadline())) == A733_CLOCK_BUSY)
        bb_power_pause("CPU-B clock release deadline");
    REQUIRE(cr == A733_CLOCK_OK, "CPU-B clock release");
    if (mv > rail_mv[1])
        raised |= 1u << 3;
    rail_mv[1] = mv;
    struct bb_line l;
    bb_line_start(&l, "VOLT");
    bb_line_hex(&l, "dcdc3_selector", s.selector);
    bb_line_dec(&l, "dcdc3_uv", s.programmed_uv);
    bb_line_end(&l);
    bb_power_print(&l);
}

/* CPU0's cycles over 1 ms of CNTPCT with IRQs masked, as bundle 4 measures
 * them; within 1% of CPU-L's configured rate. */
static void rate(void)
{
    struct a733_cpu_pmu_region region;
    struct a733_cpu_pmu_record record;
    uint64_t daif, now;
    __asm__ volatile("mrs %0, daif\n msr daifset, #2" : "=r"(daif) :: "memory");
    const int begun = a733_cpu_pmu_begin(&region);
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
    const uint64_t until = now + RATE_TICKS;
    __asm__ volatile("1: mrs %0, cntpct_el0\n cmp %0, %1\n b.lo 1b" : "=&r"(now) : "r"(until) : "cc");
    const int ended = a733_cpu_pmu_end(&region, &record);
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
    REQUIRE(begun == A733_CPU_PMU_OK && ended == A733_CPU_PMU_OK && record.ticks && record.cntfrq, "CPU0 cycle counter");
    const uint64_t hz = record.cycles * record.cntfrq / record.ticks;
    struct a733_cpu_clock_report r;
    REQUIRE(a733_cpu_clock_report(cpu_read, NULL, &r) && r.domain[A733_CPU_LITTLE].decoded, "clock report");
    const uint64_t configured = r.domain[A733_CPU_LITTLE].hz;
    struct bb_line l;
    bb_line_start(&l, "RATE");
    bb_line_dec(&l, "cpu0_hz", hz);
    bb_line_dec(&l, "configured_hz", configured);
    bb_line_dec(&l, "cycles", record.cycles);
    bb_line_dec(&l, "ticks", record.ticks);
    bb_line_end(&l);
    bb_power_print(&l);
    REQUIRE((hz > configured ? hz - configured : configured - hz) * 100u <= configured, "measured CPU-L rate");
}

/* Thermal stop: lower what this step raised, the DSU before the clusters as
 * the vendor orders it, then DCDC3, and return. */
static _Noreturn void stop(void)
{
    if ((raised & (1u << A733_CPU_DSU)) && now_mhz[A733_CPU_DSU] > 598u)
        clock_change(A733_CPU_DSU, 598, rail_mv[0]);
    if ((raised & (1u << A733_CPU_BIG)) && now_mhz[A733_CPU_BIG] > BASE_MHZ)
        clock_change(A733_CPU_BIG, BASE_MHZ, BOOT_MV);
    if ((raised & (1u << A733_CPU_LITTLE)) && now_mhz[A733_CPU_LITTLE] > BASE_MHZ)
        clock_change(A733_CPU_LITTLE, BASE_MHZ, BOOT_MV);
    if ((raised & (1u << 3)) && rail_mv[1] > BOOT_MV)
        dcdc3_change(BOOT_MV);
    controlled("STOPPED", "thermal limit");
}

static void check_temperature(void)
{
    if (sensor_set() != A733_THERMAL_NORMAL || hottest_mc >= OPP_LIMIT_MC)
        stop();
}

/* The highest voltage of a domain's column, from the driver's tables. */
static uint32_t top_mv(enum a733_cpu_domain d, unsigned bin)
{
    unsigned count;
    const struct a733_cpu_opp *t = a733_cpu_clock_opps(d, &count);
    uint32_t mv = 0;
    for (unsigned i = 0; t && i < count; ++i) {
        const uint32_t v = a733_cpu_clock_opp_mv(&t[i], bin);
        if (v > mv)
            mv = v;
    }
    return mv;
}

static void client(void *unused)
{
    (void)unused;
    platform_puts("BEGIN " BUNDLE ": ");
    platform_puts(plan.text);
    platform_puts(", cores 6 and 7 off\n");
    bb_power_wait_ready();
    uint64_t settle;
    if (!a733_deadline_after(bb_power_now(), A733_POWER_SUPPLY_SETTLE_US, &settle) || settle >= bb_power.deadline)
        bb_power_fail("settle deadline");
    while (bb_power_now() < settle)
        bb_power_pause("settle deadline");
    thermal_start();
    if (sensor_set() != A733_THERMAL_NORMAL || hottest_mc >= OPP_LIMIT_MC)
        refuse("thermal level");

    /* The grant names the plan's clusters and maxima, with a change for each
     * transition and for each lowering of a thermal stop; the session is this
     * image, bound to port 3-3 by the approval record beside it. */
    unsigned transitions = 0;
    for (unsigned i = 0; i < plan.count; ++i)
        transitions += plan.ops[i].kind == OP_CLOCK;
    const struct a733_cpu_clock_config config = {CPU_OWNER, {plan.clusters ? 1u : 0u, plan.clusters,
        {plan.max_mhz[0], plan.max_mhz[1]}, plan.clusters ? transitions + UNWIND_CHANGES : 0}};
    static const struct a733_cpu_clock_ops ops = {cpu_now, cpu_read, cpu_write};
    REQUIRE(a733_cpu_clock_init(&cpu, &config, &ops, NULL, &bb_power.clocks, &bb_power.rails, &bb_power.pmic,
                                &policy), "CPU clock init");
    for (unsigned d = 0; d < A733_CPU_DOMAINS; ++d)
        inspect((enum a733_cpu_domain)d, before_label[d]);
    entry = cpu.report;
    struct bb_line l;
    bb_line_start(&l, "SUPPLY");
    bb_line_dec(&l, "dcdc5_uv", cpu.rail_uv[0]);
    bb_line_dec(&l, "dcdc3_uv", cpu.rail_uv[1]);
    bb_line_hex(&l, "sid", entry.sid_word);
    bb_line_dec(&l, "bin", entry.binned ? entry.bin : 99u);
    bb_line_dec(&l, "dcxo_hz", entry.dcxo_hz);
    bb_line_end(&l);
    bb_power_print(&l);

    /* Admission: the approval record's board state. */
    if (!entry.binned || entry.bin != STEP_BIN)
        refuse("speed bin");
    for (unsigned r = 0; r < 2; ++r)
        rail_mv[r] = plan.entry_mv[r] ? plan.entry_mv[r] : BOOT_MV;
    if (cpu.rail_uv[1] != rail_mv[1] * 1000u || cpu.rail_uv[0] != rail_mv[0] * 1000u)
        refuse("CPU supplies");
    if (!entry.domain[A733_CPU_BIG].decoded || entry.domain[A733_CPU_BIG].hz != BASE_MHZ * 1000000ull)
        refuse("CPU-B rate");
    for (unsigned d = 0; d < A733_CPU_DOMAINS; ++d) {
        const struct a733_cpu_clock_domain *x = &entry.domain[d];
        if (!x->decoded || x->source != 3u || x->hz % 1000000u)
            refuse("CPU rate not on its PLL");
        now_mhz[d] = (uint32_t)(x->hz / 1000000u);
        if ((cpu.words[d][0] & (3u << 29)) || (cpu.words[d][1] >> 31) || (cpu.words[d][2] >> 31))
            refuse("pattern or LFM enabled");
    }
#if OPP_BUNDLE == 26 || OPP_BUNDLE == 27
    /* The boot words (bundle 14 on port 3-3): CPU-L and the DSU at 494 MHz,
     * which the ROM return restores, and CPU-B at 1014 MHz, which bundle 25
     * never changes and the workload restores before its return. */
    static const uint32_t boot_words[A733_CPU_DOMAINS][2] = {
        {0xf8802600u, 0x03010000u}, {0xf8802700u, 0x03000000u}, {0xf8802600u, 0x03010105u}};
    for (unsigned d = 0; d < A733_CPU_DOMAINS; ++d)
        if (entry.domain[d].pll != boot_words[d][0] || entry.domain[d].select != boot_words[d][1])
            refuse("boot clock words");
#endif
    /* No planned voltage may exceed the bin's top point on its rail: DCDC5
     * feeds CPU-L and the DSU, DCDC3 CPU-B (1000 and 1050 mV in vf0400). */
    uint32_t limit = top_mv(A733_CPU_LITTLE, entry.bin);
    if (top_mv(A733_CPU_DSU, entry.bin) > limit)
        limit = top_mv(A733_CPU_DSU, entry.bin);
    const uint32_t limits[2] = {limit, top_mv(A733_CPU_BIG, entry.bin)};
    for (unsigned i = 0; i < plan.count; ++i)
        if (plan.ops[i].kind != OP_RATE && plan.ops[i].mv > limits[supply((enum a733_cpu_domain)plan.ops[i].domain)])
            refuse("voltage above the bin's top point");

    for (unsigned i = 0; i < plan.count; ++i) {
        const struct op *o = &plan.ops[i];
        const unsigned before = raised;
        if (o->kind == OP_RATE) {
            rate();
            continue;
        }
        const uint32_t was = o->kind == OP_CLOCK ? now_mhz[o->domain] : rail_mv[1];
        if (o->kind == OP_CLOCK)
            clock_change((enum a733_cpu_domain)o->domain, o->mhz, o->mv);
        else
            dcdc3_change(o->mv);
        /* A sensor set after each raise of a rate or a voltage. */
        if ((o->kind == OP_CLOCK ? o->mhz > was : o->mv > was) || raised != before)
            check_temperature();
    }

    /* The plan's end state: every rate and both rails; a domain the plan did
     * not change keeps its words. */
    for (unsigned d = 0; d < A733_CPU_DOMAINS; ++d)
        inspect((enum a733_cpu_domain)d, after_label[d]);
    REQUIRE(cpu.rail_uv[0] == rail_mv[0] * 1000u && cpu.rail_uv[1] == rail_mv[1] * 1000u, "rails at the end");
    for (unsigned d = 0; d < A733_CPU_DOMAINS; ++d) {
        const struct a733_cpu_clock_domain *x = &cpu.report.domain[d], *y = &entry.domain[d];
        REQUIRE(x->decoded && x->hz == (uint64_t)now_mhz[d] * 1000000u, "rate at the end");
        if (now_mhz[d] * 1000000ull == y->hz)
            REQUIRE(x->pll == y->pll && x->select == y->select, "clock words of an unchanged domain");
    }
    /* The last sensor set stops the step as the earlier ones do. */
    check_temperature();
    thermal_stop();
    bb_power_park();
    platform_puts("COMPLETE " BUNDLE ": ");
    platform_puts(plan.text);
    platform_puts("\n");
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
        platform_panic(BUNDLE " power initialization", 0);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
