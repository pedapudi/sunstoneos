/* SPDX-License-Identifier: Apache-2.0 */
/* Actual core rings and model DMA, without TinyUSB or physical hardware. */
#define main detach_fixture_main
#include "a733-xhci-detach.c"
#undef main
static void init_dma(void)
{
    mode="dma";struct device_memory_extent extent;
    assert(device_memory_pool_init(&pool,(uintptr_t)dma,PHYS,sizeof(dma))&&device_memory_acquire(&pool,99,sizeof(dma),&lease,&extent));
    reg[0]=0x01100040;reg[1]=(1u<<24)|(1u<<8)|3;reg[4]=(0x20u<<16)|1|4|8;reg[5]=0x2000;reg[6]=0x1000;
    reg[0x80/4]=0x02000002;reg[0x84/4]=0x20425355;reg[0x88/4]=0x101;reg[0x44/4]=1;reg[0x48/4]=1;
    const struct a733_xhci_ops ops={cn,current,live,rd,wr,wr64,barrier,outside,notice};
    assert(a733_xhci_init(&x,&ops,&pool,&pool,lease,7,now+10000000,now+100000000));
    UNTIL(portsc&BIT(9));attach(1);enumerate(1);
}
static void bulk_dma(const char *which)
{
    open_bulk(1);
    assert(a733_xhci_open_interrupt(&x,1,0x83,16,4,now+1000000));
    UNTIL(x.devices[0].endpoints[3].configured);
    assert(!a733_xhci_open_interrupt(&x,1,0x84,16,4,now+1000000));
    uint8_t *pages[A733_XHCI_ISOCH_BUFFERS];uint64_t tokens[A733_XHCI_ISOCH_BUFFERS];unsigned cap;
    for(unsigned i=0;i<A733_XHCI_ISOCH_BUFFERS;++i){
        assert(a733_xhci_dma_acquire(&x,1,0x81,&pages[i],&cap,&tokens[i])==1&&cap==4096);
        if(i)assert(pages[i]!=pages[i-1]&&tokens[i]!=tokens[i-1]);
    }
    uint8_t *none;uint64_t token;assert(!a733_xhci_dma_acquire(&x,1,0x81,&none,&cap,&token));
    unsigned base=notice_count;pattern=0x48;
    assert(a733_xhci_transfer(&x,1,0x81,pages[0],512,now+1000000));
    assert(!a733_xhci_dma_release(&x,1,0x81,tokens[0]));
    expect(base,1,0x81,512);
    for(unsigned i=0;i<512;++i)assert(pages[0][i]==(uint8_t)(pattern+i));
    assert(!a733_xhci_dma_release(&x,1,2,tokens[0]));
    if(!strcmp(which,"echo")){
        base=notice_count;
        assert(a733_xhci_transfer(&x,1,2,pages[0]+2,510,now+1000000));
        UNTIL(x.devices[0].endpoints[2].pending==2);
        const struct a733_xhci_endpoint *e=&x.devices[0].endpoints[2];
        const struct a733_xhci_trb *t=&e->ring[(e->pending_trb-e->ring_pa)/16];
        assert(cpu(t->parameter_lo|((uint64_t)t->parameter_hi<<32))==pages[0]+2);
        assert(!a733_xhci_dma_release(&x,1,0x81,tokens[0]));
        expect(base,1,2,510);
    }
    if(!strcmp(which,"generation")){
        uint8_t snapshot[512];memcpy(snapshot,pages[0],512);lost=1;
        assert(a733_xhci_poll(&x)==A733_XHCI_RETAINED);
        assert(!a733_xhci_dma_release(&x,1,0x81,tokens[0])&&!memcmp(snapshot,pages[0],512));
        puts("PASS generation loss retains direct DMA pages");return;
    }
    if(!strcmp(which,"detach")){
        unsigned slot=x.devices[0].slot;unplug(1);
        UNTIL(x.devices[0].detach==3);
        assert(!(slots&BIT(slot))&&memory()==DEVICE_MEMORY_DEVICE);
        uint8_t value=pages[0][0];
        now+=6000000;assert(step()!=A733_XHCI_RETAINED&&pages[0][0]==value);
        for(unsigned i=0;i<A733_XHCI_ISOCH_BUFFERS;++i)assert(a733_xhci_dma_release(&x,1,0x81,tokens[i]));
        UNTIL(!x.devices[0].slot);assert(memory()==DEVICE_MEMORY_DEVICE);
        puts("PASS disabled slot defers page reuse until completed/reserved leases release");return;
    }
    for(unsigned i=0;i<A733_XHCI_ISOCH_BUFFERS;++i)assert(a733_xhci_dma_release(&x,1,0x81,tokens[i]));
    assert(!a733_xhci_dma_release(&x,1,0x81,tokens[0]));
    assert(a733_xhci_dma_acquire(&x,1,0x81,&none,&cap,&token)==1&&token!=tokens[0]);
    assert(!a733_xhci_dma_release(&x,1,0x81,tokens[0]));
    assert(a733_xhci_dma_release(&x,1,0x81,token));
    puts("PASS direct DMA pool exhaustion, matched completion, cross-endpoint echo and stale-token rejection");
}
static void iso_dma(const char *which)
{
    assert(a733_xhci_open_isoch(&x,1,0x81,1024|(2u<<11),1,now+1000000));
    struct a733_xhci_endpoint *e=&x.devices[0].endpoints[1];
    UNTIL(e->configured);
    assert(e->packet==1024&&e->burst==2);
    const uint8_t *context=x.devices[0].device+e->number*x.context_bytes;
    assert((get32(context+4)>>16)==1024&&((get32(context+4)>>8)&255)==2);
    assert((get32(context+16)>>16)==3072);
    assert(a733_xhci_isoch_start(&x,1,0x81));
    const uint8_t *pages[700];uint64_t tokens[700];unsigned bytes;
    for(unsigned k=0;k<700;++k){
        UNTIL(x.isoch.count>x.isoch.done);
        unsigned i=(x.isoch.head+x.isoch.done)%A733_XHCI_ISOCH_TDS;
        const struct a733_xhci_trb *t=&e->ring[(x.isoch.trb[i]-e->ring_pa)/16];
        assert(t->status==3072&&((t->control>>16)&15)==2&&!(t->control&(3u<<7)));
        uint8_t *destination=cpu((uint64_t)t->parameter_lo|((uint64_t)t->parameter_hi<<32));
        memset(destination,(uint8_t)k,3072);
        event(32,x.isoch.trb[i],13,2048,e->number,x.devices[0].slot);
        UNTIL(x.isoch.done);
        assert(a733_xhci_isoch_acquire(&x,1,0x81,&pages[k],&bytes,&tokens[k])==1&&bytes==1024&&pages[k]==destination);
        assert(!a733_xhci_isoch_release(&x,1,0x82,tokens[k]));
    }
    for(unsigned k=0;k<700;++k)for(unsigned n=0;n<1024;++n)assert(pages[k][n]==(uint8_t)k);
    if(!strcmp(which,"iso-detach")){
        unplug(1);UNTIL(x.devices[0].detach==3);
        assert(memory()==DEVICE_MEMORY_DEVICE);
    }
    for(unsigned k=0;k<700;++k)assert(a733_xhci_isoch_release(&x,1,0x81,tokens[k]));
    assert(!a733_xhci_isoch_release(&x,1,0x81,tokens[0]));
    if(!strcmp(which,"iso-detach"))UNTIL(!x.devices[0].slot);
    puts("PASS HS three-transaction contexts/TRBs, 716800 retained bytes, ring wrapping and release after Disable Slot");
}
int main(int argc,char **argv)
{
    assert(argc==2);init_dma();
    if(!strncmp(argv[1],"iso",3))iso_dma(argv[1]);else bulk_dma(argv[1]);
    return 0;
}
