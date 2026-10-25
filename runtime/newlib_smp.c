#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <reent.h>
#include <sys/lock.h>
#include <stdint.h>

struct __lock {
    StaticSemaphore_t storage;
    SemaphoreHandle_t handle;
};
struct __lock __lock___sfp_recursive_mutex;
struct __lock __lock___atexit_recursive_mutex;
struct __lock __lock___at_quick_exit_mutex;
struct __lock __lock___malloc_recursive_mutex;
struct __lock __lock___env_recursive_mutex;
struct __lock __lock___tz_mutex;
struct __lock __lock___dd_hash_mutex;
struct __lock __lock___arc4random_mutex;
static struct __lock pool[32];
static unsigned initialized;

/* Once this core has resumed a task, bits [47:0] of TPIDRRO_EL0 hold that
 * task's TCB, and EL0 can read the register (docs/smp.md). A task's own TCB
 * does not change when it migrates, so the read needs no mask, as in
 * xTaskGetCurrentTaskHandle. StaticTask_t has the TCB's layout; the kernel
 * additions check that its TLS member has the TCB's offset. The field reads
 * zero until the core first resumes a task, and the supervisor call then
 * returns the global state before the scheduler starts. */
struct _reent *__getreent(void) {
    const uintptr_t task = (uintptr_t)(ullPortGetTaskRegister() & portTASK_REGISTER_TCB_MASK);
    if (task) return &((StaticTask_t *)task)->xDummy17;
    register struct _reent *context __asm__("x0");
    __asm__ volatile("svc #118" : "=r"(context) :: "memory");
    return context;
}

static void initialize(struct __lock *lock) {
    lock->handle = xSemaphoreCreateRecursiveMutexStatic(&lock->storage);
    configASSERT(lock->handle);
}
void runtime_smp_init(void) {
    configASSERT(!initialized);
    struct __lock *locks[] = {
        &__lock___sfp_recursive_mutex, &__lock___atexit_recursive_mutex,
        &__lock___at_quick_exit_mutex, &__lock___malloc_recursive_mutex,
        &__lock___env_recursive_mutex, &__lock___tz_mutex,
        &__lock___dd_hash_mutex, &__lock___arc4random_mutex
    };
    for (unsigned i = 0; i < sizeof(locks) / sizeof(locks[0]); ++i) initialize(locks[i]);
    initialized = 1;
}
void __retarget_lock_init(_LOCK_T *result) {
    configASSERT(initialized);
    taskENTER_CRITICAL();
    *result = 0;
    for (unsigned i = 0; i < sizeof(pool) / sizeof(pool[0]); ++i) {
        if (!pool[i].handle) {
            initialize(&pool[i]);
            *result = &pool[i];
            break;
        }
    }
    taskEXIT_CRITICAL();
    configASSERT(*result);
}
void __retarget_lock_init_recursive(_LOCK_T *result) { __retarget_lock_init(result); }

void __retarget_lock_close(_LOCK_T lock) {
    configASSERT((uintptr_t)lock >= (uintptr_t)pool &&
                 (uintptr_t)lock < (uintptr_t)(pool + 32));
    taskENTER_CRITICAL();
    configASSERT(lock->handle && !xSemaphoreGetMutexHolder(lock->handle));
    vSemaphoreDelete(lock->handle);
    lock->handle = 0;
    taskEXIT_CRITICAL();
}
void __retarget_lock_close_recursive(_LOCK_T lock) { __retarget_lock_close(lock); }

static int take(_LOCK_T lock, TickType_t timeout) {
    configASSERT(initialized && lock && lock->handle);
    // Constructors run on the primary core before the scheduler starts.
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) return 1;
    return xSemaphoreTakeRecursive(lock->handle, timeout) == pdTRUE;
}
void __retarget_lock_acquire(_LOCK_T lock) { configASSERT(take(lock, portMAX_DELAY)); }
void __retarget_lock_acquire_recursive(_LOCK_T lock) { __retarget_lock_acquire(lock); }
int __retarget_lock_try_acquire(_LOCK_T lock) { return take(lock, 0); }
int __retarget_lock_try_acquire_recursive(_LOCK_T lock) { return take(lock, 0); }
void __retarget_lock_release(_LOCK_T lock) {
    configASSERT(initialized && lock && lock->handle);
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
        configASSERT(xSemaphoreGiveRecursive(lock->handle) == pdTRUE);
}
void __retarget_lock_release_recursive(_LOCK_T lock) { __retarget_lock_release(lock); }
