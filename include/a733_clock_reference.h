#ifndef SUNSTONE_A733_CLOCK_REFERENCE_H
#define SUNSTONE_A733_CLOCK_REFERENCE_H
#include <stdint.h>
/* Pure rate decoding, not ownership or physical frequency qualification.
 * XO_CTRL0 DCXO_ST:0/3=24MHz,1=19.2MHz,2=26MHz. PLL_REF uses
 * N=(bits15:8)+1 and M=(bits22:16)+1. Callers retain their oscillator
 * lifetime checks; REF validation also requires DCXO enabled/stable. Pinned source anchors: docs/clock-leases.md. */
static inline uint32_t a733_clock_dcxo_hz(uint32_t oscillator)
{
    static const uint32_t rates[4]={24000000,19200000,26000000,24000000};
    return rates[(oscillator>>14)&3];
}
static inline int a733_clock_ref24_valid(uint32_t oscillator,uint32_t reference)
{
    unsigned n=((reference>>8)&255)+1,m=((reference>>16)&127)+1;
    return (oscillator&UINT32_C(0x10000002))==UINT32_C(0x10000002)&&
        (reference&UINT32_C(0xf8000000))==UINT32_C(0xf8000000)&&
        n>=80&&n<=116&&
        (uint64_t)a733_clock_dcxo_hz(oscillator)*n==UINT64_C(24000000)*m;
}
/* PLL_PERI0_CTRL at CCU+0x0a0 (manual 4.1.6.6) with the normalized 24 MHz
 * PLL_REF parent: enable, LDO, lock enable, lock and all three output gates
 * (bits31:25) set, input divider M1=1 (bit1 clear), N=(bits15:8)+1 within
 * 53..105 and PERI0PLL2X=24 MHz*N/P0=1.2 GHz with P0=(bits22:20)+1.
 * Reserved bits24:23 are not selectors and are ignored. Callers still check
 * their own gate, effective-status and No-Auto words. */
static inline int a733_clock_peri0_valid(uint32_t pll)
{
    unsigned n=((pll>>8)&255)+1,p0=((pll>>20)&7)+1;
    return (pll&UINT32_C(0xfe000002))==UINT32_C(0xfe000000)&&n>=53&&n<=105&&24u*n==1200u*p0;
}
#endif
