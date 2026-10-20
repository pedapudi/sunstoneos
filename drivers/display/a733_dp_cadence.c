/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (C) 2024 Allwinner Electronics Co., Ltd. */
/* DP-only adaptation of pinned sunxi-cadence-combophy.c. MMIO preserves
 * 16-bit PHY accesses and32-bit top accesses. Every timeout fails permanently.
 * Native callbacks reject all later accesses after uncertainty. */
#include "a733_dp_internal.h"
#include <stdbool.h>
#include <string.h>
typedef uint32_t u32;
typedef uint16_t u16;
#define BIT(n) (1u<<(n))
#define GENMASK(h,l) ((UINT32_MAX>>(31-(h)))&(UINT32_MAX<<(l)))
#define MAX_LANE_CNT 4
#define MAX_LANE_NO 3
#define MAX_SWING 4
#define MAX_PRE 4
#define PHY_TYPE_USB3 4
#define COMBO_PHY_TYPE(mode,type) ((mode)&(1u<<(type)))
#define TYPEC_ORIENTATION_NONE 0
#define TYPEC_ORIENTATION_REVERSE 2
#define TYPEC_ORIENTATION_NORMAL 1
struct sunxi_cadence_phy {uintptr_t top_combo_reg;};
struct sunxi_cadence_combophy {
 struct a733_dp *dp;struct sunxi_cadence_phy *sunxi_cphy;
 uintptr_t top_reg,phy_reg;unsigned orientation,usb_dp_state,mode,link_rate,ssc_en;
 unsigned lane_remap[4],typec_remap[4],lane_invert[4];const char *name;
};
struct phy_configure_opts_dp {
 unsigned set_rate,set_lanes,set_voltages,link_rate,lanes,ssc,voltage[4],pre[4];
};
static uint32_t cr(struct sunxi_cadence_combophy *c,uintptr_t a,unsigned n)
{uint32_t v=0;(void)a733_dp_read(c->dp,a,n,&v);return v;}
static void cw(struct sunxi_cadence_combophy *c,uintptr_t a,unsigned n,uint32_t v)
{(void)a733_dp_write(c->dp,a,n,v);}
#define readl(a) cr(combo0,(uintptr_t)(a),4)
#define writel(v,a) cw(combo0,(uintptr_t)(a),4,(v))
#define phy_readl(a) readl(a)
#define phy_readw(a) cr(combo0,(uintptr_t)(a),2)
#define phy_writel(a,v) writel(v,a)
#define phy_writew(a,v) cw(combo0,(uintptr_t)(a),2,(v))
#define udelay(n) ((void)a733_dp_wait(combo0->dp,(n)))
#define msleep(n) udelay((n)*1000u)
#define pr_debug(...) ((void)0)
#define pr_err(...) ((void)0)
#define pr_warn(...) ((void)0)
#define EINVAL 22
#define phy_set_lbits(a,s,w,v) do {uintptr_t ca=(uintptr_t)(a);uint32_t cm=GENMASK((s)+(w)-1,s);uint32_t cv=readl(ca);writel((cv&~cm)|(((uint32_t)(v)<<(s))&cm),ca);}while(0)
/* Sub-System PCIE Bus Gating Reset Register */
#define SUBSYS_PCIE_BGR				0x0004
#define SUBSYS_PCIE_GATING			(BIT(16) | BIT(17) | BIT(18))
#define SUBSYS_DISABLE_COMBO1_AUTOGATING	BIT(29)
#define SUBSYS_COMB1_PIPE			0xc44
#define SUBSYS_COMB1_PIPE_PCIE			0x1

/* Sub-System USB3.1 Bus Gating Reset Register */
#define SUBSYS_USB3P1_BGR			0x0008
#define   USB3P1_ONLY_UTMI_CLK_SEL		BIT(21)
#define   USB3P1_ONLY_PIPE_CLK_SEL		BIT(20)
#define   USB3P1_ACLK_EN			BIT(17)
#define   USB3P1_HCLK_EN			BIT(16)
#define   USB3P1_USB2P0_PHY_RSTN		BIT(4)

/* Sub-System DBG CTRL Register */
#define SUBSYS_DBG_CTL				0x00f0
#define   COMBO1_PHY_REFCLK_AUTO_GATE_DIS	BIT(29)
#define   COMBO0_PHY_REFCLK_AUTO_GATE_DIS	BIT(28)

/* COMB0 PHY PINS Link Control Register0 */
#define COMB0_PHY_PINS_LINK_CTRL0		0x0000
#define   PHY0_TYPEC_CONN_DIR			BIT(12) /* 0: normal orientation; 1: flipped orientation */
#define   PHY0_DP_LINK_RESET_N_SOFT		BIT(8)
#define   PHY0_PIPE_LINK_RESET_N_USB3P1_MASK	BIT(5)
#define   PHY0_PIPE_LINK_RESET_N_SOFT		BIT(4)
#define   PHY0_RESET_N				BIT(0)

/* COMB0 PHY PINS Link Control Register1 */
#define COMB0_PHY_PINS_LINK_CTRL1		0x0004
#define   PHY0_PMA_CMN_PLL1_REFCLK_SEL		BIT(24)
#define   PHY0_PMA_CMN_PLL0_REFCLK_SEL		BIT(20)
#define   PHY0_PMA_CMN_REFCLK_TERM_EN		BIT(16)
#define   PHY0_PMA_CMN_REFCLK_DIG_DIV_MASK	GENMASK(13, 12)
#define   PHY0_PMA_CMN_REFCLK_DIG_DIV(n)	(((n) & 0x3) << 12)
#define   PHY0_PMA_CMN_REFCLK_INT_MODE_MASK	GENMASK(9, 8)
#define   PHY0_PMA_CMN_REFCLK_INT_MODE(n)	(((n) & 0x3) << 8)
#define   PHY0_PMA_CMN_REFCLK_MODE_MASK		GENMASK(5, 4)
#define   PHY0_PMA_CMN_REFCLK_MODE(n)		(((n) & 0x3) << 4)
#define   PHY0_PMA_CMN_REFCLK_DIG_SEL		BIT(0)

/* COMB0 PHY PINS Link Control Register2 */
#define COMB0_PHY_PINS_LINK_CTRL2		0x0008
#define   PHY0_XCVR_BSCAN_MODE_EN_SEL		BIT(21)
#define   PHY0_XCVR_BSCAN_MODE_EN_SOFT		BIT(20)
#define   PHY0_BSCAN_EXT_SELECT			BIT(16)
#define   PHY0_DBG_RX_RD_LANE_SEL_MASK		GENMASK(6, 4)
#define   PHY0_DBG_RX_RD_LANE_SEL(n)		(((n) & 0x7) << 4)
#define   PHY0_IDDQ_EN_SEL			BIT(1)
#define   PHY0_IDDQ_EN_SOFT			BIT(0)

/* COMB0 PHY PINS Link Control Register3 */
#define COMB0_PHY_PINS_LINK_CTRL3		0x000C
#define   PHY0_PMA_TX_ELEC_IDLE_LN_3		BIT(15)
#define   PHY0_PMA_TX_ELEC_IDLE_LN_2		BIT(14)
#define   PHY0_PMA_TX_ELEC_IDLE_LN_1		BIT(13)
#define   PHY0_PMA_TX_ELEC_IDLE_LN_0		BIT(12)
#define   PHY0_PMA_XCVR_POWER_STATE_REQ_LN_MASK		GENMASK(9, 4)
#define   PHY0_PMA_XCVR_POWER_STATE_REQ_LN(n)	(((n) & 0x3f) << 4)
#define   PHY0_PMA_XCVR_PLLCLK_EN_LN		BIT(0)

/* COMB0 PHY PINS Link Status Register0 */
#define COMB0_PHY_PINS_LINK_STUS0		0x0900
#define   PHY0_LINK_POWER_STATE_ACK_MASK	GENMASK(13, 8)
#define   PHY0_LINK_POWER_STATE_ACK(n)		(((n) & 0x3f) << 8)
#define   PHY0_LINK_CLOCK_EN_ACK		BIT(4)
#define   PHY0_PMA_CMN_READY			BIT(0)

/* COMB1 PHY PINS Link Control Register0 */
#define COMB1_PHY_PINS_LINK_CTRL0		0x0000
#define   PHY1_LINK_CFG_LN_MASK			GENMASK(6, 4)
#define   PHY1_LINK_CFG_LN(n)			(((n) & 0x7) << 4)
#define   PHY1_RESET_N				BIT(0)

/* COMB1 PHY PINS Link Control Register1 */
#define COMB1_PHY_PINS_LINK_CTRL1		0x0004
#define   PHY1_EN_REFCLK			BIT(28)
#define   PHY1_PMA_CMN_PLL1_REFCLK_SEL		BIT(24)
#define   PHY1_PMA_CMN_PLL0_REFCLK_SEL		BIT(20)
#define   PHY1_PMA_CMN_REFCLK_TERM_EN		BIT(16)
#define   PHY1_PMA_CMN_REFCLK_DIG_DIV_MASK	GENMASK(13, 12)
#define   PHY1_PMA_CMN_REFCLK_DIG_DIV(n)	(((n) & 0x3) << 12)
#define   PHY1_PMA_CMN_REFCLK_INT_MODE_MASK	GENMASK(9, 8)
#define   PHY1_PMA_CMN_REFCLK_INT_MODE(n)	(((n) & 0x3) << 8)
#define   PHY1_PMA_CMN_REFCLK_MODE_MASK		GENMASK(5, 4)
#define   PHY1_PMA_CMN_REFCLK_MODE(n)		(((n) & 0x3) << 4)
#define   PHY1_PMA_CMN_REFCLK_DIG_SEL		BIT(0)

/* COMB1 PHY PINS Link Control Register3 */
#define COMB1_PHY_PINS_LINK_CTRL3		0x000C
#define   PHY1_XCVR_BSCAN_MODE_EN		BIT(20)
#define   PHY1_BSCAN_EXT_SELECT			BIT(16)
#define   PHY1_DTB_MUX_SELECTE_MASK		GENMASK(12, 8)
#define   PHY1_DTB_MUX_SELECTE(n)		(((n) & 0x1f) << 8)
#define   PHY1_IDDQ_EN_SEL			BIT(1)
#define   PHY1_IDDQ_EN				BIT(0)

/* COMB1 PHY PINS Lane0 Control Register1 */
#define COMB1_PHY_PINS_LANE0_CTRL0		0x0100
#define   PHY1_L00_ENT_L1_X			BIT(16)
#define   PHY1_L00_PCIE_L1_SS_SEL		BIT(12)
#define   PHY1_PMA_FULLRT_DIV_LN_0_MASK		GENMASK(9, 8)
#define   PHY1_PMA_FULLRT_DIV_LN_0(n)		(((n) & 0x3) << 8)
#define   PHY1_L00_MODE_MASK			GENMASK(5, 4)
#define   PHY1_L00_MODE(n)			(((n) & 0x3) << 4)
#define   PHY1_L00_RESET_N_HW_DIS		BIT(1)
#define   PHY1_L00_RESET_N_SW			BIT(0)

/* COMB1 PHY PINS Link Status Register0 */
#define COMB1_PHY_PINS_LINK_STUS0		0x0900
#define   PHY1_PMA_CMN_CLOCK_STOP_ACK		BIT(8)
#define   PHY1_PMA_CMN_EXT_REFCLK_DETECTED_VALID	BIT(5)
#define   PHY1_PMA_CMN_EXT_REFCLK_DETECTED	BIT(4)
#define   PHY1_PMA_CMN_READY			BIT(0)

/* COMBO0 DP RAWS TXDATA CONVERTION CTRL Register */
#define DP_RAWS_TXDW_CONV_CTRL			0x0004
#define DP_PHY_CFG_DATA_EN_BIT			BIT(0)
#define DP_PHY_CFG_LN_EN_MASK			GENMASK(7, 4)
#define DP_PHY_CFG_LN_RSTN_MASK			GENMASK(11, 8)

/* USB3.1 PIPE CLK MAP CTRL Register */
#define USB3P1_PIPE_CLK_MAP_CTRL		0x0100
#define   USB3P1_PIPE_CLK_MAP_MASK		GENMASK(3, 0)
#define   USB3P1_PIPE_CLK_MAP(n)		(((n) & 0xf)) /* 0x0: PHY0 Lane0 or Lane3; 0x1: PHY1 Lane0 */

/* USB3.1 PIPE RXDATA MAP CTRL Register */
#define USB3P1_PIPE_RXD_MAP_CTRL		0x0104
#define   USB3P1_PIPE_RXD_MAP_MASK		GENMASK(3, 0)
#define   USB3P1_PIPE_RXD_MAP(n)		(((n) & 0xf)) /* 0x0: PHY0 Lane0 or Lane2; 0x1: PHY1 Lane0 */

/* COMB1 PHY PIPE TXDATA MAP CTRL Register */
#define COMB1_PHY_PIPE_TXDATA_MAP_CTRL		0x0c44
#define   COMB1_PHY_LN0_PIPE_TXD_MAP_MASK	GENMASK(3, 0)
#define   COMB1_PHY_LN0_PIPE_TXD_MAP(n)		(((n) & 0xf)) /* 0x0: USB3.1 PIPE; 0x1: PCIE0 Lane0 */

/* SUBSYS COMB0 PHY Version Register */
#define COMB0_PHY_VER				0x1FE0
#define   PHY0_GEN_VER				GENMASK(31, 24)
#define   PHY0_SUB_VER				GENMASK(23, 16)
#define   PHY0_PRJ_VER				GENMASK(15, 8)

/* SUBSYS COMB1 PHY Version Register */
#define COMB1_PHY_VER				0x1FE4
#define   PHY1_GEN_VER				GENMASK(31, 24)
#define   PHY1_SUB_VER				GENMASK(23, 16)
#define   PHY1_PRJ_VER				GENMASK(15, 8)

/* SUBSYS TOP Version Register */
#define SUBSYS_TOP_VER				0x1FFC
#define   TOP_GEN_VER				GENMASK(31, 24)
#define   TOP_SUB_VER				GENMASK(23, 16)
#define   TOP_PRJ_VER				GENMASK(15, 8)

/* Registers */
#define SUBSYS_REG_PCIEBGR(top_subsys_reg)		((top_subsys_reg) \
								+ SUBSYS_PCIE_BGR)
