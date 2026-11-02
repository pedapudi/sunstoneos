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
#define CORES 6u        /* control cores 1 to 5; index by core */
#define BINS 768u       /* one counter tick (41.7 ns at 24 MHz) per bin, 32 us; the last bin takes the rest */
#define TICKS_PER_US 24u

struct path {
    uint64_t period, deadline, last_entry;
    uint64_t count, missed;
    uint32_t seq, latency_max, jitter_max;
    uint32_t latency_bin[BINS], jitter_bin[BINS];
};
static struct core_state {
    volatile uint32_t requested, armed, stop;
    uint32_t rate_hz;
    struct path control, task;   /* the interrupt handler's samples; the woken task's samples */
    struct ss_ring ring;
    uint64_t consumed, gaps;
    uint32_t expected;
    TaskHandle_t task_handle;
    uint64_t fired_deadline;     /* the deadline whose interrupt woke the task */
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
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (unsigned)((mpidr >> 8) & 0xffu); /* the A733 numbers cores in affinity level 1 */
}

/* Record one entry of a path: latency against the deadline it served, jitter against the previous entry. */
static void record(struct path *p, uint64_t now, uint64_t deadline, unsigned missed)
{
    const uint64_t late = now - deadline;
    const uint32_t latency = late > 0xffffffffu ? 0xffffffffu : (uint32_t)late;
    ++p->count;
    p->missed += missed;
    ++p->latency_bin[latency < BINS ? latency : BINS - 1u];
    if (latency > p->latency_max) p->latency_max = latency;
    if (p->last_entry) {
        const uint64_t expected = p->period * (1u + missed), interval = now - p->last_entry;
        const uint64_t error = interval > expected ? interval - expected : expected - interval;
        const uint32_t jitter = error > 0xffffffffu ? 0xffffffffu : (uint32_t)error;
        ++p->jitter_bin[jitter < BINS ? jitter : BINS - 1u];
        if (jitter > p->jitter_max) p->jitter_max = jitter;
    }
    p->last_entry = now;
}

static void arm(struct core_state *s)
{
    const uintptr_t sgi = (uintptr_t)a733_cached_runtime[this_core()].redistributor + 0x10000u;
    *(volatile uint8_t *)(sgi + 0x400u + INTID) = A733_GIC_NS_WRITE_FOR_STORED(A733_GIC_CONTROL_STORED);
    *(volatile uint8_t *)(sgi + 0x400u + TASK_INTID) = A733_GIC_NS_WRITE_FOR_STORED(A733_GIC_TASK_PATH_STORED);
    *(volatile uint32_t *)(sgi + 0x100u) = (1u << INTID) | (1u << TASK_INTID);
    __asm__ volatile("dsb sy" ::: "memory");
    const uint64_t now = counter();
    s->control.deadline = now + s->control.period;
    s->task.deadline = now + s->task.period + s->task.period / 2u; /* half a period after the control handler */
    s->control.last_entry = s->task.last_entry = 0;
    program(s->control.deadline, 1);
    program_task(s->task.deadline, 1);
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
    s->task.missed += missed;
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
    unsigned started = 0;
    if (!running) {
        running = 1;
        (void)xTaskCreateStaticAffinitySet(consumer, "control-sink", 512, NULL, 2, consumer_stack, &consumer_task, 0x3e);
    }
    for (unsigned i = 0; i < n; ++i) {
        const unsigned c = cores[i];
        if (c < FIRST_CORE || c >= FIRST_CORE + CORES - 1u || !rate_hz[i] || state[c].requested) continue;
        state[c].rate_hz = rate_hz[i];
        state[c].control.period = state[c].task.period = 24000000u / rate_hz[i];
        state[c].task_handle = xTaskCreateStaticAffinitySet(task_path, "control-task", 512, &state[c], 5, path_stack[c], &path_task[c], 1u << c);
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
        if (sum >= need) return b == BINS - 1u ? maximum * 1000u / TICKS_PER_US : (b + 1u) * 1000u / TICKS_PER_US;
    }
    return maximum * 1000u / TICKS_PER_US;
}
unsigned ulSunstoneControlStatsRead(unsigned core, unsigned path, struct ss_control_stats *out)
{
    if (core < FIRST_CORE || core >= FIRST_CORE + CORES - 1u || !state[core].rate_hz || path > 1u) return 0;
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
