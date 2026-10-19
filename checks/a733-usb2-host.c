/* SPDX-License-Identifier: Apache-2.0 */
#include "a733_usb2_native.h"
#include "../drivers/usb/a733_usb3_phy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int usb_gpu_init(struct a733_gpu_resource_manager *,const struct a733_gpu_resource_ops *,void *);
#define PMIC_MODEL_NO_MAIN
#define a733_gpu_resources_init usb_gpu_init
#include "a733-pmic.c"
#undef a733_gpu_resources_init
static struct pmic_model m;
static struct a733_usb2_native n;
static struct a733_typec role;
static struct a733_twi bus;
static _Alignas(4096) uint8_t dma[A733_XHCI_MEMORY_BYTES];
static uint32_t registers[0x8000/4];
static unsigned reg_fault,lose_on_lock,platform_writes,commands,transfers,events,event_cycle=1;
static unsigned bad_status,hold_bulk,short_data,bad_config,short_descriptor;
static unsigned ss_mode,ss_bad,ss_reverse,ss_burst=15,ss_streams,ss_source=900,ss_conflict;
static uint16_t ss_analog[65536];static unsigned ss_halfwords;
static uint64_t last_command,last_transfer[A733_XHCI_DEVICES][3];
static unsigned hid_mode,device_speed=3,device_packet=64,hid_packet=8,hid_interval=10;
static unsigned hid_reports,report_bytes=8,bad_hid_config,failed_hid_event,wrong_hid_pointer,evaluates;
static const uint8_t hid_descriptor[]={5,1,9,6,0xa1,1,5,7,0x19,0xe0,0x29,0xe7,0x15,0,0x25,1,
 0x75,1,0x95,8,0x81,2,0x95,1,0x75,8,0x81,1,0x95,6,0x75,8,0x15,0,0x25,0x65,
 5,7,0x19,0,0x29,0x65,0x81,0,0xc0};
