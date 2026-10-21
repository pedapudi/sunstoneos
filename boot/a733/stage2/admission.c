/* Admission decisions of the stage-2 loader (docs/boot-slots.md). A separate
 * unit keeps each decision out of the loader's optimization, so the
 * instruction models can substitute modeled evidence at these calls. */
#include "sunxi_smhc.h"
#include <stdint.h>

int a733_stage2_card_admission(uint32_t module_hz);
int a733_stage2_card_present(void);
int a733_stage2_remote_writers_excluded(void);
int a733_stage2_watchdog_recovery(void);
int a733_stage2_entry_state(void);

/* Cold SD admission. boot0 has just read the monitor package through SDC0,
 * so the controller, its clock and its pins are in boot0's transfer state.
 * The checks compare that state with the pinned boot0 and the A733 manual:
 * - boot0 programs the SMHC0 clock and bus registers (resource setup at
 *   0x5d60c) and calls no storage exit before the monitor handoff (0x49bbc
 *   returns at once); manual 4.1.6.128 and 4.1.6.129;
 * - boot0's header configures PF0..PF5 as function 2 with pull-up and does
 *   not list PF6, which keeps its IO-disabled reset value; manual 18.14.6.68
 *   and PF_PUL0;
 * - PF is a 3.3 V bank, as the native SD platform requires; manual 18.14.6.1
 *   to 18.14.6.3;
 * - no command, transfer or internal DMA is in progress.
 * The cold path starts only this core: the cold-entry gate requires zero
 * U-Boot and OP-TEE pointers, and the monitor package holds no other image.
 * The card supply has no read-only observation, because the PMIC needs bus
 * transactions. Its rails served boot0's own read of this card, and boot0
 * runs without PMIC access (header power_mode 1).
 * Admission then makes two writes to registers that serve only SDC0: PF6
 * becomes an input with pull-up for card detection, and the module clock
 * moves from boot0's transfer rate to the identification rate that the SD
 * core assumes, SYS_CLK24M / (N=2 * M=30) = 400 kHz. The clock change follows
 * the native SD platform: gate off, change source and factors, gate on, each
 * read back. The card sees a pause and a slower clock on an idle bus. */
#define CCU_SMHC0_CLK UINT32_C(0x02002d00)
#define CCU_SMHC0_BGR UINT32_C(0x02002d0c)
#define GPIO_POW_MOD_SEL UINT32_C(0x02000040)
#define GPIO_POW_VAL UINT32_C(0x02000048)
#define GPIO_POW_VAL_SET_CTL UINT32_C(0x02000070)
#define PF_CFG0 UINT32_C(0x02000300)
#define PF_DAT UINT32_C(0x02000310)
#define PF_PUL0 UINT32_C(0x02000330)
#define SDC0 UINT32_C(0x04020000)
#define MODULE_MASK UINT32_C(0x87001f1f)
#define MODULE_GATE (UINT32_C(1) << 31)
#define IDENTIFY_MODULE UINT32_C(0x8000011d)
#define BUS_OPEN UINT32_C(0x00010001)      /* Reset deasserted, gate passing. */
#define SDC0_FUNCTIONS UINT32_C(0x00222222) /* PF0..PF5 function 2. */
#define SDC0_PULLS UINT32_C(0x00000555)     /* PF0..PF5 pull-up. */
#define PF6_FUNCTION (UINT32_C(15) << 24)
#define PF6_PULL (UINT32_C(3) << 12)
#define PF6_PULL_UP (UINT32_C(1) << 12)
#define SETTLE_TICKS 24000u /* 1 ms of the 24 MHz counter for the PF6 pull-up. */
#define SETTLE_SAMPLES (UINT32_C(1) << 24)

static int selected;

