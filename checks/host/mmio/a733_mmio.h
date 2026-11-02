/* SPDX-License-Identifier: Apache-2.0 */
/* Host replacement of include/a733_mmio.h: the same accessor names, routed to the strict register model
 * (checks/host/strict_mmio.h). It is searched before include/ by checks/host_link.py, so driver code builds
 * unchanged and only its register layer differs. The barrier forms have no effect on the host; ordering is a
 * property of the board and is checked elsewhere. checks/host_link.py verifies that this header defines every
 * accessor the real header defines. */
#ifndef SUNSTONE_A733_MMIO_H
#define SUNSTONE_A733_MMIO_H
#include <stdint.h>
#include "strict_mmio.h"

#define HOST_MMIO_WIDTH(bits, type) \
static inline type a733_mmio_read##bits(uintptr_t address) { return (type)strict_mmio_read(address, bits / 8); } \
static inline void a733_mmio_write##bits(uintptr_t address, type value) { strict_mmio_write(address, bits / 8, value); } \
static inline type a733_mmio_read##bits##_dsb_sy_both(uintptr_t address) { return a733_mmio_read##bits(address); } \
static inline type a733_mmio_read##bits##_dsb_sy_after(uintptr_t address) { return a733_mmio_read##bits(address); } \
static inline void a733_mmio_write##bits##_dsb_sy_both(uintptr_t address, type value) { a733_mmio_write##bits(address, value); } \
static inline void a733_mmio_write##bits##_dsb_sy_after(uintptr_t address, type value) { a733_mmio_write##bits(address, value); }
HOST_MMIO_WIDTH(8, uint8_t)
HOST_MMIO_WIDTH(16, uint16_t)
HOST_MMIO_WIDTH(32, uint32_t)
HOST_MMIO_WIDTH(64, uint64_t)
#undef HOST_MMIO_WIDTH

static inline void a733_mmio_dsb_sy(void) {}
static inline uint32_t a733_mmio_read32_dmb_oshld_after(uintptr_t address) { return a733_mmio_read32(address); }
static inline void a733_mmio_write32_dmb_oshst_before(uintptr_t address, uint32_t value) { a733_mmio_write32(address, value); }
#endif
