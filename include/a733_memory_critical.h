#ifndef SUNSTONE_A733_MEMORY_CRITICAL_H
#define SUNSTONE_A733_MEMORY_CRITICAL_H
/* The lock of the device-memory ledger and of the local pool preparations.
 *
 * A native image (AArch64, not Linux) uses the kernel critical section. In the
 * SMP port it masks interrupts on the calling core and takes the kernel's task
 * and ISR spinlocks, so it serializes every core; calls nest through the
 * kernel's per-core count. Before the scheduler starts it masks interrupts as
 * the kernel's own boot-time critical sections do. It asserts when entered
 * from an interrupt handler.
 *
 * Host checks run one thread and take no lock. A native fixture that runs one
 * thread without the kernel defines DEVICE_MEMORY_POOL_SINGLE_THREAD; any
 * other native translation unit without the kernel headers fails to compile,
 * so no image can link an unserialized ledger by accident. */
#if !defined(__aarch64__) || defined(__linux__) || defined(DEVICE_MEMORY_POOL_SINGLE_THREAD)
static inline void a733_memory_critical_enter(void) {}
static inline void a733_memory_critical_exit(void) {}
#elif defined(__has_include) && !__has_include(<FreeRTOS.h>)
#error "The native device-memory ledger needs FreeRTOS.h, or DEVICE_MEMORY_POOL_SINGLE_THREAD in a one-thread fixture"
#else
#include <FreeRTOS.h>
#include <task.h>
static inline void a733_memory_critical_enter(void) { taskENTER_CRITICAL(); }
static inline void a733_memory_critical_exit(void) { taskEXIT_CRITICAL(); }
#endif

/* The mapping check of one local pool [start, end) through
 * aarch64_device_translate(): one translation at the pool start and one at
 * each following multiple of a733_local_pool_stride below the end. The
 * stride is 2 MiB in images whose tables map image RAM with 2 MiB level-2
 * block descriptors, where one translation checks every page of a block, and
 * this weak default, one page, elsewhere (a733_device_memory_layout.h). */
#include "a733_mapping_check.h"
__attribute__((weak)) const uintptr_t a733_local_pool_stride = 4096;
static inline int a733_memory_local_check(uintptr_t start, uintptr_t end, const struct a733_mapping_rule *rule)
{
    uintptr_t step = a733_local_pool_stride;
    for (uintptr_t address = start; address < end; address = (address | (step - 1)) + 1)
        if (!a733_mapping_check(aarch64_device_translate, address, address, rule)) return 0;
    return start < end;
}

/* One preparation of a local-layout pool. `*result` is 0 before the first
 * call, 1 when the pool is ready or the image links none, and -1 after a
 * refusal; it never changes once set. A later call costs one acquire load.
 * `init` publishes the initialized pool with a release store of the pointer
 * that the pool's getter loads with acquire, so the getter makes one load.
 * The boot calls it once on core 0 before the scheduler starts, through
 * a733_memory_local_prepare(); fixtures may call it again.
 *
 * `check` reads only linker symbols and the translation tables, which do not
 * change once they are installed, so it runs outside the lock and concurrent
 * callers reach the same verdict. `init` writes the pool structure and runs
 * once, inside the critical section, which therefore holds no translation.
 * The release store of the pool pointer inside `init` publishes the pool to
 * its getter; the release store of `*result` orders only the verdict that
 * later calls of this function read. */
static inline int a733_memory_prepare_once(int *result, int (*check)(void), int (*init)(void))
{
    int r = __atomic_load_n(result, __ATOMIC_ACQUIRE);
    if (r) return r > 0;
    int ok = check();
    a733_memory_critical_enter();
    r = __atomic_load_n(result, __ATOMIC_RELAXED);
    if (!r) {
        r = ok && init() ? 1 : -1;
        __atomic_store_n(result, r, __ATOMIC_RELEASE);
    }
    a733_memory_critical_exit();
    return r > 0;
}
#endif
