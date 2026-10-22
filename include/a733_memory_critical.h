#ifndef SUNSTONE_A733_MEMORY_CRITICAL_H
#define SUNSTONE_A733_MEMORY_CRITICAL_H
/* The lock of the device-memory ledger and of the local pool getters.
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
#endif
