#ifndef SUNSTONE_A733_NPU_NATIVE_H
#define SUNSTONE_A733_NPU_NATIVE_H
#include "a733_npu_memory.h"
#include "counter_clock.h"

struct a733_npu_native {
    struct a733_npu_memory memory;
    struct counter_clock clock;
    uintptr_t registers;
    uint64_t register_physical;
    void (*fatal)(void *);
    void *fatal_cookie;
    unsigned initialized, failed;
};

/* Privileged EL1 adapter, zero-initialize once. Initialization checks stage-1
 * mappings/cache geometry, builds offline NPU tables, and samples the physical
 * timer. It does not read device registers. The MMIO allocation is one 4 KiB
 * Device page; its CPU/physical ranges and this context cannot alias RAM buffers
 * or the table arena. General heap and firmware RAM are not valid reservations.
 *
 * Every call requires serialization, pinned mappings, exclusive resources, and
 * a fixed counter frequency shared by all calling CPUs. No stage-2 translation
 * or bus remapping is supported. Sample at least once per 2^55 counter ticks.
 * Native cache operations use 64-byte ownership and completion to PoC.
 *
 * The platform must establish power/clocks/reset ownership, masked NPU GIC IRQ,
 * and qualified independent watchdog recovery before physical register access.
 * Invalid MMIO, cache, or memory calls latch failure and retain all resources.
 * Time/barrier callback failure invokes fatal, which must not return. Delay is
 * limited to 1000 microseconds and 1,048,576 samples; a stalled timer is fatal.
 * A synchronous bus stall requires the independent watchdog. Commands still
 * require semantic validation. This adapter does not supply a FreeRTOS service. */
int a733_npu_native_init(struct a733_npu_native *, uintptr_t register_cpu,
                         uint64_t register_physical, void *arena, size_t capacity,
                         uint64_t arena_physical, const struct a733_npu_buffer *, size_t,
                         void (*fatal)(void *), void *cookie);
extern const struct a733_npu_ops a733_npu_native_operations;
#endif
