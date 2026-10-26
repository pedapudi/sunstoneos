/* Admission decisions of the stage-2 loader (docs/boot-slots.md). A separate
 * unit keeps each decision out of the loader's optimization, so the
 * instruction models can substitute modeled evidence at these calls. */
#include "sunxi_smhc.h"
#include "a733_cpu_entry.h"
#include "a733_remote_writers.h"
#include "a733_clock_reference.h"
#include "a733_mmc_read.h"
#include <stdint.h>

int a733_stage2_card_admission(uint32_t module_hz);
int a733_stage2_card_present(void);
int a733_stage2_card_module(unsigned divisor);
int a733_stage2_card_gate(void);
int a733_stage2_time_valid(void);
int a733_stage2_remote_writers_excluded(void);
uint32_t a733_stage2_remote_writers_mask(void);
int a733_stage2_cpu_cache_owned(void);
int a733_stage2_reset_policy(void);
int a733_stage2_entry_state(void);

/* Cold SD admission. boot0 has just read the monitor package through SDC0,
 * so the controller, its clock and its pins are in boot0's transfer state.
 * The checks compare that state with the pinned boot0 and the A733 manual:
 * - boot0 programs the SMHC0 clock and bus registers (resource setup at
 *   0x5d60c); manual 4.1.6.128 and 4.1.6.129. No SDC0 teardown was found on
 *   boot0's handoff path (its call at 0x5e35c reaches a stub at 0x49bbc that
 *   returns 0). Admission does not rely on that observation: it reads the
 *   bus and clock state and refuses a closed bus or a gated clock;
 * - boot0's header configures PF0..PF5 as function 2 with pull-up and does
 *   not list PF6, which keeps its IO-disabled reset value; manual 18.14.6.68
 *   and PF_PUL0;
 * - PF is a 3.3 V bank, as the native SD platform requires; manual 18.14.6.1
 *   to 18.14.6.3;
 * - no command, transfer or internal DMA is in progress.
 * The package holds no other image and its U-Boot/OP-TEE pointers are zero.
 * That does not establish secondary-CPU exclusion or absence of dirty shared
 * cache lines. The separate CPU/cache and remote-writer admission decisions
 * must precede this controller admission.
 * The card supply has no read-only observation, because the PMIC needs bus
 * transactions. The only evidence for it is that its rails served boot0's
 * own read of this card. boot0 is configured for the AXP2202 and may program
 * PMIC rails: its header power_mode is 1, which the pinned sys_config.fex
 * reads as axp_id 1 (AXP2202) and U-Boot's older private_boot0.h reads as
 * no PMIC, and the binary contains PMU rail code. The card rail state after
 * boot0 is unverified and needs a board observation.
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
static uint32_t running_module;

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
/* Same normalized reference and PERI0_400M branch as the runtime provider.
 * Shared parents are read only throughout this cold-owner lifetime. */
static int parent(void)
{
    uint32_t required=(1u<<11)|(1u<<2)|(1u<<1);
    return a733_clock_ref24_valid(rd(0x07090160),rd(0x02002000)) &&
        a733_clock_peri0_valid(rd(0x020020a0)) && !(rd(0x020020a8)&(1u<<31)) &&
        !(rd(0x020020ac)&(1u<<27)) &&
        (rd(0x02003908)&(required|(required<<16)))==(required|(required<<16)) &&
        (rd(0x02003988)&(required<<16))==(required<<16);
}
static int masked(void)
{
    return !(rd(0x03400000)&(1u<<31)) && !(rd(0x03400118)&2u) &&
        !(rd(0x03400318)&2u) && !(rd(0x03400218)&2u);
}
/* SDC0 registers are read last, behind the checked gate and reset. */
static int inherited(void)
{
    uint32_t module = rd(CCU_SMHC0_CLK), function = rd(PF_CFG0) & 0x0fffffffu;
    uint32_t pull = rd(PF_PUL0) & 0x3fffu, pf6 = function & PF6_FUNCTION;
    return parent() && masked() && (rd(CCU_SMHC0_BGR) & BUS_OPEN) == BUS_OPEN &&
        (module & MODULE_GATE) && !(module & ~MODULE_MASK) && ((module >> 24) & 7) <= 4 &&
        (function & ~PF6_FUNCTION) == SDC0_FUNCTIONS && (pf6 == PF6_FUNCTION || !pf6) &&
        (pull & ~PF6_PULL) == SDC0_PULLS && (pull & PF6_PULL) <= PF6_PULL_UP && bank_3v3() &&
        !(rd(SDC0 + 0x18) & SUNXI_SMHC_CMD_START) && !(rd(SDC0 + 0x3c) & SUNXI_SMHC_STATUS_BUSY) &&
        !(rd(SDC0 + 0x80) & (UINT32_C(1) << 7));
}
/* Before every poll: the selected clock, bus, pins and bank voltage hold. */
static int retained(void)
{
    return selected && a733_stage2_time_valid() && parent() && masked() && (rd(CCU_SMHC0_BGR) & BUS_OPEN) == BUS_OPEN &&
        rd(CCU_SMHC0_CLK) == running_module && (rd(PF_CFG0) & 0x0fffffffu) == SDC0_FUNCTIONS &&
        (rd(PF_PUL0) & 0x3fffu) == (SDC0_PULLS | PF6_PULL_UP) && bank_3v3();
}

