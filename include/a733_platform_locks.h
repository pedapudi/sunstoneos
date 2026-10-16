#ifndef SUNSTONE_A733_PLATFORM_LOCKS_H
#define SUNSTONE_A733_PLATFORM_LOCKS_H
#include "FreeRTOS.h"
#include "semphr.h"
#include "a733_gpu_resources_native.h"
#include <stdatomic.h>

enum a733_platform_lock { A733_PLATFORM_REGISTER_LOCK, A733_PLATFORM_I2C_LOCK };
struct a733_platform_locks {
    StaticSemaphore_t storage[2];
    SemaphoreHandle_t mutex[2];
    atomic_uint retained;
    int (*now)(void *, uint64_t *);
    void *context;
    struct a733_gpu_resource_locks callbacks;
    unsigned initialized;
};
/* One static instance per platform, initialized before clients become runnable.
 * Every CCU/PCK writer and PMIC/I2C client must share these callbacks. Supply
 * the native adapter's monotonic microsecond clock. Require FreeRTOS static
 * mutexes plus scheduler-state/current-task/mutex-holder APIs. The supported
 * ports are the project's AArch64 SRE and patched SMP ports.
 * Task-only, EL1, scheduler running, outside critical sections; no recursive
 * or nested platform locks. Mutexes provide priority inheritance and task waits.
 * Callers may not delete a task that owns a lock or reset this object in use. */
int a733_platform_locks_init(struct a733_platform_locks *, int (*now)(void *, uint64_t *), void *);
/* Returns 1 only if admitted before the absolute deadline. Waiting uses finite
 * tick intervals; dispatch can occur after the deadline, but late ownership is
 * returned without permitting I/O. A lock retained by a failed transaction
 * remains owned; this API has no forced unlock or recovery path. */
int a733_platform_lock_enter(struct a733_platform_locks *, enum a733_platform_lock, uint64_t);
void a733_platform_lock_leave(struct a733_platform_locks *, enum a733_platform_lock);
/* Mark uncertain external state while still owning the mutex. Other clients
 * subsequently refuse admission. Recovery requires an independent reset. */
int a733_platform_lock_retain(struct a733_platform_locks *, enum a733_platform_lock);
#endif
