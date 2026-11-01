/* SPDX-License-Identifier: Apache-2.0 */
/* Actual core rings and model DMA, without TinyUSB or physical hardware. */
#define main detach_fixture_main
#include "a733-xhci-detach.c"
#undef main
static void init_dma(const char *which)
{
    mode="dma";struct device_memory_extent extent;
    assert(device_memory_pool_init(&pool,(uintptr_t)dma,PHYS,sizeof(dma))&&device_memory_acquire(&pool,99,sizeof(dma),&lease,&extent));
    reg[0]=0x01100040;reg[1]=(1u<<24)|(1u<<8)|3;reg[4]=(0x20u<<16)|1|4|8;reg[5]=0x2000;reg[6]=0x1000;
    if(!strcmp(which,"six-records"))reg[1]=(1u<<24)|(1u<<8)|A733_XHCI_DEVICES;
    reg[0x80/4]=0x02000002;reg[0x84/4]=0x20425355;reg[0x88/4]=0x101;reg[0x44/4]=1;reg[0x48/4]=1;
    const struct a733_xhci_ops ops={cn,current,live,rd,wr,wr64,barrier,outside,notice};
    assert(a733_xhci_init(&x,&ops,&pool,&pool,lease,7,now+10000000,now+100000000));
    if(!strcmp(which,"full-speed"))model_speed=1;
    UNTIL(portsc&BIT(9));attach(1);enumerate(1);
}
static void bulk_dma(const char *which)
{
    if(!strcmp(which,"full-speed")){
        assert(a733_xhci_device_speed(&x,1)==1);
        assert(!a733_xhci_open(&x,1,0x81,2,512,now+1000000));
        assert(a733_xhci_open(&x,1,0x81,2,64,now+1000000)&&a733_xhci_open(&x,1,2,2,64,now+1000000));
        UNTIL(x.devices[0].endpoints[1].configured&&x.devices[0].endpoints[2].configured);
    }else open_bulk(1);
#if SUNSTONE_USB_HOST_GENERIC
    assert(a733_xhci_open_interrupt(&x,1,0x83,16,4,now+1000000));
    UNTIL(x.devices[0].endpoints[3].configured);
    assert(!a733_xhci_open_interrupt(&x,1,0x84,16,4,now+1000000));
#endif
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
static void control_stall(void)
{
    const uint8_t setup[8]={0x21,10,0,0,0,0,0,0};
    struct a733_xhci_device *d=&x.devices[0];struct a733_xhci_endpoint *e=&d->endpoints[0];
    unsigned base=notice_count;hold=1;
    assert(a733_xhci_setup(&x,1,setup,now+1000000));UNTIL(e->pending==2);
    uint8_t *context=d->device+e->number*x.context_bytes;
    set_state(context,2);
    put64(context+8,e->pending_trb|(e->ring[(e->pending_trb-e->ring_pa)/16].control&1));
    event(32,e->pending_trb,6,8,e->number,d->slot);
    UNTIL(notice_count>base);
    assert(notice_count==base+1&&notices[base].result==1&&!notices[base].bytes&&
           !d->recovering&&!e->pending&&!d->control_stage);
    hold=0;uint8_t output[8];const uint8_t get[8]={0x80,6,0,1,0,0,8,0};
    control(1,get,output,8);
    puts("PASS class-control STALL retires through Reset/Dequeue before the next control request");
}
static void interrupt_out(void)
{
    assert(a733_xhci_open_interrupt(&x,1,3,16,4,now+1000000));
    struct a733_xhci_endpoint *e=&x.devices[0].endpoints[1];UNTIL(e->configured);
    const uint8_t *context=x.devices[0].device+e->number*x.context_bytes;
    assert(((get32(context+4)>>3)&7)==3&&!e->direction);
    uint8_t data[16]={1,2,3};unsigned base=notice_count;
    assert(a733_xhci_transfer(&x,1,3,data,16,now+1000000));expect(base,1,3,16);
    assert(!memcmp(e->bounce,data,16));
    puts("PASS interrupt OUT context and transfer completion");
}
static void six_records(void)
{
    assert(x.cap_slots==A733_XHCI_DEVICES&&reg[(x.operational+0x38)/4]==A733_XHCI_DEVICES);
    assert(a733_xhci_mark_hub(&x,1)&&a733_xhci_hub_configure(&x,1,7,0,0)&&
           a733_xhci_open_interrupt(&x,1,0x81,1,12,now+1000000));
    UNTIL(x.devices[0].hub_configured);
    for(unsigned j=1;j<A733_XHCI_DEVICES;++j){
        assert(a733_xhci_bind_child(&x,1,j,3,now+1000000));
        UNTIL(x.devices[j].port_ready==3);enumerate(j+1);open_bulk(j+1);bulk_read(j+1,0x20+j);
        for(unsigned k=0;k<j;++k)for(unsigned e=0;e<A733_XHCI_ENDPOINTS;++e){
            assert(x.devices[j].endpoints[e].ring!=x.devices[k].endpoints[e].ring);
            assert(x.devices[j].endpoints[e].bounce!=x.devices[k].endpoints[e].bounce);
        }
    }
    assert(!a733_xhci_bind_child(&x,1,6,3,now+1000000));
    puts("PASS six enabled slots with root hub, five children and distinct concurrent bulk endpoint pages");
}
static void legacy_iso(void)
{
    assert(!SUNSTONE_USB_HOST_GENERIC&&A733_XHCI_MEMORY_BYTES==131072);
    assert(a733_xhci_open_isoch(&x,1,0x81,128,1,now+1000000));
    struct a733_xhci_endpoint *e=&x.devices[0].endpoints[1];UNTIL(e->configured);
    assert(a733_xhci_isoch_start(&x,1,0x81));
    for(unsigned k=0;k<40;++k){
        UNTIL(x.isoch.count>x.isoch.done);
        unsigned i=(x.isoch.head+x.isoch.done)%A733_XHCI_ISOCH_TDS;
        const struct a733_xhci_trb *t=&e->ring[(x.isoch.trb[i]-e->ring_pa)/16];
        uint8_t *destination=cpu((uint64_t)t->parameter_lo|((uint64_t)t->parameter_hi<<32));
        assert(destination>=e->bounce&&destination+128<=e->bounce+4096);
        memset(destination,(uint8_t)k,128);event(32,x.isoch.trb[i],13,28,e->number,x.devices[0].slot);
        UNTIL(x.isoch.done);
        uint8_t output[128],*direct;const uint8_t *view;unsigned bytes;uint64_t token;
        assert(a733_xhci_isoch_acquire(&x,1,0x81,&view,&bytes,&token)==-1);
        assert(a733_xhci_dma_acquire(&x,1,0x81,&direct,&bytes,&token)==-1);
        assert(a733_xhci_isoch_take(&x,1,0x81,output,sizeof(output),&bytes)==1&&bytes==100);
        for(unsigned n=0;n<bytes;++n)assert(output[n]==(uint8_t)k);
    }
    puts("PASS legacy128KiB geometry, copied ISO ring wrapping and borrowed API refusal");
}
static void irq_dma(void)
{
    open_bulk(1);irq_model=1;
    assert(a733_xhci_interrupt_start(&x)==1&&a733_xhci_interrupt_finish(&x)==1);
    uint8_t *page;unsigned capacity;uint64_t token;
    assert(a733_xhci_dma_acquire(&x,1,0x81,&page,&capacity,&token)==1);
    unsigned base=notice_count;pattern=0x37;
    assert(a733_xhci_transfer(&x,1,0x81,page,512,now+1000000));
    assert(a733_xhci_poll(&x)==A733_XHCI_READY&&x.devices[0].endpoints[1].pending==2);
    transfer_step();assert(reg[0x1020/4]&1);
    assert(a733_xhci_poll(&x)==A733_XHCI_READY&&notice_count==base);
    assert(a733_xhci_interrupt_begin(&x)==1);
    assert(a733_xhci_poll(&x)==A733_XHCI_READY&&notice_count==base+1);
    assert(a733_xhci_interrupt_finish(&x)==1);
    for(unsigned i=0;i<512;++i)assert(page[i]==(uint8_t)(pattern+i));
    assert(a733_xhci_dma_release(&x,1,0x81,token));
    puts("PASS direct DMA completion remains queued until owner IRQ begin/drain/finish");
}
static void address_reuse(void)
{
    assert(a733_xhci_mark_hub(&x,1)&&a733_xhci_hub_configure(&x,1,4,0,0)&&
           a733_xhci_open_interrupt(&x,1,0x81,1,12,now+1000000));
    UNTIL(x.devices[0].hub_configured);
    for(unsigned j=1;j<3;++j){
        assert(a733_xhci_bind_child(&x,1,j,3,now+1000000));
        UNTIL(x.devices[j].port_ready==3);enumerate(j+1);open_bulk(j+1);
    }
    uint8_t *old_page,*new_page;unsigned capacity;uint64_t old_token,new_token;
    assert(a733_xhci_dma_acquire(&x,3,0x81,&old_page,&capacity,&old_token)==1);
    unsigned base=notice_count;pattern=0x61;
    assert(a733_xhci_transfer(&x,3,0x81,old_page,512,now+1000000));expect(base,3,0x81,512);
    assert(a733_xhci_detach(&x,2));UNTIL(!x.devices[1].slot);
    assert(a733_xhci_detach(&x,3));UNTIL(x.devices[2].detach==3);
    assert(a733_xhci_bind_child(&x,1,3,3,now+1000000));
    UNTIL(x.devices[1].port_ready==3);enumerate(3);open_bulk(3);
    assert(x.devices[1].address==3&&x.devices[2].address==3&&x.devices[2].detach==3);
    assert(a733_xhci_dma_acquire(&x,3,0x81,&new_page,&capacity,&new_token)==1&&new_page!=old_page);
    assert(!a733_xhci_dma_release(&x,2,0x81,old_token));
    assert(a733_xhci_dma_release(&x,3,0x81,old_token));UNTIL(!x.devices[2].slot);
    assert(x.devices[1].address==3&&x.devices[1].port_ready==3);
    assert(a733_xhci_dma_release(&x,3,0x81,new_token));
    puts("PASS old lease token remains releasable after logical USB address reuse in another record");
}
int main(int argc,char **argv)
{
    assert(argc==2);init_dma(argv[1]);
    if(!strcmp(argv[1],"address-reuse"))address_reuse();
    else if(!strcmp(argv[1],"irq-dma"))irq_dma();
    else if(!strcmp(argv[1],"legacy-iso"))legacy_iso();
    else if(!strcmp(argv[1],"six-records"))six_records();
    else if(!strcmp(argv[1],"control-stall"))control_stall();
    else if(!strcmp(argv[1],"interrupt-out"))interrupt_out();
    else if(!strncmp(argv[1],"iso",3))iso_dma(argv[1]);else bulk_dma(argv[1]);
    return 0;
}