#define SUBSYS_REG_USB3BGR(top_subsys_reg)		((top_subsys_reg) \
								+ SUBSYS_USB3P1_BGR)
#define SUBSYS_REG_DBGCTRL(top_subsys_reg)		((top_subsys_reg) \
								+ SUBSYS_DBG_CTL)

#define COMB0_REG_PHYPINS_LINK_CTRL0(top_reg)			((top_reg) \
									+ COMB0_PHY_PINS_LINK_CTRL0)

#define COMB0_REG_PHYPINS_LINK_CTRL1(top_reg)			((top_reg) \
									+ COMB0_PHY_PINS_LINK_CTRL1)

#define COMB0_REG_PHYPINS_LINK_CTRL2(top_reg)			((top_reg) \
									+ COMB0_PHY_PINS_LINK_CTRL2)

#define COMB0_REG_PHYPINS_LINK_CTRL3(top_reg)			((top_reg) \
									+ COMB0_PHY_PINS_LINK_CTRL3)

#define COMB0_REG_PHYPINS_LINK_STUS0(top_reg)			((top_reg) \
									+ COMB0_PHY_PINS_LINK_STUS0)

#define COMB1_REG_PHYPINS_LINK_CTRL0(top_reg)			((top_reg) \
									+ COMB1_PHY_PINS_LINK_CTRL0)

#define COMB1_REG_PHYPINS_LINK_CTRL1(top_reg)			((top_reg) \
									+ COMB1_PHY_PINS_LINK_CTRL1)

#define COMB1_REG_PHYPINS_LINK_CTRL3(top_reg)			((top_reg) \
									+ COMB1_PHY_PINS_LINK_CTRL3)

#define COMB1_REG_PHYPINS_LANE0_CTRL0(top_reg)			((top_reg) \
									+ COMB1_PHY_PINS_LANE0_CTRL0)

#define COMB1_REG_PHYPINS_LINK_STUS0(top_reg)			((top_reg) \
									+ COMB1_PHY_PINS_LINK_STUS0)

#define DP_RAWS_TXDW_CONV_CRTL(top_combo_reg)			((top_combo_reg) \
									+ DP_RAWS_TXDW_CONV_CTRL)

#define USB3P1_REG_PIPE_CLK_MAP_CTRL(top_combo_reg)		((top_combo_reg) \
									+ USB3P1_PIPE_CLK_MAP_CTRL)

#define USB3P1_REG_PIPE_RXD_MAP_CTRL(top_combo_reg)		((top_combo_reg) \
									+ USB3P1_PIPE_RXD_MAP_CTRL)

#define COMB1_REG_PHYPIPE_TXDATA_MAP_CTRL(top_combo_reg)	((top_combo_reg) \
									+ COMB1_PHY_PIPE_TXDATA_MAP_CTRL)

#define COMB0_REG_PHY_VER(top_combo_reg)			((top_combo_reg) \
									+ COMB0_PHY_VER)

#define COMB1_REG_PHY_VER(top_combo_reg)			((top_combo_reg) \
									+ COMB1_PHY_VER)

#define SUBSYS_REG_TOP_VER(top_combo_reg)			((top_combo_reg) \
									+ SUBSYS_TOP_VER)

struct dp_signal_level {
	u16 lane_txdrv[MAX_LANE_CNT];
	u16 lane_mgnfs[MAX_LANE_CNT];
	u16 lane_cpost[MAX_LANE_CNT];
};

#define DP_TXDRV(r0, r1, r2, r3) \
	.lane_txdrv[0] = r0, \
	.lane_txdrv[1] = r1, \
	.lane_txdrv[2] = r2, \
	.lane_txdrv[3] = r3,

#define DP_MGNFS(r0, r1, r2, r3) \
	.lane_mgnfs[0] = r0, \
	.lane_mgnfs[1] = r1, \
	.lane_mgnfs[2] = r2, \
	.lane_mgnfs[3] = r3,

#define DP_CPOST(r0, r1, r2, r3) \
	.lane_cpost[0] = r0, \
	.lane_cpost[1] = r1, \
	.lane_cpost[2] = r2, \
	.lane_cpost[3] = r3,


