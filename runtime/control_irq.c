/* SPDX-License-Identifier: Apache-2.0 */
/* Hard real-time control interrupts above the kernel API mask (SUNSTONE_CONTROL_IRQ, measurement).
 *
 * The EL1 virtual timer (PPI 27) of cores 1 to 7 fires at a fixed rate at the control priority of
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
    uint32_t requested, armed, stop, isr_stopped, task_parked, task_ready, errors;
    uint64_t published[2];
    uint32_t rate_hz, paths;
    struct path control, task;   /* the interrupt handler's samples; the woken task's samples */
    struct worst worst[WORST];
    struct worst entry_worst[WORST]; /* task path: the largest handler entry delays with the interrupted context */
    struct ss_ring ring;
    uint64_t consumed, gaps;
    uint32_t expected;
    TaskHandle_t task_handle;
    uint64_t task_irq_count, fired_sequence, task_sequence;
    uint64_t fired_deadline;     /* the deadline whose interrupt woke the task */
    uint64_t fired_entry;        /* the counter value at that interrupt's handler start */
    uint64_t fired_l1, fired_l2; /* L1D and L2D refill counts at that handler start */
    uint64_t refill_n[2], refill_l1[2], refill_l2[2]; /* by gap class: 0 at most 10 us, 1 longer: wakes and refills during the gap */
} __attribute__((aligned(64))) state[CORES];

static unsigned measurement_mode, requested_mask, stop_requested, consumer_parked;
static uint64_t frequency_hz;
#ifdef SUNSTONE_CONTROL_TEST
extern uint64_t ss_control_test_counter(void);
extern uint64_t ss_control_test_frequency(void);
extern void ss_control_test_program(unsigned, uint64_t, uint64_t);
extern uint64_t ss_control_test_irq_mask(void);
extern void ss_control_test_irq_restore(uint64_t);
#endif

static void error(struct core_state *s, unsigned flag)
{ __atomic_fetch_or(&s->errors, flag, __ATOMIC_RELAXED); }
static inline uint64_t counter(void)
{
#ifdef SUNSTONE_CONTROL_TEST
    return ss_control_test_counter();
#else
    uint64_t value;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(value) :: "memory");
    return value;
#endif
}
static inline void program(uint64_t compare, uint64_t control)
{
#ifdef SUNSTONE_CONTROL_TEST
    ss_control_test_program(0, compare, control);
#else
    __asm__ volatile("msr cntv_cval_el0, %0\n msr cntv_ctl_el0, %1\n isb" :: "r"(compare), "r"(control) : "memory");
#endif
}
static inline void program_task(uint64_t compare, uint64_t control)
{
#ifdef SUNSTONE_CONTROL_TEST
    ss_control_test_program(1, compare, control);
#else
    __asm__ volatile("msr cntp_cval_el0, %0\n msr cntp_ctl_el0, %1\n isb" :: "r"(compare), "r"(control) : "memory");
#endif
}
static inline unsigned this_core(void)
{
    return (unsigned)portGET_CORE_ID();
}

/* Stop owns both local timer compares on a requested core. A control IRQ can
 * interrupt the task timer handler, so it may disable sources but cannot ack
 * the interrupted writer. Only SGI0 or the API-priority task handler publishes
 * the final ISR ack, with DAIF.I set across both disables and publication. */
