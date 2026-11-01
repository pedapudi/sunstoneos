/* SPDX-License-Identifier: GPL-2.0-or-later
 * PMU programming follows Allwinner sunxi-nsi.c at 2ac08e8c7.
 * See docs/designs/gpu-nsi-traffic.md for references and empirical limits. */
#include "a733_gpu_nsi_pmu.h"
#include "a733_clock_reference.h"
#include <string.h>

uint32_t a733_gpu_nsi_hz(uint32_t oscillator,uint32_t reference,uint32_t peri0,
    uint32_t peri_pattern0,uint32_t peri_pattern1,
    uint32_t de,uint32_t pattern0,uint32_t pattern1,uint32_t clock)
{
    if(!a733_clock_ref24_valid(oscillator,reference) ||
       (clock&UINT32_C(0xd8000000))!=UINT32_C(0xc0000000))return 0;
    const unsigned mux=(clock>>24)&7,divider=(clock&31)+1;
    uint64_t parent=0;
    if(mux==0)parent=24000000;
    else if(mux>=2 && mux<=4) {
        if(!a733_clock_peri0_valid(peri0) || (peri_pattern0&UINT32_C(0x80000000)) ||
           (peri_pattern1&UINT32_C(0x08000000)))return 0;
        const unsigned n=((peri0>>8)&255)+1;
        const unsigned p=mux==2?((peri0>>16)&7)+1:mux==3?2*(((peri0>>20)&7)+1):((peri0>>2)&7)+1;
        parent=UINT64_C(24000000)*n/p;
    } else if(mux==5) {
        if((de&UINT32_C(0xf4000000))!=UINT32_C(0xf4000000) ||
           (pattern0&UINT32_C(0x80000000)) || (pattern1&UINT32_C(0x08000000)))return 0;
        const unsigned n=((de>>8)&255)+1,m=((de>>1)&1)+1,p=((de>>16)&7)+1;
        if(n<53 || n>105)return 0;
        parent=UINT64_C(24000000)*n/m/p;
    } else return 0; /* DDRPLL and reserved selections need their own decoder. */
    const uint64_t hz=parent/divider;
    return hz && hz<=700000000 ? (uint32_t)hz : 0;
}
static int read_value(struct a733_gpu_nsi_pmu *p,enum a733_gpu_nsi_register reg,uint32_t *v)
{ return p->ops.read(p->context,reg,v); }
static int write_value(struct a733_gpu_nsi_pmu *p,enum a733_gpu_nsi_register reg,uint32_t v)
{ return p->ops.write(p->context,reg,v); }
static int settle(struct a733_gpu_nsi_pmu *p)
{
    const uint64_t begin=p->ops.ticks(p->context);
    for(unsigned i=0;i<4096;++i) {
        const uint64_t now=p->ops.ticks(p->context);
        if(now<begin)return 0;
        if(now-begin>=240)return 1; /* Vendor's 10 us; not a claimed clear latency. */
    }
    return 0;
}
static int sample(struct a733_gpu_nsi_pmu *p,struct a733_gpu_nsi_record *r,unsigned i)
{
    if(!read_value(p,A733_NSI_READ,&r->read[i]) || !read_value(p,A733_NSI_WRITE,&r->write[i]))return 0;
    r->samples|=1u<<i;return 1;
}
static void restore(struct a733_gpu_nsi_pmu *p,struct a733_gpu_nsi_record *r)
{
    if(!p->owned)return;
    uint32_t enabled,cycle;
    const int ok=write_value(p,A733_NSI_ENABLE,p->saved_enable&~1u) &&
        write_value(p,A733_NSI_CYCLE,p->saved_cycle) &&
        read_value(p,A733_NSI_ENABLE,&enabled) && read_value(p,A733_NSI_CYCLE,&cycle) &&
        enabled==p->saved_enable && cycle==p->saved_cycle;
    if(!ok){r->status=A733_NSI_IO;p->failed=1;}
    p->owned=0;
}
void a733_gpu_nsi_begin(struct a733_gpu_nsi_pmu *p,const struct a733_gpu_nsi_ops *ops,
    void *ctx,uint32_t hz,uint32_t clock,struct a733_gpu_nsi_record *r)
{
    memset(r,0,sizeof(*r));r->clock_raw=clock;r->hz=hz;
    if(p->failed){r->status=A733_NSI_IO;return;}
    if(!ops || !ops->read || !ops->write || !ops->ticks || p->owned)return;
    p->ops=*ops;p->context=ctx;r->begin=ops->ticks(ctx);
    if(!hz || hz>700000000){r->status=A733_NSI_CLOCK;return;}
    if(!read_value(p,A733_NSI_ENABLE,&p->saved_enable) || !read_value(p,A733_NSI_CYCLE,&p->saved_cycle) ||
       !sample(p,r,0)){r->status=A733_NSI_IO;return;}
    r->saved_enable=p->saved_enable;r->saved_cycle=p->saved_cycle;
    if(p->saved_enable&1){r->status=A733_NSI_BUSY;return;}
    p->owned=1;
    /* Vendor UPDATE_CYCLE order: period, disable, clear, enable, 10 us.
     * Restrict it to the exclusively owned GPU bank; no global reset or QoS. */
    uint32_t clear;
    if(!write_value(p,A733_NSI_CYCLE,UINT32_MAX) ||
       !write_value(p,A733_NSI_ENABLE,p->saved_enable&~1u) ||
       !read_value(p,A733_NSI_CLEAR,&clear) || !write_value(p,A733_NSI_CLEAR,clear|1u) ||
       !write_value(p,A733_NSI_ENABLE,p->saved_enable|1u))goto io;
    /* Use the pre-programming time as the conservative period start. */
    p->enabled=r->begin;
    if(!settle(p) || !read_value(p,A733_NSI_CLEAR,&r->clear_raw) || !sample(p,r,1))goto io;
    uint32_t enabled,cycle;
    if(!read_value(p,A733_NSI_ENABLE,&enabled) || !read_value(p,A733_NSI_CYCLE,&cycle) ||
       enabled!=(p->saved_enable|1u) || cycle!=UINT32_MAX)goto io;
    r->status=(r->clear_raw&1)?A733_NSI_CLEAR_PENDING:A733_NSI_SAMPLED;
    if(r->status!=A733_NSI_SAMPLED)restore(p,r);
    return;
io:r->status=A733_NSI_IO;restore(p,r);
}
void a733_gpu_nsi_after_kick(struct a733_gpu_nsi_pmu *p,struct a733_gpu_nsi_record *r)
{ if(p->owned && !sample(p,r,2))r->status=A733_NSI_IO; }
void a733_gpu_nsi_end(struct a733_gpu_nsi_pmu *p,struct a733_gpu_nsi_record *r)
{
    if(!p->owned)return;
    if(!sample(p,r,3) || !write_value(p,A733_NSI_ENABLE,p->saved_enable&~1u) ||
       !sample(p,r,4) || !settle(p) || !sample(p,r,5))r->status=A733_NSI_IO;
    r->end=p->ops.ticks(p->context);
    if(r->status!=A733_NSI_IO && (r->end<p->enabled ||
       r->end-p->enabled>=UINT64_C(24000000)*UINT32_MAX/r->hz))r->status=A733_NSI_PERIOD_EXPIRED;
    if(r->status==A733_NSI_SAMPLED && (r->read[3]<r->read[1] || r->write[3]<r->write[1]))
        r->status=A733_NSI_NONMONOTONIC;
    restore(p,r);
}
