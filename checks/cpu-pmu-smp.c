/* Two-core QEMU fixture for platform/a733/cpu_pmu.c in the FreeRTOS SMP port.
 *
 * Every task runs at EL1 (the privileged-task port patch, with the selector
 * below). checks/cpu-pmu-smp.py runs this image under -icount shift=0, where
 * INST_RETIRED is exact, once on cortex-a55 and once on cortex-a76 cores.
 *
 * Both cores at once: back-to-back begin and end, and a region around a loop
 * of 2k instructions for k = 0 and K_ITERATIONS. Then on core 0: a nested
 * region, reconfiguration after PMCR_EL0.E is cleared, a region that ends
 * after E is cleared, a task with a two-core mask, and a pinned task that
 * the peer moves to core 1 inside its region. Each result line is
 * "MEASURE name=0x...". */
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "a733_cpu_pmu.h"

extern _Noreturn void platform_panic(const char *, unsigned);
#define CHECK(x) do { if (!(x)) platform_panic(__FILE__, __LINE__); } while (0)
#define K_ITERATIONS 1000u
#define ATTEMPTS 3u

/* Every task starts at EL1. */
BaseType_t npu_service_task_privileged(TaskFunction_t code, void *parameters);
BaseType_t npu_service_task_privileged(TaskFunction_t code, void *parameters)
{
    (void)code; (void)parameters;
    return pdTRUE;
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name);
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    platform_panic(name, 0);
}

#define READ(name) ({ uint64_t v_; __asm__ volatile("mrs %0, " #name : "=r"(v_) :: "memory"); v_; })

static void say(const char *label, unsigned core, uint64_t value)
{
    platform_puts("MEASURE ");
    platform_puts(label);
    platform_puts(core ? "_core1=" : "_core0=");
    platform_hex(value);
    platform_puts("\n");
}

/* Back-to-back begin and end. */
static __attribute__((noinline)) int empty(struct a733_cpu_pmu_record *record)
{
    struct a733_cpu_pmu_region region;
    if (a733_cpu_pmu_begin(&region) != A733_CPU_PMU_OK) return 0;
    return a733_cpu_pmu_end(&region, record) == A733_CPU_PMU_OK;
}

/* The asm block retires 1 instruction for k = 0 and 1 + 2k otherwise; the
 * code around it is the same for every k. */
static __attribute__((noinline)) int loop(uint64_t k, struct a733_cpu_pmu_record *record)
{
    struct a733_cpu_pmu_region region;
    if (a733_cpu_pmu_begin(&region) != A733_CPU_PMU_OK) return 0;
    __asm__ volatile("cbz %0, 2f\n1: subs %0, %0, #1\n b.ne 1b\n2:" : "+r"(k) :: "cc");
    return a733_cpu_pmu_end(&region, record) == A733_CPU_PMU_OK;
}

struct result {
    uint64_t empty, loop0, loopk, cycles, ticks;
    unsigned core, counters, supported, overflow;
    uint32_t midr, cntfrq;
    uint64_t expect_midr, expect_cntfrq, expect_supported, expect_counters;
} __attribute__((aligned(64)));
static struct result results[2];
static volatile uint32_t arrived, peer_phase;
static TaskHandle_t main_handle, peer_handle;

/* QEMU's PMCEID bits for the six events, read independently of the module. */
static unsigned qemu_supported(unsigned counters)
{
    const uint64_t c0 = READ(pmceid0_el0), c1 = READ(pmceid1_el0);
    const unsigned events[6] = {0x08, 0x03, 0x17, 0x19, 0x34, 0x35};
    unsigned mask = 0;
    for (unsigned i = 0; i < counters; ++i)
        mask |= (unsigned)(((events[i] < 0x20 ? c0 >> events[i] : c1 >> (events[i] - 0x20)) & 1) << i);
    return mask;
}

static void measure(unsigned core)
{
    struct result *r = &results[core];
    struct a733_cpu_pmu_record record;
    r->empty = r->loop0 = r->loopk = UINT64_MAX;
    /* A tick inside a region adds its handler's instructions; keep the least. */
    for (unsigned attempt = 0; attempt < ATTEMPTS; ++attempt) {
        CHECK(empty(&record));
        if (record.events[0] < r->empty) r->empty = record.events[0];
        CHECK(loop(0, &record));
        if (record.events[0] < r->loop0) r->loop0 = record.events[0];
        CHECK(loop(K_ITERATIONS, &record));
        if (record.events[0] < r->loopk) r->loopk = record.events[0];
    }
    r->cycles = record.cycles;
    r->ticks = record.ticks;
    r->core = record.core;
    r->counters = record.counters;
    r->supported = record.supported;
    r->overflow = record.overflow;
    r->midr = record.midr;
    r->cntfrq = record.cntfrq;
    const uint64_t n = (READ(pmcr_el0) >> 11) & 0x1f;
    r->expect_counters = n < 6 ? n : 6;
    r->expect_supported = qemu_supported((unsigned)r->expect_counters);
    r->expect_midr = READ(midr_el1);
    r->expect_cntfrq = READ(cntfrq_el0);
    CHECK(record.format == A733_CPU_PMU_FORMAT && record.status == A733_CPU_PMU_OK);
    CHECK(record.reserved0 == 0 && record.reserved1 == 0);
    for (unsigned i = 1; i < A733_CPU_PMU_EVENTS; ++i)
        CHECK((record.supported >> i) & 1 || record.events[i] == 0);
}

