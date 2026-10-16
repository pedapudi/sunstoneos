#include "a733_platform_locks.h"
#include "task.h"
#include "platform.h"

static struct a733_platform_locks locks;
static StaticTask_t tasks[2];
static StackType_t stacks[2][2048];
static TaskHandle_t owner_task, waiter_task;
static atomic_uint progressed;
static uint64_t frequency;
static int now(void *context, uint64_t *out)
{
    (void)context; uint64_t counter;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(counter));
    *out = (counter / frequency) * 1000000 + (counter % frequency) * 1000000 / frequency;
    return 1;
}
static uint64_t deadline(void)
{ uint64_t value; now(0, &value); return value + 100000; }
static void owner(void *context)
{
    (void)context;
    configASSERT(locks.callbacks.register_enter(&locks, deadline()));
    xTaskNotifyGive(waiter_task);
    vTaskDelay(3);
    atomic_store_explicit(&progressed, 1, memory_order_release);
    locks.callbacks.register_leave(&locks);
    configASSERT(ulTaskNotifyTake(pdTRUE, 100) == 1);
    configASSERT(locks.callbacks.i2c_enter(&locks, deadline()));
    xTaskNotifyGive(waiter_task);
    configASSERT(ulTaskNotifyTake(pdTRUE, 100) == 1);
    configASSERT(a733_platform_lock_retain(&locks, A733_PLATFORM_I2C_LOCK));
    xTaskNotifyGive(waiter_task);
    vTaskSuspend(NULL);
    configASSERT(0);
}
static void waiter(void *context)
{
    (void)context;
    configASSERT(ulTaskNotifyTake(pdTRUE, 100) == 1);
    configASSERT(locks.callbacks.register_enter(&locks, deadline()));
    configASSERT(atomic_load_explicit(&progressed, memory_order_acquire));
    configASSERT(!locks.callbacks.register_enter(&locks, deadline()));
    configASSERT(!locks.callbacks.i2c_enter(&locks, deadline()));
    locks.callbacks.register_leave(&locks);
    uint64_t value; now(0, &value);
    configASSERT(!locks.callbacks.register_enter(&locks, value));
    taskENTER_CRITICAL();
    configASSERT(!locks.callbacks.register_enter(&locks, deadline()));
    taskEXIT_CRITICAL();
    xTaskNotifyGive(owner_task);
    configASSERT(ulTaskNotifyTake(pdTRUE, 100) == 1);
    now(0, &value);
    configASSERT(!locks.callbacks.i2c_enter(&locks, value + 2000));
    configASSERT(xSemaphoreGetMutexHolder(locks.mutex[1]) == owner_task);
    xTaskNotifyGive(owner_task);
    configASSERT(ulTaskNotifyTake(pdTRUE, 100) == 1);
    configASSERT(!locks.callbacks.i2c_enter(&locks, deadline()));
    configASSERT(!a733_platform_lock_retain(&locks, A733_PLATFORM_I2C_LOCK));
    configASSERT(xSemaphoreGetMutexHolder(locks.mutex[1]) == owner_task);
    configASSERT(locks.callbacks.register_enter(&locks, deadline()));
    locks.callbacks.register_leave(&locks);
    platform_puts("PASS platform locks: two-core task waits, deadline refusal, nested/critical refusal and retained ownership\n");
    platform_exit(0);
}
BaseType_t npu_service_task_privileged(TaskFunction_t function, void *argument)
{ (void)argument; return function == owner || function == waiter; }
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{ (void)task; platform_panic(name, 0); }
void kernel_main(void)
{
    platform_init();
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    configASSERT(frequency >= 1000000 && frequency <= UINT64_MAX / 1000000);
    configASSERT(a733_platform_locks_init(&locks, now, 0));
    configASSERT(!locks.callbacks.register_enter(&locks, deadline()));
    owner_task = xTaskCreateStaticAffinitySet(owner, "reg-owner", 2048, NULL, 3, stacks[0], &tasks[0], 1);
    waiter_task = xTaskCreateStaticAffinitySet(waiter, "reg-waiter", 2048, NULL, 5, stacks[1], &tasks[1], 2);
    configASSERT(owner_task && waiter_task);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