/* The SD core calls this once, before its first controller access. */
int a733_stage2_card_admission(uint32_t module_hz)
{
    if (module_hz != 400000 || selected || !a733_stage2_time_valid() || !inherited()) return 0;
    /* PF6 takes its pull-up before it becomes an input, as in the pin manager. */
    if (!update(PF_PUL0, PF6_PULL, PF6_PULL_UP) || !update(PF_CFG0, PF6_FUNCTION, 0)) return 0;
    uint32_t gated = rd(CCU_SMHC0_CLK) & MODULE_MASK & ~MODULE_GATE;
    if (!update(CCU_SMHC0_CLK, MODULE_MASK, gated) ||
        !update(CCU_SMHC0_CLK, MODULE_MASK, IDENTIFY_MODULE & ~MODULE_GATE) ||
        !update(CCU_SMHC0_CLK, MODULE_MASK, IDENTIFY_MODULE)) return 0;
    uint64_t start = counter();
    for (uint32_t n = 0; counter() - start < SETTLE_TICKS; )
        if (++n == SETTLE_SAMPLES || !a733_stage2_time_valid()) return 0;
    running_module=IDENTIFY_MODULE;
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

/* The existing core has stopped CLKCR and completed UPDATE before this call.
 * Only SDC0's module gate/source/divider changes; the card remains at 3.3 V. */
int a733_stage2_card_module(unsigned divisor)
{
    uint32_t next;
    if (!retained() || !a733_mmc_module_setting(SUNXI_MMC_SD_HIGH_SPEED4,divisor,&next) ||
        rd(SDC0+SUNXI_SMHC_CLKCR) || (rd(SDC0+SUNXI_SMHC_CMD)&SUNXI_SMHC_CMD_START) ||
        (rd(SDC0+SUNXI_SMHC_STATUS)&SUNXI_SMHC_STATUS_BUSY)) return 0;
    if (!update(CCU_SMHC0_CLK,MODULE_MASK,running_module&~MODULE_GATE) ||
        !a733_stage2_time_valid() || !update(CCU_SMHC0_CLK,MODULE_MASK,next&~MODULE_GATE) ||
        !a733_stage2_time_valid() || !update(CCU_SMHC0_CLK,MODULE_MASK,next)) return 0;
    running_module=next;
    return retained();
}
/* Called only after the loader proved card clock off and controller idle. */
int a733_stage2_card_gate(void)
{
    if (!retained() || rd(SDC0+SUNXI_SMHC_CLKCR) ||
        (rd(SDC0+SUNXI_SMHC_CMD)&SUNXI_SMHC_CMD_START) ||
        (rd(SDC0+SUNXI_SMHC_STATUS)&SUNXI_SMHC_STATUS_BUSY) || rd(SDC0+SUNXI_SMHC_DMAC) ||
        !update(CCU_SMHC0_CLK,MODULE_MASK,running_module&~MODULE_GATE)) return 0;
    selected=0;
    return a733_stage2_time_valid();
}

/* Sample before the loader mutates SDC0 or copies any slot extent. The mask
 * is diagnostic state, not a grant to restart or stop another agent. CE
 * is admitted only with both resets asserted; reserved bit 1 is inapplicable
 * to the A733 device processor inventory (docs/remote-writers.md). */
static uint32_t remote_mask;
static int remote_sampled;
static int remote_read(void *unused, uint64_t address, uint32_t *value)
{
    (void)unused;
    if (!value || address > UINT32_MAX || (address & 3)) return 0;
    *value = rd((uint32_t)address);
    return 1;
}
int a733_stage2_remote_writers_excluded(void)
{
    const struct a733_remote_writers_ops ops = {remote_read, 0};
    remote_mask = a733_remote_writers_excluded(&ops);
    remote_sampled = 1;
    return remote_mask == 0;
}
uint32_t a733_stage2_remote_writers_mask(void)
{
    return remote_sampled ? remote_mask : A733_REMOTE_WRITERS_ALL;
}
/* The fixed resident contract selects ordinary card boot after system reset:
 * no FEL flag or CPU redirect and no prior watchdog owner. The loader repeats
 * these reads immediately before keyed arming. This admits a reset policy;
 * it does not establish physical watchdog expiry or successful reboot. The
 * BOOT input may select storage normally or FEL if the user holds it low. */
int a733_stage2_reset_policy(void)
{
    return !(rd(0x02050014) & 1) && !(rd(0x02050010) & 0x100) &&
        !rd(0x070501c0) && !rd(0x070501c4) && !rd(0x070501d4) && !rd(0x07090108);
}
/* Local entry conditions only. The cold entry already checked these before
 * any memory write; check them again at the branch boundary. The OS supplies
 * its own stack/vectors, disables timers and establishes its GIC state.
 * CNTFRQ can be zero after boot0: loader deadlines use the fixed 24 MHz
 * platform counter, checked for progress before SD access. OS entry writes
 * CNTFRQ=24000000. None of this proves remote exclusion or cache publication. */
int a733_stage2_entry_state(void)
{
    uint64_t el, mpidr, midr, sctlr, daif, frequency;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    if (el != 12) return 0;
    __asm__ volatile("mrs %0, mpidr_el1\n mrs %1, midr_el1\n mrs %2, sctlr_el3\n"
                     "mrs %3, daif\n mrs %4, cntfrq_el0"
                     : "=r"(mpidr), "=r"(midr), "=r"(sctlr), "=r"(daif), "=r"(frequency));
    return !(mpidr & ~UINT64_C(0xc1000000)) && (midr & UINT64_C(0xff0ffff0)) == UINT64_C(0x410fd050) &&
        !(sctlr & A733_ENTRY_SCTLR_REJECT) && (daif & 0x3c0) == 0x3c0 &&
        (!frequency || frequency == 24000000);
}
