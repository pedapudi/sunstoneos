#ifndef SUNSTONE_A733_PINS_NATIVE_H
#define SUNSTONE_A733_PINS_NATIVE_H
#include "a733_pins.h"

struct a733_pins_locks {
    int (*enter)(void *, uint64_t deadline);
    void (*leave)(void *);
    int (*now)(void *, uint64_t *microseconds);
    void *cookie;
};
struct a733_pins_native {
    struct a733_pins_operations operations;
    struct a733_pins_locks locks;
    unsigned initialized;
};
/* A pure allowlist shared with the focused host check. Configuration writes
 * exclude data, IRQ, voltage, clock, and other banks' registers. */
static inline int a733_pins_native_address(uintptr_t address, unsigned writing)
{
    switch (address) {
    case 0x02000100: case 0x02000104: case 0x02000120:
    case 0x02000124: case 0x02000130: return 1;
    case 0x02000110: case 0x02000150: return !writing;
    default: return 0;
    }
}
/* Initialization has no hardware accesses. Subsequent callbacks require
 * privileged AArch64 execution, Device-mapped identity addresses, prepared PIO
 * clocks, stable bank voltage, and independent watchdog recovery. MMIO can stall
 * or fault; a C return value cannot recover that condition. Callbacks belong to
 * one shared manager and are not a separate public route around its leases.
 * Supply a scheduling-aware shared lock; no spinlock or FreeRTOS task is added.
 * Pass &native.operations and &native to a733_pins_init(). */
int a733_pins_native_init(struct a733_pins_native *, const struct a733_pins_locks *);
#endif
