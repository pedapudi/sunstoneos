/* SPDX-License-Identifier: Apache-2.0 */
/* Hard real-time control interrupts above the kernel API mask (SUNSTONE_CONTROL_IRQ, measurement).
 *
 * The EL1 virtual timer (PPI 27) of cores 1 to 5 fires at a fixed rate at the control priority of
 * include/a733_gic_priority.h, numerically below the API floor, so no kernel critical section delays it
 * (the kernel masks with ICC_PMR_EL1 = the API mask). The handler calls no kernel API: it reads the
 * counter, re-arms the compare value from the previous deadline (no drift), records its latency (entry
 * minus programmed deadline) and period jitter in per-core histograms, and puts a sample into a
 * single-producer single-consumer ring. A task drains the rings and checks the sequence numbers.
 *
 * A core arms its timer from its own first interrupt after ulSunstoneControlStart, because the redistributor
 * and the virtual timer are configured by the core that owns them. Core 0 keeps the task-deadline timer. */
#include "FreeRTOS.h"
#include "task.h"
#include <stdint.h>
#include "a733_gic_priority.h"
#include "sunstone_control.h"
#include "control_ring.h"

extern void __real_vApplicationIRQHandler(uint64_t interrupt);
extern struct { uint64_t epoch, startup_deadline, diagnostic_deadline, redistributor, reserved[12]; } a733_cached_runtime[8];

#define INTID 27u       /* EL1 virtual timer: the control path */
#define TASK_INTID 30u  /* EL1 physical timer: the task path (core 0 keeps it for the scheduler tick) */
#define FIRST_CORE 1u
#define CORES 8u        /* control cores 1 to 7; index by core */
#define BINS SS_CONTROL_BINS
#define TICKS_PER_US 24u

/* The largest control-path latencies of a core with the state of the interrupted code: where it was (ELR_EL1,
 * SPSR_EL1 — the exception level and DAIF bits it held), its mask (ICC_PMR_EL1), the running priority (ICC_RPR_EL1),
 * the task register and the core's critical nesting. The handler reads them only for a new top-4 entry. */
#define WORST 4u
struct worst { uint32_t latency, pmr; uint64_t elr, spsr, rpr, task, nesting, when; };
extern volatile uint64_t ullCriticalNestings[];
struct path {
    uint64_t wfi_count, wfi_late_max, wfi_over5; /* control path only: interrupts that ended a wfi */
    uint32_t wfi_bin[SS_CONTROL_BINS];
    uint64_t period, deadline, last_entry;
    uint64_t count, missed;
    uint32_t seq, latency_max, jitter_max;
    uint32_t latency_bin[BINS], jitter_bin[BINS];
    /* Task path only: the wake split at the handler. entry = handler start minus the deadline (the time the core
     * held the interrupt off); gap = task start minus handler start (handler, exit work, switch). */
    uint32_t entry_max, gap_max;
    uint32_t entry_bin[BINS], gap_bin[BINS];
};
static struct core_state {
    volatile uint32_t requested, armed, stop;
    uint32_t rate_hz, paths;
    struct path control, task;   /* the interrupt handler's samples; the woken task's samples */
    struct worst worst[WORST];
    struct worst entry_worst[WORST]; /* task path: the largest handler entry delays with the interrupted context */
    struct ss_ring ring;
    uint64_t consumed, gaps;
    uint32_t expected;
    TaskHandle_t task_handle;
    uint64_t fired_deadline;     /* the deadline whose interrupt woke the task */
    uint64_t fired_entry;        /* the counter value at that interrupt's handler start */
    uint64_t fired_l1, fired_l2; /* L1D and L2D refill counts at that handler start */
    uint64_t refill_n[2], refill_l1[2], refill_l2[2]; /* by gap class: 0 at most 10 us, 1 longer: wakes and refills during the gap */
} __attribute__((aligned(64))) state[CORES];

static inline uint64_t counter(void)
{
    uint64_t value;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(value) :: "memory");
    return value;
}
static inline void program(uint64_t compare, uint64_t control)
{
    __asm__ volatile("msr cntv_cval_el0, %0\n msr cntv_ctl_el0, %1\n isb" :: "r"(compare), "r"(control) : "memory");
}
static inline void program_task(uint64_t compare, uint64_t control)
{
    __asm__ volatile("msr cntp_cval_el0, %0\n msr cntp_ctl_el0, %1\n isb" :: "r"(compare), "r"(control) : "memory");
}
static inline unsigned this_core(void)
{
    return (unsigned)portGET_CORE_ID(); /* the kernel's own core number; the A76 cores do not number themselves in affinity level 1 */
}

