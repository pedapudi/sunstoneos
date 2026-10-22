/* Two-core QEMU fixture for the device-memory ledger in the FreeRTOS SMP port.
 *
 * Every task runs at EL1 (the privileged-task port patch, with the selector
 * below), so the fixture can read the PMU and ICC_PMR_EL1. The runner builds
 * this image with --wrap=vTaskEnterCritical --wrap=vTaskExitCritical; the
 * wrappers below record, per core, the instructions retired between the
 * return of the outermost enter and the call of the matching exit.
 *
 * fixture_mode, patched in the ELF by the runner:
 *   0  measure: instruction counts under -icount shift=0, then a short
 *      cross-core run;
 *   1  contend: the cross-core run only, long, under multithreaded TCG;
 *   2  trace: each measured call once between semihosting markers, for an
 *      exception trace (-d int) that counts its SVCs.
 *
 * FIXTURE_LEDGER is defined for the current pool. Without it the image is
 * built against an earlier pool source and runs only the per-access
 * measurements (describe, xHCI register read, camera memory check). */
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "device_memory_pool.h"
#include <string.h>

extern _Noreturn void platform_panic(const char *, unsigned);
#define CHECK(x) do { if (!(x)) platform_panic(__FILE__, __LINE__); } while (0)

volatile uint32_t fixture_mode = 0xaabbccddu;

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

static unsigned core(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(v));
    return (unsigned)(v & 0xff);
}
/* PMU event 0x08, instructions architecturally executed; exact under -icount. */
static void pmu_start(void)
{
    uint64_t event = 8, enable = 1, control = 1;
    __asm__ volatile("msr pmevtyper0_el0, %0\n msr pmcntenset_el0, %1\n msr pmcr_el0, %2\n isb"
                     :: "r"(event), "r"(enable), "r"(control) : "memory");
}
static inline uint64_t pmu_read(void)
{
    uint64_t v;
    __asm__ volatile("isb\n mrs %0, pmevcntr0_el0" : "=r"(v) :: "memory");
    return v;
}
static void say(const char *label, uint64_t value)
{
    platform_puts("MEASURE ");
    platform_puts(label);
    platform_puts("=");
    platform_hex(value);
    platform_puts("\n");
}
/* Semihosting SYS_WRITE0: a marker in the exception trace. */
static void marker(const char *text)
{
    register uintptr_t operation __asm__("x0") = 4, argument __asm__("x1") = (uintptr_t)text;
    __asm__ volatile("hlt #0xf000" : "+r"(operation), "+r"(argument) :: "memory");
}

/* --- Critical-section probe --------------------------------------------- */
enum op { OP_NONE, OP_ACQUIRE, OP_RELEASE, OP_RETAIN, OP_OTHER, OP_COUNT };
static struct {
    unsigned depth;
    enum op op;
    uint64_t start, held;
    unsigned contended[OP_COUNT]; /* Entries attempted while the other core held an acquire. */
} probe[2] __attribute__((aligned(64)));
static unsigned in_acquire[2];
void __real_vTaskEnterCritical(void);
void __real_vTaskExitCritical(void);
void __wrap_vTaskEnterCritical(void);
void __wrap_vTaskExitCritical(void);
void __wrap_vTaskEnterCritical(void)
{
    unsigned c = core();
    if (!probe[c].depth && probe[c].op != OP_NONE && __atomic_load_n(&in_acquire[c ^ 1], __ATOMIC_ACQUIRE))
        ++probe[c].contended[probe[c].op];
    __real_vTaskEnterCritical();
    if (!probe[c].depth++) {
        if (probe[c].op == OP_ACQUIRE) __atomic_store_n(&in_acquire[c], 1, __ATOMIC_RELEASE);
        probe[c].start = pmu_read();
    }
}
void __wrap_vTaskExitCritical(void)
{
    /* tasks.c enters through its own definition, which the linker does not
     * wrap, but some of its exits are wrapped; those find depth 0. */
    unsigned c = core();
    if (probe[c].depth && !--probe[c].depth) {
        probe[c].held = pmu_read() - probe[c].start;
        __atomic_store_n(&in_acquire[c], 0, __ATOMIC_RELEASE);
    }
    __real_vTaskExitCritical();
}

