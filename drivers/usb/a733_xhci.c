/* SPDX-License-Identifier: Apache-2.0 */
/* xHCI 1.2 sections4.6,4.9,4.10,4.11.2.2,5.4/5.5,6.2/6.4/6.5.
 * Ordered NC DMA is supplied by the native owner. Never reclaim this parent. */
#include "a733_xhci.h"
#include "a733_range.h"
#include <string.h>
#define BIT(n) (UINT32_C(1)<<(n))
#define TYPE(n) ((uint32_t)(n)<<10)
#define IOC BIT(5)
#define MAX_OFFSET UINT32_C(0x8000)
#define BUSY_US 2000u /* Longest provider BUSY repeated inside one access. */
enum { P_CAP,P_EXT,P_HALTED,P_RESET,P_RESET_WAIT,P_MEMORY,P_DCBAA,P_COMMAND,
       P_ERST_SIZE,P_ERST_BASE,P_ERDP,P_IMAN,P_CONFIG,P_RUN,P_RUNNING,P_POWER,P_ACTIVE };
enum { C_ENABLE=9,C_DISABLE=10,C_ADDRESS=11,C_CONFIGURE=12,C_EVALUATE=13,C_RESET_ENDPOINT=14,C_STOP=15,C_SET_DEQUEUE=16 };
#define DETACH_US 5000000u /* Teardown bound; pinned Linux XHCI_CMD_DEFAULT_TIMEOUT. */
#define HOLD_US 1000000u /* Error hold: a hub status interval (at most 256 ms) and a port status request. */
#define ISOCH_US 100000u /* Bound between isochronous ring events while TDs are on the ring. */
#define PRESERVE (UINT32_C(0xe000000)|UINT32_C(0xc000)|BIT(9)|UINT32_C(0x1e0)) /* PORTSC RWS fields. */
enum { T_NORMAL=1,T_SETUP=2,T_DATA=3,T_STATUS=4,T_ADDRESS_STATUS=5 };
enum { CONTROL_IDLE,CONTROL_SETUP,CONTROL_DATA,CONTROL_STATUS };
static uint32_t get32(const void *v)
{const uint8_t *p=v;return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(void *v,uint32_t n)
{uint8_t *p=v;for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void put64(void *v,uint64_t n)
{put32(v,(uint32_t)n);put32((uint8_t *)v+4,(uint32_t)(n>>32));}
static int outside(const struct a733_xhci *x,const void *p,size_t bytes)
{
    return p&&a733_ranges_disjoint(x,sizeof(*x),p,bytes)&&a733_ranges_disjoint(x->pool,sizeof(*x->pool),p,bytes)&&
        a733_ranges_disjoint((void *)x->extent.cpu,x->extent.bytes,p,bytes)&&x->ops.outside(x->context,p,bytes)==1;
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
    for(unsigned j=0;j<A733_XHCI_DEVICES;++j)for(unsigned i=0;i<A733_XHCI_ENDPOINTS;++i){x->devices[j].endpoints[i].caller=NULL;x->devices[j].endpoints[i].aborted=1;}
    (void)device_memory_retain(x->pool,x->memory);
    if(x->announced&&!x->removed){x->removed=1;notify(x,A733_XHCI_REMOVE,x->devices[0].address,0,0,2);}
    return 0;
}
/* BUSY is metadata contention: before a bounded operation begins, the poll
 * returns it. Inside the operation, checked() repeats the observation without
 * MMIO until the deadline. A refused or expired guard after an I/O or
 * publication retains its uncertain externally visible state. */
static int guard(struct a733_xhci *x,uint64_t deadline)
{
    uint64_t now;struct device_memory_extent e;enum device_memory_state s;
    if(x->failed||!device_memory_describe(x->pool,x->memory,&e,&s)||s!=DEVICE_MEMORY_DEVICE||
       e.cpu!=x->extent.cpu||e.physical!=x->extent.physical||e.bytes!=x->extent.bytes||
       x->ops.now(x->context,&now)!=1||now>=x->lifetime_deadline||!a733_deadline_admit(now,&x->last_now,deadline))return -1;
    int live=x->ops.live(x->context,x->generation,deadline);
    if(live!=0&&live!=1)return -1;
    if(x->ops.now(x->context,&now)!=1||now>=x->lifetime_deadline||!a733_deadline_admit(now,&x->last_now,deadline))return -1;
    return live;
}
/* Repeat a BUSY observation without MMIO for at most BUSY_US of counter time
 * and never past the deadline, which guard() enforces. A provider can stay
 * BUSY while it waits for a lock this caller holds, so a longer BUSY fails
 * like a refusal. Submission and configuration calls use it too, because
 * their callers treat refusal as a lost session. */
static int settled(struct a733_xhci *x,uint64_t deadline)
{
    int live=guard(x,deadline);const uint64_t start=x->last_now;
    while(!live&&x->last_now-start<BUSY_US)live=guard(x,deadline);
    return live;
}
/* Every access still needs a fresh observation. */
static int checked(struct a733_xhci *x)
{if(settled(x,x->deadline)!=1)return fault(x);return 1;}
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
static struct a733_xhci_device *device_address(struct a733_xhci *x,unsigned address)
{
    if(!x||x->self!=x||address>127)return NULL;
    if(!address)return x->default_device<A733_XHCI_DEVICES&&!x->devices[x->default_device].zero_retired&&
        !x->devices[x->default_device].detach?&x->devices[x->default_device]:NULL;
    for(unsigned i=0;i<A733_XHCI_DEVICES;++i)if(x->devices[i].address==address&&!x->devices[i].detach)return &x->devices[i];
    return NULL;
}
/* A record under teardown keeps its logical address only for repeated detach
 * or abort requests; new requests never reach it. */
static int detaching(const struct a733_xhci *x,unsigned address)
{
    for(unsigned i=0;i<A733_XHCI_DEVICES;++i)if(x->devices[i].detach&&x->devices[i].address==address)return 1;
    return 0;
}
/* The record that owns a slot, including one under teardown. */
static struct a733_xhci_device *slot_record(struct a733_xhci *x,unsigned slot)
{
    for(unsigned i=0;slot&&i<A733_XHCI_DEVICES;++i)if(x->devices[i].slot==slot)return &x->devices[i];
    return NULL;
}
static struct a733_xhci_endpoint *endpoint(struct a733_xhci_device *d,unsigned address)
{
    if(!(address&15))return &d->endpoints[0];
    for(unsigned i=1;i<A733_XHCI_ENDPOINTS;++i)if(d->endpoints[i].opened&&d->endpoints[i].address==address)return &d->endpoints[i];
    return NULL;
}
/* Fixed context pair and EP0/data ring and bounce pages of one device record. */
static void pages(struct a733_xhci *x,unsigned j)
{
    uint8_t *p=(void *)x->extent.cpu;uint64_t base=x->extent.physical;struct a733_xhci_device *d=&x->devices[j];
    d->input=p+16384+8192*j;d->input_pa=base+16384+8192*j;
    d->device=p+20480+8192*j;d->device_pa=base+20480+8192*j;
    for(unsigned i=0;i<2;++i){
        unsigned n=j*2+i;struct a733_xhci_endpoint *e=&d->endpoints[i];
        e->ring=(void *)(p+40960+4096*n);e->ring_pa=base+40960+4096*n;
        e->bounce=p+69632+4096*n;e->bounce_pa=base+69632+4096*n;e->cycle=1;
    }
}
static int layout(struct a733_xhci *x)
{
    uint8_t *p=(void *)x->extent.cpu;uint64_t base=x->extent.physical;
    memset(p,0,A733_XHCI_MEMORY_BYTES);
    x->dcbaa=p;x->dcbaa_pa=base;
    x->commands=(void *)(p+4096);x->command_pa=base+4096;
    x->events=(void *)(p+8192);x->event_pa=base+8192;
    x->erst=p+12288;x->erst_pa=base+12288;
    x->scratch=p+12352;
    put64(x->erst,x->event_pa);put32(x->erst+8,A733_XHCI_EVENT_TRBS);
    if(x->scratchpads){put64(x->dcbaa,base+12352);for(unsigned i=0;i<x->scratchpads;++i)put64(x->scratch+8*i,base+98304+4096*i);}
    x->command_cycle=x->event_cycle=1;
    /* Three context pairs, seven ring/bounce pairs and eight scratch pages
     * occupy exactly128KiB. EP0+one data credit belong to each device. The
     * seventh credit is reserved for the sole admitted MSC second bulk pipe. */
    _Static_assert(16384+3*8192==40960,"context pages");
    _Static_assert(40960+7*4096==69632,"ring pages");
    _Static_assert(69632+7*4096==98304,"bounce pages");
    _Static_assert(98304+8*4096==A733_XHCI_MEMORY_BYTES,"retained parent extent");
    _Static_assert(A733_XHCI_RING_TRBS*sizeof(struct a733_xhci_trb)<=4096,"ring extent");
    _Static_assert(A733_XHCI_TRANSFER_BYTES<=4096,"bounce extent");
    for(unsigned j=0;j<A733_XHCI_DEVICES;++j)pages(x,j);
    return barrier(x);
}
/* Disable Slot completion returns the slot's contexts, rings and bounce pages
 * to software. Clear them in place for a later device; the parent remains
 * DEVICE-owned and no pointer moves. */
static int reclaim(struct a733_xhci *x,unsigned j)
{
    uint8_t *p=(void *)x->extent.cpu;struct a733_xhci_device *d=&x->devices[j];
    if(d->slot)put64(x->dcbaa+8*d->slot,0);
    memset(p+16384+8192*j,0,8192);
    for(unsigned i=0;i<2;++i){memset(p+40960+4096*(j*2+i),0,4096);memset(p+69632+4096*(j*2+i),0,4096);}
    if(x->extra_endpoint_owner==j+1){memset(p+65536,0,4096);memset(p+94208,0,4096);x->extra_endpoint_owner=0;}
    if(x->isoch.device==j+1)memset(&x->isoch,0,sizeof(x->isoch));
    memset(d,0,sizeof(*d));pages(x,j);
    if(x->default_device==j)x->default_device=0;
    return barrier(x);
}
/* Revoke every caller destination now. A TD already on its ring stays owned
 * by the controller until its completion or Stop Endpoint resolves it. An
 * Address Device in flight queues no TD; teardown waits for its completion. */
static void detach(struct a733_xhci *x,struct a733_xhci_device *d)
{
    d->detach=1;d->deadline=x->lifetime_deadline-x->last_now>DETACH_US?x->last_now+DETACH_US:x->lifetime_deadline;
    for(unsigned i=0;i<A733_XHCI_ENDPOINTS;++i){
        struct a733_xhci_endpoint *e=&d->endpoints[i];e->caller=NULL;
        if((e->pending!=2||!e->pending_trb)&&e->pending!=6)e->pending=0;
    }
    if(x->isoch.device==(unsigned)(d-x->devices)+1)x->isoch.requested=0;
}
/* A hub's teardown includes every device below it, at any tier. */
static void detach_tree(struct a733_xhci *x,struct a733_xhci_device *d)
{
    detach(x,d);
    for(unsigned more=1;more;){
        more=0;
        for(unsigned j=0;j<A733_XHCI_DEVICES;++j){
            struct a733_xhci_device *c=&x->devices[j],*p=c->parent?slot_record(x,c->parent):NULL;
            if(c->port_ready&&!c->detach&&p&&p->detach){detach(x,c);more=1;}
        }
    }
}
/* No bound record names this record's slot as its parent hub. */
static int childless(const struct a733_xhci *x,unsigned j)
{
    const struct a733_xhci_device *d=&x->devices[j];
    for(unsigned k=0;d->slot&&k<A733_XHCI_DEVICES;++k)
        if(k!=j&&x->devices[k].port_ready&&x->devices[k].parent==d->slot)return 0;
    return 1;
}
/* Publish payload before the cycle bit. The last entry is an owned Link TRB.
 * A ring holds one outstanding TD, or at most A733_XHCI_ISOCH_TDS one-TRB
 * isochronous TDs, so a slot is reused only after its completion. */
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
static int command(struct a733_xhci *x,struct a733_xhci_device *d,unsigned kind,uint32_t flags)
{
    if(x->command_kind)return 0;
    x->command_kind=kind;x->command_device=(unsigned)(d-x->devices)+1;x->command_deadline=x->deadline;
    if(!ring(x,x->commands,x->command_pa,&x->command_index,&x->command_cycle,
       kind==C_ENABLE||kind==C_DISABLE||kind==C_RESET_ENDPOINT||kind==C_STOP?0:kind==C_SET_DEQUEUE?
         (d->endpoints[0].ring_pa+16*d->endpoints[0].index)|d->endpoints[0].cycle:d->input_pa,
       0,TYPE(kind)|flags,&x->command_pointer))return 0;
    return wr(x,x->doorbell,0);
}
static void ep_context(struct a733_xhci *x,struct a733_xhci_device *d,struct a733_xhci_endpoint *e)
{
    uint8_t *p=d->input+(e->number+1)*x->context_bytes;
    /* Isoch IN has type 5 and CErr 0; its Max ESIT Payload is one packet. */
    unsigned periodic=e->transfer_type==3||e->transfer_type==1;
    unsigned type=e->number==1?4:e->transfer_type==3?7:e->transfer_type==1?5:e->direction?6:2;
    if(periodic)put32(p,e->interval<<16);
    put32(p+4,(e->packet<<16)|(e->burst<<8)|(type<<3)|(e->transfer_type==1?0:3u<<1));
    put64(p+8,(e->ring_pa+16*e->index)|e->cycle);
    put32(p+16,e->number==1?8:periodic?(e->packet<<16)|e->packet:4096);
}
static void address_context(struct a733_xhci *x,struct a733_xhci_device *d)
{
    memset(d->input,0,33*x->context_bytes);put32(d->input+4,3);
    uint8_t *slot=d->input+x->context_bytes;
    put32(slot,(1u<<27)|(d->speed<<20)|d->route);put32(slot+4,x->usb2_port<<16);
    if(d->parent&&d->speed<3){
        /* Only high-speed USB2 hubs are admitted, so the parent is the TT hub. */
        const struct a733_xhci_device *parent=slot_record(x,d->parent);
        put32(slot,get32(slot)|(parent&&parent->hub_mtt?BIT(25):0));
        put32(slot+8,d->parent|(d->parent_port<<8));
    }
    d->endpoints[0].number=1;d->endpoints[0].packet=d->speed==4?512:d->speed==3?64:8;ep_context(x,d,&d->endpoints[0]);
}
static int configure(struct a733_xhci *x,struct a733_xhci_device *d)
{
    if(!barrier(x))return 0;
    /* Copy the four defined Slot Context words only. Hardware-private tail
     * words in 32/64-byte output contexts remain zero in the input context. */
    memset(d->input,0,33*x->context_bytes);memcpy(d->input+x->context_bytes,d->device,16);
    unsigned mask=0,maximum=1;
    for(unsigned i=1;i<A733_XHCI_ENDPOINTS;++i) {
        struct a733_xhci_endpoint *e=&d->endpoints[i];
        if(!e->opened)continue;if(e->number>maximum)maximum=e->number;
        if(!e->configured){mask|=BIT(e->number);ep_context(x,d,e);}
    }
    if(!mask&&(!d->hub||d->hub_configured))return 0;
    put32(d->input+4,mask|1);uint8_t *slot=d->input+x->context_bytes;
    put32(slot,(get32(slot)&~UINT32_C(0xf8000000))|(maximum<<27));
    if(d->hub){
        if(!d->hub_ports)return 0;
        put32(slot,get32(slot)|BIT(26)|(d->hub_mtt?BIT(25):0));
        put32(slot+4,(get32(slot+4)&0xffffffu)|(d->hub_ports<<24));
        put32(slot+8,(get32(slot+8)&~(3u<<16))|(d->hub_ttt<<16));
    }
    d->command_mask=mask;d->configure_pending=1;
    return command(x,d,C_CONFIGURE,d->slot<<24);
}
static int submit_endpoint(struct a733_xhci *x,struct a733_xhci_device *d,struct a733_xhci_endpoint *e)
{
    if(e->kind==T_ADDRESS_STATUS) {
        if(!d->address_status||!checked(x))return 0;
        unsigned ep=e->address;e->pending=0;e->caller=NULL;d->control_stage=CONTROL_IDLE;
        d->address=d->pending_address;d->pending_address=0;d->address_status=0;
        notify(x,A733_XHCI_TRANSFER,0,ep,0,0);return 1;
    }
    uint64_t parameter=e->bounce_pa;uint32_t control=TYPE(e->kind)|IOC,status=e->length;
    if(e->kind==T_SETUP) {
        parameter=(uint64_t)get32(d->setup)|((uint64_t)get32(d->setup+4)<<32);
        control|=BIT(6)|(d->control_length?(d->control_in?3u:2u)<<16:0);status=8;
    } else if(e->kind==T_STATUS) {parameter=0;status=0;control|=e->direction?BIT(16):0;}
    else {
        if(e->direction)control|=BIT(2);
        if(e->kind==T_DATA&&e->direction)control|=BIT(16);
    }
    e->pending=2;
    if(!ring(x,e->ring,e->ring_pa,&e->index,&e->cycle,parameter,status,control,&e->pending_trb))return 0;
    return wr(x,x->doorbell+d->slot*4,e->number);
}
/* A TD on a Halted or Stopped endpoint whose dequeue pointer still addresses
 * it has ended without a completion; the controller no longer executes it. */
static int finished(struct a733_xhci *x,struct a733_xhci_device *d,struct a733_xhci_endpoint *e)
{
    const uint8_t *c=d->device+e->number*x->context_bytes;unsigned state=get32(c)&7;
    uint64_t dequeue=((uint64_t)get32(c+8)|((uint64_t)get32(c+12)<<32))&~UINT64_C(15);
    if((state!=2&&state!=3)||dequeue!=e->pending_trb)return 0;
    e->pending=0;e->pending_trb=0;return 1;
}
/* Position, counted from head, of the queued TD whose TRB is pointer, or
 * A733_XHCI_ISOCH_TDS when no queued TD has it. */
static unsigned isoch_position(const struct a733_xhci_isoch *s,uint64_t pointer)
{
    for(unsigned k=s->done;pointer&&k<s->count;++k)if(s->trb[(s->head+k)%A733_XHCI_ISOCH_TDS]==pointer)return k;
    return A733_XHCI_ISOCH_TDS;
}
/* The controller passed the queued TDs before position k without data. */
static void isoch_skip(struct a733_xhci_isoch *s,unsigned k)
{
    for(;s->done<k;++s->done){s->state[(s->head+s->done)%A733_XHCI_ISOCH_TDS]=3;++s->dropped;}
}
/* After a stop, the TDs still on an isochronous ring end without completions
 * when the dequeue pointer addresses one of them, or the enqueue position. TDs
 * the controller skipped without an event end the same way. */
static int isoch_finished(struct a733_xhci *x,struct a733_xhci_device *d,struct a733_xhci_endpoint *e)
{
    struct a733_xhci_isoch *s=&x->isoch;
    const uint8_t *c=d->device+e->number*x->context_bytes;
    uint64_t dequeue=((uint64_t)get32(c+8)|((uint64_t)get32(c+12)<<32))&~UINT64_C(15);
    if((get32(c)&7)!=3||(isoch_position(s,dequeue)==A733_XHCI_ISOCH_TDS&&dequeue!=e->ring_pa+16*e->index))return 0;
    while(s->done<s->count){s->state[(s->head+s->done)%A733_XHCI_ISOCH_TDS]=3;++s->done;}
    e->pending=0;return 1;
}
/* Isochronous ring events follow the pinned Linux handle_tx_event. Success and
 * short packets carry data; transaction, babble and split errors end the TD
 * without data and the stream continues (xHCI 1.2 section 4.10.2.8).
 * A Missed Service Error names the last missed TD, or none (section 4.10.3.2):
 * every queued TD up to the named one ends without data. A controller may skip
 * further TDs without an event, so until the next other event, that event may
 * name a later TD and the TDs before it end without data.
 * Ring Overrun (completion code 15) names where the controller found the ring
 * empty (section 4.10.3.1): the enqueue position, the oldest queued TD when a
 * refill came late, a later TD after missed intervals, or none on pre-1.1
 * controllers. The enqueue position and no pointer say nothing about queued
 * TDs, so they are accepted only when no TD is queued or the controller is
 * skipping. The controller removed the endpoint from its schedule, so while TDs
 * remain the core rings the doorbell again. */
static int isoch_event(struct a733_xhci *x,struct a733_xhci_device *d,struct a733_xhci_endpoint *e,
    unsigned code,unsigned residual,uint64_t pointer)
{
    struct a733_xhci_isoch *s=&x->isoch;
    unsigned k=isoch_position(s,pointer);
    if(code==15){
        if((!pointer||pointer==e->ring_pa+16*e->index)&&(s->done==s->count||s->skip))k=s->count;
        else if(!pointer||k==A733_XHCI_ISOCH_TDS||(k>s->done&&!s->skip))return 0;
        if(pointer){isoch_skip(s,k);s->skip=0;}
        ++s->overruns;
        if(s->done==s->count){e->pending=0;return 1;}
        e->deadline=x->lifetime_deadline-x->last_now>ISOCH_US?x->last_now+ISOCH_US:x->lifetime_deadline;
        return d->detach||wr(x,x->doorbell+d->slot*4,e->number);
    }
    if(code>=26&&code<=28)
        return d->detach&&d->stopping==2&&pointer>=e->ring_pa&&pointer<e->ring_pa+16*A733_XHCI_RING_TRBS;
    if(code==23){
        s->skip=1;
        if(!pointer)return 1;
        if(k==A733_XHCI_ISOCH_TDS)return 0;
        isoch_skip(s,k+1);
    }else{
        if(k==A733_XHCI_ISOCH_TDS||(k>s->done&&!s->skip))return 0;
        isoch_skip(s,k);s->skip=0;
        unsigned i=(s->head+s->done)%A733_XHCI_ISOCH_TDS;
        if(!d->detach&&code==1&&!residual){s->bytes[i]=e->packet;s->state[i]=2;}
        else if(!d->detach&&code==13&&residual<=e->packet){s->bytes[i]=e->packet-residual;s->state[i]=2;}
        else if(code==1||code==13||code==3||code==4||code==36){s->state[i]=3;++s->dropped;}
        else return 0;
        ++s->done;
    }
    if(s->done==s->count)e->pending=0;
    else e->deadline=x->lifetime_deadline-x->last_now>ISOCH_US?x->last_now+ISOCH_US:x->lifetime_deadline;
    return 1;
}
/* The isochronous bound runs from the last ring activity the core observed.
 * It expires only when the event ring holds no further event, so a late poll
 * first consumes what the controller posted while the owner was away. Outside
 * interrupt service the poll consumes no events, so waiting events would hold
 * the bound open until an interrupt arrives; it expires instead. */
static int isoch_expired(struct a733_xhci *x)
{
    const struct a733_xhci_isoch *s=&x->isoch;
    if(!s->device)return 0;
    const struct a733_xhci_device *d=&x->devices[s->device-1];const struct a733_xhci_endpoint *e=&d->endpoints[1];
    if(d->detach||e->pending!=6||x->last_now<e->deadline)return 0;
    if(x->irq_mode&&!x->irq_service)return 1;
    if(!barrier(x))return 1;
    return (x->events[x->event_index].control&1)!=x->event_cycle;
}
/* Queue one-packet Isoch TDs (ISP, IOC, Start Isoch ASAP) until every entry
 * is in use, then ring the endpoint once. */
static int isoch_refill(struct a733_xhci *x,struct a733_xhci_device *d,struct a733_xhci_endpoint *e)
{
    struct a733_xhci_isoch *s=&x->isoch;
    if(e->pending!=6)e->deadline=x->lifetime_deadline-x->last_now>ISOCH_US?x->last_now+ISOCH_US:x->lifetime_deadline;
    while(s->count<A733_XHCI_ISOCH_TDS){
        unsigned i=(s->head+s->count)%A733_XHCI_ISOCH_TDS;
        if(!ring(x,e->ring,e->ring_pa,&e->index,&e->cycle,e->bounce_pa+i*A733_XHCI_ISOCH_PACKET,e->packet,
           TYPE(5)|IOC|BIT(2)|BIT(31),&s->trb[i]))return 0;
        s->state[i]=1;s->bytes[i]=0;++s->count;
    }
    e->pending=6;
    return wr(x,x->doorbell+d->slot*4,e->number);
}
/* Under teardown, a TD may still finish by itself, by a halting error or by
 * the requested stop. No completion reaches the class. A forced stop event may
 * name another TRB of the stopped ring (pinned Linux handle_tx_event). */
static int detached_event(struct a733_xhci_device *d,unsigned number,unsigned code,uint64_t pointer)
{
    for(unsigned i=0;i<A733_XHCI_ENDPOINTS;++i){
        struct a733_xhci_endpoint *e=&d->endpoints[i];
        if(!number||e->number!=number)continue;
        unsigned stopped=code>=26&&code<=28;
        if(stopped&&d->stopping!=i+1)return 0;
        if(e->pending==2&&pointer==e->pending_trb&&
           (stopped||code==1||code==4||code==6||code==13||code==36)){e->pending=0;e->pending_trb=0;return 1;}
        return stopped&&pointer>=e->ring_pa&&pointer<e->ring_pa+16*A733_XHCI_RING_TRBS;
    }
    return 0;
}
static int transfer_event(struct a733_xhci *x,const struct a733_xhci_trb *event)
{
    unsigned slot=event->control>>24,number=(event->control>>16)&31,code=event->status>>24,residual=event->status&0xffffff;
    uint64_t pointer=(uint64_t)event->parameter_lo|((uint64_t)event->parameter_hi<<32);
    struct a733_xhci_device *d=NULL;
    for(unsigned i=0;i<A733_XHCI_DEVICES;++i)if(x->devices[i].slot==slot)d=&x->devices[i];
    if((event->control&BIT(2))||!slot||!d)return 0;
    if(x->isoch.device==(unsigned)(d-x->devices)+1&&number&&d->endpoints[1].number==number)
        return isoch_event(x,d,&d->endpoints[1],code,residual,pointer);
    if(d->detach)return detached_event(d,number,code,pointer);
    struct a733_xhci_endpoint *e=NULL;
    for(unsigned i=0;i<A733_XHCI_ENDPOINTS;++i)if(d->endpoints[i].number==number&&d->endpoints[i].pending==2)e=&d->endpoints[i];
    if(!e||e->pending_trb!=pointer||e->aborted)return 0;
    if(code==6){
        /* Only the fixed read-only MSC GET_MAX_LUN protocol stall is admitted.
         * One TD is outstanding. Revoke its CPU destination immediately, keep
         * its pointer/deadline, and publish no class completion until both
         * recovery commands have completed. No stalled payload is copied. */
        static const uint8_t max_lun[8]={0xa1,0xfe,0,0,0,0,1,0};
        if(number!=1||!d->address||d->speed<3||d->speed>4||
           memcmp(d->setup,max_lun,8)||
           (e->kind!=T_SETUP&&e->kind!=T_DATA&&e->kind!=T_STATUS))return 0;
        e->caller=NULL;
        if(!barrier(x)||(get32(d->device+x->context_bytes)&7)!=2)return 0;
        e->pending=3;return 1;
    }
    if(code==4||code==36){
        /* A transaction error retires the TD and halts its endpoint, as a
         * disconnect can before its port change is seen. Hold it without a
         * class completion until teardown, for at most HOLD_US. */
        e->caller=NULL;e->pending=5;
        if(e->deadline>x->last_now+HOLD_US)e->deadline=x->last_now+HOLD_US;
        return 1;
    }
    if(code!=1&&code!=13)return 0;
    unsigned requested=e->kind==T_SETUP?8:e->length;
    if(residual>requested||(code==1&&residual)||(code==13&&(!e->direction||e->kind==T_SETUP||e->kind==T_STATUS)))return 0;
    unsigned bytes=requested-residual;
    if(!barrier(x))return 0;
    if(e->caller&&e->direction) {
        if(!outside(x,e->caller,e->length)||!checked(x))return 0;
        memcpy(e->caller,e->bounce,bytes);
    }
    unsigned ep=e->address,address=d->address;
    if(e->kind==T_SETUP){d->control_stage=d->control_length?CONTROL_DATA:CONTROL_STATUS;ep=0;}
    else if(e->kind==T_DATA)d->control_stage=CONTROL_STATUS;
    else if(e->kind==T_STATUS)d->control_stage=CONTROL_IDLE;
    e->caller=NULL;e->pending=0;e->pending_trb=0;
    notify(x,A733_XHCI_TRANSFER,address,ep,bytes,0);return 1;
}
static int command_event(struct a733_xhci *x,const struct a733_xhci_trb *event)
{
    uint64_t pointer=(uint64_t)event->parameter_lo|((uint64_t)event->parameter_hi<<32);
    unsigned slot=event->control>>24,kind=x->command_kind,code=event->status>>24;
    if(!x->command_device||x->command_device>A733_XHCI_DEVICES)return 0;
    struct a733_xhci_device *d=&x->devices[x->command_device-1];
    /* Context State Error is accepted only for a stop that raced a halt, and
     * Transaction Error only for Address Device to a detaching device. */
    if(!kind||pointer!=x->command_pointer||
       (code!=1&&(kind!=C_STOP||code!=19)&&(kind!=C_ADDRESS||code!=4||!d->detach))||!slot||slot>x->cap_slots||slot>A733_XHCI_DEVICES||
       (kind!=C_ENABLE&&slot!=d->slot))return 0;
    x->command_kind=0;x->command_pointer=0;
    if(kind==C_ENABLE) {
        for(unsigned i=0;i<A733_XHCI_DEVICES;++i)if(x->devices[i].slot==slot)return 0;d->slot=slot;put64(x->dcbaa+8*slot,d->device_pa);d->port_ready=2;return 1;
    }
    if(!barrier(x))return 0;
    if(kind==C_STOP||(kind==C_RESET_ENDPOINT&&d->stopping)){
        /* A stop that raced a halt leaves the endpoint Halted for a reset. */
        if(d->detach!=1||!d->stopping)return 0;
        struct a733_xhci_endpoint *e=&d->endpoints[d->stopping-1];d->stopping=0;
        unsigned state=get32(d->device+e->number*x->context_bytes)&7;
        if(state!=3&&(code!=19||state!=2))return 0;
        if(e->pending==6)return isoch_finished(x,d,e);
        return e->pending!=2||finished(x,d,e);
    }
    if(kind==C_DISABLE)return d->detach==2&&reclaim(x,(unsigned)(d-x->devices));
    if(d->detach)return 1; /* Teardown owns the slot; a completed request has no further effect. */
    unsigned slot_state=get32(d->device+12)>>27;
    if(kind==C_RESET_ENDPOINT||kind==C_SET_DEQUEUE){
        struct a733_xhci_endpoint *e=&d->endpoints[0];
        const uint8_t *context=d->device+x->context_bytes;
        if((slot_state!=2&&slot_state!=3)||!e->pending_trb||e->aborted||
           (get32(context)&7)!=3)return 0;
        if(kind==C_RESET_ENDPOINT){
            if(e->pending!=3)return 0;e->pending=4;return 1;
        }
        uint64_t dequeue=(uint64_t)get32(context+8)|((uint64_t)get32(context+12)<<32);
        if(e->pending!=4||dequeue!=((e->ring_pa+16*e->index)|e->cycle))return 0;
        unsigned ep=e->address;
        e->caller=NULL;e->pending=0;e->pending_trb=0;d->control_stage=CONTROL_IDLE;
        notify(x,A733_XHCI_TRANSFER,d->address,ep,0,1);return 1;
    }
    if(kind==C_ADDRESS) {
        if(!d->pending_address){if(slot_state!=1)return 0;d->port_ready=3;d->endpoints[0].configured=1;return 1;}
        unsigned assigned=get32(d->device+12)&255;
        if(slot_state!=2||!assigned||assigned>127)return 0;
        d->hardware_address=assigned;d->address_status=1;d->control_stage=CONTROL_STATUS;
        d->endpoints[0].pending=0;notify(x,A733_XHCI_TRANSFER,0,0,8,0);return 1;
    }
    if(kind==C_EVALUATE){
        if(!d->evaluate_pending||slot_state!=2||
           (get32(d->device+x->context_bytes+4)>>16)!=d->endpoints[0].packet)return 0;
        d->evaluate_pending=0;return 1;
    }
    if(kind==C_CONFIGURE) {
        if(slot_state!=3)return 0;
        if(d->hub){
            if(!(get32(d->device)&BIT(26))||!!(get32(d->device)&BIT(25))!=d->hub_mtt||
               (get32(d->device+4)>>24)!=d->hub_ports||((get32(d->device+8)>>16)&3)!=d->hub_ttt)return 0;
            d->hub_configured=1;
        }
        for(unsigned i=1;i<A733_XHCI_ENDPOINTS;++i)if(d->command_mask&BIT(d->endpoints[i].number))d->endpoints[i].configured=1;
        d->configure_pending=0;d->command_mask=0;return 1;
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
    if(x->irq_mode)++x->irq_events;
    else if(!wr64(x,x->runtime+0x38,x->event_pa+16*x->event_index+8))return -1;
    if(type==32)return transfer_event(x,&event)?1:-1;
    if(type==33)return command_event(x,&event)?1:-1;
    if(type==34){
        unsigned port=event.parameter_lo>>24;if((event.status>>24)!=1)return -1;
        if(port==x->usb2_port)return 1;
        if((x->require_ss||x->companion_root)&&port==x->usb2_companion_port){
            uint32_t v,off=x->operational+0x400+(port-1)*16;
            if(!rd(x,off,&v)||(v&BIT(3)))return -1;
            return wr(x,off,(v&(0xe00c200u|0x1e0u))|(v&0xfe0000u))?1:-1;
        }
        return -1;
    }
    return -1;
}
static int port(struct a733_xhci *x)
{
    struct a733_xhci_device *d=&x->devices[0];
    uint32_t v,offset=x->operational+0x400+(x->usb2_port-1)*16;
    if(!rd(x,offset,&v)||(v&BIT(3)))return 0;
    if(x->detaching){x->connected=0;return 1;} /* A new connection waits for the old teardown. */
    if(!(v&1)){
        if(!x->announced){x->connected=0;return 1;}
        /* Only a reported connect-status change starts an orderly detach; a
         * lost CCS without CSC is uncertain. Children are torn down first. */
        if(!(v&BIT(17))||!wr(x,offset,(v&PRESERVE)|(v&UINT32_C(0xfe0000))))return 0;
        x->connected=0;x->detaching=1;x->removed=1;
        for(unsigned j=0;j<A733_XHCI_DEVICES;++j)if(x->devices[j].port_ready&&!x->devices[j].detach)detach(x,&x->devices[j]);
        notify(x,A733_XHCI_REMOVE,d->address,0,0,0);return 1;
    }
    x->connected=1;
    /* Preserve only RWS fields; writing back PED would disable the port. */
    if((v&UINT32_C(0xfe0000))&&!wr(x,offset,(v&PRESERVE)|(v&UINT32_C(0xfe0000))))return 0;
    if(x->require_ss){
        unsigned speed=(v>>10)&15;uint32_t pmsc;
        if(!rd(x,offset+4,&pmsc)||(pmsc&0xffff))return 0;
        if(!(v&BIT(1))||(v&BIT(4))||(v&0x1e0u)||speed!=4)return 0;
        if(x->speed&&x->speed!=speed)return 0;
        x->speed=d->speed=4;x->port_reset=3;if(!d->port_ready)d->port_ready=1;
        if(!x->announced){x->announced=1;notify(x,A733_XHCI_ATTACH,0,0,0,0);}
        return 1;
    }
    if(!x->announced){x->announced=1;notify(x,A733_XHCI_ATTACH,0,0,0,0);return 1;}
    if(x->port_reset==1) {
        if(!(v&BIT(9))||!wr(x,offset,(v&PRESERVE)|BIT(4)))return 0;x->port_reset=2;return 1;
    }
    if(x->port_reset==2) {
        if(v&BIT(4))return 1;
        unsigned speed=(v>>10)&15;
        if(!(v&BIT(1))||speed<1||speed>3||(v&UINT32_C(0x1e0)))return 0;
        if(x->speed&&x->speed!=speed)return 0;x->speed=d->speed=speed;
        x->port_reset=3;d->port_ready=1;
    }else if(x->port_reset==3&&((v>>10)&15)!=x->speed)return 0;
    return 1;
}
/* xHCI 1.2 sections 4.4, 4.6.4 and 4.6.9: before Disable Slot, every endpoint
 * must be Stopped or idle and every Transfer Event received. Stop each Running
 * endpoint and reset each Halted one, then disable the slot. Each step needs
 * its matched completion; anything else fails and retains the controller. */
static int teardown(struct a733_xhci *x,unsigned j)
{
    struct a733_xhci_device *d=&x->devices[j];
    if(!d->slot)return reclaim(x,j);
    if(!barrier(x))return 0;
    if(d->detach==1){
        for(unsigned i=0;i<A733_XHCI_ENDPOINTS;++i){
            struct a733_xhci_endpoint *e=&d->endpoints[i];
            if(!e->number)continue;
            unsigned state=get32(d->device+e->number*x->context_bytes)&7;
            if(state==1||state==2){
                d->stopping=i+1;return command(x,d,state==1?C_STOP:C_RESET_ENDPOINT,(d->slot<<24)|(e->number<<16));
            }
            if(state==4||(e->pending==2&&!finished(x,d,e))||(e->pending==6&&!isoch_finished(x,d,e)))return 0;
        }
        d->detach=2;
    }
    return command(x,d,C_DISABLE,d->slot<<24);
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
    if(!rd(x,12,&hcs2))return 0;x->root_u1=hcs2&255;x->root_u2=hcs2>>16;
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
    if((h&255)==2&&(h>>24)==3&&(x->require_ss||x->companion_root)){
        if(x->usb3_seen||!rd(x,cursor+4,&name)||!rd(x,cursor+8,&ports)||!rd(x,cursor+12,&slots)||
           name!=UINT32_C(0x20425355)||((ports>>8)&255)!=1||!(ports&255)||(ports&255)>x->cap_ports||ports>>28)return 0;
        x->usb3_seen=1;x->usb3_port=ports&255;x->usb3_slot_type=slots&31;
    }
    unsigned next=(h>>8)&255;
    if(next){x->ext_cursor=cursor+next*4;if(x->ext_cursor>=0x1000)return 0;}
    else {
        if(!x->usb2_port)return 0;
        if(x->require_ss||x->companion_root){
            if(!x->usb3_port||x->usb3_port==x->usb2_port)return 0;
            if(x->require_ss){x->usb2_companion_port=x->usb2_port;x->usb2_port=x->usb3_port;x->slot_type=x->usb3_slot_type;}
            else x->usb2_companion_port=x->usb3_port;
        }
        x->phase=P_HALTED;
    }
    return 1;
}
int a733_xhci_init(struct a733_xhci *x,const struct a733_xhci_ops *ops,void *context,
    struct device_memory_pool *pool,struct device_memory_lease lease,uint64_t generation,uint64_t deadline,uint64_t lifetime)
{
    if(!x||x->self||!ops||!pool||!context||!generation||!deadline||lifetime<deadline||
       !ops->now||!ops->current||!ops->live||!ops->read32||!ops->write32||!ops->write64||!ops->barrier||!ops->outside||!ops->notice||
       !a733_ranges_disjoint(x,sizeof(*x),ops,sizeof(*ops))||!a733_ranges_disjoint(x,sizeof(*x),pool,sizeof(*pool)))return 0;
    const struct a733_xhci_ops copied=*ops;struct device_memory_extent e;enum device_memory_state state;
    if(copied.current(context)!=1||!device_memory_describe(pool,lease,&e,&state)||state!=DEVICE_MEMORY_CPU||
       e.bytes!=A733_XHCI_MEMORY_BYTES||(e.cpu&4095)||(e.physical&4095)||
       e.physical>UINT64_MAX-e.bytes||!a733_ranges_disjoint(x,sizeof(*x),(void *)e.cpu,e.bytes)||
       !a733_ranges_disjoint(pool,sizeof(*pool),(void *)e.cpu,e.bytes)||copied.outside(context,x,sizeof(*x))!=1||
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
        if(!x->require_ss&&x->port_reset&&x->devices[0].port_ready<3&&x->reset_deadline<x->deadline)x->deadline=x->reset_deadline;
        if(x->command_kind&&x->command_deadline<x->deadline)x->deadline=x->command_deadline;
        for(unsigned j=0;j<A733_XHCI_DEVICES;++j){
            struct a733_xhci_device *d=&x->devices[j];
            if((d->detach||(d->port_ready&&d->port_ready!=3))&&d->deadline&&d->deadline<x->deadline)x->deadline=d->deadline;
            if(d->detach)continue;
            /* The isochronous bound is checked against the event ring below. */
            for(unsigned i=0;i<A733_XHCI_ENDPOINTS;++i)if(((d->endpoints[i].pending&&d->endpoints[i].pending!=6)||
                (i&&d->endpoints[i].opened&&!d->endpoints[i].configured))&&
                d->endpoints[i].deadline<x->deadline)x->deadline=d->endpoints[i].deadline;
        }
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
    case P_CONFIG:if(!wr(x,x->operational+0x38,x->cap_slots<A733_XHCI_DEVICES?x->cap_slots:A733_XHCI_DEVICES))goto failed;x->phase=P_RUN;break;
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
        if(!x->irq_mode||x->irq_service){int e=event_step(x);if(e<0)goto failed;if(e>0)break;}
        if(isoch_expired(x))goto failed;
        if(x->command_kind)break;
        {
            /* Children before their hub; a detaching hub with a bound child
             * that is not detaching is inconsistent. */
            unsigned waiting=0;
            for(unsigned j=A733_XHCI_DEVICES;j-->0;)if(x->devices[j].detach){
                if(!childless(x,j)){waiting=1;continue;}
                if(!teardown(x,j))goto failed;goto done;
            }
            if(waiting)goto failed;
        }
        if(x->isoch.requested&&x->isoch.count<A733_XHCI_ISOCH_TDS){
            struct a733_xhci_device *d=&x->devices[x->isoch.device-1];struct a733_xhci_endpoint *e=&d->endpoints[1];
            if(!d->detach&&d->port_ready==3&&e->configured&&!e->aborted){if(!isoch_refill(x,d,e))goto failed;goto done;}
        }
        if(x->detaching){x->detaching=x->announced=x->removed=x->port_reset=x->speed=0;goto done;}
        for(unsigned j=0;j<A733_XHCI_DEVICES;++j){
            struct a733_xhci_device *d=&x->devices[j];
            if(d->port_ready==1){if(!command(x,d,C_ENABLE,x->slot_type<<16))goto failed;d->port_ready=4;goto done;}
            if(d->port_ready==2){address_context(x,d);if(!command(x,d,C_ADDRESS,(d->slot<<24)|BIT(9)))goto failed;d->port_ready=4;goto done;}
            if(d->port_ready!=3)continue;
            struct a733_xhci_endpoint *control=&d->endpoints[0];
            if(control->pending==3||control->pending==4){
                /* Reserve the next unpublished producer slot for the next
                 * Setup TD, skipping every stage of the failed request. Never
                 * ring the endpoint between Reset and Set Dequeue. */
                if((control->ring[control->index].control&1)==control->cycle)goto failed;
                unsigned kind=control->pending==3?C_RESET_ENDPOINT:C_SET_DEQUEUE;
                if(!command(x,d,kind,(d->slot<<24)|BIT(16)))goto failed;goto done;
            }
            if(d->endpoints[0].pending==1&&d->pending_address&&!d->address_status){
                address_context(x,d);if(!command(x,d,C_ADDRESS,d->slot<<24))goto failed;d->endpoints[0].pending=2;goto done;
            }
            if(d->evaluate_pending){
                memset(d->input,0,33*x->context_bytes);put32(d->input+4,BIT(1));ep_context(x,d,&d->endpoints[0]);
                if(!command(x,d,C_EVALUATE,d->slot<<24))goto failed;goto done;
            }
            if(!d->hub||d->hub_ports){
                for(unsigned i=1;i<A733_XHCI_ENDPOINTS;++i)if(d->endpoints[i].opened&&!d->endpoints[i].configured){
                    if(!d->address||!configure(x,d))goto failed;goto done;
                }
            }
            for(unsigned i=0;i<A733_XHCI_ENDPOINTS;++i)if(d->endpoints[i].pending==1&&d->endpoints[i].configured){
                if(!submit_endpoint(x,d,&d->endpoints[i]))goto failed;goto done;
            }
        }
        break;
    default:goto failed;
    }
done:return x->phase==P_ACTIVE&&x->devices[0].port_ready==3&&!x->devices[0].detach?A733_XHCI_READY:A733_XHCI_PENDING;
failed:fault(x);return A733_XHCI_RETAINED;
}
int a733_xhci_bind_child(struct a733_xhci *x,unsigned parent,unsigned port_number,unsigned speed,uint64_t deadline)
{
    if(!owner(x)||x->failed||x->cap_slots<2||!port_number||port_number>7||speed<1||speed>4||
       deadline>x->lifetime_deadline||settled(x,deadline)!=1)return 0;
    struct a733_xhci_device *hub=parent?device_address(x,parent):NULL;
    if(!hub||(hub->speed==4?speed!=4:speed>3)||hub->address!=parent||!hub->hub||!hub->hub_configured||port_number>hub->hub_ports||
       (hub->speed!=3&&hub->speed!=4)||!hub->zero_retired||hub->tier>=A733_XHCI_HUB_TIERS)return 0;
    for(unsigned j=1;j<A733_XHCI_DEVICES&&j<x->cap_slots;++j){
        struct a733_xhci_device *d=&x->devices[j];
        if(d->detach||d==hub)continue;
        if(d->port_ready){
            if(d->parent!=hub->slot||d->parent_port!=port_number)continue;
            return x->default_device==j&&!d->zero_retired&&d->speed==speed;
        }
        /* TinyUSB serializes default-address enumeration. A live predecessor
         * must retire address0 before another child can claim that identity. */
        if(!x->devices[x->default_device].zero_retired)return 0;
        d->parent=hub->slot;d->parent_port=port_number;d->route=hub->route|(port_number<<(4*hub->tier));d->tier=hub->tier+1;
        d->speed=speed;d->deadline=deadline;d->port_ready=1;x->default_device=j;return 1;
    }
    return 0;
}
int a733_xhci_mark_hub(struct a733_xhci *x,unsigned address)
{
    struct a733_xhci_device *d=device_address(x,address);
    if(!owner(x)||x->failed||!d||(d->speed!=3&&d->speed!=4)||d->hub_ports||d->tier>=A733_XHCI_HUB_TIERS)return 0;
    if(d!=&x->devices[0]){
        /* A second tier is a USB2 high-speed hub below a high-speed hub. */
        const struct a733_xhci_device *parent=slot_record(x,d->parent);
        if(d->speed!=3||d->tier!=1||!parent||parent->speed!=3)return 0;
    }
    d->hub=1;return 1;
}
int a733_xhci_hub_configure(struct a733_xhci *x,unsigned address,unsigned ports,unsigned ttt,unsigned mtt)
{
    struct a733_xhci_device *d=device_address(x,address);
    if(!owner(x)||x->failed||!d||!d->hub||!address||!ports||ports>7||ttt>3||mtt>1||(d->speed==4&&(ttt||mtt))||d->hub_ports||settled(x,x->deadline)!=1)return 0;
    d->hub_ports=ports;d->hub_ttt=ttt;d->hub_mtt=mtt;return 1;
}
int a733_xhci_retire_zero(struct a733_xhci *x)
{
    struct a733_xhci_device *d=device_address(x,0);
    if(!owner(x)||x->failed||!d||!d->address||!d->hardware_address||d->pending_address||d->address_status||d->control_stage||d->endpoints[0].pending)return 0;
    d->zero_retired=1;return 1;
}
unsigned a733_xhci_device_speed(const struct a733_xhci *x,unsigned address)
{
    if(!x||x->self!=x||x->failed)return 0;
    if(!address)return x->default_device<A733_XHCI_DEVICES&&!x->devices[x->default_device].detach?x->devices[x->default_device].speed:0;
    for(unsigned i=0;i<A733_XHCI_DEVICES;++i)if(x->devices[i].address==address&&!x->devices[i].detach)return x->devices[i].speed;
    return 0;
}
int a733_xhci_reset_port(struct a733_xhci *x,uint64_t deadline)
{
    struct a733_xhci_device *d=x?&x->devices[0]:NULL;
    if(x&&x->require_ss)return owner(x)&&!x->failed&&x->connected&&x->speed==4&&settled(x,deadline)==1;
    if(!owner(x)||x->failed||x->phase!=P_ACTIVE||!x->connected||x->port_reset||d->slot||
       deadline>x->lifetime_deadline||settled(x,deadline)!=1)return 0;
    x->port_reset=1;x->reset_deadline=deadline;return 1;
}
int a733_xhci_port_connected(const struct a733_xhci *x)
{return x&&x->self==x&&!x->failed&&x->connected;}
int a733_xhci_open(struct a733_xhci *x,unsigned address,unsigned ep,unsigned type,unsigned packet,uint64_t deadline)
{
    struct a733_xhci_device *d=device_address(x,address);
    if(!owner(x)||!d||x->failed||!x->connected||address!=d->address||ep&~0x8fu||deadline>x->lifetime_deadline||settled(x,deadline)!=1)return 0;
    if(!(ep&15)) {
        if(type||ep||d->endpoints[0].pending||d->evaluate_pending||
           (d->speed==4&&packet!=512)||(d->speed==2&&packet!=8)||(d->speed==3&&packet!=8&&packet!=64)||
           (d->speed==1&&packet!=8&&packet!=16&&packet!=32&&packet!=64))return 0;
        unsigned actual=d->speed==4?512:d->speed==3?64:packet;
        if(address&&actual!=d->endpoints[0].packet)d->evaluate_pending=1;
        d->endpoints[0].opened=1;d->endpoints[0].packet=actual;d->endpoints[0].number=1;return 1;
    }
    if(!address||type!=2||packet!=(d->speed==4?1024u:512u)||(d->speed!=3&&d->speed!=4))return 0;
    struct a733_xhci_endpoint *e=endpoint(d,ep);
    if(e)return e->packet==packet&&e->transfer_type==type&&!e->aborted;
    for(unsigned i=1;i<A733_XHCI_ENDPOINTS;++i)if(!d->endpoints[i].opened) {
        e=&d->endpoints[i];
        if(i==2){
            unsigned owner=(unsigned)(d-x->devices)+1;
            if(x->extra_endpoint_owner)return 0;
            x->extra_endpoint_owner=owner;
            uint8_t *p=(void *)x->extent.cpu;uint64_t base=x->extent.physical;
            e->ring=(void *)(p+65536);e->ring_pa=base+65536;
            e->bounce=p+94208;e->bounce_pa=base+94208;e->cycle=1;
        }
        e->opened=1;e->transfer_type=type;e->number=dci(ep);e->address=ep;e->packet=packet;e->direction=!!(ep&128);e->deadline=deadline;return 1;
    }
    return 0;
}
int a733_xhci_set_burst(struct a733_xhci *x,unsigned address,unsigned ep,unsigned burst)
{
    struct a733_xhci_device *d=device_address(x,address);struct a733_xhci_endpoint *e=d?endpoint(d,ep):NULL;
    if(!owner(x)||x->failed||!d||d->speed!=4||!e||e->configured||e->pending||e->transfer_type!=2||burst>15)return 0;
    e->burst=burst;return 1;
}
int a733_xhci_open_interrupt(struct a733_xhci *x,unsigned address,unsigned ep,
    unsigned packet,unsigned interval,uint64_t deadline)
{
    struct a733_xhci_device *d=device_address(x,address);
    if(!owner(x)||!d||x->failed||!x->connected||!address||address!=d->address||
       ep&~0x8fu||!(ep&128)||!(ep&15)||!packet||packet>(d->speed==2?8u:64u)||
       (d->speed<1||d->speed>4)||!interval||interval>(d->speed>=3?16u:255u)||deadline>x->lifetime_deadline||settled(x,deadline)!=1)return 0;
    unsigned encoded;
    if(d->speed>=3)encoded=interval-1;
    else{encoded=3;for(unsigned v=interval;v>1;v>>=1)++encoded;}
    struct a733_xhci_endpoint *e=endpoint(d,ep);
    if(e)return e->packet==packet&&e->transfer_type==3&&e->interval==encoded&&!e->aborted;
    for(unsigned i=1;i<2;++i)if(!d->endpoints[i].opened){
        e=&d->endpoints[i];e->opened=1;e->transfer_type=3;e->interval=encoded;
        e->number=dci(ep);e->address=ep;e->packet=packet;e->direction=1;e->deadline=deadline;return 1;
    }
    return 0;
}
int a733_xhci_open_isoch(struct a733_xhci *x,unsigned address,unsigned ep,
    unsigned packet,unsigned interval,uint64_t deadline)
{
    struct a733_xhci_device *d=device_address(x,address);
    if(!owner(x)||!d||x->failed||!x->connected||!address||address!=d->address||
       ep&~0x8fu||!(ep&128)||!(ep&15)||!packet||packet>A733_XHCI_ISOCH_PACKET||(d->speed!=1&&d->speed!=3)||
       !interval||interval>(d->speed==3?4u:1u)||deadline>x->lifetime_deadline||settled(x,deadline)!=1)return 0;
    unsigned encoded=d->speed==3?interval-1:3;
    struct a733_xhci_endpoint *e=endpoint(d,ep);
    if(e)return e->packet==packet&&e->transfer_type==1&&e->interval==encoded&&!e->aborted;
    if(x->isoch.device||d->endpoints[1].opened)return 0;
    e=&d->endpoints[1];e->opened=1;e->transfer_type=1;e->interval=encoded;
    e->number=dci(ep);e->address=ep;e->packet=packet;e->direction=1;e->deadline=deadline;
    x->isoch=(struct a733_xhci_isoch){.device=(unsigned)(d-x->devices)+1};return 1;
}
static struct a733_xhci_endpoint *isoch_endpoint(struct a733_xhci *x,unsigned address,unsigned ep)
{
    struct a733_xhci_device *d=device_address(x,address);struct a733_xhci_endpoint *e=d?endpoint(d,ep):NULL;
    if(!owner(x)||x->failed||!address||!e||address!=d->address||e->transfer_type!=1||e->aborted||
       x->isoch.device!=(unsigned)(d-x->devices)+1)return NULL;
    return e;
}
int a733_xhci_isoch_start(struct a733_xhci *x,unsigned address,unsigned ep)
{
    if(!isoch_endpoint(x,address,ep))return 0;
    x->isoch.requested=1;return 1;
}
int a733_xhci_isoch_take(struct a733_xhci *x,unsigned address,unsigned ep,uint8_t *out,unsigned capacity,unsigned *bytes)
{
    struct a733_xhci_endpoint *e=isoch_endpoint(x,address,ep);struct a733_xhci_isoch *s=&x->isoch;
    if(!e||!bytes||capacity<e->packet||!outside(x,out,e->packet))return -1;
    while(s->done){
        unsigned i=s->head,state=s->state[i],n=s->bytes[i];
        s->state[i]=0;s->head=(i+1)%A733_XHCI_ISOCH_TDS;--s->count;--s->done;
        if(state==3)continue;
        /* The completion event returned this packet's bounce bytes to software. */
        if(state!=2||n>e->packet||!barrier(x)){fault(x);return -1;}
        memcpy(out,e->bounce+i*A733_XHCI_ISOCH_PACKET,n);*bytes=n;return 1;
    }
    return 0;
}
int a733_xhci_setup(struct a733_xhci *x,unsigned address,const uint8_t setup[8],uint64_t deadline)
{
    struct a733_xhci_device *d=device_address(x,address);
    if(!owner(x)||!d||x->failed||!outside(x,setup,8)||address!=d->address||!d->endpoints[0].opened||
       d->control_stage!=CONTROL_IDLE||d->endpoints[0].pending||deadline>x->lifetime_deadline)return 0;
    uint8_t copied[8];memcpy(copied,setup,8);unsigned length=copied[6]|(unsigned)copied[7]<<8;
    if(length>A733_XHCI_TRANSFER_BYTES||settled(x,deadline)!=1)return 0;
    unsigned set_address=copied[0]==0&&copied[1]==5;
    if(set_address&&(address||length||copied[3]||copied[4]||copied[5]||!copied[2]||copied[2]>127||d->pending_address))return 0;
    /* No other form of SET_ADDRESS may reach the physical setup ring. */
    if(copied[1]==5&&(copied[0]&0x60)==0&&!set_address)return 0;
    memcpy(d->setup,copied,8);d->control_length=length;d->control_in=!!(copied[0]&128);d->control_stage=CONTROL_SETUP;
    if(set_address)d->pending_address=copied[2];
    struct a733_xhci_endpoint *e=&d->endpoints[0];e->address=0;e->length=8;e->kind=T_SETUP;e->direction=0;e->pending=1;e->deadline=deadline;
    return 1;
}
int a733_xhci_transfer(struct a733_xhci *x,unsigned address,unsigned ep,uint8_t *buffer,unsigned bytes,uint64_t deadline)
{
    struct a733_xhci_device *d=device_address(x,address);
    if(!owner(x)||!d||x->failed||address!=d->address||ep&~0x8fu||bytes>A733_XHCI_TRANSFER_BYTES||
       (bytes&&!outside(x,buffer,bytes))||deadline>x->lifetime_deadline)return 0;
    struct a733_xhci_endpoint *e=endpoint(d,ep);
    if(!e||!e->opened||e->pending||e->aborted||e->transfer_type==1||settled(x,deadline)!=1)return 0;
    unsigned direction=!!(ep&128),kind=T_NORMAL;
    if(!(ep&15)) {
        if(d->control_stage==CONTROL_DATA) {
            if(bytes!=d->control_length||direction!=d->control_in)return 0;kind=T_DATA;
        } else if(d->control_stage==CONTROL_STATUS) {
            unsigned status_in=d->control_length?!d->control_in:1;
            if(bytes||direction!=status_in)return 0;kind=d->address_status?T_ADDRESS_STATUS:T_STATUS;
        } else return 0;
    }
    if(bytes&&!direction)memcpy(e->bounce,buffer,bytes);
    e->caller=bytes&&direction?buffer:NULL;e->length=bytes;e->direction=direction;e->kind=kind;
    e->address=ep;e->deadline=deadline;e->pending=1;return 1;
}
int a733_xhci_abort(struct a733_xhci *x,unsigned address,unsigned ep)
{
    struct a733_xhci_device *d=device_address(x,address);
    if(!owner(x)||ep&~0x8fu)return 0;
    if(!d)return detaching(x,address); /* Detach already revoked its destinations. */
    if(address!=d->address)return 0;
    struct a733_xhci_endpoint *e=endpoint(d,ep);
    if(!e||!e->pending)return 1;e->caller=NULL;e->aborted=1;fault(x);return 1;
}
int a733_xhci_detach(struct a733_xhci *x,unsigned address)
{
    if(!owner(x)||x->failed||address>127||settled(x,x->lifetime_deadline)!=1)return 0;
    struct a733_xhci_device *d=device_address(x,address);
    if(!d||!d->port_ready)return 1; /* Already detaching, or no slot or context exists. */
    if(d==&x->devices[0])return 0; /* The root device follows its port. */
    detach_tree(x,d);return 1;
}
void a733_xhci_lost(struct a733_xhci *x)
{if(owner(x))fault(x);}
/* Intel xHCI1.2 4.17,5.4.2,5.5.2; pinned Linux xhci_irq uses the same
 * EINT/IP ordering. GIC masking/EOI belong to the application dispatcher. */
int a733_xhci_interrupt_begin(struct a733_xhci *x)
{
    uint32_t status,iman;
    if(!owner(x)||x->failed||!x->irq_mode||x->irq_service)return -1;
    if(!rd(x,x->operational+4,&status)||(status&(1|BIT(2)|BIT(11)|BIT(12)))||
       !rd(x,x->runtime+0x20,&iman)||!(iman&2)||
       !wr(x,x->operational+4,BIT(3))||!wr(x,x->runtime+0x20,iman|1)||
       !rd(x,x->runtime+0x20,&iman)||!(iman&2))goto failed;
    x->irq_service=1;x->irq_events=0;x->irq_asserted=!!(status&BIT(3));return 1;
failed:fault(x);return -1;
}
int a733_xhci_interrupt_start(struct a733_xhci *x)
{
    uint32_t command,iman;
    if(!owner(x)||x->failed)return -1;
    if(x->irq_mode)return 1;
    if(x->phase!=P_ACTIVE)return 0;
    if(!rd(x,x->operational,&command)||command!=1||
       !rd(x,x->runtime+0x20,&iman)||(iman&2)||
       !wr(x,x->operational+4,BIT(3))||!wr(x,x->runtime+0x20,iman|3)||
       !rd(x,x->runtime+0x20,&iman)||!(iman&2)||
       !wr(x,x->operational,command|BIT(2))||
       !rd(x,x->operational,&command)||command!=5)goto failed;
    /* Own the startup backlog even when no CPU interrupt was delivered. */
    x->irq_mode=1;x->irq_service=1;x->irq_events=0;x->irq_asserted=!!(iman&1);return 1;
failed:fault(x);return -1;
}
int a733_xhci_interrupt_finish(struct a733_xhci *x)
{
    uint32_t iman,status;
    if(!owner(x)||x->failed||!x->irq_mode||!x->irq_service)return -1;
    if(!rd(x,x->operational+4,&status)||(status&(1|BIT(2)|BIT(11)|BIT(12)))||!barrier(x))goto failed;
    int ready=(x->events[x->event_index].control&1)==x->event_cycle;
    if(ready){
        /* Publish progress before the fixed32-entry ring can fill. The final
         * advancement is combined with EHB release, never duplicated. */
        if(x->irq_events>=A733_XHCI_EVENT_TRBS/2){
            if(!wr64(x,x->runtime+0x38,x->event_pa+16*x->event_index))goto failed;
            x->irq_events=0;
        }
        return 0;
    }
    if(x->irq_events){
        if(!wr64(x,x->runtime+0x38,(x->event_pa+16*x->event_index)|8))goto failed;
        x->irq_events=0;
    }else if(x->irq_asserted)goto failed; /* IP without an owned event is uncertain. */
    if(!barrier(x)||!rd(x,x->runtime+0x20,&iman)||!(iman&2))goto failed;
    ready=(x->events[x->event_index].control&1)==x->event_cycle;
    x->irq_service=0;x->irq_asserted=0;
    if(ready||(iman&1))return a733_xhci_interrupt_begin(x)==1?0:-1;
    return 1;
failed:fault(x);return -1;
}