/* Record one entry of a path: latency against the deadline it served, jitter against the previous entry. */
static void record(struct path *p, uint64_t now, uint64_t deadline, unsigned missed)
{
    const uint64_t late = now - deadline;
    const uint32_t latency = late > 0xffffffffu ? 0xffffffffu : (uint32_t)late;
    ++p->count;
    p->missed += missed;
    ++p->latency_bin[latency / SS_CONTROL_BIN_TICKS < BINS ? latency / SS_CONTROL_BIN_TICKS : BINS - 1u];
    if (latency > p->latency_max) p->latency_max = latency;
    if (p->last_entry) {
        const uint64_t expected = p->period * (1u + missed), interval = now - p->last_entry;
        const uint64_t error = interval > expected ? interval - expected : expected - interval;
        const uint32_t jitter = error > 0xffffffffu ? 0xffffffffu : (uint32_t)error;
        ++p->jitter_bin[jitter / SS_CONTROL_BIN_TICKS < BINS ? jitter / SS_CONTROL_BIN_TICKS : BINS - 1u];
        if (jitter > p->jitter_max) p->jitter_max = jitter;
    }
    p->last_entry = now;
}

static void arm(struct core_state *s)
{
    const uintptr_t sgi = (uintptr_t)a733_cached_runtime[this_core()].redistributor + 0x10000u;
    *(volatile uint8_t *)(sgi + 0x400u + INTID) = A733_GIC_NS_WRITE_FOR_STORED(A733_GIC_CONTROL_STORED);
    *(volatile uint8_t *)(sgi + 0x400u + TASK_INTID) = A733_GIC_NS_WRITE_FOR_STORED(A733_GIC_TASK_PATH_STORED);
    *(volatile uint32_t *)(sgi + 0x100u) = ((s->paths & 1u) ? 1u << INTID : 0u) | ((s->paths & 2u) ? 1u << TASK_INTID : 0u);
    __asm__ volatile("dsb sy" ::: "memory");
    const uint64_t now = counter();
    s->control.deadline = now + s->control.period;
    s->task.deadline = now + s->task.period + s->task.period / 2u; /* half a period after the control handler */
    s->control.last_entry = s->task.last_entry = 0;
    if (s->paths & 1u) program(s->control.deadline, 1);
    if (s->paths & 2u) program_task(s->task.deadline, 1);
    /* Count L1D (0x03) and L2D (0x17) refills in counters 0 and 1, readable from EL0, for the wake's gap. */
    {
        uint64_t en;
        __asm__ volatile("msr pmevtyper0_el0, %0\n msr pmevtyper1_el0, %1\n msr pmuserenr_el0, %2\n mrs %3, pmcntenset_el0\n orr %3, %3, #3\n msr pmcntenset_el0, %3\n mrs %3, pmcr_el0\n orr %3, %3, #1\n msr pmcr_el0, %3\n isb"
                         :: "r"((uint64_t)0x03), "r"((uint64_t)0x17), "r"((uint64_t)1), "r"(en) : "memory");
    }
    s->armed = 1;
}

/* Advance a path's deadline past `now`; returns the missed periods. */
static unsigned advance(struct path *p, uint64_t now, uint64_t *served)
{
    unsigned missed = 0;
    uint64_t next = p->deadline + p->period;
    *served = p->deadline;
    while (next <= now) { next += p->period; ++missed; }
    p->deadline = next;
    return missed;
}

static void handle(struct core_state *s)
{
    const uint64_t now = counter();
    if (s->stop) { program(0, 0); program_task(0, 0); s->armed = 0; return; }
    uint64_t served;
    const unsigned missed = advance(&s->control, now, &served);
    program(s->control.deadline, 1); /* clears the level-sensitive interrupt */
    record(&s->control, now, served, missed);
    {
        /* An interrupt that ended a wfi returns to the instruction after it (0xd503207f is WFI). */
        uint64_t elr;
        __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
        if (*(const volatile uint32_t *)(uintptr_t)(elr - 4u) == 0xd503207fu) {
            const uint64_t late = now - served;
            ++s->control.wfi_count;
            if (late > s->control.wfi_late_max) s->control.wfi_late_max = late;
            if (late > 120) ++s->control.wfi_over5;
            ++s->control.wfi_bin[late / SS_CONTROL_BIN_TICKS < BINS ? late / SS_CONTROL_BIN_TICKS : BINS - 1u];
        }
    }
    {
        const uint64_t late = now - served;
        unsigned low = 0;
        for (unsigned i = 1; i < WORST; ++i) if (s->worst[i].latency < s->worst[low].latency) low = i;
        if (late > s->worst[low].latency) {
            struct worst *w = &s->worst[low];
            uint64_t pmr, rpr;
            __asm__ volatile("mrs %0, icc_pmr_el1\n mrs %1, icc_rpr_el1\n mrs %2, elr_el1\n mrs %3, spsr_el1\n mrs %4, tpidrro_el0"
                             : "=r"(pmr), "=r"(rpr), "=r"(w->elr), "=r"(w->spsr), "=r"(w->task));
            w->latency = late > 0xffffffffu ? 0xffffffffu : (uint32_t)late;
            w->pmr = (uint32_t)pmr; w->rpr = rpr; w->nesting = ullCriticalNestings[this_core()]; w->when = now;
        }
    }
    (void)ss_ring_put(&s->ring, (struct ss_ring_sample){ s->control.seq++, (uint32_t)(now - served), now });
}