static void stop_local(struct core_state *s, unsigned acknowledge)
{
    uint64_t previous;
#ifdef SUNSTONE_CONTROL_TEST
    previous = ss_control_test_irq_mask();
#else
    __asm__ volatile("mrs %0, daif\n msr daifset, #2" : "=r"(previous) :: "memory");
#endif
    program(0, 0);
    program_task(0, 0);
    __atomic_store_n(&s->armed, 0u, __ATOMIC_RELAXED);
    if (acknowledge) __atomic_store_n(&s->isr_stopped, 1u, __ATOMIC_RELEASE);
#ifdef SUNSTONE_CONTROL_TEST
    ss_control_test_irq_restore(previous);
#else
    __asm__ volatile("msr daif, %0" :: "r"(previous) : "memory");
#endif
}
static void read_refills(uint64_t *l1, uint64_t *l2)
{
#ifdef SUNSTONE_CONTROL_TEST
    *l1 = *l2 = 0;
#else
    __asm__ volatile("mrs %0, pmevcntr0_el0\n mrs %1, pmevcntr1_el0" : "=r"(*l1), "=r"(*l2));
#endif
}
static int ended_wfi(void)
{
#ifdef SUNSTONE_CONTROL_TEST
    return 0;
#else
    uint64_t elr;
    __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
    return *(const volatile uint32_t *)(uintptr_t)(elr - 4u) == 0xd503207fu;
#endif
}
static void save_context(struct worst *w, uint32_t late, uint64_t now)
{
#ifdef SUNSTONE_CONTROL_TEST
    w->pmr = 0; w->rpr = 0; w->elr = 0; w->spsr = 0; w->task = 0; w->nesting = 0;
#else
    uint64_t pmr, rpr;
    __asm__ volatile("mrs %0, icc_pmr_el1\n mrs %1, icc_rpr_el1\n mrs %2, elr_el1\n mrs %3, spsr_el1\n mrs %4, tpidrro_el0"
                     : "=r"(pmr), "=r"(rpr), "=r"(w->elr), "=r"(w->spsr), "=r"(w->task));
    w->pmr = (uint32_t)pmr; w->rpr = rpr; w->nesting = ullCriticalNestings[this_core()];
#endif
    w->latency = late; w->when = now;
}

/* Record one entry of a path: latency against the deadline it served, jitter against the previous entry. */
static void record(struct path *p, uint64_t now, uint64_t deadline, unsigned missed)
{
    const uint64_t late = now - deadline;
    const uint32_t latency = late > 0xffffffffu ? 0xffffffffu : (uint32_t)late;
    ++p->count;
    ++p->latency_bin[latency / SS_CONTROL_BIN_TICKS < BINS ? latency / SS_CONTROL_BIN_TICKS : BINS - 1u];
    if (latency > p->latency_max) p->latency_max = latency;
    if (p->last_entry) {
        const uint64_t expected = p->period * (UINT64_C(1) + missed), interval = now - p->last_entry;
        const uint64_t error = interval > expected ? interval - expected : expected - interval;
        const uint32_t jitter = error > 0xffffffffu ? 0xffffffffu : (uint32_t)error;
        ++p->jitter_bin[jitter / SS_CONTROL_BIN_TICKS < BINS ? jitter / SS_CONTROL_BIN_TICKS : BINS - 1u];
        if (jitter > p->jitter_max) p->jitter_max = jitter;
    }
    p->last_entry = now;
}

static void arm(struct core_state *s)
{
#ifndef SUNSTONE_CONTROL_TEST
    const uintptr_t sgi = (uintptr_t)a733_cached_runtime[this_core()].redistributor + 0x10000u;
    if (s->paths & 1u) *(volatile uint8_t *)(sgi + 0x400u + INTID) = A733_GIC_NS_WRITE_FOR_STORED(A733_GIC_CONTROL_STORED);
    if (s->paths & 2u) *(volatile uint8_t *)(sgi + 0x400u + TASK_INTID) = A733_GIC_NS_WRITE_FOR_STORED(A733_GIC_TASK_PATH_STORED);
    *(volatile uint32_t *)(sgi + 0x100u) = ((s->paths & 1u) ? 1u << INTID : 0u) | ((s->paths & 2u) ? 1u << TASK_INTID : 0u);
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    const uint64_t now = counter();
    if (now > UINT64_MAX - s->task.period - s->task.period / 2u) {
        error(s, SS_CONTROL_ERROR_DEADLINE);
        __atomic_store_n(&s->stop, 1u, __ATOMIC_RELEASE);
        stop_local(s, 0);
        return;
    }
    s->control.deadline = now + s->control.period;
    s->task.deadline = now + s->task.period + s->task.period / 2u;
    s->control.last_entry = s->task.last_entry = 0;
#ifndef SUNSTONE_CONTROL_TEST
    /* Only the task path reads PMU refill counters. */
    if (s->paths & 2u) {
        uint64_t en;
        __asm__ volatile("msr pmevtyper0_el0, %1\n msr pmevtyper1_el0, %2\n msr pmuserenr_el0, %3\n mrs %0, pmcntenset_el0\n orr %0, %0, #3\n msr pmcntenset_el0, %0\n mrs %0, pmcr_el0\n orr %0, %0, #1\n msr pmcr_el0, %0\n isb"
                         : "=&r"(en) : "r"((uint64_t)0x03), "r"((uint64_t)0x17), "r"((uint64_t)1) : "memory");
    }
#endif
    if (s->paths & 1u) program(s->control.deadline, 1); else program(0, 0);
    if (s->paths & 2u) program_task(s->task.deadline, 1); else program_task(0, 0);
    __atomic_store_n(&s->armed, 1u, __ATOMIC_RELEASE);
}

