/* SPDX-License-Identifier: Apache-2.0 */
/* Bundle 4 of docs/designs/board-bundles-next.md: event counters, the core-0
 * walk A/B and idle WFI, in that order, on the returning single-core platform
 * built with the monitor's counter option (bundle 2's image).
 *
 * One task at priority 1 runs the three stages. Each stage sets a marker in
 * the log and in the progress record at RESULT_BASE + 128, which the forced
 * return to the ROM preserves:
 *
 *   magic, stage, wakes, last wake CNTPCT (four little-endian 64-bit words)
 *
 * 1. Counters, with IRQs masked around each region, so that no tick lands in
 *    one: 64 empty a733_cpu_pmu_begin/end regions; a CNTPCT loop of at least
 *    1 ms; a read of a cleaned and invalidated 64 KiB buffer, which shows
 *    whether L2D_CACHE_REFILL counts; PMCR_EL0, PMCEID0_EL0 and PMCEID1_EL0.
 * 2. Walk A/B, refused when CCU +0x588 reads MBUS_DFS_EN set: one discarded
 *    warm-up block, then twelve blocks in ABBA order. A is the uncached walk
 *    (TCR_EL1 0x200803019), B the cached walk that platform_init() installs
 *    (0x200803519). A block is 1,024 iterations of TLBI VMALLE1, DSB ISH, ISB
 *    and one load from a different 2 MiB block of the 128 MiB map; no load
 *    targets the result block or the block that holds this image, its stacks
 *    and the PMU state. Before each block a calibration region (TLBI VMALLE1,
 *    DSB ISH, ISB, then end) counts the walks of the region's own return
 *    path; a block whose DTLB_WALK differs from 1,024 plus that count is
 *    refused. No table write follows the final flush of both table pages in
 *    platform_init() (checked on the host by checks/board-bundles-apps.py),
 *    so uncached walks read the descriptors that cached walks read.
 * 3. WFI, last: over 1,000 ticks the task runs a733_cpu_idle() with IRQs
 *    unmasked, and after each wake reads PMCCNTR_EL0, the event counters and
 *    CNTPCT outside any masked region, and stores the wake count and stamp
 *    in the progress record.
 *
 * Success is status 0 with "PASS board bundle 4". If the core stops waking,
 * the monitor's secure timer returns to the ROM with status 2, interrupt 29:
 * the bundle's controlled outcome, which the runner matches on the stage
 * markers and the progress record. */
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"
#include "aarch64_device_memory.h"
#include "a733_cpu_pmu.h"
#include "a733_cpu_power.h"
#include "a733_mapping_check.h"
#include "a733_mmio.h"

#define EMPTY_REGIONS 64u
#define WALK_ITERATIONS 1024u
#define WALK_BLOCKS 13u /* one warm-up block, then twelve in ABBA order */
#define WFI_TICKS 1000u
#define TCR_CACHED UINT64_C(0x200803519)   /* platform_init(): IRGN0 = ORGN0 = 1 */
#define TCR_UNCACHED UINT64_C(0x200803019) /* IRGN0 = ORGN0 = 0 */
#define MBUS_CLK UINT32_C(0x02002588)
#define MBUS_DFS_EN (UINT32_C(1) << 28)
#define IMAGE_BASE UINT64_C(0x41000000)
#define DRAM_BASE UINT64_C(0x40000000)
#define BLOCK_SHIFT 21
#define RESULT_BLOCK ((RESULT_BASE - DRAM_BASE) >> BLOCK_SHIFT)
#define PROGRESS_MAGIC UINT64_C(0x3130535342344242) /* "BB4BSS01" */
#define DEADLINE_TICKS (COUNTER_HZ * 3 / 2)        /* 1.5 s after monitor entry */
#define L2_PROBE_BYTES 65536u

struct progress { uint64_t magic, stage, wakes, last; };
#define PROGRESS ((volatile struct progress *)(RESULT_BASE + 128))
_Static_assert(sizeof(struct boot_result) == 128 && 128 + sizeof(struct progress) <= LOG_OFFSET,
               "the progress record lies between the result header and the log");

extern char __heap_end[];
static StaticTask_t task;
static StackType_t stack[4096];
static uint8_t probe_buffer[L2_PROBE_BYTES] __attribute__((aligned(64)));