/* --- Per-access measurements --------------------------------------------- */
extern void fixture_xhci_setup(struct device_memory_pool *, struct device_memory_lease,
                               struct device_memory_extent);
extern int fixture_xhci_rd(uint32_t offset, uint32_t *value);
extern void fixture_camera_setup(struct device_memory_pool *, struct device_memory_lease,
                                 struct device_memory_extent);
extern int fixture_camera_memory_owned(void);

#define POOL_PAGES 256u
static unsigned char backing[POOL_PAGES * DEVICE_MEMORY_PAGE] __attribute__((aligned(1 << 21)));
static struct device_memory_pool pool;

/* Instructions retired by `call`, less the reading overhead; the least of
 * eight runs. */
static uint64_t read_overhead;
#define MEASURE(result, call) do { \
    uint64_t best_ = UINT64_MAX; \
    for (unsigned i_ = 0; i_ < 8; ++i_) { \
        uint64_t t_ = pmu_read(); call; uint64_t d_ = pmu_read() - t_; \
        if (d_ < best_) best_ = d_; \
    } \
    (result) = best_ - read_overhead; \
} while (0)

static void access_measurements(void)
{
    struct device_memory_lease l;
    struct device_memory_extent e, d;
    enum device_memory_state s;
    uint32_t v;
    uint64_t n;
    CHECK(device_memory_acquire(&pool, 9, 16 * DEVICE_MEMORY_PAGE, &l, &e));
    device_memory_lookup(&pool, l)->state = DEVICE_MEMORY_DEVICE; /* As the drivers' holders do. */
    fixture_xhci_setup(&pool, l, e);
    fixture_camera_setup(&pool, l, e);
    if (fixture_mode == 2) {
        marker("TRACE describe\n");
        CHECK(device_memory_describe(&pool, l, &d, &s));
        marker("TRACE xhci_rd\n");
        CHECK(fixture_xhci_rd(0x20, &v));
        marker("TRACE camera_memory\n");
        CHECK(fixture_camera_memory_owned());
        marker("TRACE end\n");
    } else {
        MEASURE(n, CHECK(device_memory_describe(&pool, l, &d, &s)));
        CHECK(s == DEVICE_MEMORY_DEVICE && !memcmp(&d, &e, sizeof(d)));
        say("describe_instructions", n);
        MEASURE(n, CHECK(fixture_xhci_rd(0x20, &v)));
        say("xhci_rd_instructions", n);
        MEASURE(n, CHECK(fixture_camera_memory_owned()));
        say("camera_memory_instructions", n);
    }
    device_memory_lookup(&pool, l)->state = DEVICE_MEMORY_CPU;
    CHECK(device_memory_release(&pool, l));
}

#if defined(FIXTURE_LEDGER)
/* --- Worst-case acquire, nesting and cross-core serialization ------------ */
#define FILLERS 31u
static struct device_memory_lease fillers[FILLERS];
static volatile unsigned peer_go, peer_done;
static unsigned peer_rounds;

/* 31 one-page extents at even pages 0..60: a two-page request then fails at
 * every filler and fits at page 61, the most placement attempts that 31
 * occupied slots allow (32). */
static void fill(void)
{
    struct device_memory_extent e;
    for (unsigned i = 0; i < FILLERS; ++i) {
        CHECK(device_memory_acquire_aligned(&pool, 100 + i, DEVICE_MEMORY_PAGE, 2 * DEVICE_MEMORY_PAGE,
                                            &fillers[i], &e));
        CHECK(e.cpu == (uintptr_t)backing + 2 * i * DEVICE_MEMORY_PAGE);
    }
}

/* No two held slots overlap. Runs inside the critical section. */
static void ledger_consistent(void)
{
    probe[core()].op = OP_OTHER;
    taskENTER_CRITICAL();
    for (unsigned i = 0; i < DEVICE_MEMORY_MAX_ALLOCATIONS; ++i) {
        const struct device_memory_allocation *a = &pool.allocations[i];
        if (a->state == DEVICE_MEMORY_FREE) continue;
        CHECK(a->generation && a->pages && a->first + a->pages <= POOL_PAGES);
        for (unsigned j = i + 1; j < DEVICE_MEMORY_MAX_ALLOCATIONS; ++j) {
            const struct device_memory_allocation *b = &pool.allocations[j];
            CHECK(b->state == DEVICE_MEMORY_FREE || a->generation != b->generation);
            CHECK(b->state == DEVICE_MEMORY_FREE || a->first + a->pages <= b->first ||
                  b->first + b->pages <= a->first);
        }
    }
    taskEXIT_CRITICAL();
    probe[core()].op = OP_NONE;
}