/* One quotient advances over any number of elapsed periods. Early/stale IRQs
 * retain their future compare, and an unrepresentable next deadline stops. */
static int advance(struct path *p, uint64_t now, uint64_t *served, unsigned *missed)
{
    *served = p->deadline;
    *missed = 0;
    if (now < p->deadline) return 0;
    if (!p->period) return -1;
    const uint64_t skipped = (now - p->deadline) / p->period;
    if (skipped > UINT32_MAX || skipped == UINT64_MAX ||
        skipped + 1 > (UINT64_MAX - p->deadline) / p->period) return -1;
    p->deadline += (skipped + 1) * p->period;
    *missed = (unsigned)skipped;
    return 1;
}
static int serve(struct core_state *s, struct path *p, uint64_t now, uint64_t *served, unsigned *missed, unsigned acknowledge)
{
    const int result = advance(p, now, served, missed);
    if (result < 0) {
        error(s, SS_CONTROL_ERROR_DEADLINE);
        __atomic_store_n(&s->stop, 1u, __ATOMIC_RELEASE);
        stop_local(s, acknowledge);
    }
    return result;
}

static void handle(struct core_state *s)
{
    if (__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) { stop_local(s, 0); return; }
    const uint64_t now = counter();
    uint64_t served;
    unsigned missed;
    const int result = serve(s, &s->control, now, &served, &missed, 0);
    if (result <= 0) { if (!result) program(s->control.deadline, 1); return; }
    program(s->control.deadline, 1);
    s->control.missed += missed;
    record(&s->control, now, served, missed);
    if (ended_wfi()) {
        const uint64_t late = now - served;
        ++s->control.wfi_count;
        if (late > s->control.wfi_late_max) s->control.wfi_late_max = late;
        if (late > 120) ++s->control.wfi_over5;
        ++s->control.wfi_bin[late / SS_CONTROL_BIN_TICKS < BINS ? late / SS_CONTROL_BIN_TICKS : BINS - 1u];
    }
    const uint64_t late = now - served;
    unsigned low = 0;
    for (unsigned i = 1; i < WORST; ++i) if (s->worst[i].latency < s->worst[low].latency) low = i;
    if (late > s->worst[low].latency)
        save_context(&s->worst[low], late > UINT32_MAX ? UINT32_MAX : (uint32_t)late, now);
    if (!(measurement_mode & SS_CONTROL_HISTOGRAM_ONLY))
        (void)ss_ring_put(&s->ring, (struct ss_ring_sample){ s->control.seq++, (uint32_t)(now - served), now });
    __atomic_store_n(&s->published[0], s->control.count, __ATOMIC_RELEASE);
}

/* The task path: an interrupt at the API floor wakes the pinned task through the kernel. */
static void handle_task_path(struct core_state *s)
{
    if (__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) { stop_local(s, 1); return; }
    const uint64_t now = counter();
    uint64_t served;
    unsigned missed;
    const int result = serve(s, &s->task, now, &served, &missed, 1);
    if (result <= 0) { if (!result) program_task(s->task.deadline, 1); return; }
    program_task(s->task.deadline, 1);
    s->fired_deadline = served;
    s->fired_entry = now;
    read_refills(&s->fired_l1, &s->fired_l2);
    ++s->fired_sequence;
    ++s->task_irq_count;
    s->task.missed += missed;
    const uint64_t late = now - served;
    const uint32_t entry = late > UINT32_MAX ? UINT32_MAX : (uint32_t)late;
    ++s->task.entry_bin[entry / SS_CONTROL_BIN_TICKS < BINS ? entry / SS_CONTROL_BIN_TICKS : BINS - 1u];
    if (entry > s->task.entry_max) s->task.entry_max = entry;
    unsigned low = 0;
    for (unsigned i = 1; i < WORST; ++i) if (s->entry_worst[i].latency < s->entry_worst[low].latency) low = i;
    if (entry > s->entry_worst[low].latency) save_context(&s->entry_worst[low], entry, now);
    BaseType_t woken = pdFALSE;
    if (s->task_handle) vTaskNotifyGiveFromISR(s->task_handle, &woken);
    portYIELD_FROM_ISR(woken);
    if (__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) stop_local(s, 1);
}

