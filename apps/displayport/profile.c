/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "a733_displayport_application.h"
#include "a733_clock_reference.h"
int a733_displayport_parameters_valid(const struct a733_displayport_session *s)
{
 if(!s||!a733_displayport_fields_valid(s)||a733_clock_dcxo_hz(s->oscillator)!=26000000||
    !a733_clock_ref24_valid(s->oscillator,s->reference))return 0;
 /* The published 1.62Gb/s Cadence table selects a26MHz physical reference.
  * Adopt an integer VIDEO1 PLL, both reference outputs enabled and locked.
  * VIDEO1-4X=24MHz*(N+1)/(M4X+1)=1188MHz; divide16 for74.25MHz.
  * No PLL programming or measured-frequency claim occurs here. */
 unsigned n=((s->video1_pll>>8)&255)+1,m=((s->video1_pll>>20)&7)+1;
 if((s->video1_pll&0xfc000002)!=0xfc000000||n<53||n>105||
    (uint64_t)24000000*n!=(uint64_t)1188000000*m)return 0;
 /* SERDES configuration uses normalized sys24M, divisor1, enabled. RTC
  * SERDES0 receives the physical oscillator through its dedicated bit4 gate.
  * Other RTC clients survive masked writes; this expected word is readback. */
 if((s->serdes_cfg&0x8700001f)!=0x80000000||!(s->rtc_serdes0&16))return 0;
 return 1;
}
