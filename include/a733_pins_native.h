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
/* Only pin configuration, data aliases and IRQ words are admitted. Main PIO
 * DATA is read-only; L/M DATA writes require the manager's full latch shadow.
 * Voltage, secure attribution, clocks and reserved words are excluded. */
static inline int a733_pins_native_address(uintptr_t address, unsigned writing)
{
    if (address&3u) return 0;
    for (unsigned b=1;b<A733_PIN_BANKS;++b) {
        uint32_t valid=a733_pin_bank_mask(b);
        uintptr_t base=b<A733_PIN_L ? A733_PINS_BASE+0x80+b*0x80 : A733_PINS_R_BASE+(b-A733_PIN_L)*0x30;
        unsigned drive=b<A733_PIN_L ? 0x20 : 0x14;
        unsigned pull=b<A733_PIN_L ? 0x30 : 0x24;
        for (unsigned word=0;word<4;++word) if (valid&(UINT32_C(0xff)<<(word*8))) {
            if (address==base+word*4 || address==base+drive+word*4) return 1;
        }
        for (unsigned word=0;word<2;++word) if (valid&(UINT32_C(0xffff)<<(word*16)))
            if (address==base+pull+word*4) return 1;
        if (address==base+0x10) return !writing || b>=A733_PIN_L;
        if (b<A733_PIN_L && (address==base+0x14 || address==base+0x18)) return 1;
        uintptr_t irq=b<A733_PIN_L ? base+0x40 : A733_PINS_R_BASE+0x200+(b-A733_PIN_L)*0x20;
        for (unsigned word=0;word<4;++word) if (valid&(UINT32_C(0xff)<<(word*8)))
            if (address==irq+word*4) return 1;
        if (address==irq+0x10 || address==irq+0x14 || address==irq+0x18) return 1;
    }
    return 0;
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
