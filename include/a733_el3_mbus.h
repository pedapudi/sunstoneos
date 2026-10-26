#ifndef SUNSTONE_A733_EL3_MBUS_H
#define SUNSTONE_A733_EL3_MBUS_H
/* MBUS back to boot0's 600 MHz at every EL3 entry (docs/designs/mbus-restore.md).
 *
 * boot0's DRAM clock routine leaves MBUS_CLK_REG (CCU +0x588) at 0x81000000:
 * gate on, source 1 (PERI0_600M), divider 1 (boot0 829ea8e3 @0x57a4c, the
 * write of 0x89000000 into the cleared source field, @0x57b1e-0x57b28). Every return
 * to the boot ROM runs the ROM's secondary clock setup again: the forced-FEL
 * entry passes 127 to the FEL dispatcher (a8e31821 @0x224-0x22c), which then
 * calls 0x2444 (@0x2534) and through 0x2160 the MBUS routine at 0x2084. That
 * routine leaves 0x84000000, source 4 (PERI0_400M), 400 MHz, so every image
 * after a ROM return starts with MBUS at two thirds of boot0's rate.
 *
 * The restore repeats the ROM's own sequence, which changes MBUS while DRAM
 * holds live contents on every ROM return, with boot0's source:
 *   1. gate off with the update bit, wait for the update bit to clear;
 *   2. assert MSI_LITE0's MBUS reset (CCU +0x594 bit 17);
 *   3. 10 us; divider 1 with the update bit; 10 us; wait;
 *   4. source 1 with the update bit; 10 us; wait;
 *   5. release MSI_LITE0's MBUS reset;
 *   6. gate on with the update bit; wait;
 * then reads back MBUS and MSI_LITE0. Each wait polls the update bit (manual
 * 4.1.6.76: MBUS_UPD auto-clears once the new configuration applies) for at
 * most 10 ms, 240,000 counter ticks, the ROM's bound (@0x2158). The divider
 * is written before the source, as the manual's rule for raising a bus clock
 * requires (4.1.4.3). The CPU is not an MBUS master (manual 4.1.6.83 lists
 * the MSI_LITE bridges, STORE, SERDES, VID_IN, NPU, GPU, VE, DE and IOMMU),
 * so EL3 code executing from DRAM keeps running while the gate is off; the
 * callers run before any driver starts an MBUS master.
 *
 * The restore changes only the ROM's exact value 0x84000000 with MSI_LITE0
 * gate and both resets released. MBUS already at 0x81000000 needs nothing;
 * any other value is left alone and reported as unchanged, so a lifetime
 * without boot0 or a later revision of the ROM keeps its inherited state and
 * the drivers' fabric checks decide. Returns 0 when MBUS reads boot0's value
 * or was left unchanged, the step (1-6) whose update wait failed, or 7 when
 * the final read-back differs.
 * A failure after step 1 re-enables the gate and releases MSI_LITE0's reset
 * before returning, so the next ROM return finds the ROM's starting state. */
#include <stdint.h>

#define A733_MBUS_CLK UINT32_C(0x02002588)
#define A733_MSI_LITE0_BGR UINT32_C(0x02002594)
#define A733_MBUS_GATE (UINT32_C(1) << 31)
#define A733_MBUS_UPDATE (UINT32_C(1) << 27)
#define A733_MBUS_SOURCE (UINT32_C(7) << 24)
#define A733_MBUS_DIVIDER UINT32_C(0x1f)
/* Gate, DFS enable (28), update, source and divider. */
#define A733_MBUS_FIELDS UINT32_C(0x9f00001f)
#define A733_MBUS_BOOT0 UINT32_C(0x81000000)
#define A733_MBUS_ROM UINT32_C(0x84000000)
#define A733_MSI_LITE0_MBUS_RESET (UINT32_C(1) << 17)
#define A733_MSI_LITE0_RELEASED UINT32_C(0x00030001)
#define A733_MBUS_SETTLE_TICKS 240u      /* 10 us at 24 MHz */
#define A733_MBUS_UPDATE_TICKS 240000u   /* 10 ms at 24 MHz */

