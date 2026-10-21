/* SPDX-License-Identifier: Apache-2.0 */
/* xHCI core detach, slot disable and reattach against a modeled controller.
 * The model reads the core's command and transfer rings, as the composed USB2
 * check does. No TinyUSB, board or USB device is involved. */
#include "a733_xhci.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BIT(n) (UINT32_C(1)<<(n))
#define PHYS UINT64_C(0x79000000)
#define HOLD 1000000u /* The core's bound on a held transaction error. */
static _Alignas(4096) uint8_t dma[A733_XHCI_MEMORY_BYTES];
static struct device_memory_pool pool;
static struct device_memory_lease lease;
static struct a733_xhci x;
static uint32_t reg[0x8000/4],portsc;
static uint64_t now=1000,last_command,last_transfer[A733_XHCI_DEVICES][A733_XHCI_ENDPOINTS];
static unsigned lost,events,event_cycle=1,slots,addresses,doorbells,hold,pattern;
static struct {unsigned kind,slot,dci;} commands[64];static unsigned command_count;
static struct a733_xhci_notice notices[64];static unsigned notice_count;
static const char *mode;
static int is(const char *m){return !strcmp(mode,m);}
static uint32_t get32(const void *v){const uint8_t *p=v;return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(void *v,uint32_t n){uint8_t *p=v;for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static uint64_t get64(const void *v){return get32(v)|(uint64_t)get32((const uint8_t *)v+4)<<32;}
static void put64(void *v,uint64_t n){put32(v,(uint32_t)n);put32((uint8_t *)v+4,(uint32_t)(n>>32));}
static uint8_t *cpu(uint64_t pa){assert(pa>=PHYS&&pa<PHYS+sizeof(dma));return dma+(pa-PHYS);}
static int zero(const void *p,size_t n){const uint8_t *b=p;for(size_t i=0;i<n;++i)if(b[i])return 0;return 1;}
static int cn(void *c,uint64_t *v){(void)c;*v=now;return 1;}
static int current(void *c){(void)c;return 1;}
static int live(void *c,uint64_t g,uint64_t d){(void)c;(void)d;return g==7&&!lost?1:-1;}
static int rd(void *c,uint32_t off,uint32_t *v){(void)c;assert(off<0x8000&&!(off&3));*v=off==0x440?portsc:reg[off/4];return 1;}
static int wr(void *c,uint32_t off,uint32_t v)
{
    (void)c;assert(off<0x8000&&!(off&3));
    if(off==0x440){
        /* PORTSC: change bits are RW1C, PED is never written, PR enables HS. */
        assert(!(v&2));portsc&=~(v&UINT32_C(0xfe0000));portsc=(portsc&~BIT(9))|(v&BIT(9));
        if(v&BIT(4)){assert((portsc&1)&&!(portsc&2));portsc|=2|BIT(21)|(3u<<10);}
        return 1;
    }
    if(off==0x2000)return 1;
    if(off>0x2000&&off<=0x200c){++doorbells;return 1;}
    reg[off/4]=v;
    if(off==0x40){if(v&2){reg[0x40/4]=0;reg[0x44/4]=1;}else if(v&1)reg[0x44/4]=0;}
    return 1;
}
static int wr64(void *c,uint32_t off,uint64_t v){return wr(c,off,(uint32_t)v)&&wr(c,off+4,(uint32_t)(v>>32));}
static int barrier(void *c){(void)c;return 1;}
static int outside(void *c,const void *p,size_t z){(void)c;(void)p;(void)z;return 1;}
static void notice(void *c,const struct a733_xhci_notice *n){(void)c;assert(notice_count<64);notices[notice_count++]=*n;}
static void event(unsigned type,uint64_t pointer,unsigned code,unsigned remaining,unsigned endpoint,unsigned slot)
{
    struct a733_xhci_trb *t=&x.events[events];assert((t->control&1)!=event_cycle);
    *t=(struct a733_xhci_trb){(uint32_t)pointer,(uint32_t)(pointer>>32),(code<<24)|remaining,
        (slot<<24)|(endpoint<<16)|(type<<10)|event_cycle};
    if(++events==A733_XHCI_EVENT_TRBS){events=0;event_cycle^=1;}
}
static uint8_t *context(unsigned slot){assert(slot&&slot<=3&&(slots&BIT(slot)));return cpu(get64(x.dcbaa+8*slot));}
static struct a733_xhci_endpoint *endpoint_number(struct a733_xhci_device *d,unsigned number)
{for(unsigned i=0;i<A733_XHCI_ENDPOINTS;++i)if(d->endpoints[i].number==number)return &d->endpoints[i];assert(0);return NULL;}
static void set_state(uint8_t *ep,unsigned state){put32(ep,(get32(ep)&~7u)|state);}
static void command_step(void)
{
    if(!x.command_kind||x.command_pointer==last_command)return;
    const struct a733_xhci_trb *t=&x.commands[(x.command_pointer-x.command_pa)/16];
    unsigned kind=(t->control>>10)&63,slot=t->control>>24,dci=(t->control>>16)&31,code=1;
    assert(kind==x.command_kind&&!t->parameter_hi&&!t->status);
    if(kind==15&&is("stop-timeout"))return;
    last_command=x.command_pointer;
    struct a733_xhci_device *d=&x.devices[x.command_device-1];
    if(kind==9){slot=1;while(slots&BIT(slot))++slot;assert(slot<=3);slots|=BIT(slot);}
    else if(kind==11&&!(t->control&BIT(9))&&x.devices[1].detach&&is("address-error"))code=4; /* SET_ADDRESS reached no device. */
    else if(kind==11){
        uint8_t *out=context(slot);assert(get32(d->input+4)==3);
        memcpy(out,d->input+64,64);memcpy(out+64,d->input+128,64);
        unsigned bsr=!!(t->control&BIT(9));put32(out+12,((bsr?1u:2u)<<27)|(bsr?0:++addresses));set_state(out+64,1);
    }else if(kind==12){
        uint8_t *out=context(slot);uint32_t add=get32(d->input+4),address=get32(out+12)&255;
        memcpy(out,d->input+64,64);put32(out+12,(3u<<27)|address);
        for(unsigned i=2;i<32;++i)if(add&BIT(i)){memcpy(out+64*i,d->input+64*(i+1),64);set_state(out+64*i,1);}
    }else if(kind==14){
        uint8_t *ep=context(slot)+64*dci;if((get32(ep)&7)!=2)code=19;else set_state(ep,3);
    }else if(kind==15){
        uint8_t *ep=context(slot)+64*dci;struct a733_xhci_endpoint *e=endpoint_number(d,dci);
        if((get32(ep)&7)!=1)code=19;
        else if(e->pending==2&&is("stop-race")){
            /* The TD halts on a transaction error just before the stop. */
            event(32,e->pending_trb,4,e->length,dci,slot);set_state(ep,2);put64(ep+8,e->pending_trb|(e->ring[(e->pending_trb-e->ring_pa)/16].control&1));code=19;
        }else{
            /* Force Stopped Event: the interrupted TD, or the empty ring's dequeue TRB. */
            unsigned busy=e->pending==2&&e->pending_trb;
            uint64_t dequeue=busy?e->pending_trb|(e->ring[(e->pending_trb-e->ring_pa)/16].control&1):(e->ring_pa+16*e->index)|e->cycle;
            event(32,dequeue&~UINT64_C(15),busy?26:27,busy?e->length:0,dci,slot);
            set_state(ep,3);put64(ep+8,dequeue);
        }
    }else if(kind==10){
        /* Contexts and rings keep their contents until this completion is consumed. */
        assert(get64(x.dcbaa+8*slot)==d->device_pa&&!zero(d->device,2048)&&!zero(d->endpoints[0].ring,4096));
        if(is("disable-fail"))code=11;
        else{slots&=~BIT(slot);memset(last_transfer[x.command_device-1],0,sizeof(last_transfer[0]));}
    }else assert(0);
    assert(command_count<64);commands[command_count++]=(typeof(commands[0])){kind,slot,dci};
    event(33,x.command_pointer,code,0,0,slot);
}
static void transfer_step(void)
{
    for(unsigned j=0;j<A733_XHCI_DEVICES;++j)for(unsigned i=0;i<A733_XHCI_ENDPOINTS;++i){
        struct a733_xhci_device *d=&x.devices[j];struct a733_xhci_endpoint *e=&d->endpoints[i];
        if(e->pending!=2||!e->pending_trb||e->pending_trb==last_transfer[j][i]||(hold&BIT(j*4+i)))continue;
        last_transfer[j][i]=e->pending_trb;
        if(e->direction&&e->kind!=2&&e->kind!=4)for(unsigned k=0;k<e->length;++k)e->bounce[k]=(uint8_t)(pattern+k);
        put64(context(d->slot)+64*e->number+8,(e->ring_pa+16*e->index)|e->cycle);
        event(32,e->pending_trb,1,0,e->number,d->slot);return;
    }
}
static enum a733_xhci_result step(void){now+=100;command_step();transfer_step();return a733_xhci_poll(&x);}
#define UNTIL(c) do{for(unsigned n_=0;!(c);++n_){enum a733_xhci_result r_=step();\
    if(r_==A733_XHCI_RETAINED||r_==A733_XHCI_INVALID||n_>4000){fprintf(stderr,"line %d: result %d\n",__LINE__,r_);abort();}}}while(0)