/* The task path: an interrupt at the API floor wakes the pinned task through the kernel. */
static void handle_task_path(struct core_state *s)
{
    const uint64_t now = counter();
    uint64_t served;
    const unsigned missed = advance(&s->task, now, &served);
    program_task(s->task.deadline, 1);
    s->fired_deadline = served;
    s->fired_entry = now;
    __asm__ volatile("mrs %0, pmevcntr0_el0\n mrs %1, pmevcntr1_el0" : "=r"(s->fired_l1), "=r"(s->fired_l2));
    s->task.missed += missed;
    {
        const uint64_t late = now - served;
        const uint32_t entry = late > 0xffffffffu ? 0xffffffffu : (uint32_t)late;
        ++s->task.entry_bin[entry / SS_CONTROL_BIN_TICKS < BINS ? entry / SS_CONTROL_BIN_TICKS : BINS - 1u];
        if (entry > s->task.entry_max) s->task.entry_max = entry;
        unsigned low = 0;
        for (unsigned i = 1; i < WORST; ++i) if (s->entry_worst[i].latency < s->entry_worst[low].latency) low = i;
        if (entry > s->entry_worst[low].latency) {
            struct worst *w = &s->entry_worst[low];
            uint64_t pmr, rpr;
            __asm__ volatile("mrs %0, icc_pmr_el1\n mrs %1, icc_rpr_el1\n mrs %2, elr_el1\n mrs %3, spsr_el1\n mrs %4, tpidrro_el0"
                             : "=r"(pmr), "=r"(rpr), "=r"(w->elr), "=r"(w->spsr), "=r"(w->task));
            w->latency = entry; w->pmr = (uint32_t)pmr; w->rpr = rpr; w->nesting = ullCriticalNestings[this_core()]; w->when = now;
        }
    }
    BaseType_t woken = pdFALSE;
    if (s->task_handle) vTaskNotifyGiveFromISR(s->task_handle, &woken);
    portYIELD_FROM_ISR(woken);
}

static void task_path(void *argument)
{
    struct core_state *s = argument;
    for (;;) {
        if (!ulTaskNotifyTake(pdTRUE, portMAX_DELAY)) continue;
        const uint64_t now = counter();
        if (s->stop) continue;
        const uint64_t deadline = s->fired_deadline;
        {
            const uint64_t gap = now - s->fired_entry;
            const uint32_t g = gap > 0xffffffffu ? 0xffffffffu : (uint32_t)gap;
            ++s->task.gap_bin[g / SS_CONTROL_BIN_TICKS < BINS ? g / SS_CONTROL_BIN_TICKS : BINS - 1u];
            if (g > s->task.gap_max) s->task.gap_max = g;
            uint64_t l1, l2;
            __asm__ volatile("mrs %0, pmevcntr0_el0\n mrs %1, pmevcntr1_el0" : "=r"(l1), "=r"(l2));
            const unsigned k = g > 240u ? 1u : 0u;
            ++s->refill_n[k]; s->refill_l1[k] += (uint32_t)(l1 - s->fired_l1); s->refill_l2[k] += (uint32_t)(l2 - s->fired_l2);
        }
        record(&s->task, now, deadline, 0);
    }
}

void __wrap_vApplicationIRQHandler(uint64_t interrupt)
{
    const unsigned core = this_core();
    if (core >= FIRST_CORE && core < FIRST_CORE + CORES - 1u) {
        struct core_state *s = &state[core];
        if (interrupt == INTID && s->armed) { handle(s); return; }
        if (interrupt == TASK_INTID && s->armed) { handle_task_path(s); return; }
        if (__atomic_load_n(&s->requested, __ATOMIC_ACQUIRE) && !s->armed && !s->stop) arm(s);
    }
    __real_vApplicationIRQHandler(interrupt);
}

