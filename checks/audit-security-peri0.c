/* Table check for the shared PLL_PERI0 validator in a733_clock_reference.h.
 * Pure register decoding; no clock ownership or frequency measurement. */
#include "a733_clock_reference.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    static const struct {uint32_t pll;int valid;} cases[]={
        {0xff126310,1}, /* Observed board value: reserved bit24 set. */
        {0xfe126310,1},{0xfe926310,1}, /* Bit24 clear; reserved bit23 set. */
        {0xff126312,0}, /* Input divider M1=2. */
        {0x7f126310,0},{0xbf126310,0},{0xdf126310,0},{0xef126310,0}, /* Enable, LDO, lock enable, lock. */
        {0xf7126310,0},{0xfb126310,0},{0xfd126310,0}, /* Output gates 0..2. */
        {0xff126210,0},{0xff126410,0}, /* N=99 or 101 misses 1.2 GHz. */
        {0xff023110,0}, /* N=50,P0=1 gives 1.2 GHz but N<53. */
        {0xff229510,0}, /* N=150,P0=3 gives 1.2 GHz but N>105. */
    };
    unsigned n=0;
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i,++n)
        if (a733_clock_peri0_valid(cases[i].pll)!=cases[i].valid) {printf("FAIL %08x\n",cases[i].pll);return 1;}
    /* P1, P2, lock/unlock levels and bit0 do not affect PERI0PLL2X. */
    for (uint32_t extra=0;extra<8;++extra,++n)
        assert(a733_clock_peri0_valid((0xff106310&~UINT32_C(0x700fd))|(extra<<16)|(extra<<2)|(extra&1)|((extra&3)<<6)));
    printf("PASS %u PERI0 validator cases; register decoding only\n",n);
    return 0;
}