static struct a733_xhci_device *device(unsigned address)
{for(unsigned j=0;j<A733_XHCI_DEVICES;++j)if(x.devices[j].address==address&&x.devices[j].port_ready&&!x.devices[j].detach)return &x.devices[j];assert(0);return NULL;}
static void expect(unsigned base,unsigned address,unsigned endpoint,unsigned bytes)
{
    UNTIL(notice_count>base);const struct a733_xhci_notice *n=&notices[base];
    assert(notice_count==base+1&&n->kind==A733_XHCI_TRANSFER&&n->generation==7&&n->address==address&&
        n->endpoint==endpoint&&n->bytes==bytes&&!n->result);
}
static void control(unsigned address,const uint8_t setup[8],uint8_t *data,unsigned bytes)
{
    assert(a733_xhci_setup(&x,address,setup,now+1000000));expect(notice_count,address,0,8);
    if(bytes){unsigned base=notice_count;assert(a733_xhci_transfer(&x,address,0x80,data,bytes,now+1000000));expect(base,address,0x80,bytes);}
    unsigned base=notice_count,status=bytes?0:0x80;
    assert(a733_xhci_transfer(&x,address,status,NULL,0,now+1000000));expect(base,address,status,0);
}
/* Default-address enumeration of the record bound to address 0. */
static void enumerate(unsigned address)
{
    assert(a733_xhci_open(&x,0,0,0,64,now+1000000));
    uint8_t descriptor[18];const uint8_t get[8]={0x80,6,0,1,0,0,18,0},set[8]={0,5,(uint8_t)address,0,0,0,0,0};
    pattern=0x40;control(0,get,descriptor,18);for(unsigned k=0;k<18;++k)assert(descriptor[k]==(uint8_t)(0x40+k));
    unsigned base=notice_count;assert(a733_xhci_setup(&x,0,set,now+1000000));
    UNTIL(notice_count>base);assert(notices[base].address==0&&notices[base].endpoint==0&&notices[base].bytes==8);
    assert(a733_xhci_transfer(&x,0,0x80,NULL,0,now+1000000));expect(base+1,0,0x80,0);
    assert(a733_xhci_retire_zero(&x)&&device(address)->hardware_address);
}
static void attach(unsigned count)
{
    portsc|=1|BIT(17);event(34,UINT64_C(1)<<24,1,0,0,0);
    unsigned base=notice_count;UNTIL(notice_count>base);
    assert(notice_count==base+1&&notices[base].kind==A733_XHCI_ATTACH);
    unsigned attaches=0;for(unsigned i=0;i<notice_count;++i)attaches+=notices[i].kind==A733_XHCI_ATTACH;assert(attaches==count);
    assert(a733_xhci_reset_port(&x,now+1000000));UNTIL(x.devices[0].port_ready==3);
}
static void open_bulk(unsigned address)
{
    assert(a733_xhci_open(&x,address,0x81,2,512,now+1000000)&&a733_xhci_open(&x,address,0x02,2,512,now+1000000));
    struct a733_xhci_device *d=device(address);UNTIL(d->endpoints[1].configured&&d->endpoints[2].configured);
}
static void bulk_read(unsigned address,unsigned value)
{
    uint8_t buffer[512];memset(buffer,0x5a,sizeof(buffer));pattern=value;
    unsigned base=notice_count;assert(a733_xhci_transfer(&x,address,0x81,buffer,512,now+1000000));expect(base,address,0x81,512);
    for(unsigned k=0;k<512;++k)assert(buffer[k]==(uint8_t)(value+k));
}
static void unplug(unsigned csc){portsc&=~(1u|2u|(15u<<10));if(csc)portsc|=BIT(17);event(34,UINT64_C(1)<<24,1,0,0,0);}
static enum device_memory_state memory(void)
{struct device_memory_extent e;enum device_memory_state s;assert(device_memory_describe(&pool,lease,&e,&s));return s;}
static unsigned count(unsigned kind,unsigned dci,unsigned from)
{unsigned n=0;for(unsigned i=from;i<command_count;++i)n+=commands[i].kind==kind&&(!dci||commands[i].dci==dci);return n;}
static void retained(void)
{
    for(unsigned i=0;i<200000;++i){enum a733_xhci_result r=step();if(r==A733_XHCI_RETAINED)break;assert(r!=A733_XHCI_INVALID);}
    assert(x.failed&&memory()==DEVICE_MEMORY_RETAINED);
}
/* Record pages hold no state after Disable Slot completion. */
static void cleared(unsigned j,unsigned slot)
{
    const uint8_t *p=(void *)x.extent.cpu;
    assert(!get64(x.dcbaa+8*slot)&&zero(p+16384+8192*j,8192)&&!x.devices[j].slot&&!x.devices[j].port_ready);
    for(unsigned i=0;i<2;++i)assert(zero(p+40960+4096*(j*2+i),4096)&&zero(p+69632+4096*(j*2+i),4096));
}
int main(int argc,char **argv)
{
    mode=argc>1?argv[1]:"direct";struct device_memory_extent extent;
    assert(device_memory_pool_init(&pool,(uintptr_t)dma,PHYS,sizeof(dma))&&device_memory_acquire(&pool,99,sizeof(dma),&lease,&extent));
    reg[0]=0x01100040;reg[1]=(1u<<24)|(1u<<8)|3;reg[4]=(0x20u<<16)|1|4|8;reg[5]=0x2000;reg[6]=0x1000;
    reg[0x80/4]=0x02000002;reg[0x84/4]=0x20425355;reg[0x88/4]=0x101;reg[0x44/4]=1;reg[0x48/4]=1;
    const struct a733_xhci_ops ops={cn,current,live,rd,wr,wr64,barrier,outside,notice};
    assert(a733_xhci_init(&x,&ops,&pool,&pool,lease,7,now+10000000,now+100000000));
    UNTIL(portsc&BIT(9)); /* Port power is the last startup phase. */
    if(!strncmp(mode,"hub",3)||!strncmp(mode,"address-",8)){
        attach(1);enumerate(3);
        assert(a733_xhci_mark_hub(&x,3)&&a733_xhci_hub_configure(&x,3,4,0,0)&&a733_xhci_open_interrupt(&x,3,0x81,1,12,now+1000000));
        UNTIL(x.devices[0].hub_configured&&x.devices[0].endpoints[1].configured);
        /* The hub status TD stays outstanding while its child is replaced. Like
         * the TinyUSB adapter's, it may carry the session lifetime. */
        uint8_t status=0x5a;hold=BIT(1);
        assert(a733_xhci_transfer(&x,3,0x81,&status,1,is("hub-error-expiry")?x.lifetime_deadline:now+50000000));
        if(is("hub-error-expiry")){
            /* The hub stays attached after the error; no port change follows. */
            struct a733_xhci_endpoint *e=&x.devices[0].endpoints[1];unsigned slot=x.devices[0].slot,base=notice_count;
            UNTIL(e->pending==2);uint8_t *ep=context(slot)+64*e->number;
            set_state(ep,2);put64(ep+8,e->pending_trb|(e->ring[(e->pending_trb-e->ring_pa)/16].control&1));
            uint64_t errored=now;event(32,e->pending_trb,4,1,e->number,slot);
            for(unsigned i=0;i<4;++i)assert(step()!=A733_XHCI_RETAINED);
            assert(e->pending==5&&!e->caller&&notice_count==base);
            retained();assert(now>=errored+HOLD&&now<errored+HOLD+1000&&now+HOLD<x.lifetime_deadline);
            assert(status==0x5a&&notice_count==base+1&&notices[base].kind==A733_XHCI_REMOVE&&notices[base].result==2);
            puts("PASS a held hub status error retains after the hold bound, long before the session lifetime");return 0;
        }
        if(!strncmp(mode,"address-",8)){
            /* The child detaches while its Address Device command is in flight. */
            assert(a733_xhci_bind_child(&x,3,2,3,now+1000000));UNTIL(x.devices[1].port_ready==3);
            assert(a733_xhci_open(&x,0,0,0,64,now+1000000));
            uint8_t descriptor[18];const uint8_t get[8]={0x80,6,0,1,0,0,18,0},set[8]={0,5,1,0,0,0,0,0};
            control(0,get,descriptor,18);
            unsigned slot=x.devices[1].slot,base=notice_count,from=command_count;
            assert(a733_xhci_setup(&x,0,set,now+1000000));
            for(unsigned i=0;!(x.command_kind==11&&x.devices[1].endpoints[0].pending==2);++i)assert(i<100&&step()!=A733_XHCI_RETAINED);
            assert(a733_xhci_detach(&x,0)==1&&!a733_xhci_device_speed(&x,0));
            UNTIL(!x.devices[1].detach);
            assert(count(11,0,from)==1&&count(15,1,from)==1&&count(10,0,from)==1&&notice_count==base);
            cleared(1,slot);assert(x.devices[0].endpoints[1].pending==2&&status==0x5a&&memory()==DEVICE_MEMORY_DEVICE);
            assert(a733_xhci_bind_child(&x,3,2,3,now+1000000));UNTIL(x.devices[1].port_ready==3);
            enumerate(1);open_bulk(1);bulk_read(1,0x30);
            printf("PASS %s: a detach during Address Device stops EP0, disables the slot, and a new child enumerates\n",mode);return 0;
        }
        for(unsigned round=0;round<2;++round){
            assert(a733_xhci_bind_child(&x,3,2,3,now+1000000));UNTIL(x.devices[1].port_ready==3);
            enumerate(1);open_bulk(1);assert(x.extra_endpoint_owner==2);bulk_read(1,0x10+round);
            if(round)break;
            uint8_t held[512];memset(held,0x5a,sizeof(held));hold|=BIT(5);
            assert(a733_xhci_transfer(&x,1,0x81,held,512,now+1000000));for(unsigned i=0;i<4;++i)assert(step()!=A733_XHCI_RETAINED);
            unsigned slot=x.devices[1].slot,hub_slot=x.devices[0].slot,base=notice_count,from=command_count,bells=doorbells;
            assert(a733_xhci_detach(&x,1)==1&&a733_xhci_detach(&x,1)==1&&a733_xhci_abort(&x,1,0x81)==1&&!x.devices[1].endpoints[1].caller);
            UNTIL(!x.devices[1].detach);hold&=~BIT(5);
            assert(count(15,0,from)==3&&count(15,1,from)==1&&count(15,3,from)==1&&count(15,4,from)==1&&count(10,0,from)==1);
            assert(commands[command_count-1].kind==10&&commands[command_count-1].slot==slot);
            cleared(1,slot);assert(!x.extra_endpoint_owner&&notice_count==base&&doorbells==bells);
            assert(x.devices[0].slot==hub_slot&&x.devices[0].hub_configured&&x.devices[0].endpoints[1].pending==2&&status==0x5a);
            for(unsigned k=0;k<512;++k)assert(held[k]==0x5a);
            assert(a733_xhci_detach(&x,1)==1&&memory()==DEVICE_MEMORY_DEVICE);
        }
        unsigned child=x.devices[1].slot,hub_slot=x.devices[0].slot,from=command_count;
        unplug(1);unsigned base=notice_count;UNTIL(notice_count>base);
        assert(notices[base].kind==A733_XHCI_REMOVE&&notices[base].address==3&&!notices[base].result);
        UNTIL(!x.detaching);
        unsigned order[2],n=0;for(unsigned i=from;i<command_count;++i)if(commands[i].kind==10)order[n++]=commands[i].slot;
        assert(n==2&&order[0]==child&&order[1]==hub_slot&&count(15,0,from)==5);
        cleared(1,child);cleared(0,hub_slot);assert(notice_count==base+1&&memory()==DEVICE_MEMORY_DEVICE);
        puts("PASS hub child detach stops its endpoints and disables only its slot; a new child reuses the cleared record; root detach disables child then hub");
        return 0;
    }
    attach(1);enumerate(1);open_bulk(1);bulk_read(1,0x10);
    struct a733_xhci_device *d=&x.devices[0];
    uint64_t rings[3]={d->endpoints[0].ring_pa,d->endpoints[1].ring_pa,d->endpoints[2].ring_pa};
    unsigned slot=d->slot;assert(x.extra_endpoint_owner==1);
    uint8_t held[512];memset(held,0x5a,sizeof(held));hold=BIT(1);
    assert(a733_xhci_transfer(&x,1,0x81,held,512,now+3000000));
    for(unsigned i=0;i<4;++i)assert(step()!=A733_XHCI_RETAINED);assert(d->endpoints[1].pending==2);
    if(is("transaction-error")||is("error-expiry")){
        /* The TD ends with a transaction error and its endpoint halts. */
        struct a733_xhci_endpoint *e=&d->endpoints[1];uint8_t *ep=context(slot)+64*e->number;
        set_state(ep,2);put64(ep+8,e->pending_trb|(e->ring[(e->pending_trb-e->ring_pa)/16].control&1));
        unsigned base=notice_count;uint64_t errored=now;event(32,e->pending_trb,4,512,e->number,slot);
        for(unsigned i=0;i<4;++i)assert(step()!=A733_XHCI_RETAINED);
        assert(e->pending==5&&!e->caller&&notice_count==base);
        if(is("error-expiry")){
            retained();assert(now>=errored+HOLD&&now<errored+HOLD+1000);
            for(unsigned k=0;k<512;++k)assert(held[k]==0x5a);
            puts("PASS a held transaction error without a disconnect retains after the hold bound");return 0;
        }
    }
    unsigned base=notice_count,from=command_count,bells=doorbells;uint64_t unplugged=now;
    unplug(!is("no-csc"));
    if(is("no-csc")){
        retained();assert(!count(15,0,from)&&notices[base].kind==A733_XHCI_REMOVE&&notices[base].result==2);
        puts("PASS CCS loss without a connect-status change retains without teardown");return 0;
    }
    UNTIL(notice_count>base);
    assert(notice_count==base+1&&notices[base].kind==A733_XHCI_REMOVE&&notices[base].address==1&&!notices[base].result);
    assert(!a733_xhci_port_connected(&x)&&a733_xhci_detach(&x,1)==1&&a733_xhci_abort(&x,1,0x81)==1);
    assert(!a733_xhci_transfer(&x,1,0x81,held,512,now+1000000)&&!a733_xhci_open(&x,1,0x83,2,512,now+1000000));
    if(is("generation")){
        lost=1;retained();assert(get64(x.dcbaa+8*slot));
        puts("PASS Type-C generation loss during teardown retains the session");return 0;
    }
    if(is("stop-timeout")||is("disable-fail")){
        retained();assert(get64(x.dcbaa+8*slot)==d->device_pa&&!zero(d->endpoints[0].ring,4096));
        if(is("stop-timeout"))assert(now-unplugged>=5000000&&now-unplugged<5100000);
        for(unsigned k=0;k<512;++k)assert(held[k]==0x5a);
        printf("PASS %s retains contexts, rings and the DMA parent\n",mode);return 0;
    }
    UNTIL(!x.detaching);
    assert(count(10,0,from)==1&&commands[command_count-1].kind==10&&commands[command_count-1].slot==slot);
    if(is("stop-race")||is("transaction-error"))assert(count(14,3,from)==1&&count(15,3,from)==(unsigned)is("stop-race"));
    else assert(count(15,0,from)==3&&count(15,1,from)==1&&count(15,3,from)==1&&count(15,4,from)==1);
    cleared(0,slot);assert(!x.extra_endpoint_owner&&!x.announced&&notice_count==base+1&&doorbells==bells&&memory()==DEVICE_MEMORY_DEVICE);
    for(unsigned k=0;k<512;++k)assert(held[k]==0x5a);
    if(is("stale-event")){
        event(32,rings[1],1,0,3,slot);retained();
        puts("PASS a transfer event for a disabled slot retains the session");return 0;
    }
    hold=0;attach(2);enumerate(1);open_bulk(1);
    assert(d->slot==slot&&d->endpoints[0].ring_pa==rings[0]&&d->endpoints[1].ring_pa==rings[1]&&d->endpoints[2].ring_pa==rings[2]);
    bulk_read(1,0x20);
    printf("PASS %s: REMOVE, stopped endpoints, Disable Slot, cleared record, second ATTACH and enumeration on the same pages\n",mode);
    return 0;
}
