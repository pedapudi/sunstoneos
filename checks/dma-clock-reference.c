/* Reuse the real platform/clock-ledger fixture; execute changed parents only. */
#define main dma_previous_platform_cases
#include "dma-platform.c"
#undef main
int main(void)
{
    const struct { uint32_t osc,ref; unsigned accepted; } cases[]={
        {0x10000002,0xf8636300,1}, /* raw24, N100/M100 */
        {0x10008002,0xf8675f00,1}, /* raw26, N96/M104 */
        {0x10004002,0xf84f6300,1}, /* raw19.2, N100/M80 */
        {0x10008002,0xf8636300,0}, /* raw26, incorrect N100/M100 */
        {0x10008002,0xe8675f00,0}, /* normalized ratio, lock absent */
    };
    for(unsigned peri=0;peri<2;++peri)for(unsigned i=0;i<5;++i){
        struct a733_dma_platform_config c=config(0);
        if(!peri){
            c.fabric[0]=0;c.fabric[1]=c.fabric[2]=0x80000000;
            c.rate_hz[0]=c.rate_hz[1]=c.rate_hz[2]=24000000;
        }
        struct model m;struct a733_dma_platform p;setup(&m,&p,c);
        m.osc=cases[i].osc;m.ccu[0]=cases[i].ref;
        if(!peri){m.ccu[0xa0/4]=0;m.fail_read=CCU+0xa0;}
        enum a733_dma_platform_result result=finish(&m,&p);
        assert(result==(cases[i].accepted?READY:RETAINED)&&!m.writes);
        assert(p.token.id&&(p.shared&A733_CLOCK_REF));
        if(cases[i].accepted){
            struct a733_clock_token writer={0};
            assert(a733_clock_acquire(&m.leases,8,0,A733_CLOCK_REF,UINT64_MAX,&writer)==A733_CLOCK_BUSY);
            if(!peri)assert(m.parent_reads==1&&!(p.shared&A733_CLOCK_PERI0));
        }else retained(&m,&p);
        printf("PASS DMA %s reference case %u\n",peri?"PERI0":"SYS24M",i);
    }
    return 0;
}
