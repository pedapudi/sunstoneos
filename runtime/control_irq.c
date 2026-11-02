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
 * A core arms its timer from its own first interrupt after ss_control_start, because the redistributor
 * and the virtual timer are configured by the core that owns them. Core 0 keeps the task-deadline timer. */
#include "FreeRTOS.h"
#include "task.h"
#include <stdint.h>
#include "a733_gic_priority.h"
#include "sunstone_control.h"
#include "control_ring.h"

extern void __real_vApplicationIRQHandler(uint64_t interrupt);
extern struct { uint64_t epoch, startup_deadline, diagnostic_deadline, redistributor, reserved[12]; } a733_cached_runtime[8];

#define INTID 27u
#define FIRST_CORE 1u
#define CORES 6u        /* control cores 1 to 5; index by core */
#define BINS 768u       /* one counter tick (41.7 ns at 24 MHz) per bin, 32 us; the last bin takes the rest */
#define TICKS_PER_US 24u

static struct core_state {
    volatile uint32_t requested, armed, stop;
    uint32_t rate_hz;
    uint64_t period, deadline, last_entry;
    uint64_t count, missed;
    uint32_t seq, latency_max, jitter_max;
    uint32_t latency_bin[BINS], jitter_bin[BINS];
    struct ss_ring ring;
    uint64_t consumed, gaps;
    uint32_t expected;
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
static inline unsigned this_core(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (unsigned)((mpidr >> 8) & 0xffu); /* the A733 numbers cores in affinity level 1 */
}

static void arm(struct core_state *s)
{
    const uintptr_t sgi = (uintptr_t)a733_cached_runtime[this_core()].redistributor + 0x10000u;
    *(volatile uint8_t *)(sgi + 0x400u + INTID) = A733_GIC_NS_WRITE_FOR_STORED(A733_GIC_CONTROL_STORED);
    *(volatile uint32_t *)(sgi + 0x100u) = 1u << INTID;
    __asm__ volatile("dsb sy" ::: "memory");
    s->deadline = counter() + s->period;
    s->last_entry = 0;
    program(s->deadline, 1);
    s->armed = 1;
}

static void handle(struct core_state *s)
{
    const uint64_t now = counter();
    if (s->stop) { program(0, 0); s->armed = 0; return; }
    const uint64_t deadline = s->deadline;
    uint64_t next = deadline + s->period;
    unsigned missed = 0;
    while (next <= now) { next += s->period; ++missed; }
    s->deadline = next;
    program(next, 1); /* clears the level-sensitive interrupt */
    const uint64_t late = now - deadline;
    const uint32_t latency = late > 0xffffffffu ? 0xffffffffu : (uint32_t)late;
    ++s->count;
    s->missed += missed;
    unsigned bin = latency < BINS ? latency : BINS - 1u;
    ++s->latency_bin[bin];
    if (latency > s->latency_max) s->latency_max = latency;
    if (s->last_entry) {
        const uint64_t expected = s->period * (1u + missed), interval = now - s->last_entry;
        const uint64_t error = interval > expected ? interval - expected : expected - interval;
        const uint32_t jitter = error > 0xffffffffu ? 0xffffffffu : (uint32_t)error;
        ++s->jitter_bin[jitter < BINS ? jitter : BINS - 1u];
        if (jitter > s->jitter_max) s->jitter_max = jitter;
    }
    s->last_entry = now;
    (void)ss_ring_put(&s->ring, (struct ss_ring_sample){ s->seq++, latency, now });
}

void __wrap_vApplicationIRQHandler(uint64_t interrupt)
{
    const unsigned core = this_core();
    if (core >= FIRST_CORE && core < FIRST_CORE + CORES - 1u) {
        struct core_state *s = &state[core];
        if (interrupt == INTID && s->armed) { handle(s); return; }
        if (__atomic_load_n(&s->requested, __ATOMIC_ACQUIRE) && !s->armed && !s->stop) arm(s);
    }
    __real_vApplicationIRQHandler(interrupt);
}

static StaticTask_t consumer_task;
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

unsigned ss_control_start(const unsigned *cores, const unsigned *rate_hz, unsigned n)
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
        state[c].period = 24000000u / rate_hz[i];
        __atomic_store_n(&state[c].requested, 1u, __ATOMIC_RELEASE);
        ++started;
    }
    return started;
}
void ss_control_stop(void)
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
unsigned ss_control_stats_read(unsigned core, struct ss_control_stats *out)
{
    if (core < FIRST_CORE || core >= FIRST_CORE + CORES - 1u || !state[core].rate_hz) return 0;
    const struct core_state *s = &state[core];
    out->core = core; out->rate_hz = s->rate_hz; out->count = s->count; out->consumed = s->consumed;
    out->gaps = s->gaps; out->dropped = s->ring.dropped; out->missed = s->missed;
    out->latency_max_ns = s->latency_max * 1000u / TICKS_PER_US;
    out->jitter_max_ns = s->jitter_max * 1000u / TICKS_PER_US;
    out->latency_p9999_ns = percentile_ns(s->latency_bin, s->count, s->latency_max);
    out->jitter_p9999_ns = percentile_ns(s->jitter_bin, s->count > 1 ? s->count - 1 : 0, s->jitter_max);
    return 1;
}