static void task_path(void *argument)
{
    struct core_state *s = argument;
    __atomic_store_n(&s->task_ready, 1u, __ATOMIC_RELEASE);
    for (;;) {
        const uint32_t notified = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const uint64_t now = counter();
        if (__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&s->task_parked, 1u, __ATOMIC_RELEASE);
            vTaskSuspend(NULL);
            return;
        }
        if (!notified) continue;
        /* The timestamp precedes this observer mask. Copy the matching timer
         * payload while API IRQs are excluded; higher control IRQs do not edit it. */
        const UBaseType_t mask = portSET_INTERRUPT_MASK();
        const uint64_t sequence = s->fired_sequence, deadline = s->fired_deadline;
        const uint64_t entry = s->fired_entry, fired_l1 = s->fired_l1, fired_l2 = s->fired_l2;
        portCLEAR_INTERRUPT_MASK(mask);
        if (notified != 1 || sequence != s->task_sequence + 1 || entry > now) {
            error(s, SS_CONTROL_ERROR_NOTIFICATION);
            s->task_sequence = sequence;
            continue;
        }
        s->task_sequence = sequence;
        const uint64_t gap = now - entry;
        const uint32_t g = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
        ++s->task.gap_bin[g / SS_CONTROL_BIN_TICKS < BINS ? g / SS_CONTROL_BIN_TICKS : BINS - 1u];
        if (g > s->task.gap_max) s->task.gap_max = g;
        uint64_t l1, l2;
        read_refills(&l1, &l2);
        const unsigned k = g > 240u ? 1u : 0u;
        ++s->refill_n[k]; s->refill_l1[k] += (uint32_t)(l1 - fired_l1); s->refill_l2[k] += (uint32_t)(l2 - fired_l2);
        record(&s->task, now, deadline, 0);
        __atomic_store_n(&s->published[1], s->task.count, __ATOMIC_RELEASE);
    }
}

void __wrap_vApplicationIRQHandler(uint64_t interrupt)
{
    const unsigned core = this_core();
    if (core >= FIRST_CORE && core < CORES) {
        struct core_state *s = &state[core];
        if (__atomic_load_n(&s->requested, __ATOMIC_ACQUIRE)) {
            const unsigned stop = __atomic_load_n(&s->stop, __ATOMIC_ACQUIRE);
            /* Both compares remain owned until the session retires, including
             * stale pending deliveries after their stop acknowledgment. */
            if (interrupt == INTID || interrupt == TASK_INTID) {
                /* A pre-arm PPI30 can retain an inherited priority. Only an
                 * in-flight selected task handler or SGI0 supplies final ack. */
                if (stop) { stop_local(s, 0); return; }
                if (!__atomic_load_n(&s->armed, __ATOMIC_ACQUIRE)) {
                    if (interrupt == INTID) program(0, 0); else program_task(0, 0);
                    return;
                }
                if (interrupt == INTID && (s->paths & 1u)) handle(s);
                else if (interrupt == TASK_INTID && (s->paths & 2u)) handle_task_path(s);
                else if (interrupt == INTID) program(0, 0);
                else program_task(0, 0);
                return;
            }
            if (stop) stop_local(s, interrupt == 0);
            else if (interrupt == 0 && !__atomic_load_n(&s->armed, __ATOMIC_ACQUIRE)) arm(s);
        }
    }
    __real_vApplicationIRQHandler(interrupt);
}

#ifdef SUNSTONE_CONTROL_TASK_EL1
/* Measurement: the woken task runs at EL1, so its masks are direct register writes and not supervisor calls. */
int ulSunstoneControlTaskPrivileged(void (*entry)(void *), void *context) { (void)context; return entry == task_path; }
#endif
static StaticTask_t consumer_task, path_task[CORES];
static StackType_t path_stack[CORES][512];
static StackType_t consumer_stack[512];
static TaskHandle_t consumer_handle;
static unsigned running;

