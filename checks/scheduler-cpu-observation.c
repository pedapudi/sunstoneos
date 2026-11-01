/* SPDX-License-Identifier: GPL-2.0-only */
#include "sunstone_cpu_observation.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct model {uint64_t registers[7],selected,pending,caches[SS_CPU_OBSERVATION_CACHES];unsigned writes,barriers,cache_reads;};
static uint64_t read_reg(void *p,enum ss_cpu_observation_register r)
{struct model *m=p;if(r==SS_CPU_REG_CCSIDR){++m->cache_reads;assert(m->selected<SS_CPU_OBSERVATION_CACHES);return m->caches[m->selected];}return m->registers[r];}
static void select_reg(void *p,uint64_t selector)
{struct model *m=p;m->pending=selector;++m->writes;}
static void isb(void *p)
{struct model *m=p;m->selected=m->pending;++m->barriers;}
static const struct ss_cpu_observation_ops ops={read_reg,select_reg,isb};
static uint64_t geometry(unsigned ccidx,unsigned ways,unsigned sets)
{return UINT64_C(2)|((uint64_t)(ways-1)<<3)|((uint64_t)(sets-1)<<(ccidx?32:13));}
int main(void)
{
    for(unsigned ccidx=0;ccidx<2;++ccidx){
        struct model m={0};m.registers[SS_CPU_REG_CSSELR]=5;m.selected=m.pending=5;
        m.registers[SS_CPU_REG_MPIDR]=UINT64_C(0x80000600);m.registers[SS_CPU_REG_MIDR]=0x410fd0b0;
        m.registers[SS_CPU_REG_ISAR0]=UINT64_C(2)<<20;m.registers[SS_CPU_REG_MMFR2]=(uint64_t)ccidx<<20;
        m.registers[SS_CPU_REG_CLIDR]=3|(4<<3)|(4<<6);
        m.caches[0]=geometry(ccidx,4,128);m.caches[1]=geometry(ccidx,4,256);
        m.caches[2]=geometry(ccidx,8,1024);m.caches[4]=geometry(ccidx,16,4096);
        struct ss_cpu_observation r;
        assert(ss_cpu_observation_capture(6,&ops,&m,&r));
        assert(r.cache_count==4&&r.flags==15&&r.core==6&&r.mpidr==UINT64_C(0x80000600));
        const uint64_t sizes[]={32768,65536,524288,4194304};
        for(unsigned i=0;i<4;++i)assert(r.cache[i].bytes==sizes[i]);
        assert(m.selected==5&&m.writes==5&&m.barriers==5&&m.cache_reads==4);
        m.registers[SS_CPU_REG_ISAR0]=UINT64_C(1)<<20;m.registers[SS_CPU_REG_MMFR2]=UINT64_C(15)<<20;
        assert(ss_cpu_observation_capture(6,&ops,&m,&r));assert(r.flags==SS_CPU_OBS_IDENTITY);
        assert(r.cache[0].ccsidr==m.caches[0]&&!r.cache[0].bytes&&m.selected==5);
        unsigned writes=m.writes;assert(!ss_cpu_observation_capture(SS_CPU_MAX,&ops,&m,&r));assert(m.writes==writes);
    }
    struct model full={0};
    full.registers[SS_CPU_REG_CSSELR]=13;full.selected=full.pending=13;
    for(unsigned level=0;level<7;++level)full.registers[SS_CPU_REG_CLIDR]|=UINT64_C(3)<<(level*3);
    for(unsigned i=0;i<SS_CPU_OBSERVATION_CACHES;++i)full.caches[i]=geometry(0,2,i+1);
    struct ss_cpu_observation all;
    assert(ss_cpu_observation_capture(SS_CPU_MAX-1,&ops,&full,&all));
    assert(all.core==63&&all.cache_count==14&&full.cache_reads==14);
    assert(full.writes==15&&full.barriers==15&&full.selected==13);
    for(unsigned i=0;i<14;++i){
        assert(all.cache[i].level==i/2+1);
        assert(all.cache[i].kind==(i%2?1u:2u));
        assert(all.cache[i].bytes==128u*(i+1));
    }
    assert(!ss_cpu_observation_copy(SS_CPU_MAX,NULL));assert(!ss_cpu_observation_copy(0,NULL));
    puts("CPU observations: legacy and CCIDX geometry, ISB selector completion, restore, unknown features, core 63, all seven levels/fourteen caches and invalid core 64 pass");
}
