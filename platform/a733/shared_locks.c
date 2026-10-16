#include "a733_platform_locks.h"
#include "task.h"

#if INCLUDE_xTaskGetSchedulerState != 1 || INCLUDE_xTaskGetCurrentTaskHandle != 1 || INCLUDE_xSemaphoreGetMutexHolder != 1
#error "Platform locks require scheduler-state, current-task and mutex-holder APIs"
#endif

static int task_context(void)
{
    uint64_t flags, nesting, critical;
    __asm__ volatile("mrs %0, daif\n msr daifset, #2" : "=r"(flags) :: "memory");
#if configNUMBER_OF_CORES > 1
    extern volatile uint64_t ullPortInterruptNestings[], ullCriticalNestings[];
    uint64_t core;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(core));
    nesting = core < configNUMBER_OF_CORES ? ullPortInterruptNestings[core] : 1;
    critical = core < configNUMBER_OF_CORES ? ullCriticalNestings[core] : 1;
#else
    extern uint64_t ullPortInterruptNesting;
    extern volatile uint64_t ullCriticalNesting;
    nesting = ullPortInterruptNesting; critical = ullCriticalNesting;
#endif
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
    return !(flags & 0x80) && !nesting && !critical &&
        xTaskGetSchedulerState() == taskSCHEDULER_RUNNING && xTaskGetCurrentTaskHandle();
}
static int reg_enter(void *p, uint64_t deadline)
{ return a733_platform_lock_enter(p, A733_PLATFORM_REGISTER_LOCK, deadline); }
static int i2c_enter(void *p, uint64_t deadline)
{ return a733_platform_lock_enter(p, A733_PLATFORM_I2C_LOCK, deadline); }
static void reg_leave(void *p) { a733_platform_lock_leave(p, A733_PLATFORM_REGISTER_LOCK); }
static void i2c_leave(void *p) { a733_platform_lock_leave(p, A733_PLATFORM_I2C_LOCK); }
int a733_platform_locks_init(struct a733_platform_locks *p, int (*now)(void *, uint64_t *), void *context)
{
    if (!p || !now || p->initialized) return 0;
    p->now = now; p->context = context;
    atomic_init(&p->retained, 0);
    p->mutex[0] = xSemaphoreCreateMutexStatic(&p->storage[0]);
    p->mutex[1] = xSemaphoreCreateMutexStatic(&p->storage[1]);
    if (!p->mutex[0] || !p->mutex[1]) return 0;
    p->callbacks = (struct a733_gpu_resource_locks){reg_enter, reg_leave, i2c_enter, i2c_leave, p};
    p->initialized = 1;
    return 1;
}
int a733_platform_lock_enter(struct a733_platform_locks *p, enum a733_platform_lock lock, uint64_t deadline)
{
    if (!p || !p->initialized || (unsigned)lock >= 2 || !task_context() ||
        (atomic_load_explicit(&p->retained, memory_order_acquire) & (1u << lock))) return 0;
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (xSemaphoreGetMutexHolder(p->mutex[0]) == self || xSemaphoreGetMutexHolder(p->mutex[1]) == self) return 0;
    uint64_t now;
    if (!p->now(p->context, &now) || now >= deadline) return 0;
    uint64_t last = now;
    /* Floor to ticks and cap at one tick: a waiter sleeps when useful, but
     * never turns a sub-tick remaining interval into an unbounded wait. */
    uint64_t tick_us = (1000000u + configTICK_RATE_HZ - 1u) / configTICK_RATE_HZ;
    TickType_t wait = deadline - now >= tick_us ? 1 : 0;
    while (xSemaphoreTake(p->mutex[lock], wait) != pdTRUE) {
        if (!p->now(p->context, &now) || now < last || now >= deadline || !wait ||
            (atomic_load_explicit(&p->retained, memory_order_acquire) & (1u << lock))) return 0;
        last = now;
        wait = deadline - now >= tick_us ? 1 : 0;
    }
    if (!p->now(p->context, &now) || now < last || now >= deadline ||
        (atomic_load_explicit(&p->retained, memory_order_acquire) & (1u << lock))) {
        configASSERT(xSemaphoreGive(p->mutex[lock]) == pdTRUE);
        return 0;
    }
    return 1;
}
void a733_platform_lock_leave(struct a733_platform_locks *p, enum a733_platform_lock lock)
{
    configASSERT(p && p->initialized && (unsigned)lock < 2 && task_context());
    configASSERT(xSemaphoreGetMutexHolder(p->mutex[lock]) == xTaskGetCurrentTaskHandle());
    configASSERT(!(atomic_load_explicit(&p->retained, memory_order_acquire) & (1u << lock)));
    configASSERT(xSemaphoreGive(p->mutex[lock]) == pdTRUE);
}
int a733_platform_lock_retain(struct a733_platform_locks *p, enum a733_platform_lock lock)
{
    if (!p || !p->initialized || (unsigned)lock >= 2 || !task_context() ||
        xSemaphoreGetMutexHolder(p->mutex[lock]) != xTaskGetCurrentTaskHandle()) return 0;
    atomic_fetch_or_explicit(&p->retained, 1u << lock, memory_order_release);
    return 1;
}
