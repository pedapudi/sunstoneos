#ifndef SUNSTONE_A733_EL3_MBUS_H
#define SUNSTONE_A733_EL3_MBUS_H
/* MBUS back to boot0's 600 MHz at every EL3 entry (docs/designs/mbus-restore.md).
 *
 * boot0's DRAM clock routine leaves MBUS_CLK_REG (CCU +0x588) at 0x81000000:
 * gate on, source 1 (PERI0_600M), divider 1 (boot0 829ea8e3 @0x57a4c, the
 * write of 0x89000000 into the cleared source field, @0x57b1e-0x57b28). Every
 * return to the boot ROM runs the ROM's secondary clock setup again: the
 * forced-FEL entry passes 127 to the FEL dispatcher (a8e31821 @0x224-0x22c),
 * which then calls 0x2444 (@0x2534) and through 0x2160 the MBUS routine at
 * 0x2084. That routine leaves 0x84000000, source 4 (PERI0_400M), 400 MHz.
 * The ROM's clock setup runs whether or not boot0 ran in the power cycle, so
 * a lifetime without boot0 also starts at 0x84000000.
 *
 * a733_el3_mbus_restore() changes only the ROM's exact value 0x84000000, with
 * MSI_LITE0's gate and both resets released; at 0x81000000 it does nothing,
 * and any other value is left alone for the drivers' fabric checks. Before
 * the change it requires the bus masters that a previous image could have
 * left running to be idle (manual 4.1.6.83 lists the MBUS masters; the CPU
 * reaches DRAM through the NSI):
 *   - GPU: module clock gate (CCU +0xb20 bit 31) and bus gate (+0xb24 bit 0)
 *     both off, so neither the GPU nor its firmware processor runs;
 *   - NPU: module clock gate (+0xb00 bit 31) and bus gate (+0xb04 bit 0) off;
 *   - DMA0: with its bus gate and reset released (+0x704 bits 0 and 16),
 *     DMA_STA_REG (0x04601030) reads 0: every channel idle and the MBUS FIFO
 *     empty (manual 13.1.7.3);
 *   - CE, which reaches DRAM through MSI_LITE0: with its bus gate and reset
 *     released (+0xac4 bits 0 and 16), no task load bit set in CE_TLR_NS or
 *     CE_TLR_S (0x04603010, 0x04603810; mask 0x8421, the bits the CE driver
 *     requires clear before a task, manual 19.1.7.5).
 * The ROM leaves CE clocked and out of reset, so CE is checked by its task
 * registers; the GPU and NPU have no documented idle register that is cheap
 * to read with their clocks off, so their clocks decide.
 *
 * The change itself runs from SRAM: a733_mbus_window (boot/a733/mbus-window.S)
 * is copied to A733_MBUS_SRAM and called there, because during its window MBUS
 * is gated and MSI_LITE0 held in reset, and an image's code, stack and data
 * are in DRAM. The routine uses no stack and touches only the two registers.
 * The SMP boots call the restore before they release any secondary core,
 * which would otherwise spin in DRAM.
 *
 * Returns 0 when MBUS reads boot0's value or was left unchanged; 1, 3, 4 or 6
 * when that step's update wait timed out (the routine has then released the
 * bridge and turned the gate on); 7 when the final read-back differs; 8 when
 * the GPU or NPU is clocked; 9 when DMA0 is busy; 10 when CE has a task
 * loaded. Codes 8 to 10 skip the restore with no write: MBUS stays at the
 * ROM's 400 MHz and the image continues, so an image that returned with a
 * master still clocked does not stop every later image of the lifetime; the
 * drivers' fabric checks then see 400 MHz. A733_MBUS_FAILED() is true only
 * for codes 1 to 7, which stop the image. */
#include <stdint.h>

#define A733_MBUS_CLK UINT32_C(0x02002588)
#define A733_MSI_LITE0_BGR UINT32_C(0x02002594)
#define A733_MBUS_GATE (UINT32_C(1) << 31)
#define A733_MBUS_UPDATE (UINT32_C(1) << 27)
/* Gate, DFS enable (28), update, source and divider. */
#define A733_MBUS_FIELDS UINT32_C(0x9f00001f)
#define A733_MBUS_BOOT0 UINT32_C(0x81000000)
#define A733_MBUS_ROM UINT32_C(0x84000000)
#define A733_MSI_LITE0_RELEASED UINT32_C(0x00030001)
#define A733_MBUS_FAILED(code) ((code) != 0u && (code) < 8u)
/* 4 KiB of SRAM A2 (0x00040000-0x00073fff) that no image or monitor uses:
 * the launchers occupy 0x52000-0x55fff, the FEL scratch 0x71000, the SRAM
 * survey 0x73000, the ROM's frame for an executed FEL call ends at 0x70000 and
 * its FEL stack lies in the shared SRAM (0xf0f00-0xf3400). The forced-FEL
 * return clears it with the rest of 0x44000-0xf3fff. checks/
 * sram-window-reservation.py fails when any SRAM layout overlaps the page. */
#define A733_MBUS_SRAM UINT32_C(0x0005f000)

/* boot/a733/mbus-window.S, assembled for AArch64; checks/el3-mbus.py
 * reassembles it and requires these words. */