static void consumer(void *unused)
{
    (void)unused;
    for (;;) {
        unsigned stopped = 1, empty = 1;
        for (unsigned c = FIRST_CORE; c < CORES; ++c) {
            struct core_state *s = &state[c];
            if (!__atomic_load_n(&s->requested, __ATOMIC_ACQUIRE)) continue;
            if (!__atomic_load_n(&s->isr_stopped, __ATOMIC_ACQUIRE)) stopped = 0;
            struct ss_ring_sample sample;
            /* A live producer cannot make this task monopolize its CPU. Once
             * stopped is acknowledged, this capacity is the complete finite tail. */
            for (unsigned n = 0; n < SS_RING_SIZE && ss_ring_get(&s->ring, &sample); ++n) {
                if (sample.seq != s->expected) s->gaps += (uint32_t)(sample.seq - s->expected);
                s->expected = sample.seq + 1u;
                ++s->consumed;
            }
            if (SS_RING_LOAD_ACQ(&s->ring.head) != SS_RING_LOAD_ACQ(&s->ring.tail)) empty = 0;
        }
        if (__atomic_load_n(&stop_requested, __ATOMIC_ACQUIRE) && stopped && empty) break;
        vTaskDelay(1);
    }
    __atomic_store_n(&running, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&consumer_parked, 1u, __ATOMIC_RELEASE);
    vTaskSuspend(NULL);
}