/* A host model defines A733_EL3_MBUS_ACCESS and these three functions. */
#ifndef A733_EL3_MBUS_ACCESS
static inline uint32_t a733_el3_mbus_read(uintptr_t address)
{ return *(volatile uint32_t *)address; }
static inline void a733_el3_mbus_write(uintptr_t address, uint32_t value)
{ *(volatile uint32_t *)address = value; __asm__ volatile("dsb sy" ::: "memory"); }
static inline uint64_t a733_el3_mbus_ticks(void)
{ uint64_t v; __asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(v) :: "memory"); return v; }
#endif

#ifndef MONITOR_QEMU
static inline void a733_el3_mbus_settle(void)
{
    uint64_t start = a733_el3_mbus_ticks();
    while (a733_el3_mbus_ticks() - start < A733_MBUS_SETTLE_TICKS) {}
}
/* A read that completes before the deadline and shows the update bit clear
 * passes; the value read is kept for the next step. */
static inline int a733_el3_mbus_applied(uint32_t *value)
{
    uint64_t start = a733_el3_mbus_ticks();
    for (;;) {
        uint32_t v = a733_el3_mbus_read(A733_MBUS_CLK);
        uint64_t now = a733_el3_mbus_ticks();
        if (!(v & A733_MBUS_UPDATE)) { *value = v; return 1; }
        if (now - start >= A733_MBUS_UPDATE_TICKS) return 0;
    }
}
static inline unsigned a733_el3_mbus_restore(void)
{
    uint32_t mbus = a733_el3_mbus_read(A733_MBUS_CLK);
    if ((mbus & A733_MBUS_FIELDS) == A733_MBUS_BOOT0) return 0;
    uint32_t msi = a733_el3_mbus_read(A733_MSI_LITE0_BGR);
    if ((mbus & A733_MBUS_FIELDS) != A733_MBUS_ROM ||
        (msi & A733_MSI_LITE0_RELEASED) != A733_MSI_LITE0_RELEASED) return 0;
    unsigned step = 1;
    a733_el3_mbus_write(A733_MBUS_CLK, (mbus & ~A733_MBUS_GATE) | A733_MBUS_UPDATE);
    if (!a733_el3_mbus_applied(&mbus)) goto failed;
    step = 2;
    a733_el3_mbus_write(A733_MSI_LITE0_BGR, msi & ~A733_MSI_LITE0_MBUS_RESET);
    a733_el3_mbus_settle();
    step = 3;
    a733_el3_mbus_write(A733_MBUS_CLK, (mbus & ~A733_MBUS_DIVIDER) | A733_MBUS_UPDATE);
    a733_el3_mbus_settle();
    if (!a733_el3_mbus_applied(&mbus)) goto failed;
    step = 4;
    a733_el3_mbus_write(A733_MBUS_CLK, (mbus & ~A733_MBUS_SOURCE) | (UINT32_C(1) << 24) | A733_MBUS_UPDATE);
    a733_el3_mbus_settle();
    if (!a733_el3_mbus_applied(&mbus)) goto failed;
    step = 5;
    a733_el3_mbus_write(A733_MSI_LITE0_BGR, msi);
    step = 6;
    a733_el3_mbus_write(A733_MBUS_CLK, mbus | A733_MBUS_GATE | A733_MBUS_UPDATE);
    if (!a733_el3_mbus_applied(&mbus)) goto failed;
    if ((mbus & A733_MBUS_FIELDS) != A733_MBUS_BOOT0 ||
        (a733_el3_mbus_read(A733_MSI_LITE0_BGR) & A733_MSI_LITE0_RELEASED) != A733_MSI_LITE0_RELEASED) return 7;
    return 0;
failed:
    /* Best effort back to a running bus: release the bridge, gate on. */
    a733_el3_mbus_write(A733_MSI_LITE0_BGR, msi);
    a733_el3_mbus_write(A733_MBUS_CLK, a733_el3_mbus_read(A733_MBUS_CLK) | A733_MBUS_GATE | A733_MBUS_UPDATE);
    return step;
}
#else
static inline unsigned a733_el3_mbus_restore(void) { return 0; }
#endif
#endif
