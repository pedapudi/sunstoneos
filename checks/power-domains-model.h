/* SPDX-License-Identifier: Apache-2.0 */
/* Shared host model for the power-domain owner and its service task.
 *
 * It composes the actual PMIC provider, rail ledger, clock registry, GPU
 * resource manager and pin manager of checks/a733-pmic.c with the actual
 * owner in drivers/power/a733_power_domains.c. The owner's register callbacks
 * reach a model of PPU domains 1, 4 and 5 and of the CCU and R_CCU words the
 * owner reads. A PPU acknowledges a policy write ack_us after it, or never
 * when stalled. The service task is the provider's worker; service_step()
 * runs one step as that task and records what a FreeRTOS shim would see: the
 * ticks it blocks for, its wakes and the notifications it receives. Time is
 * the PMIC model clock in microseconds, which advances on every time read.
 * Build with checks/a733-pmic.c's units plus drivers/power/a733_power_domains.c
 * and drivers/gpu/a733_gpu_power.c. */
#ifndef POWER_DOMAINS_MODEL_H
#define POWER_DOMAINS_MODEL_H
#define PMIC_MODEL_NO_MAIN
#include "a733-pmic.c"
#include "a733_power_domains.h"
#include "a733_gpu_power.h"
#include <stdlib.h>

#define TICK_US 1000u
#define CCU_BASE UINT32_C(0x02002000)
#define RCCU_BASE UINT32_C(0x07010000)
static struct pmic_model m;
/* Model helpers that some checks do not call. */
static void model_unused(void){(void)pmic_model_step;}
struct access { unsigned write; uintptr_t address; uint32_t value; };
struct domains_model {
    struct a733_power_domains owner;
    struct a733_power_service_config config;
    uint32_t ppu[6][0x1000/4];
    uint32_t ccu_5c0,ccu_5e0,ccu_b20,ccu_b24,rccu_1ac,ccu_e0;
    uint64_t ack_at[6];
    unsigned target[6],pending[6],stall[6],status_override[6];
    unsigned in_step,notified,notifies,self_notifies;
    unsigned steps,tick_waits,wakes,ppu_reads,ppu_writes,level_reads,fail_read;
    uint64_t ack_us,wake_at,longest_step_us;
    struct access trace[512];unsigned trace_n;
    void (*written)(uintptr_t,uint32_t); /* optional observer of owner writes */
};
static struct domains_model d;
static void trace(unsigned write,uintptr_t a,uint32_t v)
{if(d.trace_n<512)d.trace[d.trace_n++]=(struct access){write,a,v};}
static uint32_t *level_word(uintptr_t a)
{
    switch(a) {
    case CCU_BASE+0x5c0:return &d.ccu_5c0;
    case CCU_BASE+0x5e0:return &d.ccu_5e0;
    case CCU_BASE+0xb20:return &d.ccu_b20;
    case CCU_BASE+0xb24:return &d.ccu_b24;
    case CCU_BASE+0xe0:return &d.ccu_e0;
    case RCCU_BASE+0x1ac:return &d.rccu_1ac;
    default:return 0;
    }
}
static int ppu_block(uintptr_t a,unsigned *id,unsigned *offset)
{
    if(a<UINT32_C(0x07061000) || a>=UINT32_C(0x07066000))return 0;
    *id=(unsigned)((a-UINT32_C(0x07060000))>>12);*offset=(unsigned)(a&0xfff);
    return *id==1 || *id==4 || *id==5;
}
static int owner_read(void *v,uintptr_t a,uint32_t *out)
{
    assert(v==&d);unsigned id,offset;
    if(d.fail_read && !--d.fail_read)return 0;
    if(m.concurrent)m.concurrent(&m);
    if(m.access_ns)pmic_model_cost(&m,m.access_ns);
    uint32_t *w=level_word(a);
    if(w){++d.level_reads;*out=*w;trace(0,a,*out);return 1;}
    assert(ppu_block(a,&id,&offset) && !(offset&3));++d.ppu_reads;
    if(offset==8 && d.pending[id] && !d.stall[id] && m.now>=d.ack_at[id]) {
        d.ppu[id][2]=(d.ppu[id][2]&~15u)|d.target[id];d.pending[id]=0;
    }
    *out=d.ppu[id][offset/4];
    if(offset==8 && d.status_override[id])*out=(*out&~15u)|(d.status_override[id]&15);
    trace(0,a,*out);return 1;
}
static int owner_write(void *v,uintptr_t a,uint32_t value)
{
    assert(v==&d && d.in_step);unsigned id,offset;
    assert(ppu_block(a,&id,&offset) && offset!=8);++d.ppu_writes;trace(1,a,value);
    if(m.access_ns)pmic_model_cost(&m,m.access_ns);
    d.ppu[id][offset/4]=value;
    if(d.written)d.written(a,value);
    if(!offset){d.target[id]=value&15;d.pending[id]=1;d.ack_at[id]=m.now+d.ack_us;}
    return 1;
}
/* The native notify skips a wake from the service task itself, which handles
 * its own submissions within the same step. */