static uint64_t held_by(enum op op, int (*call)(void *), void *argument)
{
    uint64_t best = UINT64_MAX;
    for (unsigned i = 0; i < 8; ++i) {
        probe[0].op = op;
        CHECK(call(argument));
        probe[0].op = OP_NONE;
        if (probe[0].held < best) best = probe[0].held;
    }
    return best;
}
static struct device_memory_lease worst;
static int worst_acquire(void *unused)
{
    (void)unused;
    struct device_memory_extent e;
    int ok = device_memory_acquire(&pool, 7, 2 * DEVICE_MEMORY_PAGE, &worst, &e);
    return ok && e.cpu == (uintptr_t)backing + 61 * DEVICE_MEMORY_PAGE;
}
static int worst_release(void *unused) { (void)unused; return device_memory_release(&pool, worst); }
static int retain_retained(void *lease) { return device_memory_retain(&pool, *(struct device_memory_lease *)lease); }
static int empty_pair(void *unused) { (void)unused; taskENTER_CRITICAL(); taskEXIT_CRITICAL(); return 1; }

static void ledger_measurements(void)
{
    uint64_t pair, empty, acquire, release, retain;
    MEASURE(pair, (__real_vTaskEnterCritical(), __real_vTaskExitCritical()));
    say("critical_pair_instructions", pair);
    empty = held_by(OP_OTHER, empty_pair, NULL);
    say("probe_overhead_instructions", empty);
    fill();
    struct device_memory_lease r = fillers[FILLERS - 1];
    if (fixture_mode == 2) {
        marker("TRACE acquire\n");
        CHECK(worst_acquire(NULL));
        marker("TRACE release\n");
        CHECK(worst_release(NULL));
        marker("TRACE retain\n");
        CHECK(device_memory_retain(&pool, r));
        marker("TRACE end\n");
        return;
    }
    acquire = release = UINT64_MAX;
    for (unsigned i = 0; i < 8; ++i) {
        probe[0].op = OP_ACQUIRE;
        CHECK(worst_acquire(NULL));
        if (probe[0].held < acquire) acquire = probe[0].held;
        probe[0].op = OP_RELEASE;
        CHECK(worst_release(NULL));
        if (probe[0].held < release) release = probe[0].held;
        probe[0].op = OP_NONE;
    }
    CHECK(device_memory_retain(&pool, r));
    retain = held_by(OP_RETAIN, retain_retained, &r);
    say("acquire_hold_instructions", acquire - empty);
    say("release_hold_instructions", release - empty);
    say("retain_hold_instructions", retain - empty);
    CHECK(acquire - empty <= 15000 && release - empty <= 100 && retain - empty <= 100);
}

static uint64_t pmr(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, ICC_PMR_EL1" : "=r"(v));
    return v;
}

/* An acquire and a release inside an enclosing critical section return with
 * the priority mask still raised; the outer exit lowers it. */
static void nesting(void)
{
    struct device_memory_lease l;
    struct device_memory_extent e;
    uint64_t open = pmr();
    taskENTER_CRITICAL();
    uint64_t masked = pmr();
    CHECK(masked < open);
    CHECK(device_memory_acquire(&pool, 8, DEVICE_MEMORY_PAGE, &l, &e) && pmr() == masked);
    CHECK(device_memory_release(&pool, l) && pmr() == masked);
    taskEXIT_CRITICAL();
    CHECK(pmr() == open);
    say("priority_mask_open", open);
    say("priority_mask_masked", masked);
}

/* Core 1: one retained lease that it retains again each round, and one lease
 * it acquires and releases each round, while core 0 runs long acquires. */