static unsigned irq_model,irq_begins,irq_releases,irq_advances,irq_race;
static uint64_t irq_last_erdp;
static unsigned hub_mode,hub_protocol=1,hub_ports=3,hub_port=2,hub_bitmap,hub_reset,hub_changes=1,hub_bad,wire_device,hub_after_fault,hub_fault_case,hub_changed_port,wrong_leaf_slot;
static unsigned dual_mode,dual_reverse,dual_duplicate,dual_changes[8],dual_reset[8];
static int is_hid(unsigned j){return dual_mode?j&&(dual_duplicate||j==(dual_reverse?2u:1u)):hid_mode;}
static unsigned speed_for(unsigned j){return dual_mode?(is_hid(j)?device_speed:3):hub_mode&&!j?3:device_speed;}
static uint8_t request[8],cbw[31];
static int gpu_read(void *v,uintptr_t a,uint32_t *out){*out=*pmic_model_word(v,a);return 1;}
static int gpu_update(void *v,uintptr_t a,uint32_t required,uint32_t set,uint64_t d)
{struct pmic_model *p=v;assert(a==0x070101ac&&set==1&&p->now<d);uint32_t *w=pmic_model_word(p,a);assert((*w&required)==required);*w|=set;return 1;}
static int usb_gpu_init(struct a733_gpu_resource_manager *g,const struct a733_gpu_resource_ops *unused,void *v)
{(void)unused;const struct a733_gpu_resource_ops o={pmic_model_now,gpu_read,gpu_update,pmic_model_gpu_rail};return a733_gpu_resources_init(g,&o,v);}
static int current(void *v){(void)v;return !m.worker;}
static int admit(void *v,const struct a733_usb2_platform_config *c){assert(v==&m&&c->identity==88&&c->topology==12);return 1;}
static int lock(void *v,uint64_t d)
{struct pmic_model *p=v;if(p->register_held||reg_fault||p->now>=d)return 0;p->register_held=1;if(lose_on_lock){lose_on_lock=0;a733_typec_revoke(&role);}return 1;}
static void unlock(void *v){struct pmic_model *p=v;assert(p->register_held&&!reg_fault);p->register_held=0;}
static void retain(void *v){assert(((struct pmic_model *)v)->register_held);reg_fault=1;}
static int read_reg(void *v,uintptr_t a,uint32_t *out)
{assert(m.register_held&&!reg_fault&&(a733_usb2_platform_address(a,0)||(ss_mode&&(a733_usb3_mask32(a)||a==0x06c01900))));*out=*pmic_model_word(v,a);return 1;}
static int write_reg(void *v,uintptr_t a,uint32_t value)
{assert(m.register_held&&!reg_fault&&(a733_usb2_platform_address(a,1)||(ss_mode&&a733_usb3_mask32(a))));uint32_t *p=pmic_model_word(v,a);assert(!((*p^value)&~(ss_mode&&a733_usb3_mask32(a)?a733_usb3_mask32(a):a733_usb2_platform_mask(a))));*p=value;++platform_writes;return 1;}
static int read_half(void *v,uintptr_t a,uint16_t *out)
{(void)v;assert(m.register_held&&ss_mode&&a733_usb3_address16(a));*out=ss_analog[(a-0x06c80000)/2];return 1;}
static int write_half(void *v,uintptr_t a,uint16_t value)
{(void)v;assert(m.register_held&&ss_mode&&a733_usb3_address16(a));ss_analog[(a-0x06c80000)/2]=value;++ss_halfwords;return 1;}
static int unused_prepare(void *v,uint64_t d){(void)v;(void)d;assert(0);return -1;}
static int unused_vbus(void *v,unsigned on,uint64_t d){(void)v;(void)on;(void)d;assert(0);return -1;}
static int unused_irq(void *v,unsigned *out,uint64_t d){(void)v;(void)out;(void)d;assert(0);return -1;}
static void observe_role(void)
{
    /* Prepared-observation checkpoint: Type-C engine/physical power are not
     * requalified here. Exercise its actual public observation and revocation. */
    if(!atomic_load(&role.revoked))role.published=(struct a733_typec_token){(uintptr_t)&role,70,71,m.now,ss_reverse?2:1};
}
static void setup(void)
{
    pmic_model_setup(&m);
    m.pmic[0x11]=0x11;if(ss_mode){m.pmic[0x10]|=2;m.pmic[0x13]=30;}m.pmic[0x1a]=30;m.pmic[0x23]=8;m.pmic[0x3f]=12;m.pmic[0x33]=13;
    *pmic_model_word(&m,0x02002000)=0xf8636300;
    *pmic_model_word(&m,0x020020a0)=0xfe106310;
    *pmic_model_word(&m,0x02003908)=0x0b860b86;*pmic_model_word(&m,0x02003988)=0x0b860000;
    *pmic_model_word(&m,0x02002580)=0x83000002;*pmic_model_word(&m,0x02002588)=0x84000001;
    *pmic_model_word(&m,0x020025c0)=1u<<8;*pmic_model_word(&m,0x020025e0)=1u<<28;
    *pmic_model_word(&m,0x020033c0)=0x81000005;*pmic_model_word(&m,0x020033c4)=1u<<16;
    *pmic_model_word(&m,0x02003b28)=0x20000;
    *pmic_model_word(&m,0x07067000)=*pmic_model_word(&m,0x07067008)=8;
    *pmic_model_word(&m,0x07068000)=*pmic_model_word(&m,0x07068008)=8;
    *pmic_model_word(&m,0x06b00010)=0x22418;
    *pmic_model_word(&m,0x06a0c120)=0x3331190a;*pmic_model_word(&m,0x06a0c1a0)=0x3139302a;
    *pmic_model_word(&m,0x06a0c140)=2;*pmic_model_word(&m,0x06a0c144)=1u<<24;
    *pmic_model_word(&m,0x06a0c14c)=ss_mode?5:4;
    if(ss_mode)*pmic_model_word(&m,0x06c01900)=ss_bad==5?0:1;
    const struct a733_typec_config tc={.ops={pmic_model_outside,pmic_model_now,current,unused_prepare,current,unused_vbus,unused_irq},
        .context=&m,.bus=&bus,.owner=70,.generation=71,.qualification=72,.recovery=73,
        .source_min_mv=5000,.source_max_mv=5100,.source_ma=ss_mode&&ss_bad!=6?ss_source:500,.zero_threshold_max_mv=800,.device_id=0x80};
    assert(a733_typec_init(&role,&tc));observe_role();
    struct a733_usb2_platform_config c={.owner=99,.identity=88,.topology=12,.deadline=2000000,.lifetime_deadline=10000000,
        .superspeed=ss_mode,.pck_client=2,.fabric={0,0x83000002,0x84000001},.fabric_hz={24000000,200000000,200000000},
        .supply_uv={3300000,3300000,800000,800000,1800000},.revision=0x3139302a};
    const struct a733_usb2_platform_ops o={pmic_model_now,current,admit,lock,unlock,retain,read_reg,write_reg,pmic_model_outside,read_half,write_half};
    n.self=&n;n.initialized=1;
    int initialized=a733_usb2_platform_init(&n.platform,&c,&o,&m,&m.clocks,&m.rails,&m.provider,&m.gpu,&role);
    assert(initialized==!(ss_mode&&ss_bad==6));
}
extern void usb2_native_mapping_check(void);
extern void usb2_source_reference(uint32_t *);
static void prepare(void)
{
    for(unsigned i=0;i<10000;++i){observe_role();enum a733_usb2_result r=a733_usb2_platform_poll(&n.platform);
        if(r==A733_USB2_READY)return;
        if(r==A733_USB2_RETAINED)fprintf(stderr,"platform phase%u now%llu writes%u\n",n.platform.phase,(unsigned long long)m.now,platform_writes);
        assert(r!=A733_USB2_RETAINED);assert(pmic_model_step(&m)!=A733_PMIC_RETAINED);m.now+=20;}
    assert(0);
}
static uint32_t get32(const void *v){const uint8_t *p=v;return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(void *v,uint32_t x){uint8_t *p=v;for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(x>>(8*i));}
static void signal_irq(void)
{
    if(irq_model&&(registers[0x40/4]&4)&&(registers[0x1020/4]&2)&&!(registers[0x1038/4]&8)&&
       (n.core.events[n.core.event_index].control&1)==n.core.event_cycle){
        registers[0x44/4]|=8;registers[0x1020/4]|=1;registers[0x1038/4]|=8;
    }
}
static void event(unsigned type,uint64_t pointer,unsigned code,unsigned remaining,unsigned endpoint,unsigned slot)
{
    struct a733_xhci_trb *t=&n.core.events[events];assert((t->control&1)!=event_cycle);
    *t=(struct a733_xhci_trb){(uint32_t)pointer,(uint32_t)(pointer>>32),(code<<24)|remaining,
        (slot<<24)|(endpoint<<16)|(type<<10)|event_cycle};
    if(++events==A733_XHCI_EVENT_TRBS){events=0;event_cycle^=1;}
    signal_irq();
}
static void control_data(uint8_t *p,unsigned count)
{
    memset(p,0,count);
    if(ss_mode){
        const uint8_t device[]={18,1,0,3,0,0,0,9,0x34,0x12,0x78,0x56,0,1,0,0,0,1};
        uint8_t config[]={9,2,44,0,1,1,0,0x80,112,9,4,0,0,2,8,6,0x50,0,
            7,5,0x81,2,0,4,0,6,0x30,(uint8_t)ss_burst,(uint8_t)ss_streams,0,0,
            7,5,2,2,0,4,0,6,0x30,(uint8_t)ss_burst,(uint8_t)ss_streams,0,0};
        uint8_t bos[]={5,15,22,0,2,7,16,2,2,0,0,0,10,16,3,0,14,0,1,10,255,7};
        const uint8_t string[]={4,3,9,4};
        if(ss_bad==7)config[27]=16;if(ss_bad==8)config[28]=17;if(ss_bad==9)config[29]=1;
        if(ss_bad==1)bos[12]=9;if(ss_bad==2)config[26]=0x31;if(ss_bad==3)config[8]=ss_source==1500?150:113;
        if(request[1]==6){const uint8_t *src=request[3]==1?device:request[3]==2?config:request[3]==15?bos:string;
            unsigned size=request[3]==1?18:request[3]==2?44:request[3]==15?22:4;assert(count<=size);memcpy(p,src,count);}
        else assert(request[1]==0xfe&&count==1);
        return;
    }
    if(hub_mode&&!wire_device){
        unsigned total=hub_protocol==2?41:25;
        uint8_t device[]={18,1,0,2,9,0,(uint8_t)hub_protocol,64,0x34,0x12,0x78,0x56,0,1,0,0,0,1};
        uint8_t config[41]={9,2,(uint8_t)total,0,1,1,0,0xc0,50,9,4,0,0,1,9,0,1,0,7,5,0x81,3,1,0,12,
          9,4,0,1,1,9,0,2,0,7,5,0x81,3,1,0,12};
        const uint8_t string[]={4,3,9,4};
        if(hub_bad==1)config[7]=0x80;
        if(request[1]==6&&request[0]==0x80){
            const uint8_t *src=request[3]==1?device:request[3]==2?config:string;
            unsigned length=request[3]==1?sizeof(device):request[3]==2?total:sizeof(string);
            assert(count<=length);memcpy(p,src,count);
        }else if(request[1]==6){
            assert(request[0]==0xa0&&request[2]==0&&request[3]==0x29&&count==9);
            const uint8_t desc[]={9,0x29,(uint8_t)(hub_bad==4?8:hub_ports),0x49,0,10,0,0,255};memcpy(p,desc,9);
        }else if(request[0]==0x80){assert(request[1]==0&&count==2);p[0]=hub_bad==2?0:1;}
        else if(request[0]==0xa0){assert(request[1]==0&&count==4);p[0]=hub_bad==3?1:hub_after_fault==3?2:0;}
        else{
            assert(request[0]==0xa3&&request[1]==0&&count==4);
            unsigned port=request[4];
            if(dual_mode){
                assert(port==2||port==3);unsigned speed=speed_for(port-1);
                unsigned status=1|256|(dual_reset[port]?2:0)|(speed==3?1024:speed==2?512:0);
                p[0]=(uint8_t)status;p[1]=(uint8_t)(status>>8);p[2]=(uint8_t)dual_changes[port];
            }else{
                assert(port==(hub_changed_port?hub_changed_port:hub_port));
                unsigned status=1|256|(hub_reset?2:0)|(device_speed==3?1024:device_speed==2?512:0);
                if(hub_bad==5)status|=0x8000;if(hub_after_fault==1)status&=~1u;
                p[0]=(uint8_t)status;p[1]=(uint8_t)(status>>8);p[2]=(uint8_t)hub_changes;
            }
        }
        return;
    }
    if(is_hid(wire_device)){
        uint8_t device[]={18,1,0,2,0,0,0,(uint8_t)device_packet,0x34,0x12,0x78,0x56,0,1,0,0,0,1};
        uint8_t config[]={9,2,34,0,1,1,0,0x80,50,9,4,0,0,1,3,1,1,0,
          9,0x21,0x11,1,0,1,0x22,sizeof(hid_descriptor),0,7,5,0x81,3,(uint8_t)hid_packet,0,(uint8_t)hid_interval};
        if(bad_hid_config==1)config[33]=0;
        if(bad_hid_config==2)config[32]=8;
        if(bad_hid_config==3){config[25]=1;config[26]=1;}
        if(bad_hid_config==4)config[29]=1;
        if(request[3]==1){assert(count<=sizeof(device));memcpy(p,device,count);
            if(count==18)assert((get32(n.core.devices[wire_device].device+n.core.context_bytes+4)>>16)==device_packet&&!n.core.devices[wire_device].evaluate_pending);}
        else if(request[3]==2){assert(count<=sizeof(config));memcpy(p,config,count);}
        else if(request[3]==0x22){assert(request[0]==0x81&&count==sizeof(hid_descriptor));memcpy(p,hid_descriptor,count);}
        else{assert(request[3]==3&&count<=4);const uint8_t string[]={4,3,9,4};memcpy(p,string,count);}
        return;
    }
    static const uint8_t device[]={18,1,0,2,0,0,0,64,0x34,0x12,0x78,0x56,0,1,0,0,0,1};
    static const uint8_t config[]={9,2,32,0,1,1,0,0x80,50,9,4,0,0,2,8,6,0x50,0,7,5,0x81,2,0,2,0,7,5,2,2,0,2,0};
    if(request[1]==6){static const uint8_t string[]={4,3,9,4};const uint8_t *src=request[3]==1?device:request[3]==2?config:string;unsigned z=request[3]==1?sizeof(device):request[3]==2?sizeof(config):sizeof(string);assert(count<=z);memcpy(p,src,count);if(bad_config&&request[3]==2&&count==32){p[2]=255;p[3]=255;}if(hub_bad==8&&request[3]==1){p[4]=9;p[6]=1;}}
    else assert(request[1]==0xfe&&count==1); /* Actual GET_MAX_LUN response0. */
}
static void wire(void)
{
    struct a733_xhci *x=&n.core;
    if(x->command_kind&&x->command_pointer!=last_command){
        unsigned j=x->command_device-1;assert(j<A733_XHCI_DEVICES);
        struct a733_xhci_device *d=&x->devices[j];unsigned speed=ss_mode?4:speed_for(j);
        last_command=x->command_pointer;++commands;
        if(x->command_kind==11){
            assert((get32(d->input+x->context_bytes)&(15u<<20))==(speed<<20));
            if(hub_mode&&j){
                assert(x->devices[0].hub_configured);
                assert((get32(d->input+x->context_bytes)&0xfffffu)==d->parent_port);
                assert((get32(d->input+x->context_bytes+4)>>16&255)==1);
                assert(get32(d->input+x->context_bytes+8)==(speed==3?0:1u|(d->parent_port<<8)));
                assert(!!(get32(d->input+x->context_bytes)&(1u<<25))==(speed!=3&&hub_protocol==2));
            }
            memcpy(d->device,d->input+x->context_bytes,2*x->context_bytes);
            unsigned state=d->pending_address?2:1;put32(d->device+12,(state<<27)|(state==2?7+j:0));
        }else if(x->command_kind==13){
            assert(is_hid(j)&&speed==1&&get32(d->input+4)==2);
            assert((get32(d->input+2*x->context_bytes+4)>>16)==device_packet);
            memcpy(d->device+x->context_bytes,d->input+2*x->context_bytes,x->context_bytes);++evaluates;
        }else if(x->command_kind==12){
            assert((get32(d->input+x->context_bytes)&(15u<<20))==(speed<<20));
            if(hub_mode&&!j){
                assert(get32(d->input+x->context_bytes)&(1u<<26));
                assert((get32(d->input+x->context_bytes+4)>>24)==hub_ports);
                assert(((get32(d->input+x->context_bytes+8)>>16)&3)==2);
                assert(!!(get32(d->input+x->context_bytes)&(1u<<25))==(hub_protocol==2));
            }else if(is_hid(j)){
                struct a733_xhci_endpoint *e=&d->endpoints[1];const uint8_t *context=d->input+(e->number+1)*x->context_bytes;
                unsigned interval=speed==3?hid_interval-1:3;unsigned value=hid_interval;
                if(speed!=3)while(value>1){++interval;value>>=1;}
                assert(get32(context)==interval<<16);
                assert(get32(context+4)==((hid_packet<<16)|(7u<<3)|(3u<<1)));
                assert(get32(context+16)==((hid_packet<<16)|hid_packet));
            }
            if(ss_mode){
                for(unsigned k=1;k<3;++k){struct a733_xhci_endpoint *e=&d->endpoints[k];
                    const uint8_t *c=d->input+(e->number+1)*x->context_bytes;
                    assert(get32(c)==0); /* MaxPStreams and Mult disabled. */
                    assert(get32(c+4)==((1024u<<16)|(15u<<8)|((e->direction?6u:2u)<<3)|6));
                }
            }
            memcpy(d->device,d->input+x->context_bytes,x->context_bytes);put32(d->device+12,(3u<<27)|7+j);
            if(hub_bad==6&&!j)put32(d->device+8,get32(d->device+8)^(1u<<16));
        }
        event(33,x->command_pointer,1,0,0,j+1);return;
    }
    for(unsigned j=0;j<A733_XHCI_DEVICES;++j)for(unsigned i=0;i<3;++i){
        struct a733_xhci_device *d=&x->devices[j];struct a733_xhci_endpoint *e=&d->endpoints[i];
        if(e->pending!=2||!e->pending_trb||e->pending_trb==last_transfer[j][i])continue;
        if(hub_mode&&!j&&i){
            if(dual_mode)hub_bitmap=(dual_changes[2]?1u<<2:0)|(dual_changes[3]?1u<<3:0);
            if(!hub_bitmap)continue;
            assert(m.now>=n.hub.power_good_deadline&&d->hub_configured&&e->length==1);
            e->bounce[0]=(uint8_t)hub_bitmap;hub_bitmap=0;
        }else if(i&&hold_bulk)continue;
        last_transfer[j][i]=e->pending_trb;++transfers;unsigned residual=0;
        if(!i){
            if(e->kind==2){
                memcpy(request,d->setup,8);
                if(ss_mode&&request[1]==49)assert(request[0]==0&&request[2]==40&&request[3]==0&&!request[4]&&!request[5]&&!request[6]&&!request[7]);
                if(hub_mode&&!j&&request[0]==0x23){
                    unsigned value=request[2],port=request[4];
                    if(dual_mode){
                        assert(port==2||port==3||value==8);
                        if(request[1]==3&&value==4){dual_reset[port]=1;dual_changes[port]|=16;}
                        if(request[1]==1&&value>=16&&value<=20)dual_changes[port]&=~(1u<<(value-16));
                    }else{
                        if(request[1]==3&&value==4){assert(port==hub_port);hub_reset=1;hub_changes|=16;}
                        if(request[1]==1&&value>=16&&value<=20)hub_changes&=~(1u<<(value-16));
                    }
                }
            }else if(ss_mode&&e->kind==3&&!e->direction){
                const uint8_t expected[]={10,10,255,7,255,7};assert(request[1]==48&&e->length==6&&!memcmp(e->bounce,expected,6));
            }else if(e->kind==3&&e->direction){
                wire_device=j;control_data(e->bounce,e->length);
                if(short_descriptor&&request[1]==6&&request[3]==1)residual=1;
            }
        }else if(hub_mode&&!j){ /* already prepared owned hub status byte */ }
        else if(is_hid(j)){
            assert(e->direction&&e->address==0x81&&e->length==hid_packet);
            for(unsigned k=0;k<report_bytes;++k)e->bounce[k]=(uint8_t)(hid_reports*17+k+1);
            residual=e->length-report_bytes;++hid_reports;
            event(32,e->pending_trb+(wrong_hid_pointer?16:0),failed_hid_event?6:residual?13:1,residual,e->number,wrong_leaf_slot?1:d->slot);return;
        }else if(!e->direction){assert(e->length==31);memcpy(cbw,e->bounce,31);}
        else if(e->length==13){memset(e->bounce,0,13);put32(e->bounce,0x53425355);put32(e->bounce+4,get32(cbw+4)+(bad_status?1:0));}
        else if(cbw[15]==0x25){assert(e->length==8);const uint8_t capacity[]={0,0,0,127,0,0,2,0};memcpy(e->bounce,capacity,8);}
        else if(cbw[15]==0x28){assert(e->length==512);for(unsigned k=0;k<512;++k)e->bounce[k]=(uint8_t)(k*17+3);if(short_data)residual=1;}
        else{assert(cbw[15]==3||cbw[15]==0x12);memset(e->bounce,0,e->length);}
        event(32,e->pending_trb,ss_mode&&ss_bad==4&&request[1]==48&&!i?6:hub_mode&&!j&&i&&hub_after_fault==4?6:residual?13:1,residual,e->number,d->slot);return;
    }
}
static int cn(void *v,uint64_t *out){(void)v;*out=m.now;return 1;}
static int live(void *v,uint64_t g,uint64_t d){(void)v;return a733_usb2_platform_live(&n.platform,g,d);}
static int rr(void *v,uint32_t off,uint32_t *out){(void)v;assert(off<0x8000&&!(off&3));*out=registers[off/4];return 1;}
static int ww(void *v,uint32_t off,uint32_t value)
{
    (void)v;assert(off<0x8000&&!(off&3));
    if(irq_model&&off==0x44){assert(value==8);registers[off/4]&=~value;return 1;}
    if(irq_model&&off==0x1020){
        if(value&1)assert(!(registers[0x44/4]&8));
        registers[off/4]=(registers[off/4]&~2u)|(value&2);
        if(value&1)registers[off/4]&=~1u;
        return 1;
    }
    registers[off/4]=value;
    if(off==0x40){if(value&2){registers[off/4]=0;registers[0x44/4]=1;}else if(value&1)registers[0x44/4]=0;}
    if(off==0x440){registers[off/4]=1|(1u<<9);if(value&(1u<<4))registers[off/4]|=2|((hub_mode?3:device_speed)<<10);}
    return 1;
}
static int w64(void *v,uint32_t off,uint64_t value){
    assert(!(off&7));
    if(irq_model&&off==0x1038){
        if(n.core.irq_mode){
            assert((value&~UINT64_C(15))!=irq_last_erdp);++irq_advances;
            if(value&8){assert((n.core.events[n.core.event_index].control&1)!=n.core.event_cycle);++irq_releases;}
        }
        irq_last_erdp=value&~UINT64_C(15);
        unsigned busy=registers[off/4]&8;
        registers[off/4]=(uint32_t)(value&~UINT64_C(8))|((value&8)?0:busy);
        registers[(off+4)/4]=(uint32_t)(value>>32);
        if((value&8)&&irq_race){irq_race=0;event(34,UINT64_C(1)<<24,1,0,0,0);}
        signal_irq();return 1;
    }
    return ww(v,off,(uint32_t)value)&&ww(v,off+4,(uint32_t)(value>>32));
}
static int barrier(void *v){(void)v;return 1;}
static int out(void *v,const void *p,size_t z){(void)v;return pmic_model_outside(&m,p,z);}
static void start_core(void)
{
    struct device_memory_extent e;
    assert(device_memory_pool_init(&n.private_pool,(uintptr_t)dma,0x79000000,sizeof(dma)));
    assert(device_memory_acquire(&n.private_pool,99,sizeof(dma),&n.memory,&e));
    const struct a733_xhci_ops ops={cn,current,live,rr,ww,w64,barrier,out,a733_usb2_host_notice};
    registers[0]=0x01100040;registers[1]=((ss_mode?2u:1u)<<24)|(1u<<8)|(dual_mode?3:ss_mode?1:hub_mode?2:1);
    registers[4]=(0x20u<<16)|1|4|8;registers[5]=0x2000;registers[6]=0x1000;
    registers[0x80/4]=0x02000002;registers[0x84/4]=0x20425355;registers[0x88/4]=0x101;
    registers[0x44/4]=1;registers[0x48/4]=1;registers[0x440/4]=1;
    if(ss_mode){registers[3]=(11u<<16)|3;registers[0x80/4]=0x02000402;registers[0x90/4]=0x03000002;
        registers[0x94/4]=0x20425355;registers[0x98/4]=0x102;registers[0x9c/4]=0;
        registers[0x450/4]=1|2|512|(4u<<10);}
    n.core_started=1;assert(a733_xhci_init(&n.core,&ops,&n,&n.private_pool,n.memory,71,2000000,10000000));
    n.core.require_ss=ss_mode;assert(a733_usb2_host_bind(&n));n.stack_started=1;
}
static int poll(void)
{
    m.now+=100;observe_role();wire();
    if(irq_model&&n.core.irq_mode&&!n.core.irq_service&&(registers[0x1020/4]&1)){
        assert(a733_xhci_interrupt_begin(&n.core)==1);++irq_begins;
    }
    int result=a733_usb2_host_poll(&n);
    if(irq_model&&result>=0){
        assert(a733_xhci_interrupt_start(&n.core)>=0);
        if(n.core.irq_mode&&n.core.irq_service){int done=a733_xhci_interrupt_finish(&n.core);
            if(done<0)fprintf(stderr,"irq finish failed phase%u event%u cycle%u events%u asserted%u status%x iman%x cmd%x now%llu\n",n.core.phase,n.core.event_index,n.core.event_cycle,n.core.irq_events,n.core.irq_asserted,registers[0x44/4],registers[0x1020/4],registers[0x40/4],(unsigned long long)m.now);
            assert(done>=0);
        }
    }
    return result;
}
static void mounted(void)
{for(unsigned i=0;i<10000&&!n.mounted&&!n.failed;++i)poll();if(!n.mounted||n.failed||commands!=(hub_mode?8u:4u))fprintf(stderr,"mount failed mounted%u failed%u phase%u now%llu cmds%u xfers%u kind%u hub addr%u ports%u power%u local%u mask%x port%u defer%u requested%u root slot%u addr%u ready%u hubcfg%u ep0%u ctl%u child slot%u addr%u ready%u\n",n.mounted,n.failed,n.core.phase,(unsigned long long)m.now,commands,transfers,n.kind,n.hub.address,n.hub.ports,n.hub.device_power,n.hub.local_power,n.hub.powered_mask,n.hub.selected_port,n.hub.status_deferred,n.hub.status_requested,n.core.devices[0].slot,n.core.devices[0].address,n.core.devices[0].port_ready,n.core.devices[0].hub_configured,n.core.devices[0].endpoints[0].pending,n.core.devices[0].control_stage,n.core.devices[1].slot,n.core.devices[1].address,n.core.devices[1].port_ready);
 assert(n.mounted&&!n.failed&&n.blocks==128&&n.block_bytes==512&&commands==(hub_mode?8u:4u));}
int main(int argc,char **argv)
{
    const char *mode=argc>1?argv[1]:"read";
    if(!strncmp(mode,"irq-",4)){irq_model=1;mode+=4;}
    if(!strncmp(mode,"ss-",3)){
        ss_mode=1;ss_reverse=strstr(mode,"reverse")!=NULL;device_speed=4;
        if(strstr(mode,"bad-bos"))ss_bad=1;if(strstr(mode,"bad-companion"))ss_bad=2;
        if(strstr(mode,"power"))ss_bad=3;if(strstr(mode,"1500"))ss_source=1500;if(strstr(mode,"sel-stall"))ss_bad=4;
        if(strstr(mode,"phy-timeout"))ss_bad=5;if(strstr(mode,"source500"))ss_bad=6;
        if(strstr(mode,"stream16"))ss_streams=16;
        if(strstr(mode,"bad-burst"))ss_bad=7;if(strstr(mode,"bad-streams"))ss_bad=8;if(strstr(mode,"bad-interval"))ss_bad=9;
        if(strstr(mode,"comb0-conflict"))ss_conflict=1;if(strstr(mode,"dcdc2-conflict"))ss_conflict=2;
    }
    if(!strncmp(mode,"hub-",4)){
        hub_mode=1;hub_bitmap=1u<<hub_port;
        if(strstr(mode,"mtt"))hub_protocol=2;
        if(strstr(mode,"port7")){hub_ports=hub_port=7;hub_bitmap=1u<<7;}
        if(strstr(mode,"bus-power"))hub_bad=1;
        if(strstr(mode,"no-power"))hub_bad=2;
        if(strstr(mode,"local-power"))hub_bad=3;
        if(strstr(mode,"ports8"))hub_bad=4;
        if(strstr(mode,"bad-status"))hub_bad=5;
        if(strstr(mode,"tt-readback"))hub_bad=6;
        if(strstr(mode,"nested"))hub_bad=8;
        if(strstr(mode,"detach"))hub_fault_case=1;
        if(strstr(mode,"second-leaf"))hub_fault_case=2;
        if(strstr(mode,"overcurrent"))hub_fault_case=3;
        if(strstr(mode,"stall"))hub_fault_case=4;
        if(strstr(mode,"wrong-slot")){wrong_leaf_slot=1;mode="hub-hid-hs";}
        mode=strstr(mode,"-ls")?"hid-ls":strstr(mode,"-hs")?"hid-hs":strstr(mode,"-fs")||strstr(mode,"port7")?"hid-fs":"read";
    }
    unsigned burst=!strcmp(mode,"burst"),overflow=!strcmp(mode,"overflow"),gate=!strcmp(mode,"gate");
    if(!strcmp(mode,"race")){irq_race=1;mode="read";}
    if(burst||overflow||gate)mode="read";if(!strcmp(mode,"admission")){usb2_native_mapping_check();puts("PASS exact native NC mapping admission accepts shareability2/3 and rejects invalid spans");return 0;}setup();
    if(!strcmp(mode,"mutex-loss")){
        while(n.platform.phase<8){observe_role();assert(a733_usb2_platform_poll(&n.platform)!=A733_USB2_RETAINED);assert(pmic_model_step(&m)!=A733_PMIC_RETAINED);m.now+=20;}
        lose_on_lock=1;assert(a733_usb2_platform_poll(&n.platform)==A733_USB2_RETAINED);
        assert(!platform_writes&&reg_fault&&n.platform.clock.id&&n.platform.rail.id);puts("PASS Type-C revocation during register acquisition prevents all USB MMIO writes");return 0;}
    if(ss_conflict){
        struct a733_clock_token c={0};struct a733_rail_token r={0};
        if(ss_conflict==1)assert(a733_clock_acquire(&m.clocks,101,0,A733_CLOCK_COMB0,3000000,&c)==A733_CLOCK_OK);
        else assert(a733_rail_acquire(&m.rails,101,0,A733_RAIL_BIT(A733_RAIL_DCDC2),3000000,&r)==A733_RAIL_PENDING);
        for(unsigned i=0;i<120000&&!n.platform.failed;++i){observe_role();a733_usb2_platform_poll(&n.platform);pmic_model_step(&m);m.now+=20;}
        assert(n.platform.failed&&!platform_writes&&!ss_halfwords&&!n.core_started);
        assert(ss_conflict==1?c.id:r.id);puts("PASS COMB0/DCDC2 conflict prevents every PHY/controller publication and retains ownership");return 0;
    }
    if(ss_mode&&ss_bad==6){assert(!n.platform.self&&!platform_writes);puts("PASS modeled500mA source refuses SS platform construction");return 0;}
    if(ss_mode&&ss_bad==5){for(unsigned i=0;i<20000&&!n.platform.failed;++i){observe_role();a733_usb2_platform_poll(&n.platform);pmic_model_step(&m);m.now+=20;}assert(n.platform.failed&&!n.core_started&&n.platform.clock.exclusive&A733_CLOCK_COMB0);puts("PASS Cadence PMA timeout retains before controller DMA publication");return 0;}
    prepare();if(ss_mode){assert(ss_halfwords>100&&!!(*pmic_model_word(&m,0x06c01000)&(1u<<12))==ss_reverse&&*pmic_model_word(&m,0x06a0d024)==(1u<<30));}
    assert(platform_writes&&*pmic_model_word(&m,0x02003364)==(ss_mode?0:0x81000003));
    uint32_t reference[2];usb2_source_reference(reference);
    assert(*pmic_model_word(&m,0x06a0c12c)==reference[0]&&*pmic_model_word(&m,0x06a0c630)==reference[1]);
    if(!strncmp(mode,"hid-",4)){
        hid_mode=1;device_speed=strstr(mode,"-ls")?2:strstr(mode,"-hs")?3:1;
        device_packet=device_speed==3?64:device_speed==1?32:8;
        hid_packet=device_speed==2?8:64;hid_interval=device_speed==3?4:10;
        if(strstr(mode,"interval"))bad_hid_config=1;
        if(strstr(mode,"bandwidth"))bad_hid_config=2;
        if(strstr(mode,"descriptor"))bad_hid_config=3;
        if(strstr(mode,"direction"))bad_hid_config=4;
    }
    if(!strncmp(mode,"dual-",5)){
        dual_mode=hub_mode=n.dual_leaf=1;dual_reverse=strstr(mode,"msc-first")!=NULL;dual_duplicate=strstr(mode,"duplicate")!=NULL;
        hub_bitmap=(1u<<2)|(1u<<3);dual_changes[2]=dual_changes[3]=1;
        device_speed=strstr(mode,"-ls")?2:strstr(mode,"-hs")?3:1;
        device_packet=device_speed==3?64:device_speed==1?32:8;hid_packet=device_speed==2?8:64;hid_interval=device_speed==3?4:10;
    }
    start_core();
    if(dual_mode){
        for(unsigned i=0;i<20000&&!n.mounted&&!n.failed;++i)poll();
        if(dual_duplicate){assert(n.failed&&n.core.failed&&!n.mounted);puts("PASS duplicate leaf class retains whole session");return 0;}
        if(!n.mounted||n.failed)fprintf(stderr,"dual mount failed %u %u msc%u hid%u kind%u connected%x default%u cmds%u now%llu\n",n.mounted,n.failed,n.msc_mounted,n.hid_mounted,n.kind,n.hub.connected_mask,n.core.default_device,commands,(unsigned long long)m.now);
        assert(n.mounted&&!n.failed&&n.kind==A733_USB2_DEVICE_DUAL&&n.msc_mounted&&n.hid_mounted&&n.address!=n.hid_address);
        assert(n.address==(dual_reverse?1u:2u)&&n.hid_address==(dual_reverse?2u:1u));
        /* Every permanently published ring/bounce page is disjoint and within
         * the exact retained parent, including the one extra bulk credit. */
        uint64_t pages[14];unsigned count=0;
        for(unsigned j=0;j<A733_XHCI_DEVICES;++j)for(unsigned k=0;k<A733_XHCI_ENDPOINTS;++k){
            struct a733_xhci_endpoint *e=&n.core.devices[j].endpoints[k];if(!e->opened)continue;
            uint64_t v[]={e->ring_pa,e->bounce_pa};for(unsigned z=0;z<2;++z){assert(v[z]>=0x7900a000&&v[z]+4096<=0x79018000&&!(v[z]&4095));for(unsigned q=0;q<count;++q)assert(pages[q]!=v[z]);pages[count++]=v[z];}
        }assert(count==14&&n.core.extra_endpoint_owner==(dual_reverse?2u:3u));
        uint8_t sector[512];struct a733_usb2_hid_report report;memset(sector,0x5a,sizeof(sector));memset(&report,0x5a,sizeof(report));
        assert(a733_usb2_read_sector(&n,0,m.now+5000000)&&a733_usb2_hid_receive(&n,m.now+5000000));
        if(strstr(mode,"wrong-slot"))wrong_leaf_slot=1;
        if(strstr(mode,"stall"))failed_hid_event=1;
        if(strstr(mode,"detach"))a733_typec_revoke(&role);
        unsigned fault=wrong_leaf_slot||failed_hid_event||strstr(mode,"detach")!=NULL;
        FILE *reports=argc>2?fopen(argv[2],"wb"):NULL;if(argc>2)assert(reports);
        for(unsigned i=0;i<4&&!n.failed;++i){
            if(i){report_bytes=i==1?3:i==2?0:8;assert(a733_usb2_hid_receive(&n,m.now+5000000));}
            for(unsigned k=0;k<20000&&!n.hid.complete&&!n.failed;++k)poll();
            if(n.failed)break;assert(a733_usb2_hid_result(&n,&report)==1&&report.address==n.hid_address&&report.sequence==i+1&&report.bytes==report_bytes);
            if(reports)assert(fwrite(&report,1,sizeof(report),reports)==sizeof(report));
        }
        if(reports)assert(!fclose(reports));
        if(fault){assert(n.failed&&n.core.failed&&a733_usb2_read_result(&n,sector)<0);for(unsigned i=0;i<512;++i)assert(sector[i]==0x5a);puts("PASS dual leaf fault retains every slot and forbids sector copy");return 0;}
        for(unsigned i=0;i<20000&&!n.read_complete&&!n.failed;++i)poll();
        assert(!n.failed&&a733_usb2_read_result(&n,sector)==1);for(unsigned i=0;i<512;++i)assert(sector[i]==(uint8_t)(i*17+3));
        if(argc>2){char path[4096];assert(snprintf(path,sizeof(path),"%s.sector",argv[2])<(int)sizeof(path));FILE *f=fopen(path,"wb");assert(f&&fwrite(sector,1,512,f)==512&&!fclose(f));}
        printf("PASS dual leaf order%u HID speed%u and MSC independent credits and copied completions\n",dual_reverse,device_speed);return 0;
    }
    if(ss_mode&&ss_bad){for(unsigned i=0;i<20000&&!n.failed;++i)poll();assert(n.failed&&n.core.failed&&!n.mounted);puts("PASS SS descriptor/power/control failure retains before mount");return 0;}
    if(hub_bad){
        for(unsigned i=0;i<20000&&!n.failed;++i)poll();
        assert(n.failed&&n.core.failed&&!n.mounted&&(hub_bad==8?n.core.devices[1].slot!=0:!n.core.devices[1].slot));
        puts("PASS hub descriptor/power/status/context refusal retains before child admission");return 0;
    }
    if(hid_mode){
        for(unsigned i=0;i<10000&&!n.mounted&&!n.failed;++i)poll();
        if(bad_hid_config){assert(n.failed&&n.core.failed&&!n.mounted);puts("PASS HID descriptor refusal before TinyUSB parser");return 0;}
        assert(n.mounted&&!n.failed&&n.kind==A733_USB2_DEVICE_HID&&n.core.devices[hub_mode?1:0].speed==device_speed);
        assert(evaluates==(device_speed==1?1u:0u)&&!memcmp(n.hid.descriptor,hid_descriptor,sizeof(hid_descriptor)));
        struct a733_usb2_hid_report result;memset(&result,0x5a,sizeof(result));
        assert(a733_usb2_hid_receive(&n,m.now+5000000));
        if(strstr(mode,"stall"))failed_hid_event=1;
        if(strstr(mode,"pointer"))wrong_hid_pointer=1;
        if(strstr(mode,"detach"))a733_typec_revoke(&role);
        if(strstr(mode,"timeout")){hold_bulk=1;m.now+=5000001;}
        if(strstr(mode,"cancel")){hold_bulk=1;for(unsigned i=0;i<10;++i)assert(poll()>=0);assert(a733_xhci_abort(&n.core,n.address,0x81));}
        unsigned failure=failed_hid_event||wrong_hid_pointer||wrong_leaf_slot||strstr(mode,"detach")||strstr(mode,"timeout")||strstr(mode,"cancel");
        for(unsigned i=0;i<10000&&!n.hid.complete&&!n.failed;++i)poll();
        if(failure){assert(n.failed&&n.core.failed&&!n.hid.complete&&a733_usb2_hid_result(&n,&result)<0);
            for(unsigned j=0;j<sizeof(result);++j)assert(((const uint8_t *)&result)[j]==0x5a);
            enum device_memory_state state;struct device_memory_extent extent;
            assert(device_memory_describe(&n.private_pool,n.memory,&extent,&state)&&state==DEVICE_MEMORY_RETAINED);
            puts("PASS failed HID completion retains DMA without class result/caller copy");return 0;}
        FILE *reports=NULL;if(argc>2){reports=fopen(argv[2],"wb");assert(reports);}
        for(unsigned i=0;i<4;++i){
            if(i){report_bytes=i==1?3:i==2?0:8;assert(a733_usb2_hid_receive(&n,m.now+5000000));
                for(unsigned j=0;j<10000&&!n.hid.complete&&!n.failed;++j)poll();}
            assert(n.hid.complete&&!n.failed&&a733_usb2_hid_result(&n,&result)==1);
            assert(result.bytes==report_bytes&&result.sequence==i+1&&result.generation==71&&result.address==1&&
              result.interface==0&&result.endpoint==0x81&&result.protocol==1&&result.speed==device_speed);
            for(unsigned j=0;j<result.bytes;++j)assert(result.data[j]==(uint8_t)(i*17+j+1));
            for(unsigned j=result.bytes;j<64;++j)assert(!result.data[j]);
            if(reports)assert(fwrite(&result,1,sizeof(result),reports)==sizeof(result));
        }
        if(reports)assert(!fclose(reports));
        printf("PASS speed%u HID TinyUSB parser/callbacks, interrupt context/interval, FS Evaluate Context, four copied reports with exact short/ZLP lengths\n",device_speed);return 0;
    }
    if(!strcmp(mode,"bad-config")||!strcmp(mode,"short-descriptor")){
        bad_config=!strcmp(mode,"bad-config");short_descriptor=!strcmp(mode,"short-descriptor");
        for(unsigned i=0;i<10000&&!n.failed;++i)poll();
        assert(n.failed&&n.core.failed&&!n.mounted);printf("PASS %s refused before TinyUSB parser\n",mode);return 0;}
    mounted();
    if(ss_mode){
        assert(n.ss_ready&&n.ss_control==4&&n.core.speed==4&&n.core.usb2_port==2&&n.core.usb2_companion_port==1);
        assert(n.core.devices[0].endpoints[0].packet==512);
        for(unsigned i=1;i<3;++i){struct a733_xhci_endpoint *e=&n.core.devices[0].endpoints[i];assert(e->packet==1024&&e->burst==15);}
        uint8_t sector[512];memset(sector,0x5a,sizeof(sector));
        if(strstr(mode,"companion-event")){event(34,UINT64_C(1)<<24,1,0,0,0);assert(poll()>=0&&!n.failed);}
        if(strstr(mode,"disconnect")||strstr(mode,"wrong-port")||strstr(mode,"stale-generation")){
            if(strstr(mode,"disconnect"))registers[0x450/4]&=~1u;
            else if(strstr(mode,"wrong-port"))event(34,UINT64_C(3)<<24,1,0,0,0);
            else n.core.generation=72;
            for(unsigned i=0;i<4&&!n.failed;++i)poll();assert(n.failed&&n.core.failed);
            assert(a733_usb2_read_result(&n,sector)<0);for(unsigned i=0;i<512;++i)assert(sector[i]==0x5a);
            puts("PASS SS disconnect/unknown port/stale generation retains before callback and copy");return 0;
        }
        assert(a733_usb2_read_sector(&n,0,m.now+5000000));
        for(unsigned i=0;i<20000&&!n.read_complete&&!n.failed;++i)poll();assert(!n.failed&&a733_usb2_read_result(&n,sector)==1);
        for(unsigned i=0;i<512;++i)assert(sector[i]==(uint8_t)(i*17+3));
        if(argc>2){FILE *f=fopen(argv[2],"wb");assert(f&&fwrite(sector,1,512,f)==512&&!fclose(f));}
        puts("PASS actual Cadence orientation sequence and SS TinyUSB BOS/ISOCH/SEL/companion/BOT with copied sector");return 0;
    }
    uint8_t result[512];memset(result,0x5a,sizeof(result));
    if(hub_fault_case){
        hub_after_fault=hub_fault_case;
        assert(a733_usb2_read_sector(&n,7,m.now+500000));hold_bulk=1;
        hub_changes=1;hub_changed_port=hub_after_fault==2?hub_port+1:hub_port;
        hub_bitmap=hub_after_fault==3?1:1u<<hub_changed_port;
        for(unsigned i=0;i<20000&&!n.failed;++i)poll();
        assert(n.failed&&n.core.failed&&a733_usb2_read_result(&n,result)<0);
        for(unsigned i=0;i<512;++i)assert(result[i]==0x5a);
        for(unsigned j=0;j<A733_XHCI_DEVICES;++j)for(unsigned i=0;i<A733_XHCI_ENDPOINTS;++i)
            assert(!n.core.devices[j].endpoints[i].caller&&n.core.devices[j].endpoints[i].aborted);
        enum device_memory_state state;struct device_memory_extent extent;
        assert(device_memory_describe(&n.private_pool,n.memory,&extent,&state)&&state==DEVICE_MEMORY_RETAINED);
        puts("PASS hub removal/second leaf/overcurrent/stall retains both slots and refuses copied output");return 0;
    }
    if(burst){
        for(unsigned i=0;i<20;++i)event(34,UINT64_C(1)<<24,1,0,0,0);
        unsigned old=irq_advances;for(unsigned i=0;i<24;++i)assert(poll()>=0);
        assert(irq_advances>=old+2&&!n.core.irq_service);
    }
    if(gate){
        event(34,UINT64_C(1)<<24,1,0,0,0);unsigned at=n.core.event_index;
        assert(a733_usb2_host_poll(&n)>=0&&n.core.event_index==at);
        assert(poll()>=0&&n.core.event_index!=at);
    }
    if(overflow){
        assert(a733_usb2_read_sector(&n,7,m.now+500000));hold_bulk=1;
        event(37,0,21,0,0,0);assert(poll()<0&&n.failed&&n.core.failed);
        assert(a733_usb2_read_result(&n,result)<0);
        for(unsigned i=0;i<512;++i)assert(result[i]==0x5a);
        puts("PASS IRQ event-ring full retains DMA before class completion/caller copy");return 0;
    }
    assert(a733_usb2_read_sector(&n,7,m.now+500000));
    if(!strcmp(mode,"bad-status"))bad_status=1;
    if(!strcmp(mode,"short-data"))short_data=1;
    if(!strcmp(mode,"cancel")){
        hold_bulk=1;for(unsigned i=0;i<10;++i)assert(poll()>=0);
        assert(a733_xhci_abort(&n.core,n.address,2));assert(n.core.failed);
    }else if(!strcmp(mode,"detach")){a733_typec_revoke(&role);assert(a733_usb2_host_poll(&n)<0);}
    else for(unsigned i=0;i<10000&&!n.read_complete&&!n.failed;++i)poll();
    if(strcmp(mode,"read")){
        (void)a733_usb2_host_poll(&n);assert(n.failed&&n.core.failed);
        assert(a733_usb2_read_result(&n,result)<0);for(unsigned j=0;j<512;++j)assert(result[j]==0x5a);
        enum device_memory_state s;struct device_memory_extent e;assert(device_memory_describe(&n.private_pool,n.memory,&e,&s)&&s==DEVICE_MEMORY_RETAINED);
        printf("PASS %s retains DMA and refuses caller output\n",mode);return 0;}
    assert(n.read_complete&&!n.failed&&a733_usb2_read_result(&n,result)==1);
    for(unsigned j=0;j<512;++j)assert(result[j]==(uint8_t)(j*17+3));
    if(irq_model)assert(irq_begins&&irq_releases&&!(registers[0x1020/4]&1));
    assert(n.core.devices[hub_mode?1:0].endpoints[0].cycle==0); /* Enumeration crosses the control Link TRB. */
    printf("PASS actual PMIC/clock/role observation, xHCI%u commands/%u transfers, TinyUSB MSC enumeration and sector read\n",commands,transfers);
    return 0;
}