static void owner_notify(void *v)
{assert(v==&d);if(d.in_step){++d.self_notifies;return;}++d.notifies;d.notified=1;}
static void domains_model_setup(uint32_t divider,uint64_t wire_us)
{
    (void)model_unused;
    pmic_model_setup(&m);memset(&d,0,sizeof(d));
    *pmic_model_word(&m,0x07083014)=divider;m.wire_us=wire_us;m.now_step=1;
    /* DCDC2 and DCDC4 enabled (0x10 bits 1 and 3) at selector 30 (800 mV). */
    m.pmic[0x10]=0xff;m.pmic[0x13]=m.pmic[0x15]=30;
    d.ccu_5c0=0x04000000|(1u<<5)|(1u<<6)|(1u<<7);d.ccu_5e0=(1u<<11)|(1u<<16)|(1u<<18);
    d.rccu_1ac=0x00400001;d.ccu_b20=0x10200100;d.ccu_b24=0x24680000;
    d.ccu_e0=0x41104500; /* PLL_GPU0 as boot0 leaves it */
    for(unsigned i=1;i<6;++i){d.ppu[i][0]=0xa5a50000;d.ppu[i][2]=0x42000000;}
    d.ack_us=10;
    d.config=(struct a733_power_service_config){50,4000000,TICK_US,0};
    const struct a733_power_domain_ops ops={owner_read,owner_write,owner_notify};
    assert(a733_power_domains_init(&d.owner,&m.provider,&ops,&d,&d.config));
}
/* One service step as the provider task. */
static uint32_t service_step(void)
{
    const uint64_t start=m.now;
    m.worker=1;d.in_step=1;uint32_t ticks=a733_power_service_step(&d.owner);d.in_step=0;m.worker=0;
    ++d.steps;d.notified=0;if(ticks==1)++d.tick_waits;
    if(m.now-start>d.longest_step_us)d.longest_step_us=m.now-start;
    d.wake_at=ticks==UINT32_MAX ? UINT64_MAX : m.now+(uint64_t)ticks*TICK_US;
    return ticks;
}
/* The service blocks in ulTaskNotifyTake: it wakes on a notification or when
 * its ticks elapse. Advance model time to `until` in whole ticks, running the
 * service whenever it would wake; returns the wakes. */
static __attribute__((unused)) unsigned service_until(uint64_t until)
{
    unsigned wakes=0;
    while(m.now<until) {
        if(d.notified || m.now>=d.wake_at){service_step();++wakes;++d.wakes;continue;}
        uint64_t next=d.wake_at<until ? d.wake_at : until;
        if(next>m.now)m.now=next;
    }
    return wakes;
}
static __attribute__((unused)) struct a733_rail_token domains_rail(unsigned owner,enum a733_rail rail)
{
    struct a733_rail_token t={0};
    assert(a733_rail_acquire(&m.rails,owner,A733_RAIL_BIT(rail),0,m.now+10000000,&t)==A733_RAIL_PENDING);
    assert(a733_rail_poll(&m.rails,&t)==A733_RAIL_READY);return t;
}
/* Run the service from its first step until supply verification ends. */
static void domains_boot(void)
{
    d.wake_at=0;
    for(unsigned i=0;i<100000 && !(a733_power_authority_load(&m.rails.authority)&A733_POWER_SUPPLY_DONE);++i) {
        if(d.wake_at!=UINT64_MAX && d.wake_at>m.now)m.now=d.wake_at;
        service_step();
    }
    assert(a733_power_authority_load(&m.rails.authority)&A733_POWER_SUPPLY_DONE);
}
#endif
