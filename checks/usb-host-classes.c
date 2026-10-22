/* SPDX-License-Identifier: Apache-2.0 */
/* USB2 host topology, hub power and audio capture against a ring-level model.
 * The actual USB2 platform, xHCI core, TinyUSB adapter and pinned TinyUSB hub,
 * MSC, HID and audio classes run unchanged. The controller model reads the
 * core's command and transfer rings and contexts from the DMA parent, resolves
 * each slot through its route string to a modeled USB device tree, and posts
 * events to the event ring the core programmed. No board or USB device is used. */
#include "a733_usb2_native.h"
#include "tusb.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int usb_gpu_init(struct a733_gpu_resource_manager *,const struct a733_gpu_resource_ops *,void *);
#define PMIC_MODEL_NO_MAIN
#define a733_gpu_resources_init usb_gpu_init
#include "a733-pmic.c"
#undef a733_gpu_resources_init
#define PHYS UINT64_C(0x79000000)
#define BIT(n) (UINT32_C(1)<<(n))
#define FAIL(...) do{fprintf(stderr,"line %d: ",__LINE__);fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);abort();}while(0)
static struct pmic_model m;
static struct a733_usb2_native n;
static struct a733_typec role;
static struct a733_twi bus;
static _Alignas(4096) uint8_t dma[A733_XHCI_MEMORY_BYTES];
static uint32_t reg[0x8000/4];
static const char *mode;
static int has(const char *word){return strstr(mode,word)!=NULL;}

/* ---- Shared platform composition, as in the USB2 host check ---- */
static int gpu_read(void *v,uintptr_t a,uint32_t *out){*out=*pmic_model_word(v,a);return 1;}
static int gpu_update(void *v,uintptr_t a,uint32_t required,uint32_t set,uint64_t d)
{struct pmic_model *p=v;assert(a==0x070101ac&&set==1&&p->now<d);uint32_t *w=pmic_model_word(p,a);assert((*w&required)==required);*w|=set;return 1;}
static int usb_gpu_init(struct a733_gpu_resource_manager *g,const struct a733_gpu_resource_ops *unused,void *v)
{(void)unused;const struct a733_gpu_resource_ops o={pmic_model_now,gpu_read,gpu_update,pmic_model_gpu_rail};return a733_gpu_resources_init(g,&o,v);}
static int current(void *v){(void)v;return !m.worker;}
static int admit(void *v,const struct a733_usb2_platform_config *c){assert(v==&m&&c->identity==88&&c->topology==12);return 1;}
static int lock(void *v,uint64_t d){struct pmic_model *p=v;if(p->register_held||p->now>=d)return 0;p->register_held=1;return 1;}
static void unlock(void *v){struct pmic_model *p=v;assert(p->register_held);p->register_held=0;}
static void retain(void *v){assert(((struct pmic_model *)v)->register_held);}
static int read_reg(void *v,uintptr_t a,uint32_t *out){assert(m.register_held&&a733_usb2_platform_address(a,0));*out=*pmic_model_word(v,a);return 1;}
static int write_reg(void *v,uintptr_t a,uint32_t value)
{assert(m.register_held&&a733_usb2_platform_address(a,1));uint32_t *p=pmic_model_word(v,a);assert(!((*p^value)&~a733_usb2_platform_mask(a)));*p=value;return 1;}
static int unused_half(void *v,uintptr_t a,uint16_t *out){(void)v;(void)a;(void)out;assert(0);return 0;}
static int unused_half_write(void *v,uintptr_t a,uint16_t value){(void)v;(void)a;(void)value;assert(0);return 0;}
static int unused_prepare(void *v,uint64_t d){(void)v;(void)d;assert(0);return -1;}
static int unused_vbus(void *v,unsigned on,uint64_t d){(void)v;(void)on;(void)d;assert(0);return -1;}
static int unused_irq(void *v,unsigned *out,uint64_t d){(void)v;(void)out;(void)d;assert(0);return -1;}
static void observe_role(void)
{if(!atomic_load(&role.revoked))role.published=(struct a733_typec_token){(uintptr_t)&role,70,71,m.now,1};}
static void setup(void)
{
    pmic_model_setup(&m);
    m.pmic[0x11]=0x11;m.pmic[0x1a]=30;m.pmic[0x23]=8;m.pmic[0x3f]=12;m.pmic[0x33]=13;
    *pmic_model_word(&m,0x02002000)=0xf8636300;*pmic_model_word(&m,0x020020a0)=0xfe106310;
    *pmic_model_word(&m,0x02003908)=0x0b860b86;*pmic_model_word(&m,0x02003988)=0x0b860000;
    *pmic_model_word(&m,0x02002580)=0x83000002;*pmic_model_word(&m,0x02002588)=0x84000001;
    *pmic_model_word(&m,0x020025c0)=1u<<8;*pmic_model_word(&m,0x020025e0)=1u<<28;
    *pmic_model_word(&m,0x020033c0)=0x81000005;*pmic_model_word(&m,0x020033c4)=1u<<16;
    *pmic_model_word(&m,0x02003b28)=0x20000;
    *pmic_model_word(&m,0x07067000)=*pmic_model_word(&m,0x07067008)=8;
    *pmic_model_word(&m,0x07068000)=*pmic_model_word(&m,0x07068008)=8;
    *pmic_model_word(&m,0x06b00010)=0x22418;
    *pmic_model_word(&m,0x06a0c120)=0x3331190a;*pmic_model_word(&m,0x06a0c1a0)=0x3139302a;
    *pmic_model_word(&m,0x06a0c140)=2;*pmic_model_word(&m,0x06a0c144)=1u<<24;*pmic_model_word(&m,0x06a0c14c)=4;
    const struct a733_typec_config tc={.ops={pmic_model_outside,pmic_model_now,current,unused_prepare,current,unused_vbus,unused_irq},
        .context=&m,.bus=&bus,.owner=70,.generation=71,.qualification=72,.recovery=73,
        .source_min_mv=5000,.source_max_mv=5100,.source_ma=500,.zero_threshold_max_mv=800,.device_id=0x80};
    assert(a733_typec_init(&role,&tc));observe_role();
    struct a733_usb2_platform_config c={.owner=99,.identity=88,.topology=12,.deadline=4000000,.lifetime_deadline=30000000,
        .pck_client=2,.fabric={0,0x83000002,0x84000001},.fabric_hz={24000000,200000000,200000000},
        .supply_uv={3300000,3300000,800000,800000,1800000},.revision=0x3139302a};
    const struct a733_usb2_platform_ops o={pmic_model_now,current,admit,lock,unlock,retain,read_reg,write_reg,pmic_model_outside,unused_half,unused_half_write};
    n.self=&n;n.initialized=1;
    assert(a733_usb2_platform_init(&n.platform,&c,&o,&m,&m.clocks,&m.rails,&m.provider,&m.gpu,&role));
    for(unsigned i=0;i<10000;++i){
        observe_role();enum a733_usb2_result r=a733_usb2_platform_poll(&n.platform);
        if(r==A733_USB2_READY)return;
        assert(r!=A733_USB2_RETAINED);assert(pmic_model_step(&m)!=A733_PMIC_RETAINED);m.now+=20;
    }
    assert(0);
}

