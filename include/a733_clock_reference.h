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
#endif
