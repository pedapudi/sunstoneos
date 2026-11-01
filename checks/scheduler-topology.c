/* SPDX-License-Identifier: GPL-2.0-only */
#include "sunstone_cpu_topology.h"
#include "sunstone_board.h"
#include "a733_cpu_clock.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
/* The real clock object is linked with section garbage collection; only its
 * read-only OPP function and tables survive. */
static struct ss_cpu_info cpus[64];
static struct ss_cpu_domain domains[3];
static struct ss_cpu_cache caches[2];
static struct ss_cpu_policy policy;
static struct ss_cpu_topology fixture(unsigned n,unsigned groups,int smt)
{
    memset(cpus,0,sizeof(cpus)); memset(domains,0,sizeof(domains));
    policy=(struct ss_cpu_policy){.admitted=n==64?UINT64_MAX:(UINT64_C(1)<<n)-1};
    policy.application=policy.irq=policy.service=policy.admitted;
    for(unsigned i=0;i<n;i++) {
        unsigned d=i%groups;
        cpus[i]=(struct ss_cpu_info){.logical_id=i,.affinity=((uint64_t)(i/4)<<32)|((i%4)<<16)|3,
            .physical_core=smt?i/2:i,.siblings=smt?(UINT64_C(3)<<(i/2*2)):(UINT64_C(1)<<i),
            .cluster=d,.performance_domain=d,.capacity_max=1024,.capacity_effective=128*(d+1),
            .flags=3,.l1i=SS_ID_NONE,.l1d=SS_ID_NONE,.l2=SS_ID_NONE,.l3=0};
        domains[d].cpus|=UINT64_C(1)<<i;
    }
    caches[0]=(struct ss_cpu_cache){policy.admitted,1048576,64,3,SS_CACHE_UNIFIED,SS_ID_NONE,SS_ID_NONE,SS_ID_NONE};
    return (struct ss_cpu_topology){.version=1,.calibration_epoch=1,.operating_epoch=2,
        .calibration_source="synthetic fixture",.capacity_workload="fixed test weights",
        .cpus=cpus,.domains=domains,.caches=caches,.cpu_count=n,.domain_count=groups,
        .cache_count=1,.policy=&policy};
}
static void conservation(struct ss_cpu_topology *t,uint64_t units,uint32_t quantum)
{
    struct ss_work_range out[64]; size_t count=0;
    assert(ss_cpu_work_split(t,units,policy.application,quantum,out,64,&count)==0);
    assert(count==t->cpu_count);uint64_t end=0;
    for(size_t i=0;i<count;i++) {
        assert(out[i].cpu==i && out[i].reserved==0 && out[i].begin==end && out[i].end>=end);
        if(i+1<count) assert((out[i].end-out[i].begin)%quantum==0);
        end=out[i].end;
    }
    assert(end==units);
}
int main(void)
{
    const struct ss_cpu_topology *a=a733_cpu_topology();
    assert(ss_cpu_topology_validate(a)==0 && a->cpu_count==8);
    assert(a->policy==sunstone_board_orangepi_zero3w()->cpu_policy);
    unsigned cpu=99,n=0;
    for(unsigned i=0;i<8;i++) {
        assert(ss_cpu_from_mpidr(a,(UINT64_C(1)<<31)|((uint64_t)i<<8),&cpu)==0 && cpu==i);
        assert(a->cpus[i].capacity_max==0);
    }
    assert(ss_cpu_from_mpidr(a,1,&cpu)==SS_TOPOLOGY_INVALID);
    assert(a733_cpu_topology_opps(0,&n)==a733_cpu_clock_opps(A733_CPU_LITTLE,&cpu) && n==cpu);
    assert(a733_cpu_topology_opps(2,&n)==a733_cpu_clock_opps(A733_CPU_DSU,&cpu));
    struct ss_work_range out[64],saved[64];size_t count;
    memset(out,0xa5,sizeof(out));memcpy(saved,out,sizeof(out));
    assert(ss_cpu_work_split(a,100,255,1,out,64,&count)==SS_TOPOLOGY_UNCALIBRATED);
    assert(memcmp(saved,out,sizeof(out))==0);
    struct ss_cpu_info copied[64];
    assert(ss_cpu_topology_query(a,copied,7,&count)==SS_TOPOLOGY_SPACE && count==8);
    assert(ss_cpu_topology_query(a,copied,64,&count)==0 && copied[6].midr==0x414fd0b1);
    for(unsigned groups=1;groups<=3;groups++) {
        struct ss_cpu_topology t=fixture(12,groups,0);
        assert(ss_cpu_topology_validate(&t)==0);
        for(uint64_t units=0;units<100;units++) for(unsigned q=1;q<17;q++) conservation(&t,units,q);
        conservation(&t,UINT64_MAX,1);conservation(&t,UINT64_MAX,UINT32_MAX);
        assert(ss_cpu_from_mpidr(&t,cpus[7].affinity,&cpu)==0 && cpu==7);
        assert(ss_cpu_work_split(&t,1,policy.application,1,out,1,&count)==SS_TOPOLOGY_SPACE && count==12);
        assert(memcmp(saved,out,sizeof(out))==0);
        domains[0].cpus|=UINT64_C(1)<<63;
        assert(ss_cpu_topology_validate(&t)==SS_TOPOLOGY_INVALID);
    }
    struct ss_cpu_topology t=fixture(3,3,0);
    assert(ss_cpu_work_split(&t,60,7,1,out,3,&count)==0);
    assert(out[0].end==10 && out[1].end==30 && out[2].end==60);
    t=fixture(4,1,0);
    assert(ss_cpu_work_split(&t,5,15,2,out,4,&count)==0);
    assert(out[0].end==2 && out[1].end==4 && out[2].end==4 && out[3].end==5);
    caches[0].cpus=1; assert(ss_cpu_topology_validate(&t)==SS_TOPOLOGY_INVALID);
    t=fixture(4,1,0);cpus[1].affinity=cpus[0].affinity;
    assert(ss_cpu_topology_validate(&t)==SS_TOPOLOGY_INVALID);
    t=fixture(4,1,0);cpus[0].l2=0;
    assert(ss_cpu_topology_validate(&t)==SS_TOPOLOGY_INVALID);
    t=fixture(4,1,0);caches[0].line_bytes=63;
    assert(ss_cpu_topology_validate(&t)==SS_TOPOLOGY_INVALID);
    t=fixture(4,1,0);cpus[0].performance_domain=1;
    assert(ss_cpu_topology_validate(&t)==SS_TOPOLOGY_INVALID);
    t=fixture(4,1,0);cpus[0].capacity_effective=1025;
    assert(ss_cpu_topology_validate(&t)==SS_TOPOLOGY_INVALID);
    assert(ss_cpu_work_split(&t,UINT64_MAX,15,1,out,64,&count)==SS_TOPOLOGY_INVALID);
    t=fixture(4,1,0);t.operating_epoch=0;
    assert(ss_cpu_topology_validate(&t)==SS_TOPOLOGY_INVALID);
    t=fixture(4,1,0);cpus[0].flags=0;cpus[0].capacity_max=cpus[0].capacity_effective=0;
    assert(ss_cpu_topology_validate(&t)==0);
    assert(ss_cpu_work_split(&t,1,15,1,out,64,&count)==SS_TOPOLOGY_UNCALIBRATED);
    t=fixture(4,2,1);assert(ss_cpu_topology_validate(&t)==SS_TOPOLOGY_INVALID);
    t=fixture(4,1,1);cpus[1].cluster=1;
    assert(ss_cpu_topology_validate(&t)==SS_TOPOLOGY_INVALID);
    t=fixture(4,1,1);cpus[1].midr=0x414fd0b1;
    assert(ss_cpu_topology_validate(&t)==SS_TOPOLOGY_INVALID);
    t=fixture(4,1,1);cpus[1].l3=SS_ID_NONE;
    assert(ss_cpu_topology_validate(&t)==SS_TOPOLOGY_INVALID);
    t=fixture(4,1,1);assert(ss_cpu_topology_validate(&t)==0);
    assert(ss_cpu_work_split(&t,100,15,1,out,4,&count)==SS_TOPOLOGY_UNCALIBRATED);
    assert(ss_cpu_work_split(&t,100,5,1,out,4,&count)==0);
    cpus[0].siblings=1;assert(ss_cpu_topology_validate(&t)==SS_TOPOLOGY_INVALID);
    t=fixture(64,3,0);assert(ss_cpu_topology_validate(&t)==0);conservation(&t,UINT64_MAX,17);
    policy.reserved=1;policy.application&=~UINT64_C(1);policy.irq&=~UINT64_C(1);policy.service&=~UINT64_C(1);
    assert(ss_cpu_topology_validate(&t)==0);
    assert(ss_cpu_work_split(&t,100,1,1,out,64,&count)==SS_TOPOLOGY_INVALID);
    assert(ss_cpu_work_split(&t,100,0,1,out,64,&count)==SS_TOPOLOGY_INVALID);
    assert(ss_cpu_work_split(&t,100,2,0,out,64,&count)==SS_TOPOLOGY_INVALID);
    puts("scheduler topology: identity, domains, caches, unknown capacity, SMT and integer ranges passed");
}
