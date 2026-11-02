/* SPDX-License-Identifier: GPL-2.0-only */
#include "sunstone_cpu_observation.h"
#include <stdatomic.h>
#include <stddef.h>

static struct ss_cpu_observation snapshots[SS_CPU_MAX];
static atomic_uint valid[SS_CPU_MAX];
int ss_cpu_observation_capture(unsigned core,const struct ss_cpu_observation_ops *ops,
                               void *cookie,struct ss_cpu_observation *out)
{
    if(core>=SS_CPU_MAX||!ops||!ops->read||!ops->select||!ops->isb||!out)return 0;
    struct ss_cpu_observation r={0};r.core=core;
    r.mpidr=ops->read(cookie,SS_CPU_REG_MPIDR);r.midr=ops->read(cookie,SS_CPU_REG_MIDR);
    r.isar0=ops->read(cookie,SS_CPU_REG_ISAR0);r.mmfr2=ops->read(cookie,SS_CPU_REG_MMFR2);
    r.clidr=ops->read(cookie,SS_CPU_REG_CLIDR);
    r.flags=SS_CPU_OBS_IDENTITY;r.atomic=(r.isar0>>20)&15;r.ccidx=(r.mmfr2>>20)&15;
    if(r.atomic==0||r.atomic==2||r.atomic==3)r.flags|=SS_CPU_OBS_ATOMIC_KNOWN;
    if(r.atomic==2||r.atomic==3)r.flags|=SS_CPU_OBS_LSE;
    if(r.ccidx<=1)r.flags|=SS_CPU_OBS_CACHE_FORMAT;
    uint64_t saved=ops->read(cookie,SS_CPU_REG_CSSELR);
    for(unsigned level=0;level<7;++level){
        unsigned type=(r.clidr>>(level*3))&7;
        if(type==0)break;
        /* CLIDR: 1 instruction, 2 data, 3 separate, 4 unified. */
        if(type>4)continue;
        for(unsigned instruction=0;instruction<2;++instruction){
            if(instruction ? !(type==1||type==3) : type==1)continue;
            ops->select(cookie,(level<<1)|instruction);ops->isb(cookie);
            struct ss_cpu_cache_observation *c=&r.cache[r.cache_count++];
            c->ccsidr=ops->read(cookie,SS_CPU_REG_CCSIDR);c->level=level+1;
            c->kind=instruction?1:type==4?4:2;
            if(r.ccidx<=1){
                c->line_bytes=1u<<((c->ccsidr&7)+4);
                c->ways=(uint32_t)((c->ccsidr>>3)&(r.ccidx?0x1fffff:0x3ff))+1;
                c->sets=(uint32_t)((c->ccsidr>>(r.ccidx?32:13))&(r.ccidx?0xffffff:0x7fff))+1;
                c->bytes=(uint64_t)c->line_bytes*c->ways*c->sets;
            }
        }
    }
    ops->select(cookie,saved);ops->isb(cookie);*out=r;return 1;
}
#ifdef __aarch64__
static uint64_t native_read(void *unused,enum ss_cpu_observation_register reg)
{
    (void)unused;uint64_t value=0;
#define READ(name) __asm__ volatile("mrs %0," #name : "=r"(value));break
    switch(reg){
    case SS_CPU_REG_MPIDR:READ(mpidr_el1);
    case SS_CPU_REG_MIDR:READ(midr_el1);
    case SS_CPU_REG_ISAR0:READ(id_aa64isar0_el1);
    case SS_CPU_REG_MMFR2:READ(id_aa64mmfr2_el1);
    case SS_CPU_REG_CLIDR:READ(clidr_el1);
    case SS_CPU_REG_CSSELR:READ(csselr_el1);
    case SS_CPU_REG_CCSIDR:READ(ccsidr_el1);
    }
#undef READ
    return value;
}
static void native_select(void *unused,uint64_t value)
{(void)unused;__asm__ volatile("msr csselr_el1,%0"::"r"(value):"memory");}
static void native_isb(void *unused)
{(void)unused;__asm__ volatile("isb":::"memory");}
#endif
void ss_cpu_observe_boot(unsigned core)
{
#ifdef __aarch64__
    static const struct ss_cpu_observation_ops ops={native_read,native_select,native_isb};
    if(core<SS_CPU_MAX&&ss_cpu_observation_capture(core,&ops,NULL,&snapshots[core]))
        atomic_store_explicit(&valid[core],1,memory_order_release);
#ifdef SUNSTONE_A733_HW_FACTS
    /* The hardware facts probe (docs/hardware/a733-facts-probe.md): the same EL1 point, before any task. */
    {extern void hwf_capture(unsigned);hwf_capture(core);}
#endif
#else
    (void)core;
#endif
}
int ss_cpu_observation_copy(unsigned core,struct ss_cpu_observation *out)
{
    if(core>=SS_CPU_MAX||!out||!atomic_load_explicit(&valid[core],memory_order_acquire))return 0;
    *out=snapshots[core];return 1;
}