unsigned ulSunstoneControlStart(const unsigned *cores, const unsigned *rate_hz, unsigned n)
{
    static const unsigned both[CORES] = {3, 3, 3, 3, 3, 3, 3, 3};
    return ulSunstoneControlStartPaths(cores, rate_hz, both, n);
}
unsigned ulSunstoneControlStartPaths(const unsigned *cores, const unsigned *rate_hz, const unsigned *paths, unsigned n)
{ return ulSunstoneControlStartPathsMode(cores, rate_hz, paths, n, 0); }
unsigned ulSunstoneControlStartPathsMode(const unsigned *cores, const unsigned *rate_hz,
                                       const unsigned *paths, unsigned n, unsigned mode)
{
    if (!cores || !rate_hz || !paths || n > CORES - FIRST_CORE ||
        mode & ~SS_CONTROL_HISTOGRAM_ONLY || __atomic_load_n(&stop_requested, __ATOMIC_ACQUIRE)) return 0;
    uint64_t frequency;
#ifdef SUNSTONE_CONTROL_TEST
    frequency = ss_control_test_frequency();
#else
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
#endif
    if (!frequency || frequency > UINT32_MAX) return 0;
    if (__atomic_load_n(&running, __ATOMIC_ACQUIRE) && mode != measurement_mode) return 0;
    unsigned admitted = 0;
    for (unsigned i = 0; i < n; ++i)
        if (cores[i] >= FIRST_CORE && cores[i] < CORES && rate_hz[i] && rate_hz[i] <= frequency &&
            (paths[i] & 3u) && !__atomic_load_n(&state[cores[i]].requested, __ATOMIC_ACQUIRE)) ++admitted;
    if (!admitted) return 0;
    if (!__atomic_load_n(&running, __ATOMIC_ACQUIRE)) {
        measurement_mode = mode;
        __atomic_store_n(&frequency_hz, frequency, __ATOMIC_RELEASE);
        __atomic_store_n(&consumer_parked, !!(mode & SS_CONTROL_HISTOGRAM_ONLY), __ATOMIC_RELEASE);
        __atomic_store_n(&running, 1u, __ATOMIC_RELEASE);
        if (!(mode & SS_CONTROL_HISTOGRAM_ONLY)) {
            consumer_handle = xTaskCreateStaticAffinitySet(consumer, "control-sink", 512, NULL, 2, consumer_stack, &consumer_task, 0x3e);
            if (!consumer_handle) { __atomic_store_n(&running, 0u, __ATOMIC_RELEASE); return 0; }
        }
    }
    unsigned started = 0;
    for (unsigned i = 0; i < n; ++i) {
        const unsigned c = cores[i];
        if (c < FIRST_CORE || c >= CORES || !rate_hz[i] || rate_hz[i] > frequency || !(paths[i] & 3u) ||
            __atomic_load_n(&state[c].requested, __ATOMIC_ACQUIRE)) continue;
        struct core_state *s = &state[c];
        s->rate_hz = rate_hz[i];
        s->paths = paths[i] & 3u;
        s->control.period = s->task.period = frequency / rate_hz[i];
        if (s->paths & 2u) {
            s->task_handle = xTaskCreateStaticAffinitySet(task_path, "control-task", 512, s, 5, path_stack[c], &path_task[c], 1u << c);
            if (!s->task_handle) { error(s, SS_CONTROL_ERROR_TASK_CREATE); continue; }
        }
        __atomic_store_n(&s->requested, 1u, __ATOMIC_RELEASE);
        __atomic_fetch_or(&requested_mask, 1u << c, __ATOMIC_RELEASE);
        portYIELD_CORE(c);
        ++started;
    }
    return started;
}
void vSunstoneControlStop(void)
{
    __atomic_store_n(&stop_requested, 1u, __ATOMIC_RELEASE);
    for (unsigned c = FIRST_CORE; c < CORES; ++c) {
        struct core_state *s = &state[c];
        if (!__atomic_load_n(&s->requested, __ATOMIC_ACQUIRE)) continue;
        __atomic_store_n(&s->stop, 1u, __ATOMIC_RELEASE);
        if (s->task_handle) xTaskNotifyGive(s->task_handle);
        portYIELD_CORE(c);
    }
}
unsigned ulSunstoneControlProgressRead(struct ss_control_progress *out)
{
    *out = (struct ss_control_progress){0};
    out->frequency_hz = __atomic_load_n(&frequency_hz, __ATOMIC_ACQUIRE);
    if (!out->frequency_hz) return 0;
    out->requested_mask = __atomic_load_n(&requested_mask, __ATOMIC_ACQUIRE);
    for (unsigned c = FIRST_CORE; c < CORES; ++c) {
        struct core_state *s = &state[c];
        if (!(out->requested_mask & (1u << c))) continue;
        for (unsigned p = 0; p < 2; ++p) out->count[c][p] = __atomic_load_n(&s->published[p], __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&s->armed, __ATOMIC_ACQUIRE) &&
            (!(s->paths & 2u) || __atomic_load_n(&s->task_ready, __ATOMIC_ACQUIRE))) out->armed_mask |= 1u << c;
        if (__atomic_load_n(&s->isr_stopped, __ATOMIC_ACQUIRE) &&
            (!(s->paths & 2u) || __atomic_load_n(&s->task_parked, __ATOMIC_ACQUIRE))) out->quiescent_mask |= 1u << c;
        out->errors[c] = __atomic_load_n(&s->errors, __ATOMIC_RELAXED);
    }
    out->consumer_parked = __atomic_load_n(&consumer_parked, __ATOMIC_ACQUIRE);
    return 1;
}
unsigned ulSunstoneControlQuiescent(void)
{
    struct ss_control_progress progress;
    if (!__atomic_load_n(&stop_requested, __ATOMIC_ACQUIRE)) return 0;
    if (!ulSunstoneControlProgressRead(&progress))
        return !__atomic_load_n(&requested_mask, __ATOMIC_ACQUIRE) && !consumer_handle;
    return progress.quiescent_mask == progress.requested_mask && progress.consumer_parked;
}
unsigned ulSunstoneControlTasksSuspended(void)
{
#if ( INCLUDE_eTaskGetState == 1 )
    if (!ulSunstoneControlQuiescent()) return 0;
    if (consumer_handle && eTaskGetState(consumer_handle) != eSuspended) return 0;
    for (unsigned c = FIRST_CORE; c < CORES; ++c)
        if (__atomic_load_n(&state[c].requested, __ATOMIC_ACQUIRE) && state[c].task_handle &&
            eTaskGetState(state[c].task_handle) != eSuspended) return 0;
    return 1;
#else
    return 0;
#endif
}

