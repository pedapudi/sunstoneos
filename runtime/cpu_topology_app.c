/* SPDX-License-Identifier: GPL-2.0-only */
#include "sunstone_cpu_topology.h"
#include "sunstone_cpu_observation.h"
#include <stdatomic.h>

#ifdef SUNSTONE_A733_CPU_TOPOLOGY
static atomic_int snapshot_state;
static uint64_t observed_mask(const struct ss_cpu_topology *t)
{
    uint64_t mask=0;
    for(unsigned c=0;c<t->cpu_count;++c){
        struct ss_cpu_observation observation;
        if(ss_cpu_observation_copy(c,&observation))mask|=UINT64_C(1)<<c;
    }
    return mask;
}
/* Validate immutable data once when first queried. Concurrent first readers
 * reach the same result; no shared lock or initialization task is needed. */
static const struct ss_cpu_topology *snapshot(void)
{
    const struct ss_cpu_topology *t=a733_cpu_topology();
    int state=atomic_load_explicit(&snapshot_state,memory_order_acquire);
    if(state==1)return t;
    if(state==2||ss_cpu_topology_validate(t)!=SS_TOPOLOGY_OK)return 0;
    for(unsigned c=0;c<t->cpu_count;++c){
        if(!(t->policy->admitted&(UINT64_C(1)<<c)))continue;
        struct ss_cpu_observation observation;
        unsigned logical;
        if(!ss_cpu_observation_copy(c,&observation))return 0;
        if(!(observation.flags&SS_CPU_OBS_IDENTITY)||
           ss_cpu_from_mpidr(t,observation.mpidr,&logical)!=SS_TOPOLOGY_OK||logical!=c||
           ((observation.midr^t->cpus[c].midr)&UINT64_C(0xff0ffff0))){
            atomic_store_explicit(&snapshot_state,2,memory_order_release);return 0;
        }
    }
    atomic_store_explicit(&snapshot_state,1,memory_order_release);return t;
}
#endif

int ss_cpu_topology_info(struct ss_cpu_topology_info *out)
{
    if(!out)return SS_TOPOLOGY_INVALID;
    *out=(struct ss_cpu_topology_info){0};
#ifdef SUNSTONE_A733_CPU_TOPOLOGY
    const struct ss_cpu_topology *t=snapshot();
    if(!t)return SS_TOPOLOGY_INVALID;
    *out=(struct ss_cpu_topology_info){.version=t->version,.cpu_count=(uint32_t)t->cpu_count,
        .cache_count=(uint32_t)t->cache_count,.domain_count=(uint32_t)t->domain_count,
        .calibration_epoch=t->calibration_epoch,.operating_epoch=t->operating_epoch,
        .admitted=t->policy->admitted,.reserved=t->policy->reserved,.irq=t->policy->irq,
        .observed=observed_mask(t)};
    return SS_TOPOLOGY_OK;
#else
    return SS_TOPOLOGY_UNCALIBRATED;
#endif
}
int ss_cpu_topology(struct ss_cpu_info *out,size_t capacity,size_t *count)
{
    if(!count)return SS_TOPOLOGY_INVALID;
    *count=0;
#ifdef SUNSTONE_A733_CPU_TOPOLOGY
    const struct ss_cpu_topology *t=snapshot();
    return t?ss_cpu_topology_query(t,out,capacity,count):SS_TOPOLOGY_INVALID;
#else
    (void)out;(void)capacity;return SS_TOPOLOGY_UNCALIBRATED;
#endif
}
int ss_work_split(uint64_t units,uint64_t mask,uint32_t quantum,
                  struct ss_work_range *out,size_t capacity,size_t *count)
{
    if(!count)return SS_TOPOLOGY_INVALID;
    *count=0;
#ifdef SUNSTONE_A733_CPU_TOPOLOGY
    const struct ss_cpu_topology *t=snapshot();
    return t?ss_cpu_work_split(t,units,mask,quantum,out,capacity,count):SS_TOPOLOGY_INVALID;
#else
    (void)units;(void)mask;(void)quantum;(void)out;(void)capacity;
    return SS_TOPOLOGY_UNCALIBRATED;
#endif
}
int ss_cpu_observations(unsigned core,struct ss_cpu_observation *out)
{
#ifdef SUNSTONE_A733_CPU_TOPOLOGY
    return ss_cpu_observation_copy(core,out);
#else
    (void)core;(void)out;return 0;
#endif
}
