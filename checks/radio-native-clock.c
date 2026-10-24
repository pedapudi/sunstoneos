/* Actual native callback; only FreeRTOS observations are modeled. The Python
 * runner executes its AArch64 instructions and models register storage. */
#include "a733_platform_locks.h"
#include "a733_radio_platform_native.h"
#include "task.h"

uint64_t ullPortInterruptNesting;
volatile uint64_t ullCriticalNesting;
static unsigned mode;
static struct a733_platform_locks locks;
static struct a733_radio_platform_native native;

BaseType_t xTaskGetSchedulerState(void)
{ return mode&32?taskSCHEDULER_NOT_STARTED:taskSCHEDULER_RUNNING; }
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{ return (TaskHandle_t)(uintptr_t)(mode&1?2:1); }
TaskHandle_t xQueueGetMutexHolder(QueueHandle_t queue)
{ return queue==(QueueHandle_t)(uintptr_t)3 && !(mode&2)?(TaskHandle_t)(uintptr_t)1:0; }

/* Include the production unit to call its private write32 without exporting
 * a test-only production API. Unused callbacks are discarded by the linker. */
#ifndef RADIO_NATIVE_SOURCE
#define RADIO_NATIVE_SOURCE "../drivers/net/a733_radio_platform_native.c"
#endif
#include RADIO_NATIVE_SOURCE

int radio_native_clock_write(uintptr_t address,uint32_t value,unsigned flags)
{
    mode=flags;
    ullPortInterruptNesting=!!(flags&8);
    ullCriticalNesting=!!(flags&16);
    locks.mutex[A733_PLATFORM_REGISTER_LOCK]=(SemaphoreHandle_t)(uintptr_t)3;
    atomic_store_explicit(&locks.retained,!!(flags&4),memory_order_relaxed);
    native.locks=&locks;
    native.owner_task=(void *)(uintptr_t)1;
    native.initialized=1;
    return write32(&native,address,value);
}
