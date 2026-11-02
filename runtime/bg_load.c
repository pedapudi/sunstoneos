/* SPDX-License-Identifier: Apache-2.0 */
/* Unpinned background load for the scheduler comparison (SUNSTONE_BG_LOAD, off by default).
 *
 * Twenty-four tasks of priority 1 with the A55 cores as their affinity. Each alternates a
 * busy burst of mixed length (50 us 40%, 200 us 30%, 1 ms 20%, 4 ms 10%) with a sleep of
 * one to six ticks, started at staggered times, so work arrives unevenly. The load records,
 * per phase, bursts completed, busy time and the lateness of every wake in a base-2
 * histogram. Phase 1 covers the recognition, phase 2 an idle second afterwards.
 * Applications call the functions below; the names follow the FreeRTOS pattern that they
 * may reference. Measurement code. */
#include "FreeRTOS.h"
#include "task.h"
#include <stdint.h>

#define TASKS 24u
#define BINS 24u
#define PHASES 3u

static StaticTask_t task_state[TASKS];
static StackType_t task_stack[TASKS][512];
static struct {
    uint64_t bursts[PHASES], busy_ticks[PHASES];
    uint32_t bins[PHASES][BINS];
} __attribute__((aligned(64))) stats[TASKS];
static volatile unsigned phase, stop, created;
static uint64_t frequency;

static inline uint64_t counter(void)
{ uint64_t v; __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(v) :: "memory"); return v; }
static inline uint32_t step(uint32_t *x) { *x = *x * 1664525u + 1013904223u; return *x >> 8; }

static void load(void *argument)
{
    const unsigned id = (unsigned)(uintptr_t)argument;
    uint32_t seed = id * 2654435761u + 12345u;
    vTaskDelay(1 + id % 5);
    while (!stop) {
        const uint32_t r = step(&seed) % 10;
        const uint64_t length = r < 4 ? 50 : r < 7 ? 200 : r < 9 ? 1000 : 4000;
        const uint64_t begin = counter(), span = length * frequency / 1000000u;
        while (counter() - begin < span) {}
        const unsigned p = phase;
        stats[id].bursts[p]++;
        stats[id].busy_ticks[p] += counter() - begin;
        const TickType_t ticks = 1 + step(&seed) % 6;
        const uint64_t slept = counter();
        vTaskDelay(ticks);
        const uint64_t woke = counter();
        const uint64_t due = ticks * frequency / 1000u, elapsed = woke - slept;
        const uint64_t late_us = elapsed > due ? (elapsed - due) * 1000000u / frequency : 0;
        unsigned bin = 0;
        while ((late_us >> bin) > 1 && bin < BINS - 1) ++bin;
        stats[id].bins[phase][bin]++;
    }
    for (;;) vTaskSuspend(NULL);
}
void vSunstoneBgStart(void)
{
    if (created) return;
    created = 1;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    phase = 1;
    for (unsigned i = 0; i < TASKS; ++i)
        (void)xTaskCreateStaticAffinitySet(load, "bg-load", 512, (void *)(uintptr_t)i, 1, task_stack[i], &task_state[i], 0x3f);
}
void vSunstoneBgPhase(unsigned next) { phase = next; }
void vSunstoneBgStop(void) { stop = 1; }
/* kind 0: bursts, 1: busy microseconds, 2: lateness bin `arg` (bin b holds 2^b to 2^(b+1)-1 us; bin 0 also zero). */
unsigned long ulSunstoneBgValue(unsigned p, unsigned kind, unsigned arg)
{
    unsigned long sum = 0;
    if (p >= PHASES) return 0;
    for (unsigned i = 0; i < TASKS; ++i) {
        if (kind == 0) sum += (unsigned long)stats[i].bursts[p];
        else if (kind == 1) sum += (unsigned long)(stats[i].busy_ticks[p] * 1000000u / frequency);
        else if (arg < BINS) sum += stats[i].bins[p][arg];
    }
    return sum;
}