static struct dp_signal_level dp_lv_table_5g4[MAX_SWING][MAX_PRE] = {
	/* sw-0 */
	{
		{
			/* sw-0 pre-0 */
			DP_TXDRV(0x0003, 0x0003, 0x0003, 0x0003)
			DP_MGNFS(0x0025, 0x0024, 0x0024, 0x0025)
			DP_CPOST(0x0000, 0x0000, 0x0000, 0x0000)
		},
		{
			/* sw-0 pre-1 */
			DP_TXDRV(0x00A3, 0x00A3, 0x00A3, 0x00A3)
			DP_MGNFS(0x001E, 0x001D, 0x001D, 0x001E)
			DP_CPOST(0x000A, 0x000A, 0x000A, 0x000A)
		},
		{
			/* sw-0 pre-2 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x0011, 0x0010, 0x0010, 0x0011)
			DP_CPOST(0x001C, 0x001C, 0x001C, 0x001C)
		},
		{
			/* sw-0 pre-3 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x0001, 0x0000, 0x0000, 0x0001)
			DP_CPOST(0x0027, 0x0027, 0x0027, 0x0027)
		},
	},
	/* sw-1 */
	{
		{
			/* sw-1 pre-0 */
			DP_TXDRV(0x0002, 0x0002, 0x0002, 0x0002)
			DP_MGNFS(0x001D, 0x001C, 0x001C, 0x001D)
			DP_CPOST(0x0000, 0x0000, 0x0000, 0x0000)
		},
		{
			/* sw-1 pre-1 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x0012, 0x0011, 0x0011, 0x0012)
			DP_CPOST(0x000A, 0x000A, 0x000A, 0x000A)
		},
		{
			/* sw-1 pre-2 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x0001, 0x0000, 0x0000, 0x0001)
			DP_CPOST(0x001E, 0x001E, 0x001E, 0x001E)
		},
		{
			/* sw-1 pre-3 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x0001, 0x0000, 0x0000, 0x0001)
			DP_CPOST(0x001E, 0x001E, 0x001E, 0x001E)
		},
	},
	/* sw-2 */
	{
		{
			/* sw-2 pre-0 */
			DP_TXDRV(0x0002, 0x0002, 0x0002, 0x0002)
			DP_MGNFS(0x0010, 0x000F, 0x000F, 0x0010)
			DP_CPOST(0x0000, 0x0000, 0x0000, 0x0000)
		},
		{
			/* sw-2 pre-1 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x0000, 0x0000, 0x0000, 0x0000)
			DP_CPOST(0x000F, 0x000F, 0x000F, 0x000F)
		},
		{
			/* sw-2 pre-2 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x0000, 0x0000, 0x0000, 0x0000)
			DP_CPOST(0x000F, 0x000F, 0x000F, 0x000F)
		},
		{
			/* sw-2 pre-3 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x0000, 0x0000, 0x0000, 0x0000)
			DP_CPOST(0x000F, 0x000F, 0x000F, 0x000F)
		},
	},
	/* sw-3 */
	{
		{
			/* sw-3 pre-0 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x0000, 0x0000, 0x0000, 0x0000)
			DP_CPOST(0x0000, 0x0000, 0x0000, 0x0000)
		},
		{
			/* sw-3 pre-1 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x0000, 0x0000, 0x0000, 0x0000)
			DP_CPOST(0x0000, 0x0000, 0x0000, 0x0000)
		},
		{
			/* sw-3 pre-2 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x0000, 0x0000, 0x0000, 0x0000)
			DP_CPOST(0x0000, 0x0000, 0x0000, 0x0000)
		},
		{
			/* sw-3 pre-3 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x0000, 0x0000, 0x0000, 0x0000)
			DP_CPOST(0x0000, 0x0000, 0x0000, 0x0000)
		},
	},

};

static struct dp_signal_level dp_lv_table_2g7[MAX_SWING][MAX_PRE] = {
	/* sw-0 */
	{
		{
			/* sw-0 pre-0 */
			DP_TXDRV(0x0003, 0x0003, 0x0003, 0x0003)
			DP_MGNFS(0x0028, 0x0026, 0x0026, 0x0028)
			DP_CPOST(0x0000, 0x0000, 0x0000, 0x0000)
		},
		{
			/* sw-0 pre-1 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x0024, 0x0023, 0x0023, 0x0024)
			DP_CPOST(0x0008, 0x0008, 0x0008, 0x0008)
		},
		{
			/* sw-0 pre-2 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x001C, 0x001B, 0x001B, 0x001C)
			DP_CPOST(0x0016, 0x0016, 0x0016, 0x0016)
		},
		{
			/* sw-0 pre-3 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x000B, 0x000A, 0x000A, 0x000B)
			DP_CPOST(0x0023, 0x0023, 0x0023, 0x0023)
		},
	},
	/* sw-1 */
	{
		{
			/* sw-1 pre-0 */
			DP_TXDRV(0x0003, 0x0003, 0x0003, 0x0003)
			DP_MGNFS(0x001F, 0x001E, 0x001E, 0x001F)
			DP_CPOST(0x0000, 0x0000, 0x0000, 0x0000)
		},
		{
			/* sw-1 pre-1 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x001A, 0x0019, 0x0019, 0x001A)
			DP_CPOST(0x000A, 0x000A, 0x000A, 0x000A)
		},
		{
			/* sw-1 pre-2 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x000A, 0x0009, 0x0009, 0x000A)
			DP_CPOST(0x0019, 0x0019, 0x0019, 0x0019)
		},
		{
			/* sw-1 pre-3 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x000A, 0x0009, 0x0009, 0x000A)
			DP_CPOST(0x0019, 0x0019, 0x0019, 0x0019)
		},
	},
	/* sw-2 */
	{
		{
			/* sw-2 pre-0 */
			DP_TXDRV(0x0003, 0x0003, 0x0003, 0x0003)
			DP_MGNFS(0x0010, 0x000E, 0x000E, 0x0010)
			DP_CPOST(0x0000, 0x0000, 0x0000, 0x0000)
		},
		{
			/* sw-2 pre-1 */
			DP_TXDRV(0x0082, 0x0082, 0x0082, 0x0082)
			DP_MGNFS(0x0000, 0x0000, 0x0000, 0x0000)
			DP_CPOST(0x0011, 0x0011, 0x0011, 0x0011)
		},
		{
			/* sw-2 pre-2 */
			DP_TXDRV(0x0082, 0x0082, 0x0082, 0x0082)
			DP_MGNFS(0x0000, 0x0000, 0x0000, 0x0000)
			DP_CPOST(0x0011, 0x0011, 0x0011, 0x0011)
		},
		{
			/* sw-2 pre-3 */
			DP_TXDRV(0x0082, 0x0082, 0x0082, 0x0082)
			DP_MGNFS(0x0000, 0x0000, 0x0000, 0x0000)
			DP_CPOST(0x0011, 0x0011, 0x0011, 0x0011)
		},
	},
	/* sw-3 */
	{
		{
			/* sw-3 pre-0 */
			DP_TXDRV(0x0003, 0x0003, 0x0003, 0x0003)
			DP_MGNFS(0x0000, 0x0000, 0x0000, 0x0000)
			DP_CPOST(0x0000, 0x0000, 0x0000, 0x0000)
		},
		{
			/* sw-3 pre-1 */
			DP_TXDRV(0x0003, 0x0003, 0x0003, 0x0003)
			DP_MGNFS(0x0000, 0x0000, 0x0000, 0x0000)
			DP_CPOST(0x0000, 0x0000, 0x0000, 0x0000)
		},
		{
			/* sw-3 pre-2 */
			DP_TXDRV(0x0003, 0x0003, 0x0003, 0x0003)
			DP_MGNFS(0x0000, 0x0000, 0x0000, 0x0000)
			DP_CPOST(0x0000, 0x0000, 0x0000, 0x0000)
		},
		{
			/* sw-3 pre-3 */
			DP_TXDRV(0x0003, 0x0003, 0x0003, 0x0003)
			DP_MGNFS(0x0000, 0x0000, 0x0000, 0x0000)
			DP_CPOST(0x0000, 0x0000, 0x0000, 0x0000)
		},
	},

};

static struct dp_signal_level dp_lv_table_1g62[MAX_SWING][MAX_PRE] = {
	/* sw-0 */
	{
		{
			/* sw-0 pre-0 */
			DP_TXDRV(0x0003, 0x0003, 0x0003, 0x0003)
			DP_MGNFS(0x0027, 0x0027, 0x0027, 0x0027)
			DP_CPOST(0x0000, 0x0000, 0x0000, 0x0000)
		},
		{
			/* sw-0 pre-1 */
			DP_TXDRV(0x00A2, 0x00A2, 0x00A2, 0x00A2)
			DP_MGNFS(0x0023, 0x0023, 0x0023, 0x0023)
			DP_CPOST(0x000A, 0x000A, 0x000A, 0x000A)
		},
		{
			/* sw-0 pre-2 */
			DP_TXDRV(0x0092, 0x0092, 0x0092, 0x0092)
			DP_MGNFS(0x001A, 0x001A, 0x001A, 0x001A)
			DP_CPOST(0x0016, 0x0016, 0x0016, 0x0016)
		},
		{
			/* sw-0 pre-3 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x000E, 0x000E, 0x000E, 0x000E)
			DP_CPOST(0x0020, 0x0020, 0x0020, 0x0020)
		},
	},
	/* sw-1 */
	{
		{
			/* sw-1 pre-0 */
			DP_TXDRV(0x0003, 0x0003, 0x0003, 0x0003)
			DP_MGNFS(0x0020, 0x0020, 0x0020, 0x0020)
			DP_CPOST(0x0000, 0x0000, 0x0000, 0x0000)
		},
		{
			/* sw-1 pre-1 */
			DP_TXDRV(0x00A2, 0x00A2, 0x00A2, 0x00A2)
			DP_MGNFS(0x0019, 0x0018, 0x0018, 0x0019)
			DP_CPOST(0x000C, 0x000C, 0x000C, 0x000C)
		},
		{
			/* sw-1 pre-2 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x000D, 0x000C, 0x000C, 0x000D)
			DP_CPOST(0x0017, 0x0017, 0x0017, 0x0017)
		},
		{
			/* sw-1 pre-3 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x000D, 0x000C, 0x000C, 0x000D)
			DP_CPOST(0x0017, 0x0017, 0x0017, 0x0017)
		},
	},
	/* sw-2 */
	{
		{
			/* sw-2 pre-0 */
			DP_TXDRV(0x0003, 0x0003, 0x0003, 0x0003)
			DP_MGNFS(0x0010, 0x000F, 0x000F, 0x0010)
			DP_CPOST(0x0000, 0x0000, 0x0000, 0x0000)
		},
		{
			/* sw-2 pre-1 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x0004, 0x0003, 0x0003, 0x0004)
			DP_CPOST(0x0010, 0x0010, 0x0010, 0x0010)
		},
		{
			/* sw-2 pre-2 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x0004, 0x0003, 0x0003, 0x0004)
			DP_CPOST(0x0010, 0x0010, 0x0010, 0x0010)
		},
		{
			/* sw-2 pre-3 */
			DP_TXDRV(0x00B2, 0x00B2, 0x00B2, 0x00B2)
			DP_MGNFS(0x0004, 0x0003, 0x0003, 0x0004)
			DP_CPOST(0x0010, 0x0010, 0x0010, 0x0010)
		},
	},
	/* sw-3 */
	{
		{
			/* sw-3 pre-0 */
			DP_TXDRV(0x0003, 0x0003, 0x0003, 0x0003)
			DP_MGNFS(0x0000, 0x0000, 0x0000, 0x0000)
			DP_CPOST(0x0000, 0x0000, 0x0000, 0x0000)
		},
		{
			/* sw-3 pre-1 */
			DP_TXDRV(0x0003, 0x0003, 0x0003, 0x0003)
			DP_MGNFS(0x0000, 0x0000, 0x0000, 0x0000)
			DP_CPOST(0x0000, 0x0000, 0x0000, 0x0000)
		},
		{
			/* sw-3 pre-2 */
			DP_TXDRV(0x0003, 0x0003, 0x0003, 0x0003)
			DP_MGNFS(0x0000, 0x0000, 0x0000, 0x0000)
			DP_CPOST(0x0000, 0x0000, 0x0000, 0x0000)
		},
		{
			/* sw-3 pre-3 */
			DP_TXDRV(0x0003, 0x0003, 0x0003, 0x0003)
			DP_MGNFS(0x0000, 0x0000, 0x0000, 0x0000)
			DP_CPOST(0x0000, 0x0000, 0x0000, 0x0000)
		},
	},

};


static int combo0_configure_usb_dp(struct sunxi_cadence_combophy *combo0);
static void combo0_dp_phy_reset(struct sunxi_cadence_combophy *combo0, bool deassert)
{
	struct sunxi_cadence_phy *sunxi_cphy = combo0->sunxi_cphy;
	u32 phy0_pma_cmn_ready = 0;
	u32 reg_val;
	u32 retry_cnt = 0;

	if (deassert) {
		reg_val = readl(COMB0_REG_PHYPINS_LINK_CTRL2(combo0->top_reg));
		reg_val &= ~(PHY0_IDDQ_EN_SEL | PHY0_IDDQ_EN_SOFT);
		writel(reg_val, COMB0_REG_PHYPINS_LINK_CTRL2(combo0->top_reg));

		//PHY RESET
		phy_writel(combo0->top_reg + 0x00, 0x0);

		//PHY RESET
		phy_writel(combo0->top_reg + 0x00, 0x001);
		msleep(1);
		//DP LINK RESET
		phy_writel(combo0->top_reg + 0x00, 0x101);
		msleep(1);
		//PHY RESET
		phy_writel(combo0->top_reg + 0x00, 0x131);
		msleep(1);

		reg_val = readl(COMB0_REG_PHYPINS_LINK_CTRL0(combo0->top_reg));
		if (combo0->orientation == TYPEC_ORIENTATION_REVERSE)
			reg_val |= PHY0_TYPEC_CONN_DIR;
		else
			reg_val &= ~PHY0_TYPEC_CONN_DIR;
		writel(reg_val, COMB0_REG_PHYPINS_LINK_CTRL0(combo0->top_reg));

		msleep(1);

		while (phy0_pma_cmn_ready == 0) {
 if(combo0->dp->failed)return;
			if (++retry_cnt > 1000) {
				pr_warn("wrn: %s dp wait pll enable timeout line:%d\n", combo0->name, __LINE__);
				a733_dp_fault(combo0->dp,A733_DP_PHY);return;
			}
			reg_val = phy_readl(combo0->top_reg+0x900);
			reg_val = reg_val & (1 << 0);
			phy0_pma_cmn_ready = reg_val;

			udelay(5);
		}

		phy_set_lbits(sunxi_cphy->top_combo_reg + 0x4, 0, 1, 1);
		phy_set_lbits(sunxi_cphy->top_combo_reg + 0x4, 4, 4, 0xf);
		phy_set_lbits(sunxi_cphy->top_combo_reg + 0x4, 8, 4, 0xf);

		phy_set_lbits(combo0->top_reg + 0x0c, 0, 1, 1);

		phy0_pma_cmn_ready = 0;
		retry_cnt = 0;
		while (phy0_pma_cmn_ready == 0) {
 if(combo0->dp->failed)return;
			if (++retry_cnt > 1000) {
				pr_warn("wrn: %s dp wait pll enable timeout line:%d\n", combo0->name, __LINE__);
				a733_dp_fault(combo0->dp,A733_DP_PHY);return;
			}
			reg_val = phy_readl(combo0->top_reg+0x900);
			reg_val = (reg_val & (1 << 4)) >> 4;
			phy0_pma_cmn_ready = reg_val;

			udelay(5);
		}

		msleep(1);

		phy_set_lbits(combo0->top_reg + 0x0c, 4, 6, 1);

		phy0_pma_cmn_ready = 0;
		retry_cnt = 0;
		while (phy0_pma_cmn_ready == 0) {
 if(combo0->dp->failed)return;
			if (++retry_cnt > 100000) {
				pr_warn("wrn: %s dp wait pll enable timeout line:%d\n", combo0->name, __LINE__);
				a733_dp_fault(combo0->dp,A733_DP_PHY);return;
			}
			reg_val = phy_readl(combo0->top_reg+0x900);
			reg_val = (reg_val & (0x3f << 8)) >> 8;
			phy0_pma_cmn_ready = reg_val;

			udelay(5);
		}
	} else {
		//PHY RESET
		phy_writel(combo0->top_reg + 0x00, 0x0);
		phy_writel(sunxi_cphy->top_combo_reg + 0x4, 0x0);
		phy_writel(combo0->top_reg + 0x0c, 0x0);
		msleep(1);
	}
}
static int combo0_dp_set_rate(struct sunxi_cadence_combophy *combo0,
		       struct phy_configure_opts_dp *dp_config)
{
	u32 val;

	if (dp_config->set_rate == 1) {
		/* if in typec STATE_D/F, skip phy configure/reset which has been set */
		if (combo0->usb_dp_state) {
			switch (dp_config->link_rate) {
			case 2700:
				if (combo0->orientation == TYPEC_ORIENTATION_REVERSE) {
					phy_writew(combo0->phy_reg + (0x01c1 << 1), 0x0701);
					phy_writew(combo0->phy_reg + (0x40e7 << 1), 0x1);
					phy_writew(combo0->phy_reg + (0x42e7 << 1), 0x1);
				} else {
					phy_writew(combo0->phy_reg + (0x01c1 << 1), 0x0701);
					phy_writew(combo0->phy_reg + (0x44e7 << 1), 0x1);
					phy_writew(combo0->phy_reg + (0x46e7 << 1), 0x1);
				}
				break;
			case 5400:
				if (combo0->orientation == TYPEC_ORIENTATION_REVERSE) {
					phy_writew(combo0->phy_reg + (0x01c1 << 1), 0x0601);
					phy_writew(combo0->phy_reg + (0x40e7 << 1), 0x0);
					phy_writew(combo0->phy_reg + (0x42e7 << 1), 0x0);
				} else {
					phy_writew(combo0->phy_reg + (0x01c1 << 1), 0x0601);
					phy_writew(combo0->phy_reg + (0x44e7 << 1), 0x0);
					phy_writew(combo0->phy_reg + (0x46e7 << 1), 0x0);
				}
				break;
			case 1620:
			case 8100:
			default:
				pr_warn("[Cadence PHY]: Unsupport link_rate:%d state:%d\n",
					dp_config->link_rate, combo0->usb_dp_state);
				return -EINVAL;
			}
			combo0->link_rate = dp_config->link_rate;
		} else {
			combo0_dp_phy_reset(combo0, false);
			switch (dp_config->link_rate) {
			case 1620:
				// set ref-clk to 26M
				val = readl(COMB0_REG_PHYPINS_LINK_CTRL2(combo0->top_reg));
				val &= ~(PHY0_IDDQ_EN_SEL | PHY0_IDDQ_EN_SOFT);
				writel(val, COMB0_REG_PHYPINS_LINK_CTRL2(combo0->top_reg));

				phy_writel(combo0->top_reg + 0x4, 0x1100001);

				//CMN_SSM_BIAS_TMR
				phy_writew(combo0->phy_reg + (0x0022 << 1), 0x001a);
				//CMN_PLLSM0_PLLPRE_TMR
				phy_writew(combo0->phy_reg + (0x002A << 1), 0x0034);
				//CMN_PLLSM0_PLLLOCK_TMR
				phy_writew(combo0->phy_reg + (0x002C << 1), 0x00da);

				//CMN_PLLSM1_PLLPRE_TMR
				phy_writew(combo0->phy_reg + (0x0032 << 1), 0x0034);
				//CMN_PLLSM1_PLLLOCK_TMR
				phy_writew(combo0->phy_reg + (0x0034 << 1), 0x00da);

				//CMN_BGCAL_INIT_TMR
				phy_writew(combo0->phy_reg + (0x0064 << 1), 0x0082);
				//CMN_BGCAL_ITER_TMR
				phy_writew(combo0->phy_reg + (0x0065 << 1), 0x0082);
				//CMN_IBCAL_INIT_TMR
				phy_writew(combo0->phy_reg + (0x0074 << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x0104 << 1), 0x0020);
				phy_writew(combo0->phy_reg + (0x0105 << 1), 0x0007);
				phy_writew(combo0->phy_reg + (0x010c << 1), 0x0020);
				phy_writew(combo0->phy_reg + (0x010d << 1), 0x0007);
				phy_writew(combo0->phy_reg + (0x0114 << 1), 0x030c);
				phy_writew(combo0->phy_reg + (0x0115 << 1), 0x0007);
				phy_writew(combo0->phy_reg + (0x0124 << 1), 0x0007);
				phy_writew(combo0->phy_reg + (0x0125 << 1), 0x0003);
				phy_writew(combo0->phy_reg + (0x0126 << 1), 0x000f);
				phy_writew(combo0->phy_reg + (0x0128 << 1), 0x0132);

				phy_writew(combo0->phy_reg + (0x8044 << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x8244 << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x8444 << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x8644 << 1), 0x001a);

				phy_writew(combo0->phy_reg + (0x8045 << 1), 0x0082);
				phy_writew(combo0->phy_reg + (0x8245 << 1), 0x0082);
				phy_writew(combo0->phy_reg + (0x8445 << 1), 0x0082);
				phy_writew(combo0->phy_reg + (0x8645 << 1), 0x0082);

				phy_writew(combo0->phy_reg + (0x804c << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x824c << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x844c << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x864c << 1), 0x001a);

				phy_writew(combo0->phy_reg + (0x804d << 1), 0x0082);
				phy_writew(combo0->phy_reg + (0x824d << 1), 0x0082);
				phy_writew(combo0->phy_reg + (0x844d << 1), 0x0082);
				phy_writew(combo0->phy_reg + (0x864d << 1), 0x0082);

				phy_writew(combo0->phy_reg + (0x4123 << 1), 0x0a28);
				phy_writew(combo0->phy_reg + (0x4323 << 1), 0x0a28);
				phy_writew(combo0->phy_reg + (0x4523 << 1), 0x0a28);
				phy_writew(combo0->phy_reg + (0x4723 << 1), 0x0a28);

				//VCO calibration settings.
				phy_writew(combo0->phy_reg + (0x00c4 << 1), 0x0104);
				phy_writew(combo0->phy_reg + (0x00c5 << 1), 0x0005);
				phy_writew(combo0->phy_reg + (0x00c6 << 1), 0x0337);
				phy_writew(combo0->phy_reg + (0x00c8 << 1), 0x0335);
				phy_writew(combo0->phy_reg + (0x00c2 << 1), 0x0003);

				phy_writew(combo0->phy_reg + (0x0084 << 1), 0x0104);
				phy_writew(combo0->phy_reg + (0x0085 << 1), 0x0005);
				phy_writew(combo0->phy_reg + (0x0086 << 1), 0x0337);
				phy_writew(combo0->phy_reg + (0x0088 << 1), 0x0335);
				phy_writew(combo0->phy_reg + (0x0082 << 1), 0x0003);

				 //PLL lock detect settings
				phy_writew(combo0->phy_reg + (0x00dc << 1), 0x00cf);
				phy_writew(combo0->phy_reg + (0x00de << 1), 0x00ce);
				phy_writew(combo0->phy_reg + (0x00dF << 1), 0x0005);

				phy_writew(combo0->phy_reg + (0x009c << 1), 0x00cf);
				phy_writew(combo0->phy_reg + (0x009e << 1), 0x00ce);
				phy_writew(combo0->phy_reg + (0x009F << 1), 0x0005);

				//DP_LANE_SET as follow
				phy_writew(combo0->phy_reg + (0xc010 << 1), 0x51d9);
				//phy DP lane enabled
				phy_writew(combo0->phy_reg + (0xc011 << 1), 0x0100);

				//PHY_PMA_ISO_PLL_CTRL0   cmn_pll1_clk_en
				phy_writew(combo0->phy_reg + (0xe003 << 1), 0x0003);
				phy_writew(combo0->phy_reg + (0xe005 << 1), 0x000B);
				//PHY_PMA_ISO_PLL_CTRL1
				phy_writew(combo0->phy_reg + (0xe006 << 1), 0x2224);

				//TX_PSC_A0
				phy_writew(combo0->phy_reg + (0x4100 << 1), 0x00FB);
				phy_writew(combo0->phy_reg + (0x4300 << 1), 0x00FB);
				phy_writew(combo0->phy_reg + (0x4500 << 1), 0x00FB);
				phy_writew(combo0->phy_reg + (0x4700 << 1), 0x00FB);

				//TX_PSC_A2
				phy_writew(combo0->phy_reg + (0x4102 << 1), 0x04AA);
				phy_writew(combo0->phy_reg + (0x4302 << 1), 0x04AA);
				phy_writew(combo0->phy_reg + (0x4502 << 1), 0x04AA);
				phy_writew(combo0->phy_reg + (0x4702 << 1), 0x04AA);

				//TX_PSC_A3
				phy_writew(combo0->phy_reg + (0x4102 << 1), 0x04AA);
				phy_writew(combo0->phy_reg + (0x4302 << 1), 0x04AA);
				phy_writew(combo0->phy_reg + (0x4502 << 1), 0x04AA);
				phy_writew(combo0->phy_reg + (0x4702 << 1), 0x04AA);

				//RX all set to 0
				//RX_PSC_A0
				phy_writew(combo0->phy_reg + (0x8000 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8200 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8400 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8600 << 1), 0x0);

				//RX_PSC_A0
				phy_writew(combo0->phy_reg + (0x8000 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8202 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8402 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8602 << 1), 0x0);

				//RX_PSC_A0
				phy_writew(combo0->phy_reg + (0x8002 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8203 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8403 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8603 << 1), 0x0);

				//RX_PSC_CAL
				phy_writew(combo0->phy_reg + (0x8006 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8206 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8406 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8606 << 1), 0x0);

				//RX_REE_GCSM1_CTRL
				phy_writew(combo0->phy_reg + (0x8108 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8308 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8508 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8708 << 1), 0x0);

				//RX_REE_GCSM2_CTRL
				phy_writew(combo0->phy_reg + (0x8110 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8310 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8510 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8710 << 1), 0x0);

				//RX_REE_PERGCSM_CTRL
				phy_writew(combo0->phy_reg + (0x8118 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8318 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8518 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8718 << 1), 0x0);

				//XCVR_DIAG_BIDI_CTRL  diefferent mode with TX/RX enable
				phy_writew(combo0->phy_reg + (0x40EA << 1), 0x000F);
				phy_writew(combo0->phy_reg + (0x42EA << 1), 0x000F);
				phy_writew(combo0->phy_reg + (0x44EA << 1), 0x000F);
				phy_writew(combo0->phy_reg + (0x46EA << 1), 0x000F);

				//TX_TXCC_CTRL
				phy_writew(combo0->phy_reg + (0x4040 << 1), 0x08A4);
				phy_writew(combo0->phy_reg + (0x4240 << 1), 0x08A4);
				phy_writew(combo0->phy_reg + (0x4440 << 1), 0x08A4);
				phy_writew(combo0->phy_reg + (0x4640 << 1), 0x08A4);

				//DRV_DIAG_TX_DRV
				phy_writew(combo0->phy_reg + (0x40C6 << 1), 0x0003);
				phy_writew(combo0->phy_reg + (0x42C6 << 1), 0x0003);
				phy_writew(combo0->phy_reg + (0x44C6 << 1), 0x0003);
				phy_writew(combo0->phy_reg + (0x46C6 << 1), 0x0003);

				phy_writew(combo0->phy_reg + (0x4050 << 1), 0x002A);
				phy_writew(combo0->phy_reg + (0x4250 << 1), 0x002A);
				phy_writew(combo0->phy_reg + (0x4450 << 1), 0x002A);
				phy_writew(combo0->phy_reg + (0x4650 << 1), 0x002A);

				phy_writew(combo0->phy_reg + (0x404C << 1), 0x0000);
				phy_writew(combo0->phy_reg + (0x424C << 1), 0x0000);
				phy_writew(combo0->phy_reg + (0x444C << 1), 0x0000);
				phy_writew(combo0->phy_reg + (0x464C << 1), 0x0000);

				//CMN-pull up-down res cal
				phy_writew(combo0->phy_reg + (0x0103 << 1), 0x007F);
				phy_writew(combo0->phy_reg + (0x010B << 1), 0x007F);

				phy_writew(combo0->phy_reg + (0x4140 << 1), 0x0801);
				phy_writew(combo0->phy_reg + (0x4340 << 1), 0x0801);
				phy_writew(combo0->phy_reg + (0x4540 << 1), 0x0801);
				phy_writew(combo0->phy_reg + (0x4740 << 1), 0x0801);

				//ssc
				if (combo0->ssc_en)
					phy_writew(combo0->phy_reg + (0x00D8 << 1), 0x0001);
				else
					phy_writew(combo0->phy_reg + (0x00D8 << 1), 0x0002);
				phy_writew(combo0->phy_reg + (0x00D9 << 1), 0x04C4);
				phy_writew(combo0->phy_reg + (0x00DA << 1), 0x006A);
				phy_writew(combo0->phy_reg + (0x00DB << 1), 0x0004);

				phy_writew(combo0->phy_reg + (0x0098 << 1), 0x0001);
				phy_writew(combo0->phy_reg + (0x00D9 << 1), 0x044A);
				phy_writew(combo0->phy_reg + (0x009A << 1), 0x006b);
				phy_writew(combo0->phy_reg + (0x009B << 1), 0x0004);

				// pll configure
				phy_writew(combo0->phy_reg + (0x00d4 << 1), 0x0004);
				phy_writew(combo0->phy_reg + (0x01C4 << 1), 0x0509);
				phy_writew(combo0->phy_reg + (0x01C5 << 1), 0x0F00);
				phy_writew(combo0->phy_reg + (0x01C6 << 1), 0x0F08);

				phy_writew(combo0->phy_reg + (0x00d0 << 1), 0x0175);
				phy_writew(combo0->phy_reg + (0x00d1 << 1), 0xD89E);
				phy_writew(combo0->phy_reg + (0x00d2 << 1), 0x0002);
				phy_writew(combo0->phy_reg + (0x00d3 << 1), 0x00FA);
				phy_writew(combo0->phy_reg + (0x01c0 << 1), 0x0002);

				//SET common  PLL1_CLOCK0
				phy_writew(combo0->phy_reg + (0x1c1 << 1), 0x0F01);
				phy_writew(combo0->phy_reg + (0x004B << 1), 0x0);

				//dp XCVR(LANE) pll1 seting
				phy_writew(combo0->phy_reg + (0x40e6 << 1), 0x0001);
				phy_writew(combo0->phy_reg + (0x42e6 << 1), 0x0001);
				phy_writew(combo0->phy_reg + (0x44e6 << 1), 0x0001);
				phy_writew(combo0->phy_reg + (0x46e6 << 1), 0x0001);

				phy_writew(combo0->phy_reg + (0x40e7 << 1), 0x0002);
				phy_writew(combo0->phy_reg + (0x42e7 << 1), 0x0002);
				phy_writew(combo0->phy_reg + (0x44e7 << 1), 0x0002);
				phy_writew(combo0->phy_reg + (0x46e7 << 1), 0x0002);
				phy_writew(combo0->phy_reg + (0x40e5 << 1), 0x0019);
				phy_writew(combo0->phy_reg + (0x42e5 << 1), 0x0019);
				phy_writew(combo0->phy_reg + (0x44e5 << 1), 0x0019);
				phy_writew(combo0->phy_reg + (0x46e5 << 1), 0x0019);
				phy_writew(combo0->phy_reg + (0xe006 << 1), 0x2224);

				//PLL0 configure
				phy_writew(combo0->phy_reg + (0x1A1 << 1), 0x8600);
				phy_writew(combo0->phy_reg + (0x0094 << 1), 0x0004);
				phy_writew(combo0->phy_reg + (0x01A4 << 1), 0x0509);
				phy_writew(combo0->phy_reg + (0x01A5 << 1), 0x0F00);
				phy_writew(combo0->phy_reg + (0x01A6 << 1), 0x0F08);
				phy_writew(combo0->phy_reg + (0x0090 << 1), 0x0180);
				phy_writew(combo0->phy_reg + (0x0091 << 1), 0x9DBA);
				phy_writew(combo0->phy_reg + (0x0092 << 1), 0x0002);
				phy_writew(combo0->phy_reg + (0x0093 << 1), 0x0102);
				phy_writew(combo0->phy_reg + (0x01A0 << 1), 0x0002);
				break;
			case 2160:
				break;
			case 2430:
				break;
			case 2700:
				val = readl(COMB0_REG_PHYPINS_LINK_CTRL2(combo0->top_reg));
				val &= ~(PHY0_IDDQ_EN_SEL | PHY0_IDDQ_EN_SOFT);
				writel(val, COMB0_REG_PHYPINS_LINK_CTRL2(combo0->top_reg));

				phy_writel(combo0->top_reg + 0x4, 0x1100001);

				//CMN_SSM_BIAS_TMR
				phy_writew(combo0->phy_reg + (0x0022 << 1), 0x001a);

				//CMN_PLLSM0_PLLPRE_TMR
				phy_writew(combo0->phy_reg + (0x002A << 1), 0x0034);
				//CMN_PLLSM0_PLLLOCK_TMR
				phy_writew(combo0->phy_reg + (0x002C << 1), 0x00da);

				//CMN_PLLSM1_PLLPRE_TMR
				phy_writew(combo0->phy_reg + (0x0032 << 1), 0x0034);
				//CMN_PLLSM1_PLLLOCK_TMR
				phy_writew(combo0->phy_reg + (0x0034 << 1), 0x00da);

				//CMN_BGCAL_INIT_TMR
				phy_writew(combo0->phy_reg + (0x0064 << 1), 0x0082);
				//CMN_BGCAL_ITER_TMR
				phy_writew(combo0->phy_reg + (0x0065 << 1), 0x0082);
				//CMN_IBCAL_INIT_TMR
				phy_writew(combo0->phy_reg + (0x0074 << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x0104 << 1), 0x0020);
				phy_writew(combo0->phy_reg + (0x0105 << 1), 0x0007);
				phy_writew(combo0->phy_reg + (0x010c << 1), 0x0020);
				phy_writew(combo0->phy_reg + (0x010d << 1), 0x0007);
				phy_writew(combo0->phy_reg + (0x0114 << 1), 0x030c);
				phy_writew(combo0->phy_reg + (0x0115 << 1), 0x0007);
				phy_writew(combo0->phy_reg + (0x0124 << 1), 0x0007);
				phy_writew(combo0->phy_reg + (0x0125 << 1), 0x0003);
				phy_writew(combo0->phy_reg + (0x0126 << 1), 0x000f);
				phy_writew(combo0->phy_reg + (0x0128 << 1), 0x0132);

				phy_writew(combo0->phy_reg + (0x8044 << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x8244 << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x8444 << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x8644 << 1), 0x001a);

				phy_writew(combo0->phy_reg + (0x8045 << 1), 0x0082);
				phy_writew(combo0->phy_reg + (0x8245 << 1), 0x0082);
				phy_writew(combo0->phy_reg + (0x8445 << 1), 0x0082);
				phy_writew(combo0->phy_reg + (0x8645 << 1), 0x0082);

				phy_writew(combo0->phy_reg + (0x804c << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x824c << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x844c << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x864c << 1), 0x001a);

				phy_writew(combo0->phy_reg + (0x804d << 1), 0x0082);
				phy_writew(combo0->phy_reg + (0x824d << 1), 0x0082);
				phy_writew(combo0->phy_reg + (0x844d << 1), 0x0082);
				phy_writew(combo0->phy_reg + (0x864d << 1), 0x0082);

				phy_writew(combo0->phy_reg + (0x4123 << 1), 0x0a28);
				phy_writew(combo0->phy_reg + (0x4323 << 1), 0x0a28);
				phy_writew(combo0->phy_reg + (0x4523 << 1), 0x0a28);
				phy_writew(combo0->phy_reg + (0x4723 << 1), 0x0a28);

				//VCO calibration settings.
				phy_writew(combo0->phy_reg + (0x00c4 << 1), 0x0104);
				phy_writew(combo0->phy_reg + (0x00c5 << 1), 0x0005);
				phy_writew(combo0->phy_reg + (0x00c6 << 1), 0x0337);
				phy_writew(combo0->phy_reg + (0x00c8 << 1), 0x0335);
				phy_writew(combo0->phy_reg + (0x00c2 << 1), 0x0003);
				phy_writew(combo0->phy_reg + (0x0084 << 1), 0x0104);
				phy_writew(combo0->phy_reg + (0x0085 << 1), 0x0005);
				phy_writew(combo0->phy_reg + (0x0086 << 1), 0x0337);
				phy_writew(combo0->phy_reg + (0x0088 << 1), 0x0335);
				phy_writew(combo0->phy_reg + (0x0082 << 1), 0x0003);

				//PLL lock detect settings
				phy_writew(combo0->phy_reg + (0x00dc << 1), 0x00cf);
				phy_writew(combo0->phy_reg + (0x00de << 1), 0x00ce);
				phy_writew(combo0->phy_reg + (0x00dF << 1), 0x0005);
				phy_writew(combo0->phy_reg + (0x009c << 1), 0x00cf);
				phy_writew(combo0->phy_reg + (0x009e << 1), 0x00ce);
				phy_writew(combo0->phy_reg + (0x009F << 1), 0x0005);

				//DP_LANE_SET
				phy_writew(combo0->phy_reg + (0xc010 << 1), 0x51d9);
				phy_writew(combo0->phy_reg + (0xc011 << 1), 0x0100);
				//PHY_PMA_ISO_PLL_CTRL0   cmn_pll1_clk_en
				phy_writew(combo0->phy_reg + (0xe003 << 1), 0x0003);
				phy_writew(combo0->phy_reg + (0xe005 << 1), 0x000B);
				//PHY_PMA_ISO_PLL_CTRL1
				phy_writew(combo0->phy_reg + (0xe006 << 1), 0x2224);
				//TX_PSC_A0
				phy_writew(combo0->phy_reg + (0x4100 << 1), 0x00FB);
				phy_writew(combo0->phy_reg + (0x4300 << 1), 0x00FB);
				phy_writew(combo0->phy_reg + (0x4500 << 1), 0x00FB);
				phy_writew(combo0->phy_reg + (0x4700 << 1), 0x00FB);

				//TX_PSC_A2
				phy_writew(combo0->phy_reg + (0x4102 << 1), 0x04AA);
				phy_writew(combo0->phy_reg + (0x4302 << 1), 0x04AA);
				phy_writew(combo0->phy_reg + (0x4502 << 1), 0x04AA);
				phy_writew(combo0->phy_reg + (0x4702 << 1), 0x04AA);

				//TX_PSC_A3
				phy_writew(combo0->phy_reg + (0x4102 << 1), 0x04AA);
				phy_writew(combo0->phy_reg + (0x4302 << 1), 0x04AA);
				phy_writew(combo0->phy_reg + (0x4502 << 1), 0x04AA);
				phy_writew(combo0->phy_reg + (0x4702 << 1), 0x04AA);

				//RX all set to 0
				phy_writew(combo0->phy_reg + (0x8000 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8200 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8400 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8600 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8000 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8202 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8402 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8602 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8002 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8203 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8403 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8603 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8006 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8206 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8406 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8606 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8108 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8308 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8508 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8708 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8110 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8310 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8510 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8710 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8118 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8318 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8518 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8718 << 1), 0x0);

				//XCVR_DIAG_BIDI_CTRL
				phy_writew(combo0->phy_reg + (0x40EA << 1), 0x000F);
				phy_writew(combo0->phy_reg + (0x42EA << 1), 0x000F);
				phy_writew(combo0->phy_reg + (0x44EA << 1), 0x000F);
				phy_writew(combo0->phy_reg + (0x46EA << 1), 0x000F);

				//TX_TXCC_CTRL
				phy_writew(combo0->phy_reg + (0x4040 << 1), 0x08A4);
				phy_writew(combo0->phy_reg + (0x4240 << 1), 0x08A4);
				phy_writew(combo0->phy_reg + (0x4440 << 1), 0x08A4);
				phy_writew(combo0->phy_reg + (0x4640 << 1), 0x08A4);

				//DRV_DIAG_TX_DRV
				phy_writew(combo0->phy_reg + (0x40C6 << 1), 0x0003);
				phy_writew(combo0->phy_reg + (0x42C6 << 1), 0x0003);
				phy_writew(combo0->phy_reg + (0x44C6 << 1), 0x0003);
				phy_writew(combo0->phy_reg + (0x46C6 << 1), 0x0003);

				phy_writew(combo0->phy_reg + (0x4050 << 1), 0x002A);
				phy_writew(combo0->phy_reg + (0x4250 << 1), 0x002A);
				phy_writew(combo0->phy_reg + (0x4450 << 1), 0x002A);
				phy_writew(combo0->phy_reg + (0x4650 << 1), 0x002A);

				phy_writew(combo0->phy_reg + (0x404C << 1), 0x0000);
				phy_writew(combo0->phy_reg + (0x424C << 1), 0x0000);
				phy_writew(combo0->phy_reg + (0x444C << 1), 0x0000);
				phy_writew(combo0->phy_reg + (0x464C << 1), 0x0000);

				//CMN-pull up-down res cal
				phy_writew(combo0->phy_reg + (0x0103 << 1), 0x007F);
				phy_writew(combo0->phy_reg + (0x010B << 1), 0x007F);

				//ssc
				if (combo0->ssc_en)
					phy_writew(combo0->phy_reg + (0x00D8 << 1), 0x0001);
				else
					phy_writew(combo0->phy_reg + (0x00D8 << 1), 0x0002);
				phy_writew(combo0->phy_reg + (0x00D9 << 1), 0x04C4);
				phy_writew(combo0->phy_reg + (0x00DA << 1), 0x006A);
				phy_writew(combo0->phy_reg + (0x00DB << 1), 0x0004);

				phy_writew(combo0->phy_reg + (0x0098 << 1), 0x0001);
				phy_writew(combo0->phy_reg + (0x00D9 << 1), 0x045f);
				phy_writew(combo0->phy_reg + (0x009A << 1), 0x006b);
				phy_writew(combo0->phy_reg + (0x009B << 1), 0x0004);

				// pll configure
				phy_writew(combo0->phy_reg + (0x00d4 << 1), 0x0004);
				phy_writew(combo0->phy_reg + (0x01C4 << 1), 0x0509);
				phy_writew(combo0->phy_reg + (0x01C5 << 1), 0x0F00);
				phy_writew(combo0->phy_reg + (0x01C6 << 1), 0x0F08);

				phy_writew(combo0->phy_reg + (0x00d0 << 1), 0x019f);
				phy_writew(combo0->phy_reg + (0x00d1 << 1), 0x6276);
				phy_writew(combo0->phy_reg + (0x00d2 << 1), 0x0002);
				phy_writew(combo0->phy_reg + (0x00d3 << 1), 0x0116);
				phy_writew(combo0->phy_reg + (0x01c0 << 1), 0x0002);

				phy_writew(combo0->phy_reg + (0x1c1 << 1), 0x0701);
				phy_writew(combo0->phy_reg + (0x004B << 1), 0x0);

				//dp XCVR(LANE) pll1 seting
				phy_writew(combo0->phy_reg + (0x40e6 << 1), 0x0001);
				phy_writew(combo0->phy_reg + (0x42e6 << 1), 0x0001);
				phy_writew(combo0->phy_reg + (0x44e6 << 1), 0x0001);
				phy_writew(combo0->phy_reg + (0x46e6 << 1), 0x0001);

				phy_writew(combo0->phy_reg + (0x40e7 << 1), 0x0001);
				phy_writew(combo0->phy_reg + (0x42e7 << 1), 0x0001);
				phy_writew(combo0->phy_reg + (0x44e7 << 1), 0x0001);
				phy_writew(combo0->phy_reg + (0x46e7 << 1), 0x0001);

				phy_writew(combo0->phy_reg + (0x40e5 << 1), 0x0019);
				phy_writew(combo0->phy_reg + (0x42e5 << 1), 0x0019);
				phy_writew(combo0->phy_reg + (0x44e5 << 1), 0x0019);
				phy_writew(combo0->phy_reg + (0x46e5 << 1), 0x0019);
				phy_writew(combo0->phy_reg + (0xe006 << 1), 0x2224);

				// pll0 configure
				phy_writew(combo0->phy_reg + (0x1A1 << 1), 0x8600);
				phy_writew(combo0->phy_reg + (0x0094 << 1), 0x0004);
				phy_writew(combo0->phy_reg + (0x01A4 << 1), 0x0509);
				phy_writew(combo0->phy_reg + (0x01A5 << 1), 0x0F00);
				phy_writew(combo0->phy_reg + (0x01A6 << 1), 0x0F08);
				phy_writew(combo0->phy_reg + (0x0090 << 1), 0x0180);
				phy_writew(combo0->phy_reg + (0x0091 << 1), 0x9DBA);
				phy_writew(combo0->phy_reg + (0x0092 << 1), 0x0002);
				phy_writew(combo0->phy_reg + (0x0093 << 1), 0x0102);
				phy_writew(combo0->phy_reg + (0x01A0 << 1), 0x0002);
				break;
			case 3240:
				break;
			case 4320:
				break;
			case 5400:
				val = readl(COMB0_REG_PHYPINS_LINK_CTRL2(combo0->top_reg));
				val &= ~(PHY0_IDDQ_EN_SEL | PHY0_IDDQ_EN_SOFT);
				writel(val, COMB0_REG_PHYPINS_LINK_CTRL2(combo0->top_reg));

				phy_writel(combo0->top_reg + 0x4, 0x1100001);

				//CMN_SSM_BIAS_TMR
				phy_writew(combo0->phy_reg + (0x0022 << 1), 0x001a);
				//CMN_PLLSM0_PLLPRE_TMR
				phy_writew(combo0->phy_reg + (0x002A << 1), 0x0034);
				//CMN_PLLSM0_PLLLOCK_TMR
				phy_writew(combo0->phy_reg + (0x002C << 1), 0x00da);
				//CMN_PLLSM1_PLLPRE_TMR
				phy_writew(combo0->phy_reg + (0x0032 << 1), 0x0034);
				//CMN_PLLSM1_PLLLOCK_TMR
				phy_writew(combo0->phy_reg + (0x0034 << 1), 0x00da);
				//CMN_BGCAL_INIT_TMR
				phy_writew(combo0->phy_reg + (0x0064 << 1), 0x0082);
				//CMN_BGCAL_ITER_TMR
				phy_writew(combo0->phy_reg + (0x0065 << 1), 0x0082);
				//CMN_IBCAL_INIT_TMR
				phy_writew(combo0->phy_reg + (0x0074 << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x0104 << 1), 0x0020);
				phy_writew(combo0->phy_reg + (0x0105 << 1), 0x0007);
				phy_writew(combo0->phy_reg + (0x010c << 1), 0x0020);
				phy_writew(combo0->phy_reg + (0x010d << 1), 0x0007);
				phy_writew(combo0->phy_reg + (0x0114 << 1), 0x030c);
				phy_writew(combo0->phy_reg + (0x0115 << 1), 0x0007);
				phy_writew(combo0->phy_reg + (0x0124 << 1), 0x0007);
				phy_writew(combo0->phy_reg + (0x0125 << 1), 0x0003);
				phy_writew(combo0->phy_reg + (0x0126 << 1), 0x000f);
				phy_writew(combo0->phy_reg + (0x0128 << 1), 0x0132);
				phy_writew(combo0->phy_reg + (0x8044 << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x8244 << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x8444 << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x8644 << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x8045 << 1), 0x0082);
				phy_writew(combo0->phy_reg + (0x8245 << 1), 0x0082);
				phy_writew(combo0->phy_reg + (0x8445 << 1), 0x0082);
				phy_writew(combo0->phy_reg + (0x8645 << 1), 0x0082);
				phy_writew(combo0->phy_reg + (0x804c << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x824c << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x844c << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x864c << 1), 0x001a);
				phy_writew(combo0->phy_reg + (0x804d << 1), 0x0082);
				phy_writew(combo0->phy_reg + (0x824d << 1), 0x0082);
				phy_writew(combo0->phy_reg + (0x844d << 1), 0x0082);
				phy_writew(combo0->phy_reg + (0x864d << 1), 0x0082);
				phy_writew(combo0->phy_reg + (0x4123 << 1), 0x0a28);
				phy_writew(combo0->phy_reg + (0x4323 << 1), 0x0a28);
				phy_writew(combo0->phy_reg + (0x4523 << 1), 0x0a28);
				phy_writew(combo0->phy_reg + (0x4723 << 1), 0x0a28);

				//VCO calibration settings.
				phy_writew(combo0->phy_reg + (0x00c4 << 1), 0x0104);
				phy_writew(combo0->phy_reg + (0x00c5 << 1), 0x0005);
				phy_writew(combo0->phy_reg + (0x00c6 << 1), 0x0337);
				phy_writew(combo0->phy_reg + (0x00c8 << 1), 0x0335);
				phy_writew(combo0->phy_reg + (0x00c2 << 1), 0x0003);
				phy_writew(combo0->phy_reg + (0x0084 << 1), 0x0104);
				phy_writew(combo0->phy_reg + (0x0085 << 1), 0x0005);
				phy_writew(combo0->phy_reg + (0x0086 << 1), 0x0337);
				phy_writew(combo0->phy_reg + (0x0088 << 1), 0x0335);
				phy_writew(combo0->phy_reg + (0x0082 << 1), 0x0003);

				//PLL lock detect settings
				phy_writew(combo0->phy_reg + (0x00dc << 1), 0x00cf);
				phy_writew(combo0->phy_reg + (0x00de << 1), 0x00ce);
				phy_writew(combo0->phy_reg + (0x00dF << 1), 0x0005);
				phy_writew(combo0->phy_reg + (0x009c << 1), 0x00cf);
				phy_writew(combo0->phy_reg + (0x009e << 1), 0x00ce);
				phy_writew(combo0->phy_reg + (0x009F << 1), 0x0005);

				//DP_LANE_SET
				//lane mapping PHY_PMA_LANE_MAP
				phy_writew(combo0->phy_reg + (0xc010 << 1), 0x51d9);
				//phy DP lane enabled
				phy_writew(combo0->phy_reg + (0xc011 << 1), 0x0100);
				//PHY_PMA_ISO_PLL_CTRL0   cmn_pll1_clk_en
				phy_writew(combo0->phy_reg + (0xe003 << 1), 0x0003);
				phy_writew(combo0->phy_reg + (0xe005 << 1), 0x000B);
				//PHY_PMA_ISO_PLL_CTRL1
				phy_writew(combo0->phy_reg + (0xe006 << 1), 0x2224);
				//TX_PSC_A0
				phy_writew(combo0->phy_reg + (0x4100 << 1), 0x00FB);
				phy_writew(combo0->phy_reg + (0x4300 << 1), 0x00FB);
				phy_writew(combo0->phy_reg + (0x4500 << 1), 0x00FB);
				phy_writew(combo0->phy_reg + (0x4700 << 1), 0x00FB);
				phy_writew(combo0->phy_reg + (0x4102 << 1), 0x04AA);
				phy_writew(combo0->phy_reg + (0x4302 << 1), 0x04AA);
				phy_writew(combo0->phy_reg + (0x4502 << 1), 0x04AA);
				phy_writew(combo0->phy_reg + (0x4702 << 1), 0x04AA);
				phy_writew(combo0->phy_reg + (0x4102 << 1), 0x04AA);
				phy_writew(combo0->phy_reg + (0x4302 << 1), 0x04AA);
				phy_writew(combo0->phy_reg + (0x4502 << 1), 0x04AA);
				phy_writew(combo0->phy_reg + (0x4702 << 1), 0x04AA);

				//RX all set to 0
				phy_writew(combo0->phy_reg + (0x8000 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8200 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8400 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8600 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8000 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8202 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8402 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8602 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8002 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8203 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8403 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8603 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8006 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8206 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8406 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8606 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8108 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8308 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8508 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8708 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8110 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8310 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8510 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8710 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8118 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8318 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8518 << 1), 0x0);
				phy_writew(combo0->phy_reg + (0x8718 << 1), 0x0);

				//XCVR_DIAG_BIDI_CTRL
				phy_writew(combo0->phy_reg + (0x40EA << 1), 0x000F);
				phy_writew(combo0->phy_reg + (0x42EA << 1), 0x000F);
				phy_writew(combo0->phy_reg + (0x44EA << 1), 0x000F);
				phy_writew(combo0->phy_reg + (0x46EA << 1), 0x000F);

				//TX_TXCC_CTRL
				phy_writew(combo0->phy_reg + (0x4040 << 1), 0x08A4);
				phy_writew(combo0->phy_reg + (0x4240 << 1), 0x08A4);
				phy_writew(combo0->phy_reg + (0x4440 << 1), 0x08A4);
				phy_writew(combo0->phy_reg + (0x4640 << 1), 0x08A4);

				//DRV_DIAG_TX_DRV
				phy_writew(combo0->phy_reg + (0x40C6 << 1), 0x0003);
				phy_writew(combo0->phy_reg + (0x42C6 << 1), 0x0003);
				phy_writew(combo0->phy_reg + (0x44C6 << 1), 0x0003);
				phy_writew(combo0->phy_reg + (0x46C6 << 1), 0x0003);
				phy_writew(combo0->phy_reg + (0x4050 << 1), 0x002A);
				phy_writew(combo0->phy_reg + (0x4250 << 1), 0x002A);
				phy_writew(combo0->phy_reg + (0x4450 << 1), 0x002A);
				phy_writew(combo0->phy_reg + (0x4650 << 1), 0x002A);
				phy_writew(combo0->phy_reg + (0x404C << 1), 0x0000);
				phy_writew(combo0->phy_reg + (0x424C << 1), 0x0000);
				phy_writew(combo0->phy_reg + (0x444C << 1), 0x0000);
				phy_writew(combo0->phy_reg + (0x464C << 1), 0x0000);

				//CMN-pull-up-down res cal
				phy_writew(combo0->phy_reg + (0x0103 << 1), 0x007F);
				phy_writew(combo0->phy_reg + (0x010B << 1), 0x007F);

				//ssc
				if (combo0->ssc_en)
					phy_writew(combo0->phy_reg + (0x00D8 << 1), 0x0001);
				else
					phy_writew(combo0->phy_reg + (0x00D8 << 1), 0x0002);
				phy_writew(combo0->phy_reg + (0x00D9 << 1), 0x04C4);
				phy_writew(combo0->phy_reg + (0x00DA << 1), 0x006A);
				phy_writew(combo0->phy_reg + (0x00DB << 1), 0x0004);

				phy_writew(combo0->phy_reg + (0x0098 << 1), 0x0001);
				phy_writew(combo0->phy_reg + (0x00D9 << 1), 0x045f);
				phy_writew(combo0->phy_reg + (0x009A << 1), 0x006b);
				phy_writew(combo0->phy_reg + (0x009B << 1), 0x0004);

				// pll configure
				phy_writew(combo0->phy_reg + (0x00d4 << 1), 0x0004);
				phy_writew(combo0->phy_reg + (0x01C4 << 1), 0x0509);
				phy_writew(combo0->phy_reg + (0x01C5 << 1), 0x0F00);
				phy_writew(combo0->phy_reg + (0x01C6 << 1), 0x0F08);

				phy_writew(combo0->phy_reg + (0x00d0 << 1), 0x019f);
				phy_writew(combo0->phy_reg + (0x00d1 << 1), 0x6276);
				phy_writew(combo0->phy_reg + (0x00d2 << 1), 0x0002);
				phy_writew(combo0->phy_reg + (0x00d3 << 1), 0x0116);
				phy_writew(combo0->phy_reg + (0x01c0 << 1), 0x0002);

				//SET common  PLL1_CLOCK0
				phy_writew(combo0->phy_reg + (0x1c1 << 1), 0x0601);
				phy_writew(combo0->phy_reg + (0x004B << 1), 0x0);

				//dp XCVR(LANE) pll1 seting
				phy_writew(combo0->phy_reg + (0x40e6 << 1), 0x0001);
				phy_writew(combo0->phy_reg + (0x42e6 << 1), 0x0001);
				phy_writew(combo0->phy_reg + (0x44e6 << 1), 0x0001);
				phy_writew(combo0->phy_reg + (0x46e6 << 1), 0x0001);

				phy_writew(combo0->phy_reg + (0x40e7 << 1), 0x0000);
				phy_writew(combo0->phy_reg + (0x42e7 << 1), 0x0000);
				phy_writew(combo0->phy_reg + (0x44e7 << 1), 0x0000);
				phy_writew(combo0->phy_reg + (0x46e7 << 1), 0x0000);

				phy_writew(combo0->phy_reg + (0x40e5 << 1), 0x0019);
				phy_writew(combo0->phy_reg + (0x42e5 << 1), 0x0019);
				phy_writew(combo0->phy_reg + (0x44e5 << 1), 0x0019);
				phy_writew(combo0->phy_reg + (0x46e5 << 1), 0x0019);
				phy_writew(combo0->phy_reg + (0xe006 << 1), 0x2224);

				phy_writew(combo0->phy_reg + (0x1A1 << 1), 0x8600);
				phy_writew(combo0->phy_reg + (0x0094 << 1), 0x0004);
				phy_writew(combo0->phy_reg + (0x01A4 << 1), 0x0509);
				phy_writew(combo0->phy_reg + (0x01A5 << 1), 0x0F00);
				phy_writew(combo0->phy_reg + (0x01A6 << 1), 0x0F08);
				phy_writew(combo0->phy_reg + (0x0090 << 1), 0x0180);
				phy_writew(combo0->phy_reg + (0x0091 << 1), 0x9DBA);
				phy_writew(combo0->phy_reg + (0x0092 << 1), 0x0002);
				phy_writew(combo0->phy_reg + (0x0093 << 1), 0x0102);
				phy_writew(combo0->phy_reg + (0x01A0 << 1), 0x0002);
				break;
			case 8100:
				break;
			default:
				pr_err("[sunxi cadence phy]: not support rate(%d Mb/s) for dp\n", dp_config->link_rate);
				return -1;
			}

			combo0_dp_phy_reset(combo0, true);
			combo0->link_rate = dp_config->link_rate;
		}

	}

	return 0;
}
static int combo0_dp_set_lanes(struct sunxi_cadence_combophy *combo0,
			       struct phy_configure_opts_dp *dp_config)
{

	if (dp_config->set_lanes == 1) {
		/* usb plugout would clear congiguration, we should re-config it */
		if (!COMBO_PHY_TYPE(combo0->mode, PHY_TYPE_USB3)) {
			combo0_dp_phy_reset(combo0, false);
			combo0_configure_usb_dp(combo0);
			combo0_dp_phy_reset(combo0, true);
		}

		/* usb_dp_state need enable all lanes */
		if (combo0->usb_dp_state) {
				phy_writew(combo0->phy_reg + (0x4011 << 1), 0x0100);
		} else {
			switch (dp_config->lanes) {
			case 1:
				// close lane 321
				phy_writew(combo0->phy_reg + (0x4011 << 1), 0x010E);
				break;
			case 2:
				// close lane 32
				phy_writew(combo0->phy_reg + (0x4011 << 1), 0x010C);
				break;
			case 4:
				phy_writew(combo0->phy_reg + (0x4011 << 1), 0x0100);
				break;
			default:
				pr_err("[sunxi cadence phy]: not support lanes(%d) for dp\n", dp_config->lanes);
				return -1;;
			}
		}
	}

	return 0;
}
static int combo0_dp_set_sw_pre(struct sunxi_cadence_combophy *combo0,
			       struct phy_configure_opts_dp *dp_config)
{
	int i = 0;
	u16 lane_txdrv = 0;
	u16 lane_mgnfs = 0;
	u16 lane_cpost = 0;
	unsigned int sw = 0;
	unsigned int pre = 0;
	u32 offset;

	if (dp_config->set_voltages == 1) {
		/* configure lane before sw-pre */
		for (i = 0; i < dp_config->lanes; i++) {
			if (combo0->usb_dp_state) {
				if (combo0->orientation == TYPEC_ORIENTATION_REVERSE)
					offset = combo0->typec_remap[MAX_LANE_NO - i] * 0x200;
				else
					offset = combo0->typec_remap[i] * 0x200;
				phy_writew(combo0->phy_reg + ((0x41E7 + offset) << 1), 0x0001);
			} else {
				offset = i * 0x200;
				phy_writew(combo0->phy_reg + ((0x41E7 + offset) << 1), 0x0001);
			}
		}

		udelay(10);

		for (i = 0; i < dp_config->lanes; i++) {
			if (combo0->usb_dp_state) {
				if (combo0->orientation == TYPEC_ORIENTATION_REVERSE)
					offset = combo0->typec_remap[MAX_LANE_NO - i] * 0x200;
				else
					offset = combo0->typec_remap[i] * 0x200;
				phy_writew(combo0->phy_reg + ((0x4040 + offset) << 1), 0x08A4);
			} else {
				offset = i * 0x200;
				phy_writew(combo0->phy_reg + ((0x4040 + offset) << 1), 0x08A4);
			}
		}
		// end of configure

		for (i = 0; i < dp_config->lanes; i++) {
			sw = dp_config->voltage[i];
			pre = dp_config->pre[i];

			switch (combo0->link_rate) {
			case 5400:
				lane_txdrv = dp_lv_table_5g4[sw][pre].lane_txdrv[i];
				lane_mgnfs = dp_lv_table_5g4[sw][pre].lane_mgnfs[i];
				lane_cpost = dp_lv_table_5g4[sw][pre].lane_cpost[i];
				break;
			case 2700:
				lane_txdrv = dp_lv_table_2g7[sw][pre].lane_txdrv[i];
				lane_mgnfs = dp_lv_table_2g7[sw][pre].lane_mgnfs[i];
				lane_cpost = dp_lv_table_2g7[sw][pre].lane_cpost[i];
				break;
			case 1620:
				if (combo0->usb_dp_state) {
					pr_err("[sunxi cadence phy]: not support rate(%d Mb/s) for dp signal level\n",
					       dp_config->link_rate);
					return -1;
				}
				lane_txdrv = dp_lv_table_1g62[sw][pre].lane_txdrv[i];
				lane_mgnfs = dp_lv_table_1g62[sw][pre].lane_mgnfs[i];
				lane_cpost = dp_lv_table_1g62[sw][pre].lane_cpost[i];
				break;
			default:
				pr_err("[sunxi cadence phy]: not support rate(%d Mb/s) for dp signal level\n",
				       dp_config->link_rate);
				return -1;
			}

			if (combo0->usb_dp_state) {
				/* FIXME:
				 * this usp_dp param only works if dp->typec connection use
				 * standard hardware design in 733 evb/P50AI */
				if (combo0->orientation == TYPEC_ORIENTATION_REVERSE) {
					offset = combo0->typec_remap[MAX_LANE_NO - i] * 0x400;
					phy_writew(combo0->phy_reg + (0x818C + offset), lane_txdrv);
					phy_writew(combo0->phy_reg + (0x80A0 + offset), lane_mgnfs);
					phy_writew(combo0->phy_reg + (0x8098 + offset), lane_cpost);
				} else {
					offset = combo0->typec_remap[i] * 0x400;
					phy_writew(combo0->phy_reg + (0x818C + offset), lane_txdrv);
					phy_writew(combo0->phy_reg + (0x80A0 + offset), lane_mgnfs);
					phy_writew(combo0->phy_reg + (0x8098 + offset), lane_cpost);
				}
			} else {
				phy_writew(combo0->phy_reg + (0x818C + (i * 0x400)), lane_txdrv);
				phy_writew(combo0->phy_reg + (0x80A0 + (i * 0x400)), lane_mgnfs);
				phy_writew(combo0->phy_reg + (0x8098 + (i * 0x400)), lane_cpost);
			}

		}

		/* configure after sw-pre */
		udelay(10);

		for (i = 0; i < dp_config->lanes; i++) {
			if (combo0->usb_dp_state) {
				if (combo0->orientation == TYPEC_ORIENTATION_REVERSE)
					offset = combo0->typec_remap[MAX_LANE_NO - i] * 0x200;
				else
					offset = combo0->typec_remap[i] * 0x200;
				phy_writew(combo0->phy_reg + ((0x41E7 + offset) << 1), 0x0000);
			} else {
				offset = i * 0x200;
				phy_writew(combo0->phy_reg + ((0x41E7 + offset) << 1), 0x0000);
			}
		}
	}

	return 0;
}
static int combo0_configure_usb_dp(struct sunxi_cadence_combophy *combo0)
{
	u32 val = 0;

	pr_debug("==================== orientation: %s USB configured: %s ====================",
		 typec_orientation_name[combo0->orientation], COMBO_PHY_TYPE(combo0->mode, PHY_TYPE_USB3) ? "yes" : "no");

	val = readl(COMB0_REG_PHYPINS_LINK_CTRL2(combo0->top_reg));
	val &= ~(PHY0_IDDQ_EN_SEL | PHY0_IDDQ_EN_SOFT);
	writel(val, COMB0_REG_PHYPINS_LINK_CTRL2(combo0->top_reg));

	phy_writew(combo0->phy_reg + (0xE005 << 1), 0x0003);
	phy_writew(combo0->phy_reg + (0xE006 << 1), 0x1124);
	if (combo0->orientation == TYPEC_ORIENTATION_REVERSE) {
		/* Restore DP Normal orientation register configuration */
		phy_writew(combo0->phy_reg + (0x4500 << 1), 0x00FF);
		phy_writew(combo0->phy_reg + (0x4700 << 1), 0x00FF);
		phy_writew(combo0->phy_reg + (0x4502 << 1), 0x04AE);
		phy_writew(combo0->phy_reg + (0x4702 << 1), 0x04AE);
		phy_writew(combo0->phy_reg + (0x4503 << 1), 0x04AE);
		phy_writew(combo0->phy_reg + (0x4703 << 1), 0x04AE);
		phy_writew(combo0->phy_reg + (0x8400 << 1), 0x091D);
		phy_writew(combo0->phy_reg + (0x8600 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8402 << 1), 0x0900);
		phy_writew(combo0->phy_reg + (0x8602 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8403 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8603 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8406 << 1), 0x010F);
		phy_writew(combo0->phy_reg + (0x8606 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x44EA << 1), 0x00FF);
		phy_writew(combo0->phy_reg + (0x46EA << 1), 0x00FF);
		phy_writew(combo0->phy_reg + (0x8508 << 1), 0x0009);
		phy_writew(combo0->phy_reg + (0x8708 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8510 << 1), 0x0009);
		phy_writew(combo0->phy_reg + (0x8710 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8518 << 1), 0x000D);
		phy_writew(combo0->phy_reg + (0x8718 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x4440 << 1), 0x2A84);
		phy_writew(combo0->phy_reg + (0x4640 << 1), 0x2A84);
		phy_writew(combo0->phy_reg + (0x44C6 << 1), 0x00A3);
		phy_writew(combo0->phy_reg + (0x46C6 << 1), 0x00A3);
		phy_writew(combo0->phy_reg + (0x4450 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x4650 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x444C << 1), 0x001C);
		phy_writew(combo0->phy_reg + (0x464C << 1), 0x001C);
	} else {
		/* Restore DP Reverse orientation register configuration */
		phy_writew(combo0->phy_reg + (0x4100 << 1), 0x00FF);
		phy_writew(combo0->phy_reg + (0x4300 << 1), 0x00FF);
		phy_writew(combo0->phy_reg + (0x4102 << 1), 0x04AE);
		phy_writew(combo0->phy_reg + (0x4302 << 1), 0x04AE);
		phy_writew(combo0->phy_reg + (0x4103 << 1), 0x04AE);
		phy_writew(combo0->phy_reg + (0x4303 << 1), 0x04AE);
		phy_writew(combo0->phy_reg + (0x8000 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8200 << 1), 0x091D);
		phy_writew(combo0->phy_reg + (0x8002 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8202 << 1), 0x0900);
		phy_writew(combo0->phy_reg + (0x8003 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8203 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8006 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8206 << 1), 0x010F);
		phy_writew(combo0->phy_reg + (0x40EA << 1), 0x00FF);
		phy_writew(combo0->phy_reg + (0x42EA << 1), 0x00FF);
		phy_writew(combo0->phy_reg + (0x8108 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8308 << 1), 0x0009);
		phy_writew(combo0->phy_reg + (0x8110 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8310 << 1), 0x0009);
		phy_writew(combo0->phy_reg + (0x8118 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8318 << 1), 0x000D);
		phy_writew(combo0->phy_reg + (0x4040 << 1), 0x2A84);
		phy_writew(combo0->phy_reg + (0x4240 << 1), 0x2A84);
		phy_writew(combo0->phy_reg + (0x40C6 << 1), 0x00A3);
		phy_writew(combo0->phy_reg + (0x42C6 << 1), 0x00A3);
		phy_writew(combo0->phy_reg + (0x4050 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x4250 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x404C << 1), 0x001C);
		phy_writew(combo0->phy_reg + (0x424C << 1), 0x001C);
	}

	phy_writel(combo0->top_reg + 0x4, 0x01100001);
	phy_writew(combo0->phy_reg + (0x0022 << 1), 0x001A);
	phy_writew(combo0->phy_reg + (0x002A << 1), 0x0034);
	phy_writew(combo0->phy_reg + (0x002C << 1), 0x00DA);
	phy_writew(combo0->phy_reg + (0x0032 << 1), 0x0034);
	phy_writew(combo0->phy_reg + (0x0034 << 1), 0x00DA);
	phy_writew(combo0->phy_reg + (0x0064 << 1), 0x0082);
	phy_writew(combo0->phy_reg + (0x0065 << 1), 0x0082);
	phy_writew(combo0->phy_reg + (0x0074 << 1), 0x001A);
	phy_writew(combo0->phy_reg + (0x0104 << 1), 0x0020);
	phy_writew(combo0->phy_reg + (0x0105 << 1), 0x0007);
	phy_writew(combo0->phy_reg + (0x010C << 1), 0x0020);
	phy_writew(combo0->phy_reg + (0x010D << 1), 0x0007);
	phy_writew(combo0->phy_reg + (0x0114 << 1), 0x030C);
	phy_writew(combo0->phy_reg + (0x0115 << 1), 0x0007);
	phy_writew(combo0->phy_reg + (0x0124 << 1), 0x0007);
	phy_writew(combo0->phy_reg + (0x0125 << 1), 0x0003);
	phy_writew(combo0->phy_reg + (0x0126 << 1), 0x000F);
	phy_writew(combo0->phy_reg + (0x0128 << 1), 0x0132);
	phy_writew(combo0->phy_reg + (0x8044 << 1), 0x001A);
	phy_writew(combo0->phy_reg + (0x8244 << 1), 0x001A);
	phy_writew(combo0->phy_reg + (0x8444 << 1), 0x001A);
	phy_writew(combo0->phy_reg + (0x8644 << 1), 0x001A);
	phy_writew(combo0->phy_reg + (0x8045 << 1), 0x0082);
	phy_writew(combo0->phy_reg + (0x8245 << 1), 0x0082);
	phy_writew(combo0->phy_reg + (0x8445 << 1), 0x0082);
	phy_writew(combo0->phy_reg + (0x8645 << 1), 0x0082);
	phy_writew(combo0->phy_reg + (0x804C << 1), 0x001A);
	phy_writew(combo0->phy_reg + (0x824C << 1), 0x001A);
	phy_writew(combo0->phy_reg + (0x844C << 1), 0x001A);
	phy_writew(combo0->phy_reg + (0x864C << 1), 0x001A);
	phy_writew(combo0->phy_reg + (0x804D << 1), 0x0082);
	phy_writew(combo0->phy_reg + (0x824D << 1), 0x0082);
	phy_writew(combo0->phy_reg + (0x844D << 1), 0x0082);
	phy_writew(combo0->phy_reg + (0x864D << 1), 0x0082);
	phy_writew(combo0->phy_reg + (0x4123 << 1), 0x0A28);
	phy_writew(combo0->phy_reg + (0x4323 << 1), 0x0A28);
	phy_writew(combo0->phy_reg + (0x4523 << 1), 0x0A28);
	phy_writew(combo0->phy_reg + (0x4723 << 1), 0x0A28);
	phy_writew(combo0->phy_reg + (0x01A1 << 1), 0x8600);
	phy_writew(combo0->phy_reg + (0x01C1 << 1), 0x0701);

	if (combo0->orientation == TYPEC_ORIENTATION_REVERSE) {
		/* Restore USB Normal orientation register configuration */
		phy_writew(combo0->phy_reg + (0x40E5 << 1), 0x0012);
		phy_writew(combo0->phy_reg + (0x42E5 << 1), 0x0012);
		phy_writew(combo0->phy_reg + (0x44E6 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x46E6 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x44E7 << 1), 0x0001);
		phy_writew(combo0->phy_reg + (0x46E7 << 1), 0x0001);
		phy_writew(combo0->phy_reg + (0x44E5 << 1), 0x0012);
		phy_writew(combo0->phy_reg + (0x46E5 << 1), 0x0012);

		phy_writew(combo0->phy_reg + (0x44E5 << 1), 0x0041);
		phy_writew(combo0->phy_reg + (0x46E5 << 1), 0x0041);
		phy_writew(combo0->phy_reg + (0x40E6 << 1), 0x0001);
		phy_writew(combo0->phy_reg + (0x42E6 << 1), 0x0001);
		phy_writew(combo0->phy_reg + (0x40E7 << 1), 0x0001);
		phy_writew(combo0->phy_reg + (0x42E7 << 1), 0x0001);
		phy_writew(combo0->phy_reg + (0x40E5 << 1), 0x0019);
		phy_writew(combo0->phy_reg + (0x42E5 << 1), 0x0019);
	} else {
		/* Restore USB Reverse orientation register configuration */
		phy_writew(combo0->phy_reg + (0x44E5 << 1), 0x0012);
		phy_writew(combo0->phy_reg + (0x46E5 << 1), 0x0012);
		phy_writew(combo0->phy_reg + (0x40E6 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x42E6 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x40E7 << 1), 0x0001);
		phy_writew(combo0->phy_reg + (0x42E7 << 1), 0x0001);
		phy_writew(combo0->phy_reg + (0x40E5 << 1), 0x0012);
		phy_writew(combo0->phy_reg + (0x42E5 << 1), 0x0012);

		phy_writew(combo0->phy_reg + (0x40E5 << 1), 0x0041);
		phy_writew(combo0->phy_reg + (0x42E5 << 1), 0x0041);
		phy_writew(combo0->phy_reg + (0x44E6 << 1), 0x0001);
		phy_writew(combo0->phy_reg + (0x46E6 << 1), 0x0001);
		phy_writew(combo0->phy_reg + (0x44E7 << 1), 0x0001);
		phy_writew(combo0->phy_reg + (0x46E7 << 1), 0x0001);
		phy_writew(combo0->phy_reg + (0x44E5 << 1), 0x0019);
		phy_writew(combo0->phy_reg + (0x46E5 << 1), 0x0019);
	}

	phy_writew(combo0->phy_reg + (0x0094 << 1), 0x0004);
	phy_writew(combo0->phy_reg + (0x00D4 << 1), 0x0004);
	phy_writew(combo0->phy_reg + (0x01A4 << 1), 0x0509);
	phy_writew(combo0->phy_reg + (0x01C4 << 1), 0x0509);
	phy_writew(combo0->phy_reg + (0x01A5 << 1), 0x0F00);
	phy_writew(combo0->phy_reg + (0x01C5 << 1), 0x0F00);
	phy_writew(combo0->phy_reg + (0x01A6 << 1), 0x0F08);
	phy_writew(combo0->phy_reg + (0x01C6 << 1), 0x0F08);
	phy_writew(combo0->phy_reg + (0x0090 << 1), 0x0180);
	phy_writew(combo0->phy_reg + (0x00D0 << 1), 0x019F);
	phy_writew(combo0->phy_reg + (0x0091 << 1), 0x9D8A);
	phy_writew(combo0->phy_reg + (0x00D1 << 1), 0x6276);
	phy_writew(combo0->phy_reg + (0x0092 << 1), 0x0002);
	phy_writew(combo0->phy_reg + (0x00D2 << 1), 0x0002);
	phy_writew(combo0->phy_reg + (0x0093 << 1), 0x0102);
	phy_writew(combo0->phy_reg + (0x00D3 << 1), 0x0116);
	phy_writew(combo0->phy_reg + (0x01A0 << 1), 0x0002);
	phy_writew(combo0->phy_reg + (0x01C0 << 1), 0x0002);
	phy_writew(combo0->phy_reg + (0x0098 << 1), 0x0001);
	if (combo0->ssc_en)
		phy_writew(combo0->phy_reg + (0x00D8 << 1), 0x0001);
	else
		phy_writew(combo0->phy_reg + (0x00D8 << 1), 0x0002);
	phy_writew(combo0->phy_reg + (0x0099 << 1), 0x045F);
	phy_writew(combo0->phy_reg + (0x00D9 << 1), 0x04C4);
	phy_writew(combo0->phy_reg + (0x009A << 1), 0x006B);
	phy_writew(combo0->phy_reg + (0x00DA << 1), 0x006A);
	phy_writew(combo0->phy_reg + (0x009B << 1), 0x0004);
	phy_writew(combo0->phy_reg + (0x00DB << 1), 0x0004);
	phy_writew(combo0->phy_reg + (0x0084 << 1), 0x0104);
	phy_writew(combo0->phy_reg + (0x00C4 << 1), 0x0104);
	phy_writew(combo0->phy_reg + (0x0085 << 1), 0x0005);
	phy_writew(combo0->phy_reg + (0x00C5 << 1), 0x0005);
	phy_writew(combo0->phy_reg + (0x0086 << 1), 0x0337);
	phy_writew(combo0->phy_reg + (0x00C6 << 1), 0x0337);
	phy_writew(combo0->phy_reg + (0x0088 << 1), 0x0335);
	phy_writew(combo0->phy_reg + (0x00C8 << 1), 0x0335);
	phy_writew(combo0->phy_reg + (0x0082 << 1), 0x0003);
	phy_writew(combo0->phy_reg + (0x00C2 << 1), 0x0003);
	phy_writew(combo0->phy_reg + (0x009C << 1), 0x00CF);
	phy_writew(combo0->phy_reg + (0x00DC << 1), 0x00CF);
	phy_writew(combo0->phy_reg + (0x009E << 1), 0x00CE);
	phy_writew(combo0->phy_reg + (0x00DE << 1), 0x00CE);
	phy_writew(combo0->phy_reg + (0x009F << 1), 0x0005);
	phy_writew(combo0->phy_reg + (0x00DF << 1), 0x0005);

	val = readl(COMB0_REG_PHYPINS_LINK_CTRL0(combo0->top_reg));
	if (combo0->orientation == TYPEC_ORIENTATION_REVERSE)
		val |= PHY0_TYPEC_CONN_DIR;
	else
		val &= ~PHY0_TYPEC_CONN_DIR;
	writel(val, COMB0_REG_PHYPINS_LINK_CTRL0(combo0->top_reg));

	phy_writew(combo0->phy_reg + (0xC010 << 1), 0x5100);
	phy_writew(combo0->phy_reg + (0xC011 << 1), 0x0100);
	phy_writew(combo0->phy_reg + (0xC018 << 1), 0x0A0A);
	phy_writew(combo0->phy_reg + (0xC01A << 1), 0x1000);
	phy_writew(combo0->phy_reg + (0xC01B << 1), 0x0010);
	phy_writew(combo0->phy_reg + (0x0041 << 1), 0x8200);
	phy_writew(combo0->phy_reg + (0x0047 << 1), 0x8200);

	if (combo0->orientation == TYPEC_ORIENTATION_REVERSE) {
		/* Restore USB Normal orientation register configuration */
		phy_writew(combo0->phy_reg + (0x4100 << 1), 0x00FF);
		phy_writew(combo0->phy_reg + (0x4101 << 1), 0x04AF);
		phy_writew(combo0->phy_reg + (0x4102 << 1), 0x04AE);
		phy_writew(combo0->phy_reg + (0x4103 << 1), 0x04AE);
		phy_writew(combo0->phy_reg + (0x8200 << 1), 0x091D);
		phy_writew(combo0->phy_reg + (0x8201 << 1), 0x091D);
		phy_writew(combo0->phy_reg + (0x8202 << 1), 0x0900);
		phy_writew(combo0->phy_reg + (0x8203 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x4040 << 1), 0x2A84);
		phy_writew(combo0->phy_reg + (0x404D << 1), 0x0011);
		phy_writew(combo0->phy_reg + (0x8290 << 1), 0x000C);
		phy_writew(combo0->phy_reg + (0x8308 << 1), 0x0009);
		phy_writew(combo0->phy_reg + (0x8349 << 1), 0x0c02);
		phy_writew(combo0->phy_reg + (0x8377 << 1), 0x06F6);
		phy_writew(combo0->phy_reg + (0x8378 << 1), 0x4606);
		phy_writew(combo0->phy_reg + (0x42EB << 1), 0x0006);
		phy_writew(combo0->phy_reg + (0x8371 << 1), 0x0519);
		phy_writew(combo0->phy_reg + (0x8372 << 1), 0x0519);
		phy_writew(combo0->phy_reg + (0x83E8 << 1), 0x1002);
		phy_writew(combo0->phy_reg + (0x83E5 << 1), 0x0B98);
		phy_writew(combo0->phy_reg + (0x83E2 << 1), 0x0C01);
		phy_writew(combo0->phy_reg + (0x83E3 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x83F5 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x83F4 << 1), 0x0311);
		phy_writew(combo0->phy_reg + (0x83FF << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x81FF << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8280 << 1), 0x010a);
		phy_writew(combo0->phy_reg + (0x8282 << 1), 0x0003);
		phy_writew(combo0->phy_reg + (0x40EA << 1), 0x00FF);
		phy_writew(combo0->phy_reg + (0x42EA << 1), 0x00FF);

		phy_writew(combo0->phy_reg + (0x4700 << 1), 0x02FF);
		phy_writew(combo0->phy_reg + (0x4701 << 1), 0x06AF);
		phy_writew(combo0->phy_reg + (0x4702 << 1), 0x06AE);
		phy_writew(combo0->phy_reg + (0x4703 << 1), 0x06AE);
		phy_writew(combo0->phy_reg + (0x8400 << 1), 0x0D1D);
		phy_writew(combo0->phy_reg + (0x8401 << 1), 0x0D1D);
		phy_writew(combo0->phy_reg + (0x8402 << 1), 0x0D00);
		phy_writew(combo0->phy_reg + (0x8403 << 1), 0x0500);
		phy_writew(combo0->phy_reg + (0x4640 << 1), 0x2A82);
		phy_writew(combo0->phy_reg + (0x464D << 1), 0x0014);
		phy_writew(combo0->phy_reg + (0x8490 << 1), 0x0013);
		phy_writew(combo0->phy_reg + (0x8508 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8549 << 1), 0x0C02);
		phy_writew(combo0->phy_reg + (0x8577 << 1), 0x0330);
		phy_writew(combo0->phy_reg + (0x8578 << 1), 0x0300);
		phy_writew(combo0->phy_reg + (0x44EB << 1), 0x0003);
		phy_writew(combo0->phy_reg + (0x8571 << 1), 0x0019);
		phy_writew(combo0->phy_reg + (0x8572 << 1), 0x0019);
		phy_writew(combo0->phy_reg + (0x85E8 << 1), 0x1004);
		phy_writew(combo0->phy_reg + (0x85E5 << 1), 0x00F9);
		phy_writew(combo0->phy_reg + (0x85E2 << 1), 0x0C01);
		phy_writew(combo0->phy_reg + (0x85E3 << 1), 0x0002);
		phy_writew(combo0->phy_reg + (0x85F5 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x85F4 << 1), 0x0031);
		phy_writew(combo0->phy_reg + (0x85FF << 1), 0x0001);
		phy_writew(combo0->phy_reg + (0x87FF << 1), 0x0002);
		phy_writew(combo0->phy_reg + (0x8480 << 1), 0x018C);
		phy_writew(combo0->phy_reg + (0x8482 << 1), 0x0003);
		phy_writew(combo0->phy_reg + (0x46EA << 1), 0x000F);
		phy_writew(combo0->phy_reg + (0x44EA << 1), 0x00F0);
	} else {
		/* Restore USB Reverse orientation register configuration */
		phy_writew(combo0->phy_reg + (0x4700 << 1), 0x00FF);
		phy_writew(combo0->phy_reg + (0x4701 << 1), 0x04AF);
		phy_writew(combo0->phy_reg + (0x4702 << 1), 0x04AE);
		phy_writew(combo0->phy_reg + (0x4703 << 1), 0x04AE);
		phy_writew(combo0->phy_reg + (0x8400 << 1), 0x091D);
		phy_writew(combo0->phy_reg + (0x8401 << 1), 0x091D);
		phy_writew(combo0->phy_reg + (0x8402 << 1), 0x0900);
		phy_writew(combo0->phy_reg + (0x8403 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x4640 << 1), 0x2A84);
		phy_writew(combo0->phy_reg + (0x464D << 1), 0x0011);
		phy_writew(combo0->phy_reg + (0x8490 << 1), 0x000C);
		phy_writew(combo0->phy_reg + (0x8508 << 1), 0x0009);
		phy_writew(combo0->phy_reg + (0x8549 << 1), 0x0C02);
		phy_writew(combo0->phy_reg + (0x8577 << 1), 0x06F6);
		phy_writew(combo0->phy_reg + (0x8578 << 1), 0x4606);
		phy_writew(combo0->phy_reg + (0x44EB << 1), 0x0006);
		phy_writew(combo0->phy_reg + (0x8571 << 1), 0x0519);
		phy_writew(combo0->phy_reg + (0x8572 << 1), 0x0519);
		phy_writew(combo0->phy_reg + (0x85E8 << 1), 0x1002);
		phy_writew(combo0->phy_reg + (0x85E5 << 1), 0x0B98);
		phy_writew(combo0->phy_reg + (0x85E2 << 1), 0x0C01);
		phy_writew(combo0->phy_reg + (0x85E3 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x85F5 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x85F4 << 1), 0x0311);
		phy_writew(combo0->phy_reg + (0x85FF << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x87FF << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8480 << 1), 0x010A);
		phy_writew(combo0->phy_reg + (0x8482 << 1), 0x0003);
		phy_writew(combo0->phy_reg + (0x46EA << 1), 0x00FF);
		phy_writew(combo0->phy_reg + (0x44EA << 1), 0x00FF);

		phy_writew(combo0->phy_reg + (0x4100 << 1), 0x02FF);
		phy_writew(combo0->phy_reg + (0x4101 << 1), 0x06AF);
		phy_writew(combo0->phy_reg + (0x4102 << 1), 0x06AE);
		phy_writew(combo0->phy_reg + (0x4103 << 1), 0x06AE);
		phy_writew(combo0->phy_reg + (0x8200 << 1), 0x0D1D);
		phy_writew(combo0->phy_reg + (0x8201 << 1), 0x0D1D);
		phy_writew(combo0->phy_reg + (0x8202 << 1), 0x0D00);
		phy_writew(combo0->phy_reg + (0x8203 << 1), 0x0500);
		phy_writew(combo0->phy_reg + (0x4040 << 1), 0x2A82);
		phy_writew(combo0->phy_reg + (0x404D << 1), 0x0014);
		phy_writew(combo0->phy_reg + (0x8290 << 1), 0x0013);
		phy_writew(combo0->phy_reg + (0x8308 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8349 << 1), 0x0C02);
		phy_writew(combo0->phy_reg + (0x8377 << 1), 0x0330);
		phy_writew(combo0->phy_reg + (0x8378 << 1), 0x0300);
		phy_writew(combo0->phy_reg + (0x42EB << 1), 0x0003);
		phy_writew(combo0->phy_reg + (0x8371 << 1), 0x0019);
		phy_writew(combo0->phy_reg + (0x8372 << 1), 0x0019);
		phy_writew(combo0->phy_reg + (0x83E8 << 1), 0x1004);
		phy_writew(combo0->phy_reg + (0x83E5 << 1), 0x00F9);
		phy_writew(combo0->phy_reg + (0x83E2 << 1), 0x0C01);
		phy_writew(combo0->phy_reg + (0x83E3 << 1), 0x0002);
		phy_writew(combo0->phy_reg + (0x83F5 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x83F4 << 1), 0x0031);
		phy_writew(combo0->phy_reg + (0x83FF << 1), 0x0001);
		phy_writew(combo0->phy_reg + (0x81FF << 1), 0x0002);
		phy_writew(combo0->phy_reg + (0x8280 << 1), 0x018C);
		phy_writew(combo0->phy_reg + (0x8282 << 1), 0x0003);
		phy_writew(combo0->phy_reg + (0x40EA << 1), 0x000F);
		phy_writew(combo0->phy_reg + (0x42EA << 1), 0x00F0);
	}

	phy_writew(combo0->phy_reg + (0xE005 << 1), 0x000B);
	phy_writew(combo0->phy_reg + (0xE006 << 1), 0x2224);
	if (combo0->orientation == TYPEC_ORIENTATION_REVERSE) {
		phy_writew(combo0->phy_reg + (0x4100 << 1), 0x00FB);
		phy_writew(combo0->phy_reg + (0x4300 << 1), 0x00FB);
		phy_writew(combo0->phy_reg + (0x4102 << 1), 0x04AA);
		phy_writew(combo0->phy_reg + (0x4302 << 1), 0x04AA);
		phy_writew(combo0->phy_reg + (0x4103 << 1), 0x04AA);
		phy_writew(combo0->phy_reg + (0x4303 << 1), 0x04AA);
		phy_writew(combo0->phy_reg + (0x8000 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8200 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8002 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8202 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8003 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8203 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8006 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8206 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x40EA << 1), 0x000F);
		phy_writew(combo0->phy_reg + (0x42EA << 1), 0x000F);
		phy_writew(combo0->phy_reg + (0x8108 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8308 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8110 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8310 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8118 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8318 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x4040 << 1), 0x08A4);
		phy_writew(combo0->phy_reg + (0x4240 << 1), 0x08A4);
		phy_writew(combo0->phy_reg + (0x40C6 << 1), 0x0003);
		phy_writew(combo0->phy_reg + (0x42C6 << 1), 0x0003);
		phy_writew(combo0->phy_reg + (0x4050 << 1), 0x002A);
		phy_writew(combo0->phy_reg + (0x4250 << 1), 0x002A);
		phy_writew(combo0->phy_reg + (0x404C << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x424C << 1), 0x0000);
	} else {
		phy_writew(combo0->phy_reg + (0x4500 << 1), 0x00FB);
		phy_writew(combo0->phy_reg + (0x4700 << 1), 0x00FB);
		phy_writew(combo0->phy_reg + (0x4502 << 1), 0x04AA);
		phy_writew(combo0->phy_reg + (0x4702 << 1), 0x04AA);
		phy_writew(combo0->phy_reg + (0x4503 << 1), 0x04AA);
		phy_writew(combo0->phy_reg + (0x4703 << 1), 0x04AA);
		phy_writew(combo0->phy_reg + (0x8400 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8600 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8402 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8602 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8403 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8603 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8406 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8606 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x44EA << 1), 0x000F);
		phy_writew(combo0->phy_reg + (0x46EA << 1), 0x000F);
		phy_writew(combo0->phy_reg + (0x8508 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8708 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8510 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8710 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8518 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x8718 << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x4440 << 1), 0x08A4);
		phy_writew(combo0->phy_reg + (0x4640 << 1), 0x08A4);
		phy_writew(combo0->phy_reg + (0x44C6 << 1), 0x0003);
		phy_writew(combo0->phy_reg + (0x46C6 << 1), 0x0003);
		phy_writew(combo0->phy_reg + (0x4450 << 1), 0x002A);
		phy_writew(combo0->phy_reg + (0x4650 << 1), 0x002A);
		phy_writew(combo0->phy_reg + (0x444C << 1), 0x0000);
		phy_writew(combo0->phy_reg + (0x464C << 1), 0x0000);
	}
	phy_writew(combo0->phy_reg + (0x0103 << 1), 0x007F);
	phy_writew(combo0->phy_reg + (0x010B << 1), 0x007F);

	return 0;
}

static void construct(struct a733_dp *d,struct sunxi_cadence_combophy *c,struct sunxi_cadence_phy *p)
{
 memset(c,0,sizeof(*c));p->top_combo_reg=0x06c06000;
 c->dp=d;c->sunxi_cphy=p;c->top_reg=0x06c01000;c->phy_reg=0x06c80000;
 c->orientation=d->config.polarity;c->link_rate=1620;
 for(unsigned i=0;i<4;i++){c->lane_remap[i]=i;c->typec_remap[i]=i;c->lane_invert[i]=1;}
}
int a733_dp_cadence_prepare(struct a733_dp *d)
{
 struct sunxi_cadence_phy p;struct sunxi_cadence_combophy c,*combo0=&c;
 struct phy_configure_opts_dp o={.set_lanes=1,.set_rate=1,.lanes=4,.link_rate=1620};
 construct(d,&c,&p);
 /* Exact-board lane_invert1111, identity remap. Orientation is handled by
  * PHY0_TYPEC_CONN_DIR in reset, not by reversing the remap as well. */
 phy_writel(p.top_combo_reg+0xc24,0x3210);
 for(unsigned i=0;i<4;i++){
  uintptr_t a=c.phy_reg+((0xf000+i*0x100)<<1);
  phy_writew(a,phy_readw(a)|(1u<<8));
 }
 if(combo0_dp_set_lanes(&c,&o)||d->failed||combo0_dp_set_rate(&c,&o)||d->failed)return 0;
 /* Source aux_hpd power_on, with independent retained supplies/reference
  * established by native construction before any of these accesses. */
 if(!a733_dp_wait(d,10000))return 0;
 const uintptr_t aux=0x06c01e00;
 phy_set_lbits(aux,24,4,3);phy_set_lbits(aux,20,2,3);
 phy_set_lbits(aux,16,2,3);phy_set_lbits(aux,12,2,1);phy_set_lbits(aux,8,1,1);
 phy_set_lbits(aux+4,28,1,0);phy_set_lbits(aux+4,24,1,0);
 phy_set_lbits(aux+4,16,1,1);phy_set_lbits(aux+4,8,5,14);phy_set_lbits(aux+4,0,5,13);
 phy_writel(aux+8,0);phy_set_lbits(aux,4,1,1);phy_set_lbits(aux,5,1,1);phy_set_lbits(aux,0,1,1);
 return !d->failed;
}
int a733_dp_cadence_levels(struct a733_dp *d,const uint8_t training[4])
{
 struct sunxi_cadence_phy p;struct sunxi_cadence_combophy c;
 struct phy_configure_opts_dp o={.set_voltages=1,.lanes=4,.link_rate=1620};
 construct(d,&c,&p);
 for(unsigned i=0;i<4;i++){
  o.voltage[i]=training[i]&3;o.pre[i]=(training[i]>>3)&3;
  if(o.voltage[i]+o.pre[i]>3)return a733_dp_fault(d,A733_DP_TRAINING);
 }
 return combo0_dp_set_sw_pre(&c,&o)==0&&!d->failed;
}
