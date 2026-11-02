/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_A733_MMIO_H
#define SUNSTONE_A733_MMIO_H
/* The virtual OS board's register accessors. The A733's peripherals do not
 * exist in QEMU, so every register access of the OS and its driver adapters
 * goes to the board's register model (platform/qemu-os/mmio.c), which
 * presents the registers, access widths and timing that the drivers use.
 * The barrier forms and everything else are the A733 header's, line for line:
 * only the eight plain accessors differ, and the check compares the two files
 * so that this copy cannot drift from include/a733_mmio.h. This directory
 * precedes include/ on the include path of the virtual board's image. */
#include <stdint.h>
uint64_t qemu_os_mmio_read(uintptr_t address, unsigned bytes);
void qemu_os_mmio_write(uintptr_t address, unsigned bytes, uint64_t value);


static inline uint8_t a733_mmio_read8(uintptr_t address)
{ return (uint8_t)qemu_os_mmio_read(address, 1); }
static inline uint16_t a733_mmio_read16(uintptr_t address)
{ return (uint16_t)qemu_os_mmio_read(address, 2); }
static inline uint32_t a733_mmio_read32(uintptr_t address)
{ return (uint32_t)qemu_os_mmio_read(address, 4); }
static inline uint64_t a733_mmio_read64(uintptr_t address)
{ return (uint64_t)qemu_os_mmio_read(address, 8); }
static inline void a733_mmio_write8(uintptr_t address, uint8_t value)
{ qemu_os_mmio_write(address, 1, value); }
static inline void a733_mmio_write16(uintptr_t address, uint16_t value)
{ qemu_os_mmio_write(address, 2, value); }
static inline void a733_mmio_write32(uintptr_t address, uint32_t value)
{ qemu_os_mmio_write(address, 4, value); }
static inline void a733_mmio_write64(uintptr_t address, uint64_t value)
{ qemu_os_mmio_write(address, 8, value); }

#if defined(__aarch64__) || defined(__arm__)
static inline void a733_mmio_dsb_sy(void)
{ __asm__ volatile("dsb sy" ::: "memory"); }

static inline uint8_t a733_mmio_read8_dsb_sy_both(uintptr_t address)
{ a733_mmio_dsb_sy(); uint8_t v = a733_mmio_read8(address); a733_mmio_dsb_sy(); return v; }
static inline uint16_t a733_mmio_read16_dsb_sy_both(uintptr_t address)
{ a733_mmio_dsb_sy(); uint16_t v = a733_mmio_read16(address); a733_mmio_dsb_sy(); return v; }
static inline uint32_t a733_mmio_read32_dsb_sy_both(uintptr_t address)
{ a733_mmio_dsb_sy(); uint32_t v = a733_mmio_read32(address); a733_mmio_dsb_sy(); return v; }
static inline uint64_t a733_mmio_read64_dsb_sy_both(uintptr_t address)
{ a733_mmio_dsb_sy(); uint64_t v = a733_mmio_read64(address); a733_mmio_dsb_sy(); return v; }
static inline void a733_mmio_write8_dsb_sy_both(uintptr_t address, uint8_t value)
{ a733_mmio_dsb_sy(); a733_mmio_write8(address, value); a733_mmio_dsb_sy(); }
static inline void a733_mmio_write16_dsb_sy_both(uintptr_t address, uint16_t value)
{ a733_mmio_dsb_sy(); a733_mmio_write16(address, value); a733_mmio_dsb_sy(); }
static inline void a733_mmio_write32_dsb_sy_both(uintptr_t address, uint32_t value)
{ a733_mmio_dsb_sy(); a733_mmio_write32(address, value); a733_mmio_dsb_sy(); }
static inline void a733_mmio_write64_dsb_sy_both(uintptr_t address, uint64_t value)
{ a733_mmio_dsb_sy(); a733_mmio_write64(address, value); a733_mmio_dsb_sy(); }

static inline uint8_t a733_mmio_read8_dsb_sy_after(uintptr_t address)
{ uint8_t v = a733_mmio_read8(address); a733_mmio_dsb_sy(); return v; }
static inline uint16_t a733_mmio_read16_dsb_sy_after(uintptr_t address)
{ uint16_t v = a733_mmio_read16(address); a733_mmio_dsb_sy(); return v; }
static inline uint32_t a733_mmio_read32_dsb_sy_after(uintptr_t address)
{ uint32_t v = a733_mmio_read32(address); a733_mmio_dsb_sy(); return v; }
static inline uint64_t a733_mmio_read64_dsb_sy_after(uintptr_t address)
{ uint64_t v = a733_mmio_read64(address); a733_mmio_dsb_sy(); return v; }
static inline void a733_mmio_write8_dsb_sy_after(uintptr_t address, uint8_t value)
{ a733_mmio_write8(address, value); a733_mmio_dsb_sy(); }
static inline void a733_mmio_write16_dsb_sy_after(uintptr_t address, uint16_t value)
{ a733_mmio_write16(address, value); a733_mmio_dsb_sy(); }
static inline void a733_mmio_write32_dsb_sy_after(uintptr_t address, uint32_t value)
{ a733_mmio_write32(address, value); a733_mmio_dsb_sy(); }
static inline void a733_mmio_write64_dsb_sy_after(uintptr_t address, uint64_t value)
{ a733_mmio_write64(address, value); a733_mmio_dsb_sy(); }
#if defined(__aarch64__)
/* The ordering of Linux readl and writel on arm64 (arch/arm64/include/asm/io.h):
 * a read followed by DMB OSHLD, so later normal-memory reads wait for it; a
 * DMB OSHST before a write, so earlier normal-memory writes reach the device
 * first. Device-nGnRnE accesses to one device stay in program order. A caller
 * that needs completion, such as before a doorbell or before reading memory
 * the device wrote, still issues a733_mmio_dsb_sy(). */
static inline uint32_t a733_mmio_read32_dmb_oshld_after(uintptr_t address)
{ uint32_t v = a733_mmio_read32(address); __asm__ volatile("dmb oshld" ::: "memory"); return v; }
static inline void a733_mmio_write32_dmb_oshst_before(uintptr_t address, uint32_t value)
{ __asm__ volatile("dmb oshst" ::: "memory"); a733_mmio_write32(address, value); }
#endif
#endif
#endif