static void put(const char *name, uint64_t value)
{ platform_puts(name); platform_hex(value); platform_puts("\n"); }
static _Noreturn void fail(const char *text, uint64_t value)
{
    platform_puts("FAIL board bundle 4: ");
    platform_puts(text);
    platform_puts(" ");
    platform_hex(value);
    platform_puts("\n");
    platform_exit(1);
}
static void marker(unsigned stage, const char *text)
{
    PROGRESS->stage = stage;
    platform_puts(text);
    platform_puts("\n");
}
static void within_deadline(const char *stage)
{
    if (counter() - RESULT->start_counter >= DEADLINE_TICKS)
        fail(stage, counter() - RESULT->start_counter);
}
static uint64_t irq_mask(void)
{
    uint64_t daif;
    __asm__ volatile("mrs %0, daif\n msr daifset, #2" : "=r"(daif) :: "memory");
    return daif;
}
static void irq_restore(uint64_t daif)
{ __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory"); }
#define SYSREG(name) ({ uint64_t v_; __asm__ volatile("mrs %0, " #name : "=r"(v_) :: "memory"); v_; })

static void record_put(const char *name, const struct a733_cpu_pmu_record *r)
{
    platform_puts(name);
    platform_puts("\n");
    put("  cycles=", r->cycles);
    put("  ticks=", r->ticks);
    put("  supported=", r->supported);
    put("  overflow=", r->overflow);
    for (unsigned i = 0; i < A733_CPU_PMU_EVENTS; ++i)
        put("  event=", r->events[i]);
}
static void measure_status(int status, const char *what)
{
    if (status != A733_CPU_PMU_OK)
        fail(what, (uint64_t)status);
}

static void sort(uint64_t *v, unsigned n)
{
    for (unsigned i = 1; i < n; ++i)
        for (unsigned j = i; j && v[j - 1] > v[j]; --j) {
            const uint64_t t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
}

static void counters(void)
{
    struct a733_cpu_pmu_region region;
    struct a733_cpu_pmu_record record;
    static uint64_t cycles[EMPTY_REGIONS], instructions[EMPTY_REGIONS];
    uint64_t daif = irq_mask();
    for (unsigned i = 0; i < EMPTY_REGIONS; ++i) {
        measure_status(a733_cpu_pmu_begin(&region), "empty region begin");
        measure_status(a733_cpu_pmu_end(&region, &record), "empty region end");
        cycles[i] = record.cycles;
        instructions[i] = record.events[A733_CPU_PMU_INST_RETIRED];
    }
    irq_restore(daif);
    put("PMU midr=", record.midr);
    put("PMU cntfrq=", record.cntfrq);
    put("PMU counters=", record.counters);
    put("PMU supported=", record.supported);
    put("PMCR_EL0=", SYSREG(pmcr_el0));
    put("PMCEID0_EL0=", SYSREG(pmceid0_el0));
    put("PMCEID1_EL0=", SYSREG(pmceid1_el0));
    sort(cycles, EMPTY_REGIONS);
    sort(instructions, EMPTY_REGIONS);
    put("EMPTY cycles min=", cycles[0]);
    put("EMPTY cycles median=", cycles[EMPTY_REGIONS / 2]);
    put("EMPTY cycles max=", cycles[EMPTY_REGIONS - 1]);
    put("EMPTY instructions min=", instructions[0]);
    put("EMPTY instructions median=", instructions[EMPTY_REGIONS / 2]);
    put("EMPTY instructions max=", instructions[EMPTY_REGIONS - 1]);

    /* At least 1 ms of CNTPCT: cycles per tick give the CPU-L rate. */
    daif = irq_mask();
    measure_status(a733_cpu_pmu_begin(&region), "loop begin");
    uint64_t now, until = SYSREG(cntpct_el0) + COUNTER_HZ / 1000;
    __asm__ volatile("1: mrs %0, cntpct_el0\n cmp %0, %1\n b.lo 1b" : "=&r"(now) : "r"(until) : "cc");
    measure_status(a733_cpu_pmu_end(&region, &record), "loop end");
    irq_restore(daif);
    record_put("LOOP 1 ms", &record);
    if (record.ticks)
        put("LOOP cpu_hz=", record.cycles * COUNTER_HZ / record.ticks);

    /* One load per line of a buffer cleaned and invalidated to the point of
     * coherency: every load misses L1 and L2. */
    for (uintptr_t a = (uintptr_t)probe_buffer; a < (uintptr_t)probe_buffer + L2_PROBE_BYTES; a += 64)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    daif = irq_mask();
    measure_status(a733_cpu_pmu_begin(&region), "refill begin");
    for (unsigned i = 0; i < L2_PROBE_BYTES; i += 64)
        (void)*(volatile const uint8_t *)(probe_buffer + i);
    measure_status(a733_cpu_pmu_end(&region, &record), "refill end");
    irq_restore(daif);
    record_put("REFILL 1024 lines", &record);
    put("L2D_CACHE_REFILL supported=", (record.supported >> A733_CPU_PMU_L2D_CACHE_REFILL) & 1);
    put("L2D_CACHE_REFILL counted=", record.events[A733_CPU_PMU_L2D_CACHE_REFILL]);
}

static void tcr_install(uint64_t tcr)
{
    __asm__ volatile("msr tcr_el1, %0\n isb\n tlbi vmalle1\n dsb ish\n isb" :: "r"(tcr) : "memory");
    if (SYSREG(tcr_el1) != tcr)
        fail("TCR_EL1 readback", SYSREG(tcr_el1));
}

/* n iterations; k is the first block index. Registers only: no stack and no
 * other data access, so each iteration makes one data walk. */
static __attribute__((noinline)) void walk(uint64_t k, uint64_t n, uint64_t image)
{
    uint64_t address, value;
    __asm__ volatile(
        "1: tlbi vmalle1\n dsb ish\n isb\n"
        "   lsl %[a], %[k], #21\n add %[a], %[a], %[base]\n ldr %[v], [%[a]]\n"
        "   add %[k], %[k], #1\n and %[k], %[k], #63\n"
        "   cmp %[k], %[skip]\n cinc %[k], %[k], eq\n"
        "   cmp %[k], %[image]\n cinc %[k], %[k], eq\n"
        "   subs %[n], %[n], #1\n b.ne 1b\n"
        : [k] "+r"(k), [n] "+r"(n), [a] "=&r"(address), [v] "=&r"(value)
        : [base] "r"(DRAM_BASE), [skip] "r"((uint64_t)RESULT_BLOCK), [image] "r"(image)
        : "memory", "cc");
}

static void walks(void)
{
#ifdef MONITOR_QEMU
    const uint32_t mbus = 0; /* The QEMU virt model has no CCU. */
#else
    const uint32_t mbus = a733_mmio_read32_dsb_sy_both(MBUS_CLK);
#endif
    put("MBUS_CLK=", mbus);
    if (mbus & MBUS_DFS_EN) {
        marker(2, "STAGE 2 walk refused: MBUS_DFS_EN set");
        return;
    }
    const uint64_t image = IMAGE_BASE >> BLOCK_SHIFT & 63;
    /* The image, its stacks and the PMU state share one block, and the walk
     * cycle's two skips cannot meet or wrap. */
    if (((uintptr_t)__heap_end - 1 - DRAM_BASE) >> BLOCK_SHIFT != image ||
        ((uintptr_t)stack - DRAM_BASE) >> BLOCK_SHIFT != image || image + 1 == RESULT_BLOCK ||
        image == RESULT_BLOCK + 1 || image == 63)
        fail("image block", (uintptr_t)__heap_end);
    if (SYSREG(tcr_el1) != TCR_CACHED)
        fail("TCR_EL1 is not platform_init()'s value", SYSREG(tcr_el1));
    static const uint8_t uncached[WALK_BLOCKS] = {1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
    struct a733_cpu_pmu_region region;
    struct a733_cpu_pmu_record record, calibration;
    uint64_t installed = TCR_CACHED, k = 0;
    for (unsigned b = 0; b < WALK_BLOCKS; ++b) {
        const uint64_t tcr = uncached[b] ? TCR_UNCACHED : TCR_CACHED;
        const uint64_t daif = irq_mask();
        if (tcr != installed)
            tcr_install(installed = tcr);
        measure_status(a733_cpu_pmu_begin(&region), "calibration begin");
        __asm__ volatile("tlbi vmalle1\n dsb ish\n isb" ::: "memory");
        measure_status(a733_cpu_pmu_end(&region, &calibration), "calibration end");
        measure_status(a733_cpu_pmu_begin(&region), "walk begin");
        walk(k, WALK_ITERATIONS, image);
        measure_status(a733_cpu_pmu_end(&region, &record), "walk end");
        irq_restore(daif);
        for (unsigned i = 0; i < WALK_ITERATIONS; ++i) {
            k = (k + 1) & 63;
            k += k == RESULT_BLOCK;
            k += k == image;
        }
        const uint32_t expected = WALK_ITERATIONS + calibration.events[A733_CPU_PMU_DTLB_WALK];
        const unsigned counted = (record.supported >> A733_CPU_PMU_DTLB_WALK) & 1 &&
            !((record.overflow >> A733_CPU_PMU_DTLB_WALK) & 1);
        put(b ? "WALK block=" : "WALK warm-up block=", b);
        platform_puts(uncached[b] ? "  mode=A uncached walk\n" : "  mode=B cached walk\n");
        record_put("  record", &record);
        put("  calibration cycles=", calibration.cycles);
        put("  calibration dtlb_walk=", calibration.events[A733_CPU_PMU_DTLB_WALK]);
        put("  refused=", !counted || record.events[A733_CPU_PMU_DTLB_WALK] != expected);
    }
    const uint64_t daif = irq_mask();
    tcr_install(TCR_CACHED);
    irq_restore(daif);
    marker(2, "STAGE 2 walk complete");
}

static uint32_t event(unsigned i)
{
    switch (i) {
    case 0: return (uint32_t)SYSREG(pmevcntr0_el0);
    case 1: return (uint32_t)SYSREG(pmevcntr1_el0);
    case 2: return (uint32_t)SYSREG(pmevcntr2_el0);
    case 3: return (uint32_t)SYSREG(pmevcntr3_el0);
    case 4: return (uint32_t)SYSREG(pmevcntr4_el0);
    default: return (uint32_t)SYSREG(pmevcntr5_el0);
    }
}

/* The counters run free from the last region: every read here happens with
 * IRQs unmasked, outside any begin and end region. */
static void idle_wfi(unsigned counters)
{
    uint64_t events[A733_CPU_PMU_EVENTS] = {0}, last[A733_CPU_PMU_EVENTS] = {0};
    uint64_t wakes = 0, cycles = 0, span = 0, gap_max = 0, gap_min = UINT64_MAX;
    PROGRESS->wakes = 0;
    PROGRESS->last = counter();
    PROGRESS->magic = PROGRESS_MAGIC;
    marker(3, "STAGE 3 wfi begin");
    for (unsigned i = 0; i < counters; ++i)
        last[i] = event(i);
    uint64_t previous_cycles = SYSREG(pmccntr_el0), previous = SYSREG(cntpct_el0);
    const TickType_t first = xTaskGetTickCount();
#if BB4_MODEL_HANG
    /* Model only: stop the tick so that WFI never wakes. */
    __asm__ volatile("msr cntp_ctl_el0, xzr\n isb" ::: "memory");
#endif
    for (;;) {
        a733_cpu_idle();
        const uint64_t c = SYSREG(pmccntr_el0), t = SYSREG(cntpct_el0);
        for (unsigned i = 0; i < counters; ++i) {
            const uint32_t v = event(i);
            events[i] += (uint32_t)(v - (uint32_t)last[i]);
            last[i] = v;
        }
        PROGRESS->wakes = ++wakes;
        PROGRESS->last = t;
        cycles += c - previous_cycles;
        span += t - previous;
        if (t - previous > gap_max)
            gap_max = t - previous;
        if (t - previous < gap_min)
            gap_min = t - previous;
        previous_cycles = c;
        previous = t;
        if (xTaskGetTickCount() - first >= WFI_TICKS)
            break;
    }
    const TickType_t ticks = xTaskGetTickCount() - first;
    put("WFI wakes=", wakes);
    put("WFI ticks=", ticks);
    put("WFI cycles=", cycles);
    put("WFI cntpct=", span);
    put("WFI cntpct gap min=", gap_min);
    put("WFI cntpct gap max=", gap_max);
    for (unsigned i = 0; i < counters; ++i)
        put("WFI event=", events[i]);
    marker(4, "STAGE 3 wfi complete");
}

static void bundle(void *unused)
{
    (void)unused;
    static const struct a733_mapping_rule rule = {A733_MAPPING_READ, 1, {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}}, 0, 1};
    PROGRESS->magic = PROGRESS_MAGIC;
    PROGRESS->stage = 0;
    platform_puts("BEGIN board bundle 4: event counters, walk A/B and WFI\n");
    if (!a733_mapping_check(aarch64_device_translate, MBUS_CLK & ~UINT32_C(0xfff), MBUS_CLK & ~UINT32_C(0xfff), &rule))
        fail("CCU mapping", MBUS_CLK);
    counters();
    marker(1, "STAGE 1 counters complete");
    within_deadline("deadline after counters");
    walks();
    within_deadline("deadline after walks");
    struct a733_cpu_pmu_region region;
    struct a733_cpu_pmu_record record;
    const uint64_t daif = irq_mask();
    measure_status(a733_cpu_pmu_begin(&region), "counter count begin");
    measure_status(a733_cpu_pmu_end(&region, &record), "counter count end");
    irq_restore(daif);
    idle_wfi(record.counters);
    within_deadline("deadline after WFI");
    platform_puts("PASS board bundle 4: event counters, walk A/B and WFI\n");
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
    configASSERT(xTaskCreateStatic(bundle, "bundle4", 4096, NULL, 1, stack, &task) != NULL);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
