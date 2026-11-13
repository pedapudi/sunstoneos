/* SPDX-License-Identifier: Apache-2.0 */
/* Unpinned background load for the scheduler comparison (SUNSTONE_BG_LOAD, off by default).
 *
 * Twenty-four tasks of priority 1 with the A55 cores as their affinity. Each alternates a
 * busy burst of mixed length (50 us 40%, 200 us 30%, 1 ms 20%, 4 ms 10%) with a sleep of
 * one to six milliseconds, started at staggered times, so work arrives unevenly. The load records,
 * per phase, bursts completed, busy time and the lateness of every wake in a base-2
 * histogram. Phase 1 covers the recognition, phase 2 an idle second afterwards.
 * Applications call the functions below; the names follow the FreeRTOS pattern that they
 * may reference. Measurement code. */
#include "FreeRTOS.h"
#include "task.h"
#include "sunstone_deadline.h"
#include "sunstone_notify.h"
#include "sunstone_bg.h"
#include <stdatomic.h>
#include <stdint.h>

#define TASKS SUNSTONE_BG_TASKS
#define BINS 24u
#define PHASES 3u

static StaticTask_t task_state[TASKS];
static StackType_t task_stack[TASKS][512];
static TaskHandle_t task_handle[TASKS];
static struct {
    uint64_t bursts[PHASES], busy_ticks[PHASES];
    uint32_t bins[PHASES][BINS];
} __attribute__((aligned(64))) stats[TASKS];
static atomic_uint phase, stop, created, stopped;
static _Atomic(TaskHandle_t) stop_waiter;
static uint64_t frequency;

static inline uint64_t counter(void)
{ uint64_t v; __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(v) :: "memory"); return v; }
static inline uint32_t step(uint32_t *x) { *x = *x * 1664525u + 1013904223u; return *x >> 8; }
static void sleep_until(uint64_t until)
{
    while(!atomic_load_explicit(&stop,memory_order_acquire)&&sunstone_deadline_now_us()<until)
        configASSERT(sunstone_deadline_wait_until(until)!=SUNSTONE_DEADLINE_ERROR);
}
static void stopped_one(void)
{
    if(atomic_fetch_add_explicit(&stopped,1,memory_order_acq_rel)+1==TASKS){
        TaskHandle_t waiter=atomic_load_explicit(&stop_waiter,memory_order_acquire);
        if(waiter)xTaskNotifyGiveIndexed(waiter,xTaskGetApplicationTaskTag(waiter)?SS_NOTIFY_DEADLINE:SS_NOTIFY_OWN);
    }
}

static void load(void *argument)
{
    const unsigned id = (unsigned)(uintptr_t)argument;
    uint32_t seed = id * 2654435761u + 12345u;
    configASSERT(sunstone_deadline_register());
    sleep_until(sunstone_deadline_now_us()+(1+id%5)*UINT64_C(1000));
    while (!stop) {
        const uint32_t r = step(&seed) % 10;
        const uint64_t length = r < 4 ? 50 : r < 7 ? 200 : r < 9 ? 1000 : 4000;
        const uint64_t begin = counter(), span = length * frequency / 1000000u;
        while (counter() - begin < span) {}
        const unsigned p = phase;
        stats[id].bursts[p]++;
        stats[id].busy_ticks[p] += counter() - begin;
        const uint64_t sleep_us = (1 + step(&seed) % 6)*UINT64_C(1000);
        const uint64_t slept = counter();
        sleep_until(sunstone_deadline_now_us()+sleep_us);
        const uint64_t woke = counter();
        const uint64_t due = sleep_us * frequency / 1000000u, elapsed = woke - slept;
        const uint64_t late_us = elapsed > due ? (elapsed - due) * 1000000u / frequency : 0;
        unsigned bin = 0;
        while ((late_us >> bin) > 1 && bin < BINS - 1) ++bin;
        stats[id].bins[phase][bin]++;
    }
    stopped_one();
    for (;;) vTaskSuspend(NULL);
}
void vSunstoneBgStart(void)
{
    if (created) return;
    created = 1;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    phase = 1;
    for (unsigned i = 0; i < TASKS; ++i)
        task_handle[i]=xTaskCreateStaticAffinitySet(load, "bg-load", 512, (void *)(uintptr_t)i, 1, task_stack[i], &task_state[i], 0x3f);
}
void vSunstoneBgPhase(unsigned next) { phase = next; }
void vSunstoneBgStop(void)
{
    atomic_store_explicit(&stop_waiter,xTaskGetCurrentTaskHandle(),memory_order_release);
    atomic_store_explicit(&stop,1,memory_order_release);
    for(unsigned i=0;i<TASKS;++i)if(task_handle[i])
        xTaskNotifyGiveIndexed(task_handle[i],xTaskGetApplicationTaskTag(task_handle[i])?SS_NOTIFY_DEADLINE:SS_NOTIFY_OWN);
}
/* A release from each worker precedes this final acquire. Statistics can be
 * read without concurrent writers once every accepted worker has stopped. */
unsigned ulSunstoneBgStopped(void)
{return atomic_load_explicit(&stopped,memory_order_acquire);}
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