static StaticTask_t consumer_task, path_task[CORES];
static StackType_t path_stack[CORES][512];
static StackType_t consumer_stack[512];
static volatile unsigned running;

static void consumer(void *unused)
{
    (void)unused;
    while (running) {
        for (unsigned c = FIRST_CORE; c < FIRST_CORE + CORES - 1u; ++c) {
            struct core_state *s = &state[c];
            struct ss_ring_sample sample;
            while (ss_ring_get(&s->ring, &sample)) {
                if (sample.seq != s->expected) s->gaps += (uint32_t)(sample.seq - s->expected);
                s->expected = sample.seq + 1u;
                ++s->consumed;
            }
        }
        vTaskDelay(1);
    }
    vTaskSuspend(NULL);
}

unsigned ulSunstoneControlStart(const unsigned *cores, const unsigned *rate_hz, unsigned n)
{
    static const unsigned both[CORES] = {3, 3, 3, 3, 3, 3};
    return ulSunstoneControlStartPaths(cores, rate_hz, both, n);
}
unsigned ulSunstoneControlStartPaths(const unsigned *cores, const unsigned *rate_hz, const unsigned *paths, unsigned n)
{
    unsigned started = 0;
    if (!running) {
        running = 1;
        (void)xTaskCreateStaticAffinitySet(consumer, "control-sink", 512, NULL, 2, consumer_stack, &consumer_task, 0x3e);
    }
    for (unsigned i = 0; i < n; ++i) {
        const unsigned c = cores[i];
        if (c < FIRST_CORE || c >= FIRST_CORE + CORES - 1u || !rate_hz[i] || state[c].requested) continue;
        state[c].rate_hz = rate_hz[i];
        state[c].paths = paths[i] & 3u;
        state[c].control.period = state[c].task.period = 24000000u / rate_hz[i];
        if (state[c].paths & 2u) state[c].task_handle = xTaskCreateStaticAffinitySet(task_path, "control-task", 512, &state[c], 5, path_stack[c], &path_task[c], 1u << c);
        __atomic_store_n(&state[c].requested, 1u, __ATOMIC_RELEASE);
        ++started;
    }
    return started;
}
void vSunstoneControlStop(void)
{
    for (unsigned c = FIRST_CORE; c < FIRST_CORE + CORES - 1u; ++c) state[c].stop = 1;
}