static void report(unsigned core)
{
    const struct result *r = &results[core];
    say("empty_instructions", core, r->empty);
    say("loop0_instructions", core, r->loop0);
    say("loopk_instructions", core, r->loopk);
    say("loopk_cycles", core, r->cycles);
    say("loopk_ticks", core, r->ticks);
    say("record_core", core, r->core);
    say("record_counters", core, r->counters);
    say("record_supported", core, r->supported);
    say("record_overflow", core, r->overflow);
    say("record_midr", core, r->midr);
    say("record_cntfrq", core, r->cntfrq);
    say("expect_counters", core, r->expect_counters);
    say("expect_supported", core, r->expect_supported);
    say("expect_midr", core, r->expect_midr);
    say("expect_cntfrq", core, r->expect_cntfrq);
}

static void barrier(uint32_t round)
{
    __atomic_add_fetch(&arrived, 1, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&arrived, __ATOMIC_ACQUIRE) < 2 * round) {}
}

static volatile int roamer_status = -1;
static void roamer(void *unused)
{
    (void)unused;
    struct a733_cpu_pmu_region region;
    roamer_status = a733_cpu_pmu_begin(&region);
    xTaskNotifyGive(main_handle);
    vTaskDelete(NULL);
}

static StaticTask_t tasks[3];
static StackType_t stacks[3][4096];

static void refusals(void)
{
    struct a733_cpu_pmu_region outer, inner;
    struct a733_cpu_pmu_record record;
    /* Nested region: the inner begin changes the core's generation. */
    CHECK(a733_cpu_pmu_begin(&outer) == A733_CPU_PMU_OK);
    CHECK(a733_cpu_pmu_begin(&inner) == A733_CPU_PMU_OK);
    CHECK(a733_cpu_pmu_end(&inner, &record) == A733_CPU_PMU_OK);
    CHECK(a733_cpu_pmu_end(&outer, &record) == A733_CPU_PMU_GENERATION_CHANGED);
    CHECK(record.status == A733_CPU_PMU_GENERATION_CHANGED && record.core == 0 && record.events[0] == 0);
    say("nested_status", 0, record.status);
    /* PMCR_EL0.E cleared between regions: begin configures again. */
    uint64_t pmcr = READ(pmcr_el0) & ~UINT64_C(7);
    __asm__ volatile("msr pmcr_el0, %0\n isb" :: "r"(pmcr) : "memory");
    CHECK(empty(&record) && record.events[0] == results[0].empty);
    say("reconfigured_empty_instructions", 0, record.events[0]);
    /* E cleared inside a region, as by a warm reset: end refuses. */
    CHECK(a733_cpu_pmu_begin(&outer) == A733_CPU_PMU_OK);
    __asm__ volatile("msr pmcr_el0, %0\n isb" :: "r"(pmcr) : "memory");
    CHECK(a733_cpu_pmu_end(&outer, &record) == A733_CPU_PMU_DISABLED);
    say("disabled_status", 0, record.status);
    CHECK(empty(&record));
    /* A task allowed on both cores is refused at begin. */
    CHECK(xTaskCreateStaticAffinitySet(roamer, "roamer", 4096, NULL, 4, stacks[2], &tasks[2], 3));
    CHECK(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
    CHECK(roamer_status == A733_CPU_PMU_NOT_PINNED);
    say("two_core_mask_status", 0, (uint64_t)roamer_status);
    /* The peer moves this pinned task to core 1 inside its region. */
    CHECK(a733_cpu_pmu_begin(&outer) == A733_CPU_PMU_OK);
    __atomic_store_n(&peer_phase, 1, __ATOMIC_RELEASE);
    while (READ(tpidr_el1) != 1) {}
    CHECK(a733_cpu_pmu_end(&outer, &record) == A733_CPU_PMU_CORE_CHANGED);
    CHECK(record.status == A733_CPU_PMU_CORE_CHANGED && record.core == 1);
    say("moved_status", 0, record.status);
    vTaskCoreAffinitySet(NULL, 1);
    CHECK(READ(tpidr_el1) == 0);
    /* Null arguments and a refused region. */
    CHECK(a733_cpu_pmu_begin(NULL) == A733_CPU_PMU_BAD_ARGUMENT);
    CHECK(a733_cpu_pmu_end(NULL, &record) == A733_CPU_PMU_BAD_ARGUMENT && record.core == A733_CPU_PMU_NO_CORE);
    outer.core = A733_CPU_PMU_NO_CORE;
    CHECK(a733_cpu_pmu_end(&outer, &record) == A733_CPU_PMU_NOT_BEGUN);
    CHECK(a733_cpu_pmu_end(&outer, NULL) == A733_CPU_PMU_BAD_ARGUMENT);
    xTaskNotifyGive(peer_handle);
}

static void peer(void *unused)
{
    (void)unused;
    barrier(1);
    measure(1);
    barrier(2);
    while (!__atomic_load_n(&peer_phase, __ATOMIC_ACQUIRE)) {}
    vTaskCoreAffinitySet(main_handle, 2);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for (;;) __asm__ volatile("wfe");
}

static void main_task(void *unused)
{
    (void)unused;
    barrier(1);
    measure(0);
    barrier(2);
    report(0);
    report(1);
    refusals();
    platform_puts("PASS cpu-pmu SMP fixture\n");
    platform_exit(0);
}

void kernel_main(void);
void kernel_main(void)
{
    platform_init();
    main_handle = xTaskCreateStaticAffinitySet(main_task, "main", 4096, NULL, 3, stacks[0], &tasks[0], 1);
    CHECK(main_handle);
    peer_handle = xTaskCreateStaticAffinitySet(peer, "peer", 4096, NULL, 3, stacks[1], &tasks[1], 2);
    CHECK(peer_handle);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
