/* SPDX-License-Identifier: BSD-3-Clause */
/* Reset workarounds adapted from Trusted Firmware-A.
 * Copyright (c) 2017-2026, Arm Limited and Contributors. All rights reserved.
 */
#include "a733_cpu_coherence.h"
#define BIT(n) (UINT64_C(1)<<(n))
static int overlap(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return an>UINTPTR_MAX-x || bn>UINTPTR_MAX-y || (x<y+bn && y<x+an);
}
int a733_cpu_coherence_plan(struct a733_cpu_coherence_plan *out,
                            const struct a733_cpu_coherence_identity *id,unsigned core)
{
    struct a733_cpu_coherence_plan p={0};
    if(!out || !id || core>7 || overlap(out,sizeof(*out),id,sizeof(*id)) ||
       (id->mpidr&~UINT64_C(0xc1000700)) || ((id->mpidr>>8)&7)!=core ||
       (id->midr&UINT64_C(0xffffffffff0ffff0))!=(core<6?0x410fd050u:0x410fd0b0u) ||
       (id->mmfr0&15)<2 || (id->mmfr0&15)>5 || ((id->mmfr0>>28)&15)!=0 ||
       id->clusteridr>UINT32_MAX || id->clustercfr>UINT32_MAX)return -1;
    unsigned r=((id->midr>>20)&15)*16+(id->midr&15);
    p.core=(uint8_t)core;p.revision=(uint8_t)r;p.requirements=A733_CPU_COHERENCE_AT;
    if(core<6) {
        if(r!=0 && r!=1 && r!=0x10 && r!=0x20)return -1;
        if(r<=1)p.actlr=BIT(31)|BIT(49); /* 768277/798797, 846532/903758. */
        if(!r && !(id->clidr&(UINT64_C(7)<<6))) {
            p.ectlr=UINT64_C(3)<<25;p.actlr|=BIT(24); /* 778703. */
        }
        if(r<=0x10)p.patch=1; /* 1221012. */
    } else {
        if(r!=0 && r!=0x10 && r!=0x20 && r!=0x30 && r!=0x31 && r!=0x40 && r!=0x41)return -1;
        if(r<=0x10)p.actlr|=BIT(6); /* 1073348. */
        if(r<=0x40)p.actlr|=BIT(13); /* 1262606/1275112/1868343. */
        /* 2356586, 3888013; static CVE-2018-3639 mitigation. */
        p.actlr2=BIT(0)|BIT(22)|BIT(16);
        if(r<=0x20)p.actlr2|=BIT(59)|BIT(15)|BIT(11); /* 1130799/1165347/1207823. */
        if(r<=0x40)p.actlr2|=BIT(2); /* 1791580. */
        if(r<=0x30)p.actlr3=BIT(10); /* 1257314. */
        if(r<=0x20)p.ectlr|=UINT64_C(3)<<24; /* 1220197. */
        if(r<=0x30)p.ectlr|=BIT(51); /* 1262888. */
        p.patch=2; /* 1946160. */
        p.requirements|=A733_CPU_COHERENCE_BHB|A733_CPU_COHERENCE_POWER_DSB|A733_CPU_COHERENCE_TLBI;
    }
    /* DSU workarounds attached to both core implementations by pinned TF-A. */
    unsigned d=(unsigned)(id->clusteridr&255);
    if(d!=0 && d!=1 && d!=0x10 && d!=0x11 && d!=0x20 && d!=0x21 &&
       d!=0x30 && d!=0x31 && d!=0x40 && d!=0x41)return -1;
    if(!d)p.clusteractlr=BIT(15); /* 798953. */
    if(d<0x20 && (id->clustercfr&BIT(11)))p.clusteractlr|=UINT64_C(3)<<15; /* 936184. */
    *out=p;return 1;
}
int a733_cpu_coherence_tables(uint64_t *l1,uint64_t *l2,uint64_t pa,unsigned el)
{
    if((el!=1 && el!=3) || !l1 || !l2 || ((uintptr_t)l1&4095) || ((uintptr_t)l2&4095) ||
       overlap(l1,4096,l2,4096) || (pa&4095) ||
       pa<A733_CPU_COHERENCE_MONITOR || pa>=A733_CPU_COHERENCE_MONITOR_END)return -1;
    for(unsigned i=0;i<512;++i)l1[i]=l2[i]=0;
    /* Device, Outer Shareable field, EL1 RW, XN. */
    l1[0]=BIT(54)|(el==1?BIT(53):BIT(6))|BIT(10)|(UINT64_C(2)<<8)|1;
    l1[1]=pa|3;
    for(unsigned i=0;i<64;++i) {
        uint64_t a=UINT64_C(0x40000000)+((uint64_t)i<<21);
        uint64_t v=a|BIT(10)|(UINT64_C(3)<<8)|4|1; /* AF, ISH, WBWA. */
        if(a==UINT64_C(0x41000000))v|=UINT64_C(3)<<6; /* EL0/1 RO executable. */
        else {
            v|=BIT(53)|BIT(54);
            if(a==A733_CPU_COHERENCE_MONITOR)v=(v&~UINT64_C(0x1c))|8; /* NC, privileged. */
            else v|=BIT(6); /* EL0/1 RW. */
        }
        if(el==3)v=(v&~BIT(53))|BIT(6)|BIT(5); /* AP RES1, nonsecure RAM. */
        l2[i]=v;
    }
    return 1;
}
static int fail(struct a733_cpu_coherence *s,unsigned e)
{ s->error=e;s->state=A733_COH_RETAINED;return -1; }
static int tick(struct a733_cpu_coherence *s)
{
    uint64_t n;
    if(s->ops.now_us(s->context,&n)!=1 || n<s->now || n>=s->deadline)return fail(s,1);
    s->now=n;return 1;
}
static int readreg(struct a733_cpu_coherence *s,unsigned r,uint64_t *v)
{ if(s->ops.read(s->context,r,v)!=1)return fail(s,2);return tick(s); }
static int writereg(struct a733_cpu_coherence *s,unsigned r,uint64_t v)
{ if(s->ops.write(s->context,r,v)!=1)return fail(s,2);return tick(s); }
int a733_cpu_coherence_init(struct a733_cpu_coherence *s,
    const struct a733_cpu_coherence_ops *ops,void *ctx,unsigned core,uint64_t token,uint64_t deadline)
{
    if(!s || !ops || s->state!=A733_COH_NEW || s->token || core>7 || !token || !deadline ||
       !ops->read || !ops->write || !ops->now_us || overlap(s,sizeof(*s),ops,sizeof(*ops)) ||
       (ctx && overlap(s,sizeof(*s),ctx,1)))return -1;
    *s=(struct a733_cpu_coherence){.ops=*ops,.context=ctx,.core=core,.token=token,
        .deadline=deadline,.state=A733_COH_IDENTIFY};return 1;
}
static uint64_t control_mask(const struct a733_cpu_coherence_plan *p,unsigned i)
{
    switch(i) { case 0:return p->actlr;case 1:return p->actlr2;
    case 2:return p->actlr3;case 3:return p->ectlr;default:return p->clusteractlr; }
}
static uint64_t patch_value(unsigned patch,unsigned i)
{
    /* Selector, opcode, mask, control. Patch registers belong to the boot owner. */
    static const uint64_t a55[]={0,0x08500020,0x21ff00000,0x011003fd,
        1,0x08d00040,0x21ff00040,0x011003fd};
    static const uint64_t a76[]={3,0x10e3900002,0x10fff00083,0x2001003ff,
        4,0x10e3800082,0x10fff00083,0x2001003ff,
        5,0x10e3800200,0x10fff003e0,0x2001003ff};
    return patch==1?a55[i]:a76[i];
}
int a733_cpu_coherence_poll(struct a733_cpu_coherence *s)
{
    uint64_t v;
    if(!s || !s->token || s->state==A733_COH_NEW || s->state==A733_COH_RETAINED)return -1;
    if(s->state==A733_COH_PREPARED)return 1;
    if(tick(s)!=1)return -1;
    if(s->state==A733_COH_IDENTIFY) {
        if(s->index==10) {
            if(a733_cpu_coherence_plan(&s->plan,&s->identity,s->core)!=1)return fail(s,3);
            s->index=s->phase=0;s->state=A733_COH_CONTROL;return 0;
        }
        if(readreg(s,s->index,&v)!=1)return -1;
        switch(s->index) {
        case 0:if(v!=12)return fail(s,3);break;
        case 1:case 2:if(v&UINT64_C(0x02001005))return fail(s,3);break;
        case 3:if((v&0x3c0)!=0x3c0)return fail(s,3);break;
        case 4:s->identity.midr=v;break;case 5:s->identity.mpidr=v;break;
        case 6:s->identity.clidr=v;break;case 7:s->identity.mmfr0=v;break;
        case 8:s->identity.clusteridr=v;break;case 9:s->identity.clustercfr=v;break;
        }
        ++s->index;return 0;
    }
    if(s->state==A733_COH_CONTROL) {
        if(s->index==5) { s->index=s->phase=0;s->state=A733_COH_PATCH;return 0; }
        uint64_t mask=control_mask(&s->plan,s->index);
        if(!mask) { ++s->index;return 0; }
        unsigned reg=A733_COH_ACTLR+s->index;
        if(!s->phase) {
            if(readreg(s,reg,&v)!=1)return -1;
            if(s->index==4 && s->core) {
                if((v&mask)!=mask)return fail(s,4);
                ++s->index;return 0; /* Only primary writes shared DSU. */
            }
            s->value=v|mask;s->phase=1;
        } else if(s->phase==1) {
            if(writereg(s,reg,s->value)!=1)return -1;
            s->phase=2;
        } else {
            if(readreg(s,reg,&v)!=1)return -1;
            if(v!=s->value)return fail(s,4);
            ++s->index;s->phase=0;
        }
        return 0;
    }
    if(s->state==A733_COH_PATCH) {
        unsigned count=s->plan.patch==1?8:s->plan.patch==2?12:0;
        if(s->index==count) { s->state=A733_COH_PREPARED;return 1; }
        /* Source provides write sequences, not a readable patch-store contract. */
        if(writereg(s,A733_COH_PATCH_SELECT+(s->index%4),patch_value(s->plan.patch,s->index))!=1)return -1;
        ++s->index;return 0;
    }
    return fail(s,3);
}
#ifdef __aarch64__
extern int a733_cpu_coherence_reg_read(void *,unsigned,uint64_t *);
extern int a733_cpu_coherence_reg_write(void *,unsigned,uint64_t);
static int native_now(void *unused,uint64_t *us)
{
    uint64_t t,f;(void)unused;
    __asm__ volatile("mrs %0, cntfrq_el0\n\tisb\n\tmrs %1, cntpct_el0":"=r"(f),"=r"(t));
    if(f!=24000000)return -1;
    *us=t/24;return 1;
}
const struct a733_cpu_coherence_ops a733_cpu_coherence_native_ops={
    native_now,a733_cpu_coherence_reg_read,a733_cpu_coherence_reg_write};
#endif
