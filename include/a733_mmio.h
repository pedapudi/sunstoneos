/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_A733_MMIO_H
#define SUNSTONE_A733_MMIO_H
/* Memory-mapped register accessors with the barrier placement in the name.
 *
 * Plain accessors perform exactly one volatile access and nothing else: no
 * barrier and no compiler memory clobber. The compiler keeps volatile accesses
 * in program order relative to each other, but may move ordinary memory
 * accesses across them.
 *
 * The barrier forms use DSB SY with a compiler memory clobber, the strength
 * the native adapters use today:
 *   _dsb_sy_both   DSB SY; access; DSB SY
 *   _dsb_sy_after  access; DSB SY
 * a733_mmio_dsb_sy() issues the barrier alone, for sequences that fence several
 * plain accesses together. A weaker ordering needs its own named form and
 * board qualification: the _dmb_ forms below give readl/writel ordering. Address
 * allowlists and mapping checks stay with each caller. */
#include <stdint.h>

static inline uint8_t a733_mmio_read8(uintptr_t address)
{ return *(volatile const uint8_t *)address; }
static inline uint16_t a733_mmio_read16(uintptr_t address)
{ return *(volatile const uint16_t *)address; }
static inline uint32_t a733_mmio_read32(uintptr_t address)
{ return *(volatile const uint32_t *)address; }
static inline uint64_t a733_mmio_read64(uintptr_t address)
{ return *(volatile const uint64_t *)address; }
static inline void a733_mmio_write8(uintptr_t address, uint8_t value)
{ *(volatile uint8_t *)address = value; }
static inline void a733_mmio_write16(uintptr_t address, uint16_t value)
{ *(volatile uint16_t *)address = value; }
static inline void a733_mmio_write32(uintptr_t address, uint32_t value)
{ *(volatile uint32_t *)address = value; }
static inline void a733_mmio_write64(uintptr_t address, uint64_t value)
{ *(volatile uint64_t *)address = value; }

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
