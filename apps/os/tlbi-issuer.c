/* A task that issues broadcast TLB invalidations at a fixed rate from one core, to measure what they cost the other cores
 * (docs/designs/application-memory-layout.md, step 7). It is a measurement fixture of the OS image, selected by
 * SUNSTONE_TLBI_ISSUER, and changes nothing else: the scheduler soak, the control interrupts and the application run as
 * they do without it.
 *
 *   SUNSTONE_TLBI_ISSUER        the form: 0 the task wakes and issues nothing (the control), 1 TLBI VAE1IS, 2 TLBI ASIDE1IS,
 *                               3 TLBI VMALLE1IS
 *   SUNSTONE_TLBI_PERIOD_TICKS  kernel ticks between invalidations (1 kHz tick): 1 is 1000 per second, 10 is 100 per second
 *   SUNSTONE_TLBI_CORE          the core that issues them
 *
 * The task runs at EL1 on its core, sleeps between invalidations, and stops when the application has finished. Every form
 * is followed by DSB ISH, which is when the invalidation has completed on all cores. */
#include "os.h"
#include "application.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdatomic.h>
#include <stdint.h>

#ifndef SUNSTONE_TLBI_PERIOD_TICKS
#define SUNSTONE_TLBI_PERIOD_TICKS 1
#endif
#ifndef SUNSTONE_TLBI_CORE
#define SUNSTONE_TLBI_CORE 5
#endif
#define STACK_WORDS 512

/* What the task did, for a host check and for a debugger: wakes, invalidations, the core it ran on, and the most counter
 * ticks (24 MHz) one invalidation and its DSB took. */
struct os_tlbi_stats {
    atomic_uint wakes, issued, core;
    atomic_uint_fast64_t max_ticks, total_ticks;
};
struct os_tlbi_stats os_tlbi_stats;
static StaticTask_t task_state;
static StackType_t task_stack[STACK_WORDS];

static void issue(void)
{
#if SUNSTONE_TLBI_ISSUER == 1
    __asm__ volatile("tlbi vae1is, %0" :: "r"((uint64_t)(uintptr_t)&os_tlbi_stats >> 12) : "memory");
#elif SUNSTONE_TLBI_ISSUER == 2
    __asm__ volatile("tlbi aside1is, %0" :: "r"((uint64_t)1 << 48) : "memory");
#elif SUNSTONE_TLBI_ISSUER == 3
    __asm__ volatile("tlbi vmalle1is" ::: "memory");
#endif
    __asm__ volatile("dsb ish\n isb" ::: "memory");
}

static void issuer(void *unused)
{
    (void)unused;
    TickType_t last = xTaskGetTickCount();
    atomic_store_explicit(&os_tlbi_stats.core, portGET_CORE_ID(), memory_order_relaxed);
    for (;;) {
        /* A fixed rate without drift: sleep until the next multiple of the period, whatever the last pass took. */
        last += SUNSTONE_TLBI_PERIOD_TICKS;
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(last - now) > 0) vTaskDelay(last - now); else last = now;
        if (os_application_finished()) vTaskSuspend(NULL);
        atomic_fetch_add_explicit(&os_tlbi_stats.wakes, 1, memory_order_relaxed);
#if SUNSTONE_TLBI_ISSUER
        uint64_t begin, end;
        __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(begin) :: "memory");
        issue();
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(end) :: "memory");
        const uint64_t cost = end - begin;
        atomic_fetch_add_explicit(&os_tlbi_stats.total_ticks, cost, memory_order_relaxed);
        if (cost > atomic_load_explicit(&os_tlbi_stats.max_ticks, memory_order_relaxed))
            atomic_store_explicit(&os_tlbi_stats.max_ticks, cost, memory_order_relaxed);
        atomic_fetch_add_explicit(&os_tlbi_stats.issued, 1, memory_order_relaxed);
#endif
    }
}

int os_tlbi_task_privileged(void (*entry)(void *), void *context)
{ return entry == issuer && !context; }
int os_tlbi_pins(struct a733_pin_handoff *pins)
{ (void)pins; return 1; }
int os_tlbi_start(void)
{
    return xTaskCreateStaticAffinitySet(issuer, "tlbi-issuer", STACK_WORDS, NULL, 4, task_stack, &task_state,
                                        (UBaseType_t)1 << SUNSTONE_TLBI_CORE) != NULL;
}