/* ---- DMA and register helpers ---- */
static uint32_t get32(const void *v){const uint8_t *p=v;return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(void *v,uint32_t x){uint8_t *p=v;for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(x>>(8*i));}
static uint64_t get64(const void *v){return get32(v)|(uint64_t)get32((const uint8_t *)v+4)<<32;}
static void put64(void *v,uint64_t x){put32(v,(uint32_t)x);put32((uint8_t *)v+4,(uint32_t)(x>>32));}
static unsigned le16(const uint8_t *p){return p[0]|(unsigned)p[1]<<8;}
/* Every DMA address the core publishes must lie in its own parent. */
static uint8_t *cpu(uint64_t pa,size_t bytes)
{
    if(pa<PHYS||pa+bytes>PHYS+sizeof(dma))FAIL("DMA address %llx+%zu outside the parent",(unsigned long long)pa,bytes);
    return dma+(pa-PHYS);
}

/* ---- Modeled USB devices ---- */
enum { HUB=1,MSC,HID,AUDIO };
struct dev {
    unsigned kind,speed,present,port,bus_powered,self_reported,max_power,protocol,ports;
    unsigned audio_hs,audio_big,audio_hid,audio_rate,audio_alt,audio_set_cur;
    struct dev *parent,*child[8];
    unsigned status[8],change[8],configured,alternate[4],slot;
    uint8_t setup[8],data[512];unsigned data_len;
    uint8_t cbw[31];unsigned msc_phase,hid_reports,hid_sent;
    uint8_t descriptor[18],config[256];unsigned config_len;
};
static struct dev devices[8];static unsigned device_count;
static struct dev *root;
static const uint8_t hid_report[]={5,1,9,6,0xa1,1,5,7,0x19,0xe0,0x29,0xe7,0x15,0,0x25,1,
 0x75,1,0x95,8,0x81,2,0x95,1,0x75,8,0x81,1,0x95,6,0x75,8,0x15,0,0x25,0x65,
 5,7,0x19,0,0x29,0x65,0x81,0,0xc0};
static void describe(struct dev *d)
{
    uint8_t *c=d->config;unsigned at=0;
#define B(...) do{const uint8_t v_[]={__VA_ARGS__};memcpy(c+at,v_,sizeof(v_));at+=sizeof(v_);}while(0)
    const uint8_t packet0=d->speed==3?64:d->speed==2?8:64;
    const uint8_t base[18]={18,1,0,2,(uint8_t)(d->kind==HUB?9:0),0,(uint8_t)(d->kind==HUB?d->protocol:0),packet0,0x34,0x12,
        (uint8_t)(0x10+d->kind),0x56,0,1,0,0,0,1};
    memcpy(d->descriptor,base,18);
    const uint8_t attributes=d->bus_powered?0x80:0xc0,power=(uint8_t)(d->max_power/2);
    if(d->kind==HUB){
        B(9,2,0,0,1,1,0,attributes,power,9,4,0,0,1,9,0,1,0,7,5,0x81,3,1,0,12);
        if(d->protocol==2)B(9,4,0,1,1,9,0,2,0,7,5,0x81,3,1,0,12);
    }else if(d->kind==MSC){
        B(9,2,0,0,1,1,0,attributes,power,9,4,0,0,2,8,6,0x50,0,7,5,0x81,2,0,2,0,7,5,2,2,0,2,0);
    }else if(d->kind==HID){
        B(9,2,0,0,1,1,0,attributes,power,9,4,0,0,1,3,1,1,0,9,0x21,0x11,1,0,1,0x22,sizeof(hid_report),0,
          7,5,0x81,3,8,0,10);
    }else{
        /* UAC1 microphone: Audio Control with input terminal, feature unit
         * (master mute and volume) and USB-streaming output terminal; one
         * streaming interface with a mono and a stereo alternate. */
        const uint8_t interval=d->audio_hs?4:1,mono=d->audio_big?0x60:96,mono_hi=d->audio_big?2:0;
        B(9,2,0,0,(uint8_t)(d->audio_hid?3:2),1,0,attributes,power);
        B(9,4,0,0,0,1,1,0,0, 9,0x24,1,0,1,40,0,1,1, 12,0x24,2,1,1,2,0,2,0,0,0,0,
          10,0x24,6,2,1,1,3,0,0,0, 9,0x24,3,3,1,1,0,2,0);
        B(9,4,1,0,0,1,2,0,0);
        B(9,4,1,1,1,1,2,0,0, 7,0x24,1,3,1,1,0, 14,0x24,2,1,1,2,16,2,0x80,0x3e,0,0x80,0xbb,0,
          9,5,0x81,5,mono,mono_hi,interval,0,0, 7,0x25,1,1,0,0,0);
        B(9,4,1,2,1,1,2,0,0, 7,0x24,1,3,1,1,0, 11,0x24,2,1,2,2,16,1,0x80,0xbb,0,
          9,5,0x81,5,(uint8_t)(d->audio_big?0x60:192),mono_hi,interval,0,0, 7,0x25,1,1,0,0,0);
        if(d->audio_hid)B(9,4,2,0,1,3,0,0,0,9,0x21,0x11,1,0,1,0x22,sizeof(hid_report),0,7,5,0x82,3,8,0,10);
    }
#undef B
    put32(c+2,(get32(c+2)&~0xffffu)|at);d->config_len=at;
}
static struct dev *make(unsigned kind,unsigned speed,struct dev *parent,unsigned port)
{
    assert(device_count<8);struct dev *d=&devices[device_count++];
    *d=(struct dev){.kind=kind,.speed=speed,.parent=parent,.port=port,.present=1,.max_power=100,.protocol=1,.ports=4,.audio_alt=0};
    if(parent){assert(!parent->child[port]);parent->child[port]=d;}
    return d;
}
static void finish_devices(void){for(unsigned i=0;i<device_count;++i)describe(&devices[i]);}

/* ---- Controller model state ---- */
struct ep { uint64_t deq,next;unsigned cycle,state,type,packet,esit,scheduled,kick,late; };
static struct dev *slots[4];static struct ep eps[4][32];static unsigned slot_used;
static uint64_t command_deq;static unsigned command_cycle,command_kick;
static unsigned event_index,event_cycle=1,events_made;
static unsigned disables[8],disable_count,root_enabled;
static unsigned iso_missed_every,iso_services,iso_stalled,iso_frames_lost;
/* Missed-service forms (xHCI 1.2 section 4.10.3.2): 0 one event per missed TD
 * at its own TRB; 1 three TDs missed and one event for the last of them, as
 * after an Event Ring full condition; 2 an event without a TRB pointer; 3 one
 * event, then two further TDs missed without events, as without CFC. */
static unsigned iso_miss_kind,iso_silent,iso_tds_missed;
/* The controller finds the ring empty just before the refill and posts Ring
 * Overrun only after the refill's doorbell, which it then ignores. */
/* Overrun at the next interval: after the refill, without a TRB pointer, or
 * with the Event Ring Full code 21 in place of Ring Overrun 15. */
static unsigned iso_late_overrun,iso_overrun_no_trb,iso_overrun_code=15;
static uint8_t *event_ring(void){return cpu(get64(cpu(get64(&reg[0x1030/4]),16)),16*A733_XHCI_EVENT_TRBS);}
static void event(unsigned type,uint64_t pointer,unsigned code,unsigned remaining,unsigned dci,unsigned slot)
{
    uint64_t erdp=get64(&reg[0x1038/4])&~UINT64_C(15),base=get64(cpu(get64(&reg[0x1030/4]),16));
    unsigned consumed=(unsigned)((erdp-base)/16);
    if((event_index+1)%A733_XHCI_EVENT_TRBS==consumed)FAIL("event ring full");
    uint8_t *t=event_ring()+16*event_index;
    put64(t,pointer);put32(t+8,(code<<24)|remaining);put32(t+12,(slot<<24)|(dci<<16)|(type<<10)|event_cycle);
    if(++event_index==A733_XHCI_EVENT_TRBS){event_index=0;event_cycle^=1;}
    ++events_made;
}
/* The core reads PORTSC on every poll; before Run/Stop the event ring is not yet published. */
static void port_event(void){if(reg[0x40/4]&1)event(34,UINT64_C(1)<<24,1,0,0,0);}
static uint8_t *output(unsigned slot){return cpu(get64(cpu(get64(&reg[0x70/4])+8*slot,8)),2048);}
static struct dev *resolve(unsigned route,unsigned root_port)
{
    struct dev *d=root;if(root_port!=1||!d||!d->present||!root_enabled)return NULL;
    for(unsigned tier=0;route>>(4*tier);++tier){
        unsigned port=(route>>(4*tier))&15;
        if(!d||d->kind!=HUB||!port||port>d->ports||!(d->status[port]&2))return NULL;
        d=d->child[port];if(!d||!d->present)return NULL;
    }
    return d;
}
/* Some endpoint descriptor of any alternate setting has this DCI, type,
 * packet size and xHCI interval encoding. Configure Endpoint precedes
 * SET_INTERFACE, so the model cannot know the alternate yet. */
static int described(struct dev *d,unsigned dci,unsigned type,unsigned packet,unsigned interval)
{
    for(unsigned at=9;at<d->config_len;at+=d->config[at]){
        const uint8_t *e=d->config+at;if(e[1]!=5)continue;
        unsigned address=e[2],number=(address&15)*2+!!(address&128),kind=e[3]&3,want;
        if(number!=dci||(le16(e+4)&0x7ff)!=packet)continue;
        if(type!=(kind==1?(address&128?5u:1u):kind==3?(address&128?7u:3u):(address&128?6u:2u)))continue;
        if(kind==1)want=d->speed==3?e[6]-1u:e[6]+2u;
        else if(kind==3&&d->speed==3)want=e[6]-1u;
        else if(kind==3){want=3;for(unsigned b=e[6];b>1;b>>=1)++want;}
        else return 1;
        if(want==interval)return 1;
    }
    return 0;
}
static int reachable(const struct dev *d){for(;d;d=d->parent)if(!d->present)return 0;return 1;}
static void command(const uint8_t *t,uint64_t pointer)
{
    unsigned kind=(get32(t+12)>>10)&63,slot=get32(t+12)>>24,dci=(get32(t+12)>>16)&31,code=1;
    uint64_t parameter=get64(t);
    if(kind==9){
        slot=1;while(slot<4&&(slot_used&BIT(slot)))++slot;
        if(slot==4)code=9;else{slot_used|=BIT(slot);memset(eps[slot],0,sizeof(eps[slot]));}
    }else if(!(slot_used&BIT(slot)))FAIL("command %u for disabled slot %u",kind,slot);
    else if(kind==11||kind==12||kind==13){
        const uint8_t *in=cpu(parameter,33*64),*sc=in+64;uint8_t *out=output(slot);
        uint32_t add=get32(in+4);
        if(kind==11){
            unsigned route=get32(sc)&0xfffff,speed=(get32(sc)>>20)&15,mtt=!!(get32(sc)&BIT(25));
            unsigned tt_slot=get32(sc+8)&255,tt_port=(get32(sc+8)>>8)&255;
            struct dev *d=resolve(route,(get32(sc+4)>>16)&255);
            if(add!=3||!d)FAIL("Address Device route %x reaches no enabled device",route);
            if(speed!=d->speed)FAIL("Address Device speed %u for a speed-%u device",speed,d->speed);
            /* Low/full speed below a high-speed hub uses that hub's TT. */
            unsigned want_slot=0,want_port=0,want_mtt=0;
            if(d->speed<3&&d->parent){want_slot=d->parent->slot;want_port=d->port;want_mtt=d->parent->protocol==2&&d->parent->alternate[0]==1;}
            if(tt_slot!=want_slot||tt_port!=want_port||mtt!=want_mtt)FAIL("TT %u/%u/%u, expected %u/%u/%u",tt_slot,tt_port,mtt,want_slot,want_port,want_mtt);
            memcpy(out,sc,64);memcpy(out+64,in+128,64);
            unsigned bsr=!!(get32(t+12)&BIT(9));
            put32(out+12,((bsr?1u:2u)<<27)|(bsr?0:slot));put32(out+64,(get32(out+64)&~7u)|1);
            slots[slot]=d;d->slot=slot;
            uint64_t ring=get64(in+128+8);eps[slot][1]=(struct ep){.deq=ring&~UINT64_C(15),.cycle=ring&1,.state=1,.type=4};
        }else if(kind==13){
            if(add!=2)FAIL("Evaluate Context add flags %x",add);
            put32(out+64+4,(get32(out+64+4)&0xffff)|(get32(in+128+4)&0xffff0000u));
        }else{
            struct dev *d=slots[slot];
            put32(out,get32(sc));put32(out+4,get32(sc+4));put32(out+8,get32(sc+8));
            put32(out+12,(3u<<27)|(get32(out+12)&255));
            for(unsigned i=2;i<32;++i)if(add&BIT(i)){
                const uint8_t *e=in+64*(i+1);unsigned type=(get32(e+4)>>3)&7,packet=get32(e+4)>>16,cerr=(get32(e+4)>>1)&3;
                unsigned interval=(get32(e)>>16)&255,esit=get32(e+16)>>16;
                if(!described(d,i,type,packet,interval)||(type==5&&(cerr||esit!=packet))||(type!=5&&cerr!=3))
                    FAIL("Configure Endpoint dci %u type %u packet %u interval %u cerr %u esit %u",i,type,packet,interval,cerr,esit);
                memcpy(out+64*i,e,64);put32(out+64*i,(get32(out+64*i)&~7u)|1);
                uint64_t ring=get64(e+8);
                eps[slot][i]=(struct ep){.deq=ring&~UINT64_C(15),.cycle=ring&1,.state=1,.type=type,.packet=packet,
                    .esit=type==5?125u<<interval:0};
            }
        }
    }else if(kind==14||kind==15){
        struct ep *e=&eps[slot][dci];uint8_t *c=output(slot)+64*dci;
        if(kind==14){if(e->state!=2)code=19;else e->state=3;}
        else if(e->state!=1)code=19;
        else{
            /* Stop on a TD posts Stopped; with no TD, Stopped - Length Invalid. */
            const uint8_t *trb=cpu(e->deq,16);unsigned ready=(get32(trb+12)&1)==e->cycle;
            event(32,e->deq,ready?26:27,ready?get32(trb+8)&0x1ffff:0,dci,slot);
            e->state=3;e->scheduled=0;
        }
        put32(c,(get32(c)&~7u)|e->state);put64(c+8,e->deq|e->cycle);
    }else if(kind==10){
        if(disable_count<8)disables[disable_count++]=slot;
        if(slots[slot]&&slots[slot]->slot==slot)slots[slot]->slot=0;
        slots[slot]=NULL;slot_used&=~BIT(slot);memset(eps[slot],0,sizeof(eps[slot]));
    }else if(kind==16){eps[slot][dci].deq=parameter&~UINT64_C(15);eps[slot][dci].cycle=parameter&1;}
    else FAIL("unexpected command %u",kind);
    event(33,pointer,code,0,0,slot);
}
static void commands(void)
{
    if(!command_kick)return;command_kick=0;
    for(unsigned guard=0;guard<64;++guard){
        const uint8_t *t=cpu(command_deq,16);uint32_t control=get32(t+12);
        if((control&1)!=command_cycle)return;
        if(((control>>10)&63)==6){if(control&2)command_cycle^=1;command_deq=get64(t);continue;}
        uint64_t pointer=command_deq;command_deq+=16;command(t,pointer);
    }
}

/* ---- Device behavior ---- */
static void hub_port_power(struct dev *h,unsigned port)
{
    h->status[port]|=256;
    if(h->child[port]&&h->child[port]->present&&!(h->status[port]&1)){h->status[port]|=1;h->change[port]|=1;}
}
static unsigned speed_bits(const struct dev *d){return d->speed==3?1024:d->speed==2?512:0;}
/* Returns 0 for a STALL handshake. */
static int request(struct dev *d)
{
    const uint8_t *s=d->setup;unsigned value=le16(s+2),index=le16(s+4),length=le16(s+6);
    d->data_len=0;
    if(s[0]==0x80&&s[1]==6){
        const uint8_t string[]={4,3,9,4};
        const uint8_t *src=(value>>8)==1?d->descriptor:(value>>8)==2?d->config:(value>>8)==3?string:NULL;
        unsigned size=(value>>8)==1?18:(value>>8)==2?d->config_len:4;
        if(!src)return 0;
        d->data_len=length<size?length:size;memcpy(d->data,src,d->data_len);
    }else if(s[0]==0x81&&s[1]==6&&value==0x2200){d->data_len=sizeof(hid_report);memcpy(d->data,hid_report,sizeof(hid_report));}
    else if(s[0]==0x80&&s[1]==0){d->data_len=2;d->data[0]=(uint8_t)d->self_reported;d->data[1]=0;}
    else if(d->kind==HUB&&s[0]==0xa0&&s[1]==6){
        const uint8_t hub[9]={9,0x29,(uint8_t)d->ports,0x49,0,10,0,0,255};d->data_len=9;memcpy(d->data,hub,9);
    }else if(d->kind==HUB&&s[0]==0xa0&&s[1]==0){d->data_len=4;memset(d->data,0,4);}
    else if(d->kind==HUB&&s[0]==0xa3&&s[1]==0){
        d->data_len=4;put32(d->data,d->status[index]|(uint32_t)d->change[index]<<16);
    }else if(d->kind==MSC&&s[0]==0xa1&&s[1]==0xfe){d->data_len=1;d->data[0]=0;}
    else if(d->kind==AUDIO&&s[0]==0xa1&&s[1]>=0x82&&s[1]<=0x84){
        /* Volume minimum -32 dB, maximum 0 dB, resolution 1 dB. */
        const unsigned v=s[1]==0x82?0xe000:s[1]==0x83?0:0x100;
        if(value!=0x0200||index!=0x0200||length!=2)return 0;
        d->data_len=2;d->data[0]=(uint8_t)v;d->data[1]=(uint8_t)(v>>8);
    }else if(s[0]&0x80)return 0;
    return 1;
}
static void status_stage(struct dev *d)
{
    const uint8_t *s=d->setup;unsigned value=le16(s+2),index=le16(s+4);
    if(s[0]==0&&s[1]==9)d->configured=value;
    else if(s[0]==1&&s[1]==11){assert(index<4);d->alternate[index]=value;if(d->kind==AUDIO&&index==1)d->audio_alt=value;}
    else if(d->kind==HUB&&s[0]==0x23&&s[1]==3&&value==8)hub_port_power(d,index);
    else if(d->kind==HUB&&s[0]==0x23&&s[1]==3&&value==4){
        struct dev *c=d->child[index];
        if(c&&c->present&&(d->status[index]&1)){d->status[index]|=2|speed_bits(c);d->change[index]|=16;}
    }else if(d->kind==HUB&&s[0]==0x23&&s[1]==1&&value>=16&&value<=20)d->change[index]&=~(1u<<(value-16));
    else if(d->kind==AUDIO&&s[0]==0x22&&s[1]==1){
        assert(value==0x0100&&index==0x81&&d->data_len==3);
        d->audio_rate=d->data[0]|(unsigned)d->data[1]<<8|(unsigned)d->data[2]<<16;++d->audio_set_cur;
    }
}
/* One frame index per sample frame; the device keeps sampling whether or
 * not the host collects a packet. */
static unsigned audio_packet(struct dev *d,uint8_t *p,uint64_t service_us)
{
    unsigned channels=d->audio_alt==2?2:1,rate=d->audio_alt==2?48000:d->audio_rate,frames=rate/1000;
    uint64_t first=service_us/1000*frames;
    for(unsigned f=0;f<frames;++f)for(unsigned c=0;c<channels;++c){
        uint16_t v=(uint16_t)((first+f)^(c?0x5555u:0));p[(f*channels+c)*2]=(uint8_t)v;p[(f*channels+c)*2+1]=(uint8_t)(v>>8);
    }
    return frames*channels*2;
}
/* Move past the TD at the dequeue pointer and return its TRB address. */
static uint64_t advance(struct ep *e)
{
    uint64_t pointer=e->deq;
    e->deq+=16;
    const uint8_t *next=cpu(e->deq,16);
    if(((get32(next+12)>>10)&63)==6&&(get32(next+12)&1)==e->cycle){if(get32(next+12)&2)e->cycle^=1;e->deq=get64(next);}
    return pointer;
}
static void complete(unsigned slot,unsigned dci,struct ep *e,const uint8_t *t,unsigned code,unsigned residual)
{
    uint64_t pointer=advance(e);
    if(get32(t+12)&BIT(5)||code!=1)event(32,pointer,code,residual,dci,slot);
}
/* Process available TDs: control and bulk at once, interrupt when data
 * exists, isochronous one per service interval. Returns after the first TD
 * that waits for data. */
static void service(unsigned slot,unsigned dci,uint64_t now)
{
    struct ep *e=&eps[slot][dci];struct dev *d=slots[slot];
    if(!d||(e->state!=1&&e->state!=3))return;
    if(e->kick&&e->state==3)e->state=1;
    if(e->state!=1)return;
    for(unsigned guard=0;guard<32;++guard){
        const uint8_t *t=cpu(e->deq,16);uint32_t control=get32(t+12);
        if((control&1)!=e->cycle){
            if(e->type==5&&e->scheduled&&now>=e->next){
                /* Ring Overrun: the ring was empty at a service interval. */
                if(iso_late_overrun)e->late=1;else event(32,iso_overrun_no_trb?0:e->deq,iso_overrun_code,0,dci,slot);
                e->scheduled=0;
            }
            break;
        }
        unsigned type=(control>>10)&63,length=get32(t+8)&0x1ffff;uint8_t *buffer=NULL;
        if(type==6){if(control&2)e->cycle^=1;e->deq=get64(t);continue;}
        if(type!=2&&type!=4&&length)buffer=cpu(get64(t),length);
        if(!reachable(d)){
            /* Transaction error: isochronous rings continue, others halt. */
            complete(slot,dci,e,t,4,length);
            if(e->type==5)continue;
            e->state=2;uint8_t *c=output(slot)+64*dci;put32(c,(get32(c)&~7u)|2);put64(c+8,e->deq|e->cycle);break;
        }
        if(dci==1){
            if(type==2){memcpy(d->setup,t,8);if(!request(d))FAIL("stalled request %02x %02x",d->setup[0],d->setup[1]);complete(slot,dci,e,t,1,0);}
            else if(type==3&&(control&BIT(16))){unsigned k=d->data_len<length?d->data_len:length;memcpy(buffer,d->data,k);complete(slot,dci,e,t,k<length?13:1,length-k);}
            else if(type==3){memcpy(d->data,buffer,length);d->data_len=length;complete(slot,dci,e,t,1,0);}
            else if(type==4){status_stage(d);complete(slot,dci,e,t,1,0);}
            else FAIL("TRB type %u on EP0",type);
        }else if(e->type==2||e->type==6){
            if(e->type==2){assert(length==31);memcpy(d->cbw,buffer,31);d->msc_phase=get32(d->cbw+8)?1:2;complete(slot,dci,e,t,1,0);continue;}
            if(d->msc_phase==1){
                unsigned op=d->cbw[15];memset(buffer,0,length);
                if(op==0x25){const uint8_t capacity[]={0,0,0,127,0,0,2,0};memcpy(buffer,capacity,8);}
                else if(op==0x28)for(unsigned k=0;k<512;++k)buffer[k]=(uint8_t)(k*17+3);
                d->msc_phase=2;complete(slot,dci,e,t,1,0);
            }else{
                assert(length==13);memset(buffer,0,13);put32(buffer,0x53425355);put32(buffer+4,get32(d->cbw+4));
                d->msc_phase=0;complete(slot,dci,e,t,1,0);
            }
        }else if(e->type==7){
            if(d->kind==HUB){
                unsigned map=0;for(unsigned p=1;p<=d->ports;++p)if(d->change[p])map|=1u<<p;
                if(!map)break;
                buffer[0]=(uint8_t)map;complete(slot,dci,e,t,length>1?13:1,length-1);
            }else{
                if(d->hid_sent>=d->hid_reports)break;
                for(unsigned k=0;k<8;++k)buffer[k]=(uint8_t)(d->hid_sent*17+k+1);
                ++d->hid_sent;complete(slot,dci,e,t,length>8?13:1,length-8);
            }
        }else if(e->type==5){
            if(iso_stalled)break;
            if(!e->scheduled){e->scheduled=1;e->next=(now/1000+1)*1000;}
            if(now<e->next)break;
            uint64_t at=e->next;e->next+=e->esit;++iso_services;
            if(iso_silent){
                --iso_silent;++iso_tds_missed;uint64_t pointer=advance(e);
                if(!iso_silent&&iso_miss_kind==1)event(32,pointer,23,length,dci,slot);
                continue;
            }
            if(iso_missed_every&&iso_services%iso_missed_every==0){
                iso_frames_lost+=(d->audio_alt==2?48000:d->audio_rate)/1000;++iso_tds_missed;
                if(iso_miss_kind==0||iso_miss_kind==3)complete(slot,dci,e,t,23,length);
                else advance(e);
                if(iso_miss_kind==1||iso_miss_kind==3)iso_silent=2;
                if(iso_miss_kind==2)event(32,0,23,length,dci,slot);
                continue;
            }
            unsigned bytes=audio_packet(d,buffer,at);assert(bytes<=length);
            complete(slot,dci,e,t,bytes<length?13:1,length-bytes);
        }else FAIL("endpoint type %u",e->type);
    }
    e->kick=0;
}
static void tick(void)
{
    commands();
    for(unsigned s=1;s<4;++s)if(slot_used&BIT(s))for(unsigned i=1;i<32;++i)
        if(eps[s][i].type&&(eps[s][i].kick||eps[s][i].type==7||(eps[s][i].type==5&&eps[s][i].scheduled)))service(s,i,m.now);
}

/* ---- Registers ---- */
static int cn(void *v,uint64_t *out){(void)v;*out=m.now;return 1;}
static int live(void *v,uint64_t g,uint64_t d){(void)v;return a733_usb2_platform_live(&n.platform,g,d);}
static uint32_t portsc(void)
{
    uint32_t v=reg[0x440/4]&(BIT(9)|0xfe0000u);
    if(root&&root->present)v|=1;
    if(root_enabled&&root&&root->present)v|=2|((uint32_t)root->speed<<10);
    return v;
}
static int rr(void *v,uint32_t off,uint32_t *out){(void)v;assert(off<0x8000&&!(off&3));*out=off==0x440?portsc():reg[off/4];return 1;}
static int ww(void *v,uint32_t off,uint32_t value)
{
    (void)v;assert(off<0x8000&&!(off&3));
    if(off==0x440){
        assert(!(value&2));
        reg[off/4]=(reg[off/4]&~(value&0xfe0000u)&~BIT(9))|(value&BIT(9));
        if(value&BIT(4)){if(root&&root->present){root_enabled=1;reg[off/4]|=BIT(21);}}
        return 1;
    }
    if(off==0x2000){command_kick=1;return 1;}
    if(off>0x2000&&off<0x2010){
        unsigned slot=(off-0x2000)/4;assert(slot_used&BIT(slot)&&value&&value<32);struct ep *e=&eps[slot][value];
        if(e->late){e->late=0;event(32,e->deq,15,0,value,slot);}else e->kick=1;
        return 1;
    }
    reg[off/4]=value;
    if(off==0x40){if(value&2){reg[0x40/4]=0;reg[0x44/4]=1;}else if(value&1)reg[0x44/4]=0;}
    if(off==0x5c){command_deq=get64(&reg[0x58/4])&~UINT64_C(63);command_cycle=reg[0x58/4]&1;}
    return 1;
}
static int w64(void *v,uint32_t off,uint64_t value){return ww(v,off,(uint32_t)value)&&ww(v,off+4,(uint32_t)(value>>32));}
static int barrier(void *v){(void)v;return 1;}
static int out(void *v,const void *p,size_t z){(void)v;return pmic_model_outside(&m,p,z);}
static unsigned removals,remove_result;
static void notice(void *v,const struct a733_xhci_notice *e){if(e->kind==A733_XHCI_REMOVE){++removals;remove_result=e->result;}a733_usb2_host_notice(v,e);}
static void start_core(void)
{
    struct device_memory_extent e;
    assert(device_memory_pool_init(&n.private_pool,(uintptr_t)dma,PHYS,sizeof(dma)));
    assert(device_memory_acquire(&n.private_pool,99,sizeof(dma),&n.memory,&e));
    const struct a733_xhci_ops ops={cn,current,live,rr,ww,w64,barrier,out,notice};
    reg[0]=0x01100040;reg[1]=(1u<<24)|(1u<<8)|3;reg[4]=(0x20u<<16)|1|4|8;reg[5]=0x2000;reg[6]=0x1000;
    reg[0x80/4]=0x02000002;reg[0x84/4]=0x20425355;reg[0x88/4]=0x101;reg[0x44/4]=1;reg[0x48/4]=1;
    n.core_started=1;assert(a733_xhci_init(&n.core,&ops,&n,&n.private_pool,n.memory,71,4000000,30000000));
    assert(a733_usb2_host_bind(&n));n.stack_started=1;
}
static int poll(void){m.now+=100;observe_role();tick();return a733_usb2_host_poll(&n);}
static enum device_memory_state parent_state(void)
{enum device_memory_state s;struct device_memory_extent e;assert(device_memory_describe(&n.private_pool,n.memory,&e,&s));return s;}
static void until_mounted(void)
{
    for(unsigned i=0;i<60000&&!n.mounted&&!n.failed;++i)poll();
    if(!n.mounted||n.failed)FAIL("not mounted: failed %u kind %u now %llu",n.failed,n.kind,(unsigned long long)m.now);
}
static void retained(void)
{
    for(unsigned i=0;i<60000&&!n.failed;++i)poll();
    if(!n.failed||!n.core.failed||parent_state()!=DEVICE_MEMORY_RETAINED)FAIL("session not retained");
}
static void attach_root(void){root->present=1;reg[0x440/4]|=BIT(17);port_event();}
static void unplug_root(void){root->present=0;root_enabled=0;reg[0x440/4]|=BIT(17);port_event();}
static void read_sector(void)
{
    uint8_t sector[512];memset(sector,0x5a,sizeof(sector));
    assert(a733_usb2_read_sector(&n,7,m.now+2000000));
    for(unsigned i=0;i<20000&&!n.read_complete&&!n.failed;++i)poll();
    if(n.failed||a733_usb2_read_result(&n,sector)!=1)FAIL("sector read failed");
    for(unsigned k=0;k<512;++k)assert(sector[k]==(uint8_t)(k*17+3));
}
static void hid_reports(struct dev *d,unsigned speed)
{
    for(unsigned k=0;k<4;++k){
        struct a733_usb2_hid_report r;++d->hid_reports;assert(a733_usb2_hid_receive(&n,m.now+2000000));
        for(unsigned i=0;i<20000&&!n.hid.complete&&!n.failed;++i)poll();
        if(n.failed||a733_usb2_hid_result(&n,&r)!=1)FAIL("report %u failed",k);
        assert(r.bytes==8&&r.speed==speed&&r.data[0]==(uint8_t)(k*17+1));
    }
}
/* The route string and TT fields the core published for each slot. */
static void slot_fields(const struct dev *d,unsigned route,unsigned tt_slot,unsigned tt_port)
{
    assert(d->slot);const uint8_t *o=output(d->slot);
    if((get32(o)&0xfffff)!=route||(get32(o+8)&255)!=tt_slot||((get32(o+8)>>8)&255)!=tt_port)
        FAIL("slot %u route %x TT %u/%u",d->slot,get32(o)&0xfffff,get32(o+8)&255,(get32(o+8)>>8)&255);
}
static void unplug_child(struct dev *h,unsigned port)
{
    struct dev *c=h->child[port];c->present=0;
    h->status[port]&=~(1u|2u|512u|1024u);h->change[port]|=1;
}
static void replug_child(struct dev *h,unsigned port)
{
    struct dev *c=h->child[port];c->present=1;c->configured=0;
    if(h->status[port]&256){h->status[port]|=1;h->change[port]|=1;}
}

/* ---- Audio capture ---- */
static uint64_t frames_read,next_frame;static unsigned gaps,gap_frames,blocks;
static int audio_collect(unsigned channels,unsigned ms)
{
    struct a733_usb2_audio_block b;uint64_t end=m.now+ms*1000ull;
    while(m.now<end){
        if(poll()<0)return 0;
        int r=a733_usb2_audio_read(&n,&b);
        if(r<0)return 0;
        if(!r)continue;
        assert(b.channels==channels&&b.sample_bytes==2&&b.frames&&b.frames*channels*2<=sizeof(b.data)&&b.sequence==++blocks);
        for(unsigned f=0;f<b.frames;++f){
            uint16_t v=(uint16_t)(b.data[f*channels*2]|b.data[f*channels*2+1]<<8);
            if(channels==2){uint16_t w=(uint16_t)(b.data[f*4+2]|b.data[f*4+3]<<8);assert(w==(uint16_t)(v^0x5555));}
            if(frames_read&&v!=(uint16_t)next_frame){++gaps;gap_frames+=(uint16_t)(v-(uint16_t)next_frame);}
            next_frame=(uint16_t)(v+1);++frames_read;
        }
    }
    return 1;
}
static void audio_contexts(const struct dev *d,unsigned packet,unsigned interval)
{
    const uint8_t *c=output(d->slot)+64*3; /* EP 0x81 is DCI 3. */
    unsigned type=(get32(c+4)>>3)&7,cerr=(get32(c+4)>>1)&3;
    if(type!=5||cerr||(get32(c+4)>>16)!=packet||((get32(c)>>16)&255)!=interval||(get32(c+16)>>16)!=packet)
        FAIL("isochronous context type %u cerr %u packet %u interval %u",type,cerr,get32(c+4)>>16,(get32(c)>>16)&255);
}

int main(int argc,char **argv)
{
    mode=argc>1?argv[1]:"nested-msc";setup();
    n.hub_tiers=(has("nested")&&!has("refused"))||has("third-tier")?2:1;
    n.bus_powered_hubs=has("bus-")&&!has("bus-refused");
    n.audio_capture=has("audio")&&!has("audio-disabled");
    if(has("nested")||has("third-tier")){
        /* Root-port hub, a hub on its port 2 and the leaf on port 3 of that hub. */
        root=make(HUB,3,NULL,0);root->self_reported=1;
        struct dev *tier=make(HUB,3,root,2);tier->self_reported=1;tier->protocol=has("mtt")?2:1;
        if(has("third-tier")){struct dev *h=make(HUB,3,tier,3);h->self_reported=1;}
        else if(has("hid-ls"))make(HID,2,tier,3);
        else make(MSC,3,tier,3);
        finish_devices();start_core();attach_root();
        if(has("third-tier")||has("nested-refused")){
            retained();
            /* The refused hub never receives SET_CONFIGURATION. */
            if(has("refused")?tier->configured:!tier->configured||tier->child[3]->configured)FAIL("refused at the wrong stage");
            if(!n.core.devices[1].slot||!!n.core.devices[1].hub_configured==!!has("refused")||!!n.core.devices[2].slot!=has("third-tier"))
                FAIL("refusal after the wrong slot");
            puts("PASS a hub below the admitted tiers is refused before its child receives a slot; the session is retained");return 0;
        }
        until_mounted();
        struct dev *leaf=tier->child[3];
        slot_fields(tier,2,0,0);slot_fields(leaf,0x32,leaf->speed<3?tier->slot:0,leaf->speed<3?3:0);
        if(has("hid-ls")){
            assert(get32(output(leaf->slot))&BIT(25)?tier->protocol==2:tier->protocol==1);
            hid_reports(leaf,2);puts("PASS low-speed keyboard below two hub tiers uses the second hub's TT and returns four reports");return 0;
        }
        read_sector();
        if(has("leaf-unplug")){
            unsigned leaf_slot=leaf->slot;unplug_child(tier,3);
            for(unsigned i=0;i<30000&&(n.mounted||n.core.devices[2].port_ready);++i)assert(poll()>=0);
            if(disable_count!=1||disables[0]!=leaf_slot||!n.core.devices[1].hub_configured)FAIL("leaf removal disabled %u slots",disable_count);
            replug_child(tier,3);until_mounted();read_sector();
            puts("PASS a leaf removed from the second hub tier disables only its slot; its replacement reads");return 0;
        }
        if(has("unplug")){
            unsigned hub_slot=tier->slot,leaf_slot=leaf->slot;
            unplug_child(root,2);
            for(unsigned i=0;i<30000&&(n.core.devices[1].port_ready||n.core.devices[2].port_ready);++i)assert(poll()>=0);
            if(disable_count!=2||disables[0]!=leaf_slot||disables[1]!=hub_slot)FAIL("disable order %u: %u %u",disable_count,disables[0],disables[1]);
            assert(!n.mounted&&n.core.devices[0].hub_configured&&parent_state()==DEVICE_MEMORY_DEVICE&&!removals&&!n.hubs[1].present);
            replug_child(root,2);tier->status[3]=0;tier->change[3]=0;
            for(unsigned p=1;p<8;++p){tier->status[p]=0;tier->change[p]=0;}
            until_mounted();read_sector();
            puts("PASS removing the second hub tier disables the leaf slot before the hub slot; both re-enumerate and read");return 0;
        }
        puts("PASS high-speed storage below two hub tiers: route strings, actual hub classes and a copied sector");return 0;
    }
    if(has("bus-")){
        /* Bus-powered root-port hub; USB 2.0 section 7.2.1 budgets. */
        root=make(HUB,3,NULL,0);root->bus_powered=1;root->self_reported=0;root->max_power=100;
        if(has("budget"))root->ports=7;
        if(has("status-mismatch"))root->self_reported=1;
        struct dev *leaf;
        if(has("nested")){/* not reached: nested cases above */}
        if(has("bus-self-tier")){
            n.hub_tiers=2;struct dev *h=make(HUB,3,root,1);h->self_reported=1;h->max_power=has("heavy")?200:100;
            leaf=make(MSC,3,h,2);leaf->max_power=500;
        }else if(has("bus-bus-tier")){
            n.hub_tiers=2;struct dev *h=make(HUB,3,root,1);h->bus_powered=1;h->max_power=50;leaf=make(HID,1,h,2);
        }else{leaf=make(HID,1,root,1);leaf->max_power=has("leaf-power")?200:100;}
        finish_devices();start_core();attach_root();
        if(has("refused")||has("budget")||has("leaf-power")||has("status-mismatch")||has("bus-bus-tier")||has("heavy")){
            retained();
            /* Where each rule applies: hub configuration descriptor, device
             * status, hub descriptor (no port powered) or leaf configuration. */
            struct dev *h=root->child[1]&&root->child[1]->kind==HUB?root->child[1]:root;
            unsigned at_config=has("refused")||has("heavy"),at_status=has("status-mismatch"),at_ports=has("budget")||has("bus-bus-tier");
            unsigned powered=0;for(unsigned p=1;p<8;++p)powered|=h->status[p]&256;
            if((at_config&&h->configured)||((at_status||at_ports)&&(!h->configured||powered))||
               (has("leaf-power")&&(!powered||!leaf->slot||leaf->configured))||(h!=root&&!root->configured))FAIL("refused at the wrong stage");
            printf("PASS %s is refused before the leaf receives a configuration; the session is retained\n",mode);return 0;
        }
        until_mounted();
        if(has("bus-self-tier")){read_sector();puts("PASS a self-powered hub below a bus-powered hub supplies a 500 mA storage leaf");return 0;}
        hid_reports(leaf,1);puts("PASS bus-powered hub within its 500 mA budget serves a 100 mA full-speed keyboard");return 0;
    }
    if(has("audio")){
        struct dev *mic;
        if(has("hub")){root=make(HUB,3,NULL,0);root->self_reported=1;mic=make(AUDIO,1,root,4);}
        else root=mic=make(AUDIO,has("audio-hs")?3:1,NULL,0);
        mic->audio_hs=has("audio-hs");mic->audio_big=has("large");mic->audio_hid=has("hid-interface");
        iso_missed_every=has("missed")?10:0;
        iso_miss_kind=has("batch")?1:has("null")?2:has("silent")?3:0;iso_late_overrun=has("refill");
        iso_overrun_no_trb=has("no-trb");if(has("ring-full"))iso_overrun_code=21;
        finish_devices();start_core();attach_root();
        if(has("disabled")||has("large")||has("hid-interface")){
            retained();if(mic->configured||!mic->slot)FAIL("refused at the wrong stage");
            printf("PASS %s refuses the audio configuration before SET_CONFIGURATION\n",mode);return 0;
        }
        until_mounted();
        assert(a733_usb2_device_kind(&n)==A733_USB2_DEVICE_AUDIO&&mic->configured==1);
        assert(!a733_usb2_audio_start(&n,44100,1,m.now+1000000)&&!n.failed);
        unsigned stereo=has("stereo"),rate=has("16k")?16000:48000,channels=stereo?2:1;
        if(!a733_usb2_audio_start(&n,rate,channels,m.now+1000000))FAIL("start refused");
        for(unsigned i=0;i<20000&&!n.audio.started&&!n.failed;++i)poll();
        if(!n.audio.started||n.failed)FAIL("start incomplete");
        assert(mic->audio_alt==(stereo?2u:1u)&&mic->audio_set_cur==1&&(stereo||mic->audio_rate==rate));
        audio_contexts(mic,stereo?192:96,has("audio-hs")?3:3);
        if(has("stall")){
            iso_stalled=1;struct a733_usb2_audio_block b;uint64_t at=m.now;
            for(unsigned i=0;i<20000&&!n.failed;++i){poll();(void)a733_usb2_audio_read(&n,&b);}
            if(!n.failed||m.now-at<98000||m.now-at>110000||parent_state()!=DEVICE_MEMORY_RETAINED)
                FAIL("stall retained after %llu us",(unsigned long long)(m.now-at));
            memset(&b,0x5a,sizeof(b));assert(a733_usb2_audio_read(&n,&b)<0&&b.data[0]==0x5a);
            puts("PASS an isochronous ring without events is retained 100 ms after its last activity, before any further copy");return 0;
        }
        if(!audio_collect(channels,300))FAIL("capture failed");
        struct a733_usb2_audio_block last;
        if(has("overrun")||has("late-poll")){
            /* The owner stops for 30 ms, or for 150 ms, longer than the
             * isochronous bound: eight TDs complete, then the ring is empty.
             * Their events wait in the event ring until the owner polls. */
            for(unsigned i=0;i<(has("late-poll")?1500u:300u);++i){m.now+=100;tick();}
            if(has("ring-full")){
                struct a733_usb2_audio_block b;
                for(unsigned i=0;i<20000&&!n.failed;++i){poll();(void)a733_usb2_audio_read(&n,&b);}
                if(!n.failed||n.core.isoch.overruns||parent_state()!=DEVICE_MEMORY_RETAINED)FAIL("code 21 not retained");
                puts("PASS an Event Ring Full code on the isochronous endpoint is not taken for a Ring Overrun; the session is retained");return 0;
            }
            if(!audio_collect(channels,200))FAIL("capture after overrun failed");
            if(n.failed||n.core.isoch.overruns!=1||!gaps||n.core.isoch.dropped)
                FAIL("failed %u overruns %u gaps %u dropped %u",n.failed,n.core.isoch.overruns,gaps,n.core.isoch.dropped);
            if(has("late-poll"))
                printf("PASS a 150 ms owner gap consumes the waiting ring events first; one overrun and a %u-frame gap, the stream continues\n",gap_frames);
            else if(has("no-trb"))
                printf("PASS an overrun without a TRB pointer after every queued TD completed; the stream restarts with a %u-frame gap\n",gap_frames);
            else if(has("refill"))
                printf("PASS an overrun posted after the refill names the first new TD; the core rings again and the stream continues after a %u-frame gap\n",gap_frames);
            else printf("PASS an empty isochronous ring reports one overrun; the stream restarts at the next TD with a %u-frame gap\n",gap_frames);
            return 0;
        }
        if(has("missed")&&iso_miss_kind){
            /* Missed TDs at the end may not be retired yet: up to three TDs
             * wait for a later event. Every gap the reader sees was counted. */
            unsigned per=rate/1000,dropped=n.core.isoch.dropped;
            if(n.failed||!gaps||n.core.isoch.overruns||gap_frames%per||gap_frames/per>dropped||dropped>iso_tds_missed||iso_tds_missed-dropped>3)
                FAIL("dropped %u missed %u gaps %u gap frames %u",dropped,iso_tds_missed,gaps,gap_frames);
            printf("PASS %u of %u missed service intervals (%s) retired without data in %u gaps; the stream continues\n",dropped,iso_tds_missed,
                iso_miss_kind==1?"one event for three TDs":iso_miss_kind==2?"events without a TRB pointer":"two TDs skipped without events",gaps);
            return 0;
        }
        if(has("missed")){
            /* The last missed interval may follow the last read. */
            unsigned per=rate/1000;
            if(!n.core.isoch.dropped||n.core.isoch.overruns||gaps+1<n.core.isoch.dropped||gaps>n.core.isoch.dropped||gap_frames!=gaps*per)
                FAIL("dropped %u gaps %u gap frames %u lost %u",n.core.isoch.dropped,gaps,gap_frames,iso_frames_lost);
            printf("PASS %u missed service intervals are counted and appear as %u-frame gaps; the stream continues\n",n.core.isoch.dropped,gap_frames);return 0;
        }
        if(gaps||n.core.isoch.dropped||n.core.isoch.overruns||frames_read<(rate/1000)*250)FAIL("gaps %u dropped %u overruns %u frames %llu",gaps,n.core.isoch.dropped,n.core.isoch.overruns,(unsigned long long)frames_read);
        if(has("unplug")){
            unplug_root();
            for(unsigned i=0;i<30000&&(n.core.announced||n.core.detaching);++i)assert(poll()>=0);
            assert(removals==1&&!remove_result&&!n.failed&&disable_count==1&&parent_state()==DEVICE_MEMORY_DEVICE);
            assert(a733_usb2_audio_read(&n,&last)<0&&a733_usb2_device_kind(&n)==A733_USB2_DEVICE_NONE);
            mic->audio_alt=0;mic->configured=0;mic->audio_set_cur=0;frames_read=0;gaps=0;blocks=0;
            attach_root();until_mounted();
            assert(a733_usb2_audio_start(&n,rate,channels,m.now+1000000));
            for(unsigned i=0;i<20000&&!n.audio.started&&!n.failed;++i)poll();
            if(!audio_collect(channels,100)||gaps)FAIL("capture after replug failed");
            puts("PASS a root disconnect stops the isochronous ring and disables the slot; the replugged microphone streams again");return 0;
        }
        printf("PASS UAC1 %s capture at %u Hz, %u channel(s): %llu contiguous frames, no dropped or overrun intervals\n",
            has("audio-hs")?"high-speed":has("hub")?"full-speed (behind a hub TT)":"full-speed",rate,channels,(unsigned long long)frames_read);
        return 0;
    }
    FAIL("unknown case %s",mode);
}