static uint32_t percentile_ticks(const uint32_t *bins, uint64_t total, uint32_t maximum)
{
    if (!total) return 0;
    if (total < 10000u) return maximum;
    const uint64_t need = total - total / 10000u;
    uint64_t sum = 0;
    for (unsigned b = 0; b < BINS; ++b) {
        sum += bins[b];
        if (sum >= need) return b == BINS - 1u ? maximum : (b + 1u) * SS_CONTROL_BIN_TICKS;
    }
    return maximum;
}
static uint32_t ticks_to_ns(unsigned core, uint32_t ticks)
{
    const uint64_t frequency = __atomic_load_n(&frequency_hz, __ATOMIC_ACQUIRE);
    if (!frequency) return 0;
    const uint64_t ns = (uint64_t)ticks * UINT64_C(1000000000) / frequency;
    if (ns > UINT32_MAX) { error(&state[core], SS_CONTROL_ERROR_NS_RANGE); return UINT32_MAX; }
    return (uint32_t)ns;
}
static uint64_t ticks_to_us(uint64_t ticks)
{
    const uint64_t frequency = __atomic_load_n(&frequency_hz, __ATOMIC_ACQUIRE);
    if (!frequency) return 0;
    const uint64_t seconds = ticks / frequency;
    if (seconds > UINT64_MAX / UINT64_C(1000000)) return UINT64_MAX;
    return seconds * UINT64_C(1000000) + (ticks % frequency) * UINT64_C(1000000) / frequency;
}
unsigned ulSunstoneControlMetricRead(unsigned core, unsigned path, unsigned kind,
                                    struct ss_control_metric *out)
{
    if (!out || !ulSunstoneControlQuiescent() || core < FIRST_CORE || core >= CORES ||
        !state[core].rate_hz || path > 1u || !(state[core].paths & (1u << path)) || kind > (path ? 3u : 1u)) return 0;
    const struct core_state *s = &state[core];
    const struct path *p = path ? &s->task : &s->control;
    const uint32_t *bins;
    uint64_t total;
    uint32_t maximum;
    switch (kind) {
    case 0: bins = p->latency_bin; total = p->count; maximum = p->latency_max; break;
    case 1: bins = p->jitter_bin; total = p->count > 1 ? p->count - 1 : 0; maximum = p->jitter_max; break;
    case 2: bins = p->entry_bin; total = s->task_irq_count; maximum = p->entry_max; break;
    default: bins = p->gap_bin; total = p->count; maximum = p->gap_max; break;
    }
    *out = (struct ss_control_metric){ total, maximum, percentile_ticks(bins, total, maximum) };
    return 1;
}
unsigned ulSunstoneControlStatsRead(unsigned core, unsigned path, struct ss_control_stats *out)
{
    if (core < FIRST_CORE || core >= FIRST_CORE + CORES - 1u || !state[core].rate_hz || path > 1u || !(state[core].paths & (1u << path))) return 0;
    const struct core_state *s = &state[core];
    const struct path *p = path ? &s->task : &s->control;
    out->path = path; out->core = core; out->rate_hz = s->rate_hz; out->count = p->count;
    out->consumed = path ? p->count : s->consumed; out->gaps = path ? 0 : s->gaps;
    out->dropped = path ? 0 : s->ring.dropped; out->missed = p->missed;
    out->latency_max_ns = ticks_to_ns(core, p->latency_max);
    out->jitter_max_ns = ticks_to_ns(core, p->jitter_max);
    out->latency_p9999_ns = ticks_to_ns(core, percentile_ticks(p->latency_bin, p->count, p->latency_max));
    out->jitter_p9999_ns = ticks_to_ns(core, percentile_ticks(p->jitter_bin, p->count > 1 ? p->count - 1 : 0, p->jitter_max));
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
    out->latency_ns = ticks_to_ns(core, w->latency); out->pmr = w->pmr; out->elr = w->elr; out->spsr = w->spsr;
    out->rpr = w->rpr; out->task = w->task; out->nesting = w->nesting; out->when_us = ticks_to_us(w->when);
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
    out->latency_ns = ticks_to_ns(core, w->latency); out->pmr = w->pmr; out->elr = w->elr; out->spsr = w->spsr;
    out->rpr = w->rpr; out->task = w->task; out->nesting = w->nesting; out->when_us = ticks_to_us(w->when);
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