static uint32_t percentile_ns(const uint32_t *bins, uint64_t total, uint32_t maximum)
{
    if (!total) return 0;
    const uint64_t need = (total * 9999u + 9999u) / 10000u;
    uint64_t sum = 0;
    if (total < 10000u) return maximum * 1000u / TICKS_PER_US;
    for (unsigned b = 0; b < BINS; ++b) {
        sum += bins[b];
        if (sum >= need) return b == BINS - 1u ? maximum * 1000u / TICKS_PER_US : (b + 1u) * SS_CONTROL_BIN_TICKS * 1000u / TICKS_PER_US;
    }
    return maximum * 1000u / TICKS_PER_US;
}
unsigned ulSunstoneControlStatsRead(unsigned core, unsigned path, struct ss_control_stats *out)
{
    if (core < FIRST_CORE || core >= FIRST_CORE + CORES - 1u || !state[core].rate_hz || path > 1u || !(state[core].paths & (1u << path))) return 0;
    const struct core_state *s = &state[core];
    const struct path *p = path ? &s->task : &s->control;
    out->path = path; out->core = core; out->rate_hz = s->rate_hz; out->count = p->count;
    out->consumed = path ? p->count : s->consumed; out->gaps = path ? 0 : s->gaps;
    out->dropped = path ? 0 : s->ring.dropped; out->missed = p->missed;
    out->latency_max_ns = p->latency_max * 1000u / TICKS_PER_US;
    out->jitter_max_ns = p->jitter_max * 1000u / TICKS_PER_US;
    out->latency_p9999_ns = percentile_ns(p->latency_bin, p->count, p->latency_max);
    out->jitter_p9999_ns = percentile_ns(p->jitter_bin, p->count > 1 ? p->count - 1 : 0, p->jitter_max);
    return 1;
}
const uint32_t *ulSunstoneControlHistogram(unsigned core, unsigned path, unsigned kind)
{
    if (core < FIRST_CORE || core >= FIRST_CORE + CORES - 1u || path > 1u || kind > (path ? 3u : 1u)) return 0;
    const struct path *p = path ? &state[core].task : &state[core].control;
    switch (kind) {
    case 0: return p->latency_bin;
    case 1: return p->jitter_bin;
    case 2: return p->entry_bin;
    default: return p->gap_bin;
    }
}
/* Rank r (0 largest) of the task path's handler entry delays with the interrupted context. */
unsigned ulSunstoneTaskEntryWorst(unsigned core, unsigned rank, struct ss_control_worst *out)
{
    if (core < FIRST_CORE || core >= FIRST_CORE + CORES - 1u || rank >= WORST || !state[core].rate_hz) return 0;
    const struct worst *copy[WORST];
    for (unsigned i = 0; i < WORST; ++i) copy[i] = &state[core].entry_worst[i];
    for (unsigned i = 0; i < WORST; ++i)
        for (unsigned j = i + 1; j < WORST; ++j)
            if (copy[j]->latency > copy[i]->latency) { const struct worst *t = copy[i]; copy[i] = copy[j]; copy[j] = t; }
    const struct worst *w = copy[rank];
    if (!w->latency) return 0;
    out->latency_ns = w->latency * 1000u / TICKS_PER_US; out->pmr = w->pmr; out->elr = w->elr; out->spsr = w->spsr;
    out->rpr = w->rpr; out->task = w->task; out->nesting = w->nesting; out->when_us = w->when / TICKS_PER_US;
    return 1;
}
/* Rank r (0 largest) of the worst control-path latencies of a core; fields in the order of struct ss_control_worst. */
unsigned ulSunstoneControlWorst(unsigned core, unsigned rank, struct ss_control_worst *out)
{
    if (core < FIRST_CORE || core >= FIRST_CORE + CORES - 1u || rank >= WORST || !state[core].rate_hz) return 0;
    const struct worst *copy[WORST];
    for (unsigned i = 0; i < WORST; ++i) copy[i] = &state[core].worst[i];
    for (unsigned i = 0; i < WORST; ++i)
        for (unsigned j = i + 1; j < WORST; ++j)
            if (copy[j]->latency > copy[i]->latency) { const struct worst *t = copy[i]; copy[i] = copy[j]; copy[j] = t; }
    const struct worst *w = copy[rank];
    if (!w->latency) return 0;
    out->latency_ns = w->latency * 1000u / TICKS_PER_US; out->pmr = w->pmr; out->elr = w->elr; out->spsr = w->spsr;
    out->rpr = w->rpr; out->task = w->task; out->nesting = w->nesting; out->when_us = w->when / TICKS_PER_US;
    return 1;
}
/* Present only in a fork image built with SUNSTONE_OWNER_COUNTERS; the weak copy keeps other images linking. */
__attribute__((weak)) uint64_t ulSunstonePhase(unsigned cpu, unsigned phase, unsigned field) { (void)cpu; (void)phase; (void)field; return 0; }
__attribute__((weak)) uint64_t ulSunstoneOwnerCounters(unsigned cpu, unsigned which) { (void)cpu; (void)which; return 0; }
/* Interrupts of a control core that ended a wfi: count, longest and count over 5 us, and the histogram. */
unsigned ulSunstoneControlWfi(unsigned core, uint64_t *count, uint64_t *max_ticks, uint64_t *over5, const uint32_t **bins)
{
    if (core < FIRST_CORE || core >= FIRST_CORE + CORES - 1u || !state[core].rate_hz || !(state[core].paths & 1u)) return 0;
    *count = state[core].control.wfi_count; *max_ticks = state[core].control.wfi_late_max; *over5 = state[core].control.wfi_over5;
    *bins = state[core].control.wfi_bin;
    return 1;
}
/* Idle pull diagnostics of the partitioned scheduler; the weak copies keep the stock kernel linking. */
__attribute__((weak)) unsigned long ulSunstoneIdleWaiting(unsigned core) { (void)core; return 0; }
__attribute__((weak)) unsigned long ulSunstoneIdlePulls(void) { return 0; }
/* Mean refills during the gap, by gap class (0 at most 10 us, 1 longer): field 0 count, 1 L1D refills, 2 L2D refills. */
uint64_t ulSunstoneTaskRefills(unsigned core, unsigned klass, unsigned field)
{
    if (core < FIRST_CORE || core >= FIRST_CORE + CORES - 1u || klass > 1u || field > 2u) return 0;
    return field == 0 ? state[core].refill_n[klass] : field == 1 ? state[core].refill_l1[klass] : state[core].refill_l2[klass];
}
