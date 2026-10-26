/* Remote-writer exclusion for the cold boot path; docs/remote-writers.md.
 * Every decision comes from one read through the caller's callback. A master
 * counts as excluded only when manual-documented state shows it held in reset,
 * its bus clock gated, or its DMA engine disabled and idle. The E902
 * configuration block is never read, because its bus clock may be gated. */
#include "a733_remote_writers.h"
#include <stddef.h>

#define CCU UINT64_C(0x02002000)
#define PRCM UINT64_C(0x07010000)
#define RISCV_24M_CLK (PRCM + 0x210)       /* 4.2.5.23: bit 31 clock on. */
#define RISCV_BGR (PRCM + 0x21c)           /* 4.2.5.24: bus gates 1:0, CFG reset 16. */
#define RISCV_STARTED UINT32_C(0x10003)
#define CPUIDLE_E902 UINT64_C(0x07050008)  /* Bit 0 set by the shipped start sequence. */
#define MBUS_GATE_EN (CCU + 0x5e4)         /* 4.1.6.84: bit 9 passes the ISP MBUS clock. */
#define ISP_CLK (CCU + 0x1860)             /* 4.1.6.246: bit 31 clock on. */
#define BUS_GATE 1u                        /* Bit 0 of every BGR register below. */
#define DMA_STA 0x30u                      /* 13.1.7.3: bit 31 FIFO, 15:0 channel busy. */
#define DMA_EN(n) (0x100u + (n) * 0x40u)   /* 13.1.7.6: bit 0 channel enable. */
#define SMHC_DMAC 0x80u                    /* 11.3.8.25: bit 7 IDMAC enable. */
#define RST(bit) (UINT32_C(1) << (bit))

enum { RESET_ONLY, DMAC, SMHC };
/* Each listed reset bit reads 0 when asserted (manual 4.1.6, CCU BGR registers). */
static const struct master {
    uint8_t writer, kind;
    uint16_t bgr;
    uint32_t resets;
    uint64_t base;
} masters[] = {
    {A733_REMOTE_DMA0, DMAC, 0x704, RST(16), 0x04601000},
    {A733_REMOTE_DMA1, DMAC, 0x70c, RST(16), 0x04024000},
    {A733_REMOTE_CE, RESET_ONLY, 0xac4, RST(16) | RST(17), 0},
    {A733_REMOTE_SMHC0, SMHC, 0xd0c, RST(16), 0x04020000},
    {A733_REMOTE_SMHC1, SMHC, 0xd1c, RST(16), 0x04021000},
    {A733_REMOTE_SMHC2, SMHC, 0xd2c, RST(16), 0x04022000},
    {A733_REMOTE_SMHC3, SMHC, 0xd3c, RST(16), 0x04023000},
    {A733_REMOTE_NAND0, RESET_ONLY, 0xc8c, RST(16), 0},
    {A733_REMOTE_UFS, RESET_ONLY, 0xd8c, RST(16) | RST(17) | RST(18) | RST(19), 0},
    /* 4.1.6.167; 11.5.5.7: private descriptor DMA through an AHB master. */
    {A733_REMOTE_SPIF, RESET_ONLY, 0xf1c, RST(16), 0},
    {A733_REMOTE_USB0, RESET_ONLY, 0x1304, RST(16) | RST(20) | RST(24), 0},
    {A733_REMOTE_USB1, RESET_ONLY, 0x130c, RST(16) | RST(20), 0},
    {A733_REMOTE_USB2, RESET_ONLY, 0x135c, RST(16), 0},
    {A733_REMOTE_PCIE0, RESET_ONLY, 0x138c, RST(16) | RST(17), 0},
    {A733_REMOTE_GMAC0, RESET_ONLY, 0x141c, RST(16) | RST(17), 0},
    {A733_REMOTE_DE0, RESET_ONLY, 0xa04, RST(16), 0},
    {A733_REMOTE_DI, RESET_ONLY, 0xa24, RST(16), 0},
    {A733_REMOTE_G2D, RESET_ONLY, 0xa44, RST(16), 0},
    {A733_REMOTE_EINK, RESET_ONLY, 0xa6c, RST(16), 0},
    {A733_REMOTE_VE, RESET_ONLY, 0xa8c, RST(16) | RST(18), 0},
    {A733_REMOTE_NPU, RESET_ONLY, 0xb04, RST(16) | RST(17) | RST(18), 0},
    {A733_REMOTE_GPU0, RESET_ONLY, 0xb24, RST(16), 0},
    {A733_REMOTE_CSI, RESET_ONLY, 0x1844, RST(16), 0},
};

static int rd(const struct a733_remote_writers_ops *ops, uint64_t address, uint32_t *value)
{
    return ops->read32(ops->context, address, value) == 1;
}

/* Clock bits are read before the undocumented CPUIDLE bit; any start sign,
 * including one inferred from the shipped monitor, leaves the core unproven. */
static int e902_gated(const struct a733_remote_writers_ops *ops)
{
    uint32_t bgr, clk, idle;
    return rd(ops, RISCV_BGR, &bgr) && !(bgr & RISCV_STARTED) &&
        rd(ops, RISCV_24M_CLK, &clk) && !(clk & RST(31)) &&
        rd(ops, CPUIDLE_E902, &idle) && !(idle & 1u);
}

static int isp_gated(const struct a733_remote_writers_ops *ops)
{
    uint32_t clk, mbus;
    return rd(ops, ISP_CLK, &clk) && !(clk & RST(31)) &&
        rd(ops, MBUS_GATE_EN, &mbus) && !(mbus & RST(9));
}

/* The manual defines a disabled channel as idle; the status word then also
 * requires no busy channel and an empty MBUS FIFO. */
static int dmac_idle(const struct a733_remote_writers_ops *ops, uint64_t base)
{
    uint32_t v;
    for (unsigned n = 0; n < 16; ++n)
        if (!rd(ops, base + DMA_EN(n), &v) || (v & 1u)) return 0;
    return rd(ops, base + DMA_STA, &v) && !(v & UINT32_C(0x8000ffff));
}

uint32_t a733_remote_writers_excluded(const struct a733_remote_writers_ops *ops)
{
    uint32_t unproven = 0, v;
    if (!ops || !ops->read32) return A733_REMOTE_WRITERS_ALL;
    if (!e902_gated(ops)) unproven |= A733_REMOTE_WRITER_BIT(A733_REMOTE_E902);
    if (!isp_gated(ops)) unproven |= A733_REMOTE_WRITER_BIT(A733_REMOTE_ISP);
    for (size_t i = 0; i < sizeof masters / sizeof masters[0]; ++i) {
        const struct master *m = &masters[i];
        int excluded;
        if (!rd(ops, CCU + m->bgr, &v)) excluded = 0;
        else if (!(v & m->resets)) excluded = 1;
        /* Out of reset: only a readable, documented DMA disable counts. */
        else if (m->kind == RESET_ONLY || !(v & BUS_GATE)) excluded = 0;
        else if (m->kind == DMAC) excluded = dmac_idle(ops, m->base);
        else excluded = rd(ops, m->base + SMHC_DMAC, &v) && !(v & RST(7));
        if (!excluded) unproven |= A733_REMOTE_WRITER_BIT(m->writer);
    }
    return unproven;
}