static const uint32_t a733_mbus_window_code[] = {
    0xaa1e03efu, 0x2a0003ecu, 0xd284b109u, 0xf2a04009u, 0xd284b28au, 0xf2a0400au,
    0x5280002du, 0xb9400121u, 0x12007821u, 0x32050021u, 0xb9000121u, 0xd5033f9fu,
    0x94000032u, 0x350004e2u, 0x5280004du, 0x120e7981u, 0xb9000141u, 0xd5033f9fu,
    0x9400003bu, 0x5280006du, 0xb9400121u, 0x121b6821u, 0x32050021u, 0xb9000121u,
    0xd5033f9fu, 0x94000034u, 0x94000024u, 0x35000322u, 0x5280008du, 0xb9400121u,
    0x12057021u, 0x32080021u, 0x32050021u, 0xb9000121u, 0xd5033f9fu, 0x9400002au,
    0x9400001au, 0x350001e2u, 0x528000adu, 0xb900014cu, 0xd5033f9fu, 0x528000cdu,
    0xb9400121u, 0x32010021u, 0x32050021u, 0xb9000121u, 0xd5033f9fu, 0x9400000fu,
    0x35000082u, 0x52800000u, 0xaa0f03feu, 0xd65f03c0u, 0xb900014cu, 0xd5033f9fu,
    0xb9400121u, 0x32010021u, 0x32050021u, 0xb9000121u, 0xd5033f9fu, 0x2a0d03e0u,
    0xaa0f03feu, 0xd65f03c0u, 0xd5033fdfu, 0xd53be023u, 0xb9400125u, 0xd5033fdfu,
    0xd53be024u, 0x36d80105u, 0xcb030084u, 0xd2953006u, 0xf2a00066u, 0xeb06009fu,
    0x54ffff03u, 0x52800022u, 0xd65f03c0u, 0x52800002u, 0xd65f03c0u, 0xd5033fdfu,
    0xd53be023u, 0xd5033fdfu, 0xd53be024u, 0xcb030084u, 0xf103c09fu, 0x54ffff83u,
    0xd65f03c0u
};

/* A host model defines A733_EL3_MBUS_ACCESS and a733_el3_mbus_read,
 * a733_el3_mbus_write and a733_el3_mbus_window. */
#ifndef A733_EL3_MBUS_ACCESS
static inline uint32_t a733_el3_mbus_read(uintptr_t address)
{ return *(volatile uint32_t *)address; }
/* Copy the window to SRAM and call it there. EL3 runs with the MMU off, so
 * the copy reaches SRAM directly; the instruction cache is invalidated before
 * the branch in case SCTLR_EL3.I is set. */
static inline unsigned a733_el3_mbus_window(uint32_t msi)
{
    volatile uint32_t *sram = (volatile uint32_t *)(uintptr_t)A733_MBUS_SRAM;
    for (unsigned i = 0; i < sizeof(a733_mbus_window_code) / sizeof(a733_mbus_window_code[0]); ++i)
        sram[i] = a733_mbus_window_code[i];
    __asm__ volatile("dsb sy\n ic iallu\n dsb sy\n isb" ::: "memory");
    unsigned (*window)(uint32_t) = (unsigned (*)(uint32_t))(uintptr_t)A733_MBUS_SRAM;
    return window(msi);
}
#endif

#ifndef MONITOR_QEMU
static inline unsigned a733_el3_mbus_restore(void)
{
    uint32_t mbus = a733_el3_mbus_read(A733_MBUS_CLK);
    if ((mbus & A733_MBUS_FIELDS) == A733_MBUS_BOOT0) return 0;
    uint32_t msi = a733_el3_mbus_read(A733_MSI_LITE0_BGR);
    if ((mbus & A733_MBUS_FIELDS) != A733_MBUS_ROM ||
        (msi & A733_MSI_LITE0_RELEASED) != A733_MSI_LITE0_RELEASED) return 0;
    if ((a733_el3_mbus_read(0x02002b20u) & A733_MBUS_GATE) || (a733_el3_mbus_read(0x02002b24u) & 1u) ||
        (a733_el3_mbus_read(0x02002b00u) & A733_MBUS_GATE) || (a733_el3_mbus_read(0x02002b04u) & 1u)) return 8;
    if ((a733_el3_mbus_read(0x02002704u) & 0x10001u) == 0x10001u && a733_el3_mbus_read(0x04601030u)) return 9;
    if ((a733_el3_mbus_read(0x02002ac4u) & 0x10001u) == 0x10001u &&
        ((a733_el3_mbus_read(0x04603010u) | a733_el3_mbus_read(0x04603810u)) & 0x8421u)) return 10;
    unsigned step = a733_el3_mbus_window(msi);
    if (step) return step;
    if ((a733_el3_mbus_read(A733_MBUS_CLK) & A733_MBUS_FIELDS) != A733_MBUS_BOOT0 ||
        (a733_el3_mbus_read(A733_MSI_LITE0_BGR) & A733_MSI_LITE0_RELEASED) != A733_MSI_LITE0_RELEASED) return 7;
    return 0;
}
#else
static inline unsigned a733_el3_mbus_restore(void) { return 0; }
#endif
#endif
