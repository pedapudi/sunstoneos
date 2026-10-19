/* SPDX-License-Identifier: Apache-2.0 */
/* xHCI 1.2 sections4.6,4.9,4.10,4.11.2.2,5.4/5.5,6.2/6.4/6.5.
 * Ordered NC DMA is supplied by the native owner. Never reclaim this parent. */
#include "a733_xhci.h"
#include <string.h>
#define BIT(n) (UINT32_C(1)<<(n))
#define TYPE(n) ((uint32_t)(n)<<10)
#define IOC BIT(5)
#define MAX_OFFSET UINT32_C(0x8000)
enum { P_CAP,P_EXT,P_HALTED,P_RESET,P_RESET_WAIT,P_MEMORY,P_DCBAA,P_COMMAND,
       P_ERST_SIZE,P_ERST_BASE,P_ERDP,P_IMAN,P_CONFIG,P_RUN,P_RUNNING,P_POWER,P_ACTIVE };
enum { C_ENABLE=9,C_ADDRESS=11,C_CONFIGURE=12,C_EVALUATE=13 };
enum { T_NORMAL=1,T_SETUP=2,T_DATA=3,T_STATUS=4,T_ADDRESS_STATUS=5 };
enum { CONTROL_IDLE,CONTROL_SETUP,CONTROL_DATA,CONTROL_STATUS };
static uint32_t get32(const void *v)
{const uint8_t *p=v;return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(void *v,uint32_t n)
{uint8_t *p=v;for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void put64(void *v,uint64_t n)
{put32(v,(uint32_t)n);put32((uint8_t *)v+4,(uint32_t)(n>>32));}
static int separate(const void *a,size_t an,const void *b,size_t bn)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return a&&b&&an<=UINTPTR_MAX-x&&bn<=UINTPTR_MAX-y&&(x+an<=y||y+bn<=x);}
static int outside(const struct a733_xhci *x,const void *p,size_t bytes)
{
    return p&&separate(x,sizeof(*x),p,bytes)&&separate(x->pool,sizeof(*x->pool),p,bytes)&&
        separate((void *)x->extent.cpu,x->extent.bytes,p,bytes)&&x->ops.outside(x->context,p,bytes)==1;
}
static int owner(const struct a733_xhci *x)
{return x&&x->self==x&&!x->notifying&&x->ops.current(x->context)==1;}
static void notify(struct a733_xhci *x,enum a733_xhci_event event,unsigned address,unsigned ep,unsigned bytes,unsigned result)
{
    const struct a733_xhci_notice n={event,x->generation,address,ep,bytes,result};
    x->notifying=1;x->ops.notice(x->context,&n);x->notifying=0;
}
static int fault(struct a733_xhci *x)
{
    if(x->failed)return 0;
    x->failed=1;x->connected=0;
    for(unsigned i=0;i<A733_XHCI_ENDPOINTS;++i){x->endpoints[i].caller=NULL;x->endpoints[i].aborted=1;}
    (void)device_memory_retain(x->pool,x->memory);
    if(x->announced&&!x->removed){x->removed=1;notify(x,A733_XHCI_REMOVE,x->address,0,0,2);}
    return 0;
}
/* BUSY is retryable only before a bounded operation begins. A failed guard
 * after an I/O or publication retains its uncertain externally visible state. */
static int guard(struct a733_xhci *x,uint64_t deadline)
{
    uint64_t now;struct device_memory_extent e;enum device_memory_state s;
    if(x->failed||!device_memory_describe(x->pool,x->memory,&e,&s)||s!=DEVICE_MEMORY_DEVICE||
       e.cpu!=x->extent.cpu||e.physical!=x->extent.physical||e.bytes!=x->extent.bytes||
       x->ops.now(x->context,&now)!=1||now<x->last_now||now>=deadline||now>=x->lifetime_deadline)return -1;
    x->last_now=now;int live=x->ops.live(x->context,x->generation,deadline);
    if(live!=0&&live!=1)return -1;
    if(x->ops.now(x->context,&now)!=1||now<x->last_now||now>=deadline||now>=x->lifetime_deadline)return -1;
    x->last_now=now;return live;
}
static int checked(struct a733_xhci *x)
{if(guard(x,x->deadline)!=1)return fault(x);return 1;}
static int rd(struct a733_xhci *x,uint32_t offset,uint32_t *out)
{return offset<MAX_OFFSET&&!(offset&3)&&checked(x)&&x->ops.read32(x->context,offset,out)==1&&checked(x);}
static int wr(struct a733_xhci *x,uint32_t offset,uint32_t v)
{return offset<MAX_OFFSET&&!(offset&3)&&checked(x)&&x->ops.write32(x->context,offset,v)==1&&checked(x);}
static int wr64(struct a733_xhci *x,uint32_t offset,uint64_t v)
{return offset<MAX_OFFSET-7&&!(offset&7)&&checked(x)&&x->ops.write64(x->context,offset,v)==1&&checked(x);}
static int barrier(struct a733_xhci *x)
{return checked(x)&&x->ops.barrier(x->context)==1&&checked(x);}
static unsigned dci(unsigned address)
{return (address&15)?2*(address&15)+!!(address&128):1;}
static struct a733_xhci_endpoint *endpoint(struct a733_xhci *x,unsigned address)
{
    if(!(address&15))return &x->endpoints[0];
    for(unsigned i=1;i<A733_XHCI_ENDPOINTS;++i)if(x->endpoints[i].opened&&x->endpoints[i].address==address)return &x->endpoints[i];
    return NULL;
}
static int layout(struct a733_xhci *x)
{
    uint8_t *p=(void *)x->extent.cpu;uint64_t base=x->extent.physical;
    memset(p,0,A733_XHCI_MEMORY_BYTES);
    x->dcbaa=p;x->dcbaa_pa=base;
    x->commands=(void *)(p+4096);x->command_pa=base+4096;
    x->events=(void *)(p+8192);x->event_pa=base+8192;
    x->erst=p+12288;x->erst_pa=base+12288;
    x->input=p+16384;x->input_pa=base+16384;
    x->device=p+20480;x->device_pa=base+20480;
    x->scratch=p+12352;
    put64(x->erst,x->event_pa);put32(x->erst+8,A733_XHCI_EVENT_TRBS);
    if(x->scratchpads){put64(x->dcbaa,base+12352);for(unsigned i=0;i<x->scratchpads;++i)put64(x->scratch+8*i,base+49152+4096*i);}
    x->command_cycle=x->event_cycle=1;
    for(unsigned i=0;i<A733_XHCI_ENDPOINTS;++i) {
        struct a733_xhci_endpoint *e=&x->endpoints[i];
        e->ring=(void *)(p+24576+4096*i);e->ring_pa=base+24576+4096*i;
        e->bounce=p+36864+4096*i;e->bounce_pa=base+36864+4096*i;e->cycle=1;
    }
    return barrier(x);
}
/* Publish payload before the cycle bit. The last entry is an owned Link TRB;
 * each ring has one outstanding TD, so a completed slot is not reused early. */
static int ring(struct a733_xhci *x,struct a733_xhci_trb *trbs,uint64_t pa,unsigned *index,unsigned *cycle,
    uint64_t parameter,uint32_t status,uint32_t control,uint64_t *published)
{
    unsigned at=*index,c=*cycle;struct a733_xhci_trb *t=&trbs[at];
    t->control=control|(c^1u);t->parameter_lo=(uint32_t)parameter;t->parameter_hi=(uint32_t)(parameter>>32);t->status=status;
    if(at==A733_XHCI_RING_TRBS-2) {
        struct a733_xhci_trb *link=&trbs[A733_XHCI_RING_TRBS-1];
        link->control=TYPE(6)|BIT(1)|(c^1u);link->parameter_lo=(uint32_t)pa;link->parameter_hi=(uint32_t)(pa>>32);link->status=0;
        if(!barrier(x))return 0;link->control=TYPE(6)|BIT(1)|c;
    }
    if(!barrier(x))return 0;t->control=control|c;
    *published=pa+at*16;
    if(at==A733_XHCI_RING_TRBS-2){*index=0;*cycle=c^1u;}else *index=at+1;
    return barrier(x);
}
static int command(struct a733_xhci *x,unsigned kind,uint32_t flags)
{
    if(x->command_kind)return 0;
    x->command_kind=kind;x->command_deadline=x->deadline;
    if(!ring(x,x->commands,x->command_pa,&x->command_index,&x->command_cycle,
       kind==C_ENABLE?0:x->input_pa,0,TYPE(kind)|flags,&x->command_pointer))return 0;
    return wr(x,x->doorbell,0);
}
static void ep_context(struct a733_xhci *x,struct a733_xhci_endpoint *e)
{
    uint8_t *p=x->input+(e->number+1)*x->context_bytes;
    unsigned type=e->number==1?4:e->transfer_type==3?7:e->direction?6:2;
    if(e->transfer_type==3)put32(p,e->interval<<16);
    put32(p+4,(e->packet<<16)|(type<<3)|(3u<<1));
    put64(p+8,(e->ring_pa+16*e->index)|e->cycle);
    put32(p+16,e->number==1?8:e->transfer_type==3?(e->packet<<16)|e->packet:4096);
}
static void address_context(struct a733_xhci *x)
{
    memset(x->input,0,33*x->context_bytes);put32(x->input+4,3);
    uint8_t *slot=x->input+x->context_bytes;
    put32(slot,(1u<<27)|(x->speed<<20));put32(slot+4,x->usb2_port<<16);
    x->endpoints[0].number=1;x->endpoints[0].packet=x->speed==3?64:8;ep_context(x,&x->endpoints[0]);
}
static int configure(struct a733_xhci *x)
{
    if(!barrier(x))return 0;
    /* Copy the four defined Slot Context words only. Hardware-private tail
     * words in 32/64-byte output contexts remain zero in the input context. */
    memset(x->input,0,33*x->context_bytes);memcpy(x->input+x->context_bytes,x->device,16);
    unsigned mask=0,maximum=1;
    for(unsigned i=1;i<A733_XHCI_ENDPOINTS;++i) {
        struct a733_xhci_endpoint *e=&x->endpoints[i];
        if(!e->opened)continue;if(e->number>maximum)maximum=e->number;
        if(!e->configured){mask|=BIT(e->number);ep_context(x,e);}
    }
    if(!mask)return 0;
    put32(x->input+4,mask|1);uint8_t *slot=x->input+x->context_bytes;
    put32(slot,(get32(slot)&~UINT32_C(0xf8000000))|(maximum<<27));
    x->command_mask=mask;x->configure_pending=1;
    return command(x,C_CONFIGURE,x->slot<<24);
}
static int submit_endpoint(struct a733_xhci *x,struct a733_xhci_endpoint *e)
{
    if(e->kind==T_ADDRESS_STATUS) {
        if(!x->address_status||!checked(x))return 0;
        unsigned ep=e->address;e->pending=0;e->caller=NULL;x->control_stage=CONTROL_IDLE;
        x->address=x->pending_address;x->pending_address=0;x->address_status=0;
        notify(x,A733_XHCI_TRANSFER,0,ep,0,0);return 1;
    }
    uint64_t parameter=e->bounce_pa;uint32_t control=TYPE(e->kind)|IOC,status=e->length;
    if(e->kind==T_SETUP) {
        parameter=(uint64_t)get32(x->setup)|((uint64_t)get32(x->setup+4)<<32);
        control|=BIT(6)|(x->control_length?(x->control_in?3u:2u)<<16:0);status=8;
    } else if(e->kind==T_STATUS) {parameter=0;status=0;control|=e->direction?BIT(16):0;}
    else {
        if(e->direction)control|=BIT(2);
        if(e->kind==T_DATA&&e->direction)control|=BIT(16);
    }
    e->pending=2;
    if(!ring(x,e->ring,e->ring_pa,&e->index,&e->cycle,parameter,status,control,&e->pending_trb))return 0;
    return wr(x,x->doorbell+x->slot*4,e->number);
}
static int transfer_event(struct a733_xhci *x,const struct a733_xhci_trb *event)
{
    unsigned slot=event->control>>24,number=(event->control>>16)&31,code=event->status>>24,residual=event->status&0xffffff;
    uint64_t pointer=(uint64_t)event->parameter_lo|((uint64_t)event->parameter_hi<<32);
    if((event->control&BIT(2))||slot!=x->slot)return 0;
    struct a733_xhci_endpoint *e=NULL;
    for(unsigned i=0;i<A733_XHCI_ENDPOINTS;++i)if(x->endpoints[i].number==number&&x->endpoints[i].pending==2)e=&x->endpoints[i];
    if(!e||e->pending_trb!=pointer||e->aborted)return 0;
    if(code!=1&&code!=13)return 0; /* Stall/halt/reset recovery remains retained. */
    unsigned requested=e->kind==T_SETUP?8:e->length;
    if(residual>requested||(code==1&&residual)||(code==13&&(!e->direction||e->kind==T_SETUP||e->kind==T_STATUS)))return 0;
    unsigned bytes=requested-residual;
    if(!barrier(x))return 0;
    if(e->caller&&e->direction) {
        if(!outside(x,e->caller,e->length)||!checked(x))return 0;
        memcpy(e->caller,e->bounce,bytes);
    }
    unsigned ep=e->address,address=x->address;
    if(e->kind==T_SETUP){x->control_stage=x->control_length?CONTROL_DATA:CONTROL_STATUS;ep=0;}
    else if(e->kind==T_DATA)x->control_stage=CONTROL_STATUS;
    else if(e->kind==T_STATUS)x->control_stage=CONTROL_IDLE;
    e->caller=NULL;e->pending=0;e->pending_trb=0;
    notify(x,A733_XHCI_TRANSFER,address,ep,bytes,0);return 1;
}
static int command_event(struct a733_xhci *x,const struct a733_xhci_trb *event)
{
    uint64_t pointer=(uint64_t)event->parameter_lo|((uint64_t)event->parameter_hi<<32);
    unsigned slot=event->control>>24,kind=x->command_kind;
    if(!kind||pointer!=x->command_pointer||(event->status>>24)!=1||!slot||slot>x->cap_slots||
       (kind!=C_ENABLE&&slot!=x->slot))return 0;
    x->command_kind=0;x->command_pointer=0;
    if(kind==C_ENABLE) {
        if(slot!=1)return 0;x->slot=slot;put64(x->dcbaa+8*slot,x->device_pa);x->port_ready=2;return 1;
    }
    if(!barrier(x))return 0;
    unsigned slot_state=get32(x->device+12)>>27;
    if(kind==C_ADDRESS) {
        if(!x->pending_address){if(slot_state!=1)return 0;x->port_ready=3;x->endpoints[0].configured=1;return 1;}
        unsigned assigned=get32(x->device+12)&255;
        if(slot_state!=2||!assigned||assigned>127)return 0;
        x->hardware_address=assigned;x->address_status=1;x->control_stage=CONTROL_STATUS;
        x->endpoints[0].pending=0;notify(x,A733_XHCI_TRANSFER,0,0,8,0);return 1;
    }
    if(kind==C_EVALUATE){
        if(!x->evaluate_pending||slot_state!=2||
           (get32(x->device+x->context_bytes+4)>>16)!=x->endpoints[0].packet)return 0;
        x->evaluate_pending=0;return 1;
    }
    if(kind==C_CONFIGURE) {
        if(slot_state!=3)return 0;
        for(unsigned i=1;i<A733_XHCI_ENDPOINTS;++i)if(x->command_mask&BIT(x->endpoints[i].number))x->endpoints[i].configured=1;
        x->configure_pending=0;x->command_mask=0;return 1;
    }
    return 0;
}
static int event_step(struct a733_xhci *x)
{
    if(!barrier(x))return -1;
    volatile struct a733_xhci_trb *p=&x->events[x->event_index];uint32_t control=p->control;
    if((control&1)!=x->event_cycle)return 0;
    if(!barrier(x))return -1;
    const struct a733_xhci_trb event={p->parameter_lo,p->parameter_hi,p->status,control};
    unsigned type=(control>>10)&63;
    /* Release the consumed event before callbacks. No callback may reenter. */
    if(++x->event_index==A733_XHCI_EVENT_TRBS){x->event_index=0;x->event_cycle^=1;}
    if(!wr64(x,x->runtime+0x38,x->event_pa+16*x->event_index+8))return -1;
    if(type==32)return transfer_event(x,&event)?1:-1;
    if(type==33)return command_event(x,&event)?1:-1;
    if(type==34)return (event.parameter_lo>>24)==x->usb2_port&&(event.status>>24)==1?1:-1;
    return -1;
}
static int port(struct a733_xhci *x)
{
    uint32_t v,offset=x->operational+0x400+(x->usb2_port-1)*16;
    if(!rd(x,offset,&v)||(v&BIT(3)))return 0;
    if(!(v&1)){if(x->announced)return 0;x->connected=0;return 1;}
    x->connected=1;
    if(v&UINT32_C(0xfe0000)) {
        /* Preserve only RWS fields; writing back PED would disable the port. */
        uint32_t preserve=v&(UINT32_C(0xe000000)|UINT32_C(0xc000)|BIT(9)|UINT32_C(0x1e0));
        if(!wr(x,offset,preserve|(v&UINT32_C(0xfe0000))))return 0;
    }
    if(!x->announced){x->announced=1;notify(x,A733_XHCI_ATTACH,0,0,0,0);return 1;}
    if(x->port_reset==1) {
        uint32_t preserve=v&(UINT32_C(0xe000000)|UINT32_C(0xc000)|BIT(9)|UINT32_C(0x1e0));
        if(!(v&BIT(9))||!wr(x,offset,preserve|BIT(4)))return 0;x->port_reset=2;return 1;
    }
    if(x->port_reset==2) {
        if(v&BIT(4))return 1;
        unsigned speed=(v>>10)&15;
        if(!(v&BIT(1))||speed<1||speed>3||(v&UINT32_C(0x1e0)))return 0;
        if(x->speed&&x->speed!=speed)return 0;x->speed=speed;
        x->port_reset=3;x->port_ready=1;
    }else if(x->port_reset==3&&((v>>10)&15)!=x->speed)return 0;
    return 1;
}
static int capabilities(struct a733_xhci *x)
{
    uint32_t first,hcs,hcs2,hcc,db,rt;
    if(!rd(x,0,&first)||!rd(x,4,&hcs)||!rd(x,8,&hcs2)||!rd(x,16,&hcc)||!rd(x,20,&db)||!rd(x,24,&rt))return 0;
    unsigned version=first>>16,cap=first&255;
    if(version<0x100||version>0x120||cap<0x20||(cap&3)||cap>0x100||!(hcs&255)||!((hcs>>8)&0x7ff)||
       !(hcs>>24)||(hcs>>24)>32||db&3||rt&31||db<0x1000||db>=MAX_OFFSET-1024||rt<0x1000||rt>=MAX_OFFSET-64||
       !(db+4*((hcs&255)+1)<=rt||rt+64<=db))return 0;
    x->cap_slots=hcs&255;x->cap_ports=hcs>>24;x->scratchpads=((hcs2>>16)&0x3e0)|((hcs2>>27)&31);
    if(x->scratchpads>8||(!(hcc&1)&&x->extent.physical+x->extent.bytes>UINT64_C(0x100000000)))return 0;
    x->context_bytes=hcc&4?64:32;x->operational=cap;x->doorbell=db;x->runtime=rt;x->port_power_control=!!(hcc&8);
    x->ext_cursor=(hcc>>16)*4;
    if(!x->ext_cursor||x->ext_cursor<0x20||x->ext_cursor>=0x1000)return 0;
    return 1;
}
static int extension(struct a733_xhci *x)
{
    uint32_t h,name,ports,slots;unsigned cursor=x->ext_cursor;
    if(++x->ext_steps>64||cursor>0xff0||!rd(x,cursor,&h))return 0;
    if((h&255)==1) {if(h&BIT(16))return 0;}
    if((h&255)==2&&(h>>24)==2) {
        if(x->usb2_port||!rd(x,cursor+4,&name)||!rd(x,cursor+8,&ports)||!rd(x,cursor+12,&slots)||
           name!=UINT32_C(0x20425355)||((ports>>8)&255)!=1||!(ports&255)||(ports&255)>x->cap_ports||
           (ports>>28))return 0; /* This first profile uses the standard USB2 speed IDs. */
        x->usb2_port=ports&255;x->slot_type=slots&31;
    }
    unsigned next=(h>>8)&255;
    if(next){x->ext_cursor=cursor+next*4;if(x->ext_cursor>=0x1000)return 0;}
    else {if(!x->usb2_port)return 0;x->phase=P_HALTED;}
    return 1;
}
int a733_xhci_init(struct a733_xhci *x,const struct a733_xhci_ops *ops,void *context,
    struct device_memory_pool *pool,struct device_memory_lease lease,uint64_t generation,uint64_t deadline,uint64_t lifetime)
{
    if(!x||x->self||!ops||!pool||!context||!generation||!deadline||lifetime<deadline||
       !ops->now||!ops->current||!ops->live||!ops->read32||!ops->write32||!ops->write64||!ops->barrier||!ops->outside||!ops->notice||
       !separate(x,sizeof(*x),ops,sizeof(*ops))||!separate(x,sizeof(*x),pool,sizeof(*pool)))return 0;
    const struct a733_xhci_ops copied=*ops;struct device_memory_extent e;enum device_memory_state state;
    if(copied.current(context)!=1||!device_memory_describe(pool,lease,&e,&state)||state!=DEVICE_MEMORY_CPU||
       e.bytes!=A733_XHCI_MEMORY_BYTES||(e.cpu&4095)||(e.physical&4095)||
       e.physical>UINT64_MAX-e.bytes||!separate(x,sizeof(*x),(void *)e.cpu,e.bytes)||
       !separate(pool,sizeof(*pool),(void *)e.cpu,e.bytes)||copied.outside(context,x,sizeof(*x))!=1||
       copied.outside(context,(void *)e.cpu,e.bytes)!=1)return 0;
    uint64_t now;if(copied.now(context,&now)!=1||now>=deadline||copied.live(context,generation,deadline)!=1)return 0;
    struct device_memory_allocation *allocation=device_memory_lookup(pool,lease);
    if(!allocation||allocation->state!=DEVICE_MEMORY_CPU)return 0;
    *x=(struct a733_xhci){.self=x,.ops=copied,.context=context,.pool=pool,.memory=lease,.extent=e,
        .generation=generation,.deadline=deadline,.lifetime_deadline=lifetime,.last_now=now};
    allocation->state=DEVICE_MEMORY_DEVICE;
    if(!checked(x))return 0;return 1;
}
enum a733_xhci_result a733_xhci_poll(struct a733_xhci *x)
{
    if(!owner(x))return A733_XHCI_INVALID;if(x->failed)return A733_XHCI_RETAINED;
    if(x->phase==P_ACTIVE) {
        x->deadline=x->lifetime_deadline;
        if(x->port_reset&&x->port_ready<3&&x->reset_deadline<x->deadline)x->deadline=x->reset_deadline;
        if(x->command_kind&&x->command_deadline<x->deadline)x->deadline=x->command_deadline;
        for(unsigned i=0;i<A733_XHCI_ENDPOINTS;++i)if((x->endpoints[i].pending||(i&&x->endpoints[i].opened&&!x->endpoints[i].configured))&&
            x->endpoints[i].deadline<x->deadline)x->deadline=x->endpoints[i].deadline;
    }
    int live=guard(x,x->deadline);if(!live)return A733_XHCI_BUSY;if(live<0)goto failed;
    uint32_t v;
    switch(x->phase) {
    case P_CAP:if(!capabilities(x))goto failed;x->phase=P_EXT;break;
    case P_EXT:if(!extension(x))goto failed;break;
    case P_HALTED:
        if(!rd(x,x->operational,&v)||(v&3)||!rd(x,x->operational+4,&v)||!(v&1)||(v&(BIT(2)|BIT(12))))goto failed;
        if(v&BIT(11))break;
        if(!rd(x,x->operational+8,&v)||!(v&1))goto failed;x->phase=P_RESET;break;
    case P_RESET:if(!wr(x,x->operational,2))goto failed;x->phase=P_RESET_WAIT;break;
    case P_RESET_WAIT:
        if(!rd(x,x->operational,&v))goto failed;if(v&2)break;
        if(!rd(x,x->operational+4,&v)||!(v&1)||(v&(BIT(2)|BIT(12))))goto failed;if(v&BIT(11))break;
        x->phase=P_MEMORY;break;
    case P_MEMORY:if(!layout(x))goto failed;x->phase=P_DCBAA;break;
    case P_DCBAA:if(!wr64(x,x->operational+0x30,x->dcbaa_pa))goto failed;x->publication=1;x->phase=P_COMMAND;break;
    case P_COMMAND:if(!wr64(x,x->operational+0x18,x->command_pa|1))goto failed;x->phase=P_ERST_SIZE;break;
    case P_ERST_SIZE:if(!wr(x,x->runtime+0x28,1))goto failed;x->phase=P_ERST_BASE;break;
    case P_ERST_BASE:if(!wr64(x,x->runtime+0x30,x->erst_pa))goto failed;x->phase=P_ERDP;break;
    case P_ERDP:if(!wr64(x,x->runtime+0x38,x->event_pa))goto failed;x->phase=P_IMAN;break;
    case P_IMAN:if(!wr(x,x->runtime+0x20,1))goto failed;x->phase=P_CONFIG;break;
    case P_CONFIG:if(!wr(x,x->operational+0x38,1))goto failed;x->phase=P_RUN;break;
    case P_RUN:if(!wr(x,x->operational,1))goto failed;x->phase=P_RUNNING;break;
    case P_RUNNING:
        if(!rd(x,x->operational+4,&v)||(v&(BIT(2)|BIT(12))))goto failed;if(v&1)break;x->phase=P_POWER;break;
    case P_POWER:
        if(!rd(x,x->operational+0x400+(x->usb2_port-1)*16,&v)||(v&BIT(3)))goto failed;
        if(!(v&BIT(9))) {
            uint32_t preserve=v&(UINT32_C(0xe000000)|UINT32_C(0xc000)|UINT32_C(0x1e0));
            if(!x->port_power_control||!wr(x,x->operational+0x400+(x->usb2_port-1)*16,preserve|BIT(9)))goto failed;
        }
        x->phase=P_ACTIVE;break;
    case P_ACTIVE:
        if(!rd(x,x->operational+4,&v)||(v&(1|BIT(2)|BIT(11)|BIT(12)))||!port(x))goto failed;
        if(x->failed)goto failed;
        {int e=event_step(x);if(e<0)goto failed;if(e>0)break;}
        if(x->command_kind)break;
        if(x->port_ready==1){if(!command(x,C_ENABLE,x->slot_type<<16))goto failed;x->port_ready=4;break;}
        if(x->port_ready==2){address_context(x);if(!command(x,C_ADDRESS,(x->slot<<24)|BIT(9)))goto failed;x->port_ready=4;break;}
        if(x->port_ready!=3)break;
        if(x->endpoints[0].pending==1&&x->pending_address&&!x->address_status) {
            address_context(x);if(!command(x,C_ADDRESS,x->slot<<24))goto failed;x->endpoints[0].pending=2;break;
        }
        if(x->evaluate_pending){
            memset(x->input,0,33*x->context_bytes);put32(x->input+4,BIT(1));
            ep_context(x,&x->endpoints[0]);
            if(!command(x,C_EVALUATE,x->slot<<24))goto failed;break;
        }
        for(unsigned i=1;i<A733_XHCI_ENDPOINTS;++i)if(x->endpoints[i].opened&&!x->endpoints[i].configured) {
            if(!x->address||!configure(x))goto failed;goto done;
        }
        for(unsigned i=0;i<A733_XHCI_ENDPOINTS;++i)if(x->endpoints[i].pending==1) {
            if(!submit_endpoint(x,&x->endpoints[i]))goto failed;break;
        }
        break;
    default:goto failed;
    }
done:return x->phase==P_ACTIVE&&x->port_ready==3?A733_XHCI_READY:A733_XHCI_PENDING;
failed:fault(x);return A733_XHCI_RETAINED;
}
int a733_xhci_reset_port(struct a733_xhci *x,uint64_t deadline)
{
    if(!owner(x)||x->failed||x->phase!=P_ACTIVE||!x->connected||x->port_reset||x->slot||
       deadline>x->lifetime_deadline||guard(x,deadline)!=1)return 0;
    x->port_reset=1;x->reset_deadline=deadline;return 1;
}
int a733_xhci_port_connected(const struct a733_xhci *x)
{return x&&x->self==x&&!x->failed&&x->connected;}
int a733_xhci_open(struct a733_xhci *x,unsigned address,unsigned ep,unsigned type,unsigned packet,uint64_t deadline)
{
    if(!owner(x)||x->failed||!x->connected||address!=x->address||ep&~0x8fu||deadline>x->lifetime_deadline||guard(x,deadline)!=1)return 0;
    if(!(ep&15)) {
        if(type||ep||x->endpoints[0].pending||x->evaluate_pending||
           (x->speed==2&&packet!=8)||(x->speed==3&&packet!=8&&packet!=64)||
           (x->speed==1&&packet!=8&&packet!=16&&packet!=32&&packet!=64))return 0;
        unsigned actual=x->speed==3?64:packet;
        if(address&&actual!=x->endpoints[0].packet)x->evaluate_pending=1;
        x->endpoints[0].opened=1;x->endpoints[0].packet=actual;x->endpoints[0].number=1;return 1;
    }
    if(!address||type!=2||packet!=512||x->speed!=3)return 0;
    struct a733_xhci_endpoint *e=endpoint(x,ep);
    if(e)return e->packet==packet&&e->transfer_type==type&&!e->aborted;
    for(unsigned i=1;i<A733_XHCI_ENDPOINTS;++i)if(!x->endpoints[i].opened) {
        e=&x->endpoints[i];e->opened=1;e->transfer_type=type;e->number=dci(ep);e->address=ep;e->packet=packet;e->direction=!!(ep&128);e->deadline=deadline;return 1;
    }
    return 0;
}
int a733_xhci_open_interrupt(struct a733_xhci *x,unsigned address,unsigned ep,
    unsigned packet,unsigned interval,uint64_t deadline)
{
    if(!owner(x)||x->failed||!x->connected||!address||address!=x->address||
       ep&~0x8fu||!(ep&128)||!(ep&15)||!packet||packet>(x->speed==2?8u:64u)||
       !interval||interval>(x->speed==3?16u:255u)||deadline>x->lifetime_deadline||guard(x,deadline)!=1)return 0;
    unsigned encoded;
    if(x->speed==3)encoded=interval-1;
    else{encoded=3;for(unsigned v=interval;v>1;v>>=1)++encoded;}
    struct a733_xhci_endpoint *e=endpoint(x,ep);
    if(e)return e->packet==packet&&e->transfer_type==3&&e->interval==encoded&&!e->aborted;
    for(unsigned i=1;i<A733_XHCI_ENDPOINTS;++i)if(!x->endpoints[i].opened){
        e=&x->endpoints[i];e->opened=1;e->transfer_type=3;e->interval=encoded;
        e->number=dci(ep);e->address=ep;e->packet=packet;e->direction=1;e->deadline=deadline;return 1;
    }
    return 0;
}
int a733_xhci_setup(struct a733_xhci *x,unsigned address,const uint8_t setup[8],uint64_t deadline)
{
    if(!owner(x)||x->failed||!outside(x,setup,8)||address!=x->address||!x->endpoints[0].opened||
       x->control_stage!=CONTROL_IDLE||x->endpoints[0].pending||deadline>x->lifetime_deadline)return 0;
    uint8_t copied[8];memcpy(copied,setup,8);unsigned length=copied[6]|(unsigned)copied[7]<<8;
    if(length>A733_XHCI_TRANSFER_BYTES||guard(x,deadline)!=1)return 0;
    unsigned set_address=copied[0]==0&&copied[1]==5;
    if(set_address&&(address||length||copied[3]||copied[4]||copied[5]||!copied[2]||copied[2]>127||x->pending_address))return 0;
    /* No other form of SET_ADDRESS may reach the physical setup ring. */
    if(copied[1]==5&&(copied[0]&0x60)==0&&!set_address)return 0;
    memcpy(x->setup,copied,8);x->control_length=length;x->control_in=!!(copied[0]&128);x->control_stage=CONTROL_SETUP;
    if(set_address)x->pending_address=copied[2];
    struct a733_xhci_endpoint *e=&x->endpoints[0];e->address=0;e->length=8;e->kind=T_SETUP;e->direction=0;e->pending=1;e->deadline=deadline;
    return 1;
}
int a733_xhci_transfer(struct a733_xhci *x,unsigned address,unsigned ep,uint8_t *buffer,unsigned bytes,uint64_t deadline)
{
    if(!owner(x)||x->failed||address!=x->address||ep&~0x8fu||bytes>A733_XHCI_TRANSFER_BYTES||
       (bytes&&!outside(x,buffer,bytes))||deadline>x->lifetime_deadline)return 0;
    struct a733_xhci_endpoint *e=endpoint(x,ep);
    if(!e||!e->opened||e->pending||e->aborted||guard(x,deadline)!=1)return 0;
    unsigned direction=!!(ep&128),kind=T_NORMAL;
    if(!(ep&15)) {
        if(x->control_stage==CONTROL_DATA) {
            if(bytes!=x->control_length||direction!=x->control_in)return 0;kind=T_DATA;
        } else if(x->control_stage==CONTROL_STATUS) {
            unsigned status_in=x->control_length?!x->control_in:1;
            if(bytes||direction!=status_in)return 0;kind=x->address_status?T_ADDRESS_STATUS:T_STATUS;
        } else return 0;
    }
    if(bytes&&!direction)memcpy(e->bounce,buffer,bytes);
    e->caller=bytes&&direction?buffer:NULL;e->length=bytes;e->direction=direction;e->kind=kind;
    e->address=ep;e->deadline=deadline;e->pending=1;return 1;
}
int a733_xhci_abort(struct a733_xhci *x,unsigned address,unsigned ep)
{
    if(!owner(x)||address!=x->address||ep&~0x8fu)return 0;
    struct a733_xhci_endpoint *e=endpoint(x,ep);
    if(!e||!e->pending)return 1;e->caller=NULL;e->aborted=1;fault(x);return 1;
}
void a733_xhci_lost(struct a733_xhci *x)
{if(owner(x))fault(x);}