static void peer_rounds_run(void)
{
    while (!__atomic_load_n(&peer_go, __ATOMIC_ACQUIRE)) __asm__ volatile("wfe");
    struct device_memory_lease r, l;
    struct device_memory_extent e, d;
    enum device_memory_state s;
    probe[1].op = OP_ACQUIRE;
    CHECK(device_memory_acquire(&pool, 50, DEVICE_MEMORY_PAGE, &r, &e));
    probe[1].op = OP_RETAIN;
    CHECK(device_memory_retain(&pool, r));
    for (unsigned i = 0; i < peer_rounds; ++i) {
        probe[1].op = OP_ACQUIRE;
        CHECK(device_memory_acquire(&pool, 51, DEVICE_MEMORY_PAGE, &l, &e));
        CHECK(device_memory_describe(&pool, l, &d, &s) && s == DEVICE_MEMORY_CPU && d.cpu == e.cpu);
        probe[1].op = OP_RELEASE;
        CHECK(device_memory_release(&pool, l));
        CHECK(!device_memory_describe(&pool, l, &d, &s));
        probe[1].op = OP_RETAIN;
        CHECK(device_memory_retain(&pool, r));
        probe[1].op = OP_NONE;
        if (!(i & 63)) ledger_consistent();
    }
    probe[1].op = OP_RELEASE;
    CHECK(!device_memory_release(&pool, r)); /* Nothing frees a retained slot. */
    probe[1].op = OP_NONE;
    __atomic_store_n(&peer_done, 1, __ATOMIC_RELEASE);
}

static void contention(unsigned rounds)
{
    /* Two fillers go back: one slot for the peer's retained lease, one for
     * its cycling lease, and core 0's acquire still passes 29 fillers. */
    CHECK(device_memory_release(&pool, fillers[0]) && device_memory_release(&pool, fillers[1]));
    peer_rounds = rounds;
    __atomic_store_n(&peer_go, 1, __ATOMIC_RELEASE);
    __asm__ volatile("dsb ish\n sev" ::: "memory");
    struct device_memory_lease l;
    struct device_memory_extent e;
    unsigned done = 0;
    while (!__atomic_load_n(&peer_done, __ATOMIC_ACQUIRE)) {
        probe[0].op = OP_ACQUIRE;
        CHECK(device_memory_acquire(&pool, 60, 2 * DEVICE_MEMORY_PAGE, &l, &e));
        probe[0].op = OP_NONE;
        if (!(done & 63)) ledger_consistent();
        probe[0].op = OP_RELEASE;
        CHECK(device_memory_release(&pool, l));
        probe[0].op = OP_NONE;
        ++done;
    }
    ledger_consistent();
    say("contended_rounds_core0", done);
    say("contended_release_entries", probe[1].contended[OP_RELEASE]);
    say("contended_retain_entries", probe[1].contended[OP_RETAIN]);
    say("contended_acquire_entries", probe[1].contended[OP_ACQUIRE]);
}
#endif

/* Core 1 waits in WFE except while it contends, which keeps an exception or
 * execution trace of core 0 small. */
static void peer(void *unused)
{
    (void)unused;
    pmu_start();
#if defined(FIXTURE_LEDGER)
    if (fixture_mode != 2) peer_rounds_run();
#endif
    for (;;) __asm__ volatile("wfe");
}

static void main_task(void *unused)
{
    (void)unused;
    pmu_start();
    uint64_t t = pmu_read();
    read_overhead = pmu_read() - t;
    CHECK(fixture_mode <= 2);
    CHECK(device_memory_pool_init(&pool, (uintptr_t)backing, (uintptr_t)backing, sizeof(backing)));
    if (fixture_mode != 1) access_measurements();
#if defined(FIXTURE_LEDGER)
    if (fixture_mode != 1) {
        ledger_measurements();
        if (fixture_mode == 2) platform_exit(0);
        nesting();
    } else fill();
    contention(fixture_mode == 1 ? 200000u : 2000u);
#endif
    platform_puts("PASS memory-architecture SMP fixture\n");
    platform_exit(0);
}

static StaticTask_t tasks[2];
static StackType_t stacks[2][4096];
void kernel_main(void);
void kernel_main(void)
{
    platform_init();
    CHECK(xTaskCreateStaticAffinitySet(main_task, "main", 4096, NULL, 3, stacks[0], &tasks[0], 1));
    CHECK(xTaskCreateStaticAffinitySet(peer, "peer", 4096, NULL, 3, stacks[1], &tasks[1], 2));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