static uint32_t rd(uint32_t address)
{
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t value = *(volatile const uint32_t *)(uintptr_t)address;
    __asm__ volatile("dsb sy" ::: "memory");
    return value;
}
static void wr(uint32_t address, uint32_t value)
{
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)(uintptr_t)address = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
/* Writes the masked field only when it differs and requires the readback. */
static int update(uint32_t address, uint32_t mask, uint32_t bits)
{
    uint32_t old = rd(address), next = (old & ~mask) | bits;
    if (next != old) wr(address, next);
    return rd(address) == next;
}
static uint64_t counter(void)
{
    uint64_t n;
    __asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(n) :: "memory");
    return n;
}
static int bank_3v3(void)
{
    return ((rd(GPIO_POW_MOD_SEL) >> 10) & 3) <= 2 && ((rd(GPIO_POW_VAL) >> 10) & 3) == 2 &&
        (rd(GPIO_POW_VAL_SET_CTL) & 1);
}
/* SDC0 registers are read last, behind the checked gate and reset. */
static int inherited(void)
{
    uint32_t module = rd(CCU_SMHC0_CLK), function = rd(PF_CFG0) & 0x0fffffffu;
    uint32_t pull = rd(PF_PUL0) & 0x3fffu, pf6 = function & PF6_FUNCTION;
    return (rd(CCU_SMHC0_BGR) & BUS_OPEN) == BUS_OPEN &&
        (module & MODULE_GATE) && !(module & ~MODULE_MASK) && ((module >> 24) & 7) <= 4 &&
        (function & ~PF6_FUNCTION) == SDC0_FUNCTIONS && (pf6 == PF6_FUNCTION || !pf6) &&
        (pull & ~PF6_PULL) == SDC0_PULLS && (pull & PF6_PULL) <= PF6_PULL_UP && bank_3v3() &&
        !(rd(SDC0 + 0x18) & SUNXI_SMHC_CMD_START) && !(rd(SDC0 + 0x3c) & SUNXI_SMHC_STATUS_BUSY) &&
        !(rd(SDC0 + 0x80) & (UINT32_C(1) << 7));
}
/* Before every poll: the selected clock, bus, pins and bank voltage hold. */
static int retained(void)
{
    return selected && (rd(CCU_SMHC0_BGR) & BUS_OPEN) == BUS_OPEN &&
        rd(CCU_SMHC0_CLK) == IDENTIFY_MODULE && (rd(PF_CFG0) & 0x0fffffffu) == SDC0_FUNCTIONS &&
        (rd(PF_PUL0) & 0x3fffu) == (SDC0_PULLS | PF6_PULL_UP) && bank_3v3();
}

/* The SD core calls this once, before its first controller access. */
int a733_stage2_card_admission(uint32_t module_hz)
{
    if (module_hz != 400000 || selected || !inherited()) return 0;
    /* PF6 takes its pull-up before it becomes an input, as in the pin manager. */
    if (!update(PF_PUL0, PF6_PULL, PF6_PULL_UP) || !update(PF_CFG0, PF6_FUNCTION, 0)) return 0;
    uint32_t gated = rd(CCU_SMHC0_CLK) & MODULE_MASK & ~MODULE_GATE;
    if (!update(CCU_SMHC0_CLK, MODULE_MASK, gated) ||
        !update(CCU_SMHC0_CLK, MODULE_MASK, IDENTIFY_MODULE & ~MODULE_GATE) ||
        !update(CCU_SMHC0_CLK, MODULE_MASK, IDENTIFY_MODULE)) return 0;
    uint64_t start = counter();
    for (uint32_t n = 0; counter() - start < SETTLE_TICKS; )
        if (++n == SETTLE_SAMPLES) return 0;
    selected = 1;
    return a733_stage2_card_present();
}

/* The loader's retained() callback asks this before every SD core poll.
 * Presence comes from the active-low PF6 card-detect pin (docs/mmc.md),
 * read under the configuration that admission selected. */
int a733_stage2_card_present(void)
{
    return retained() && !(rd(PF_DAT) & (UINT32_C(1) << 6));
}

/* Prerequisites for entering a verified image. The loader checks all of
 * them after loading and before the watchdog is armed or any launch
 * authority is built. Each one returns 1 only with its own evidence; none is
 * established, so entry is refused. */

/* No stop or drain procedure exists for the E902 core or other DMA-capable
 * agents, and the boot0 staging area at 0x42e00000 has no owner
 * (docs/cold-entry.md). */
int a733_stage2_remote_writers_excluded(void)
{
    return 0;
}
/* A watchdog expiry after a cold boot has not been shown to restart the
 * board into this monitor with both cards preserved. */
int a733_stage2_watchdog_recovery(void)
{
    return 0;
}
/* The CPU, GIC and timer state that boot0 leaves at EL3 has not been
 * compared with the reset state that FEL-launched images see. */
int a733_stage2_entry_state(void)
{
    return 0;
}
