#ifndef SUNSTONE_A733_NPU_PLL_H
#define SUNSTONE_A733_NPU_PLL_H
#include "a733_npu_platform.h"

/* PLL_NPU at CCU+0x2a0 (A733 User Manual V1.00 §4.1.6.56-60): enable 31,
 * LDO 30, lock enable 29, LOCK status 28, output gate 27, factors N 8-15,
 * P0 20-22 and M1 1. The profile is N=82, M1=1, P0=4: a 1,968 MHz
 * oscillator divided to 492 MHz (docs/npu-pll.md). */
#define A733_NPU_PLL_CONTROL UINT32_C(0x020022a0)
#define A733_NPU_PLL_LOCK (UINT32_C(1)<<28)
#define A733_NPU_PLL_HZ UINT32_C(492000000)
/* Method-2 enable of a disabled PLL (manual §4.1.4.1): software gate enable
 * before No-Auto in CCU+0x1920, then LDO, factors, PLL enable and lock
 * enable. The output gate stays closed until LOCK. */
#define A733_NPU_PLL_ENABLE_STEPS 6u
extern const struct a733_npu_step a733_npu_pll_enable[A733_NPU_PLL_ENABLE_STEPS];
/* LOCK sampling (manual §4.1.4.2 and the lock helper of the pinned board
 * clock source): three locked observations in a row, 3 us apart, at most
 * 100 observations, then a 20 us wait. */
#define A733_NPU_PLL_SAMPLE_US 3u
#define A733_NPU_PLL_SAMPLES 100u
#define A733_NPU_PLL_SETTLE_US 20u
/* The control word after a completed enable table, with LOCK clear. */
static inline int a733_npu_pll_locked(uint32_t control)
{return (control&~A733_NPU_PLL_LOCK)==UINT32_C(0xe0305100) && (control&A733_NPU_PLL_LOCK);}
/* Disabled entry state: enable, lock enable, LOCK and output gate clear, and
 * every bit outside the documented fields clear. The LDO and the factors may
 * hold any value; the enable table writes them. */
static inline int a733_npu_pll_disabled(uint32_t control)
{return !(control&UINT32_C(0xb8000000)) && !(control&~UINT32_C(0xf870ff02));}
#endif
