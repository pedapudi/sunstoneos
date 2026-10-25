#ifndef SUNSTONE_A733_EL3_SECURITY_H
#define SUNSTONE_A733_EL3_SECURITY_H
/* The cold-boot security setup of the vendor's EL3 firmware, which both EL3
 * monitors make as their last step before entering EL1
 * (docs/designs/gpu-vendor-default.md). The writes are BL31's
 * sunxi_security_setup (monitor.fex, SHA-256 8af16b86...5b2632, loaded at
 * 0x48000000), cold-boot writes 9 to 17 of docs/vendor-firmware/
 * tf-a-monitor.md, in BL31's order, with their file offsets:
 *  9 SPC ports 0-13 set registers, 0x02054004 + 0x10 n = 0xffffffff (0xc3cc);
 * 10 SPC ports 16-23 set registers, 0x02054104 + 0x10 n = 0xffffffff (0xc3e0);
 *    both read 0 on the board before and after the write;
 * 11 S_TZMA 0x07003000, +0x4, +0x8 = 0, SRAM areas non-secure (0xc408);
 * 12 S_SPC 0x07002004, +0x14, +0x24 = 0xffffffff, always-on peripherals
 *    non-secure (0xc41c);
 * 13 PRCM_SEC_SWITCH_REG 0x07010290 |= 7: CPUS bus clock, PRCM PLL and power
 *    registers non-secure (0xc438; manual 4.2.5.26);
 * 14 CCMU_SEC_SWITCH_REG 0x02003f00 = 7: PLL, bus and MBUS clock registers
 *    non-secure (0xc448; manual 4.1.6.278);
 * 15 DMAC0 DMA_SEC_REG 0x04601020 = 0xffff, all DMA0 channels non-secure
 *    (0xc458);
 * 16 DMAC1 DMA_SEC_REG 0x04024020 = 0xffff, all DMA1 channels non-secure
 *    (0xc460);
 * 17 SPC port 14 set register 0x020540e0 = 0xfffffbff, bit 10 left secure;
 *    port 15 is never written (0xc470).
 * BL31 makes them on every cold boot and every resume, whatever device the
 * OS uses. Writes 15 and 16 are made only when the controller's bus gate is
 * open and its reset released (CCU 0x704 and 0x70c, bits 0 and 16): a
 * FEL-booted lifetime starts with both controllers gated and in reset, a
 * write there is not known to complete, and a reset discards it; the
 * channels then hold their reset value, which the manual gives as
 * non-secure (13.1.7.1). BL31's TIMER0 writes (cold-boot writes 18 to 20)
 * are not security writes and are left out. The writes only open devices to
 * non-secure software and are not restored before a ROM return; a power
 * removal resets them. The QEMU machine has none of these blocks, so
 * MONITOR_QEMU builds make no write. Requires boot.h. */
#include <stdint.h>

#ifndef MONITOR_QEMU
static inline void a733_el3_security_setup(void)
{
    /* EL3 runs with the MMU off, so every access is Device-nGnRnE and in
     * program order; one DSB SY completes them all before EL1 starts. */
    for (unsigned n = 0; n < 14; ++n) mmio_write(0x02054004u + 0x10u * n, 0xffffffffu);
    for (unsigned n = 16; n < 24; ++n) mmio_write(0x02054004u + 0x10u * n, 0xffffffffu);
    for (unsigned n = 0; n < 3; ++n) mmio_write(0x07003000u + 4u * n, 0);
    for (unsigned n = 0; n < 3; ++n) mmio_write(0x07002004u + 0x10u * n, 0xffffffffu);
    mmio_write(0x07010290u, mmio_read(0x07010290u) | 7u);
    mmio_write(0x02003f00u, 7u);
    if ((mmio_read(0x02002704u) & 0x10001u) == 0x10001u) mmio_write(0x04601020u, 0xffffu);
    if ((mmio_read(0x0200270cu) & 0x10001u) == 0x10001u) mmio_write(0x04024020u, 0xffffu);
    mmio_write(0x020540e0u, 0xfffffbffu);
    __asm__ volatile("dsb sy" ::: "memory");
}
#else
static inline void a733_el3_security_setup(void) {}
#endif
#endif
