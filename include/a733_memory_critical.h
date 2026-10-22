#ifndef SUNSTONE_A733_MEMORY_CRITICAL_H
#define SUNSTONE_A733_MEMORY_CRITICAL_H
/* The lock of the device-memory ledger and of the local pool getters.
 *
 * A native image (AArch64, not Linux, FreeRTOS headers on the include path)
 * uses the kernel critical section. In the SMP port it masks interrupts on the
 * calling core and takes the kernel's task and ISR spinlocks, so it serializes
 * every core; calls nest through the kernel's per-core count. Before the
 * scheduler starts it masks interrupts as the kernel's own boot-time critical
 * sections do. It asserts when entered from an interrupt handler.
 *
 * Host checks and freestanding fixtures without FreeRTOS headers run one
 * thread and take no lock. A host check that defines
 * DEVICE_MEMORY_POOL_TRACE supplies device_memory_trace() and observes each
 * entry and exit. */
#if defined(__aarch64__) && !defined(__linux__) && defined(__has_include)
#if __has_include(<FreeRTOS.h>)
#define A733_MEMORY_KERNEL_LOCK 1
#endif
#endif

#if defined(A733_MEMORY_KERNEL_LOCK)
#include <FreeRTOS.h>
#include <task.h>
static inline void a733_memory_critical_enter(void) { taskENTER_CRITICAL(); }
static inline void a733_memory_critical_exit(void) { taskEXIT_CRITICAL(); }
#elif defined(DEVICE_MEMORY_POOL_TRACE)
#include <stddef.h>
enum device_memory_trace_event { DEVICE_MEMORY_TRACE_ENTER, DEVICE_MEMORY_TRACE_EXIT,
                                 DEVICE_MEMORY_TRACE_ATTEMPT, DEVICE_MEMORY_TRACE_CLEAN,
                                 DEVICE_MEMORY_TRACE_ZERO };
void device_memory_trace(enum device_memory_trace_event, size_t);
static inline void a733_memory_critical_enter(void) { device_memory_trace(DEVICE_MEMORY_TRACE_ENTER, 0); }
static inline void a733_memory_critical_exit(void) { device_memory_trace(DEVICE_MEMORY_TRACE_EXIT, 0); }
#else
static inline void a733_memory_critical_enter(void) {}
static inline void a733_memory_critical_exit(void) {}
#endif
#endif
