/* SPDX-License-Identifier: GPL-2.0-only */
#include "sunstone_app.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef SUNSTONE_A733_CPU_TOPOLOGY
const struct ss_cpu_topology *a733_cpu_topology_data(void);
static struct ss_cpu_topology fixture;
static struct ss_cpu_info cpus[SS_CPU_MAX];
static struct ss_cpu_observation observed[SS_CPU_MAX];
static uint64_t available;
const struct ss_cpu_topology *a733_cpu_topology(void) { return &fixture; }
int ss_cpu_observation_copy(unsigned core,struct ss_cpu_observation *out)
{
    if(core>=fixture.cpu_count||!out||!(available&(UINT64_C(1)<<core)))return 0;
    *out=observed[core];return 1;
}
static void prepare(void)
{
    fixture=*a733_cpu_topology_data();
    memcpy(cpus,fixture.cpus,fixture.cpu_count*sizeof(*cpus));fixture.cpus=cpus;
    for(unsigned c=0;c<fixture.cpu_count;++c)
        observed[c]=(struct ss_cpu_observation){.mpidr=UINT64_C(0x80000000)|cpus[c].affinity,
            .midr=cpus[c].midr,.core=c,.flags=SS_CPU_OBS_IDENTITY};
    available=fixture.policy->admitted;
}
#endif
int main(int argc,char **argv)
{
    assert(argc==2);
    struct ss_cpu_topology_info info;
    struct ss_cpu_info out[SS_CPU_MAX],saved[SS_CPU_MAX];
    struct ss_work_range ranges[SS_CPU_MAX],saved_ranges[SS_CPU_MAX];
    size_t count=99;
    memset(out,0xa5,sizeof(out));memcpy(saved,out,sizeof(out));
    memset(ranges,0xa5,sizeof(ranges));memcpy(saved_ranges,ranges,sizeof(ranges));
    assert(ss_cpu_topology_info(NULL)==SS_TOPOLOGY_INVALID);
    assert(ss_cpu_topology(out,SS_CPU_MAX,NULL)==SS_TOPOLOGY_INVALID);
    assert(ss_work_split(1,1,1,ranges,SS_CPU_MAX,NULL)==SS_TOPOLOGY_INVALID);
    assert(ss_work_split_max(1,1,1,ranges,SS_CPU_MAX,NULL)==SS_TOPOLOGY_INVALID);
#ifndef SUNSTONE_A733_CPU_TOPOLOGY
    (void)argv;
    memset(&info,0xa5,sizeof(info));
    assert(ss_cpu_topology_info(&info)==SS_TOPOLOGY_UNCALIBRATED);
    struct ss_cpu_topology_info zero={0};assert(!memcmp(&info,&zero,sizeof(info)));
    assert(ss_cpu_topology(out,SS_CPU_MAX,&count)==SS_TOPOLOGY_UNCALIBRATED&&count==0);
    assert(ss_work_split(1,1,1,ranges,SS_CPU_MAX,&count)==SS_TOPOLOGY_UNCALIBRATED&&count==0);
    assert(ss_work_split_max(1,1,1,ranges,SS_CPU_MAX,&count)==SS_TOPOLOGY_UNCALIBRATED&&count==0);
    assert(!ss_cpu_observations(0,NULL));
#else
    prepare();
    if(!strcmp(argv[1],"missing")){
        available&=~UINT64_C(0x80);
        assert(ss_cpu_topology_info(&info)==SS_TOPOLOGY_INVALID);
        assert(ss_cpu_topology(out,SS_CPU_MAX,&count)==SS_TOPOLOGY_INVALID&&count==0);
        available|=UINT64_C(0x80); /* Boot publication may complete later. */
    }else if(!strcmp(argv[1],"affinity")||!strcmp(argv[1],"midr")||!strcmp(argv[1],"identity")){
        if(!strcmp(argv[1],"affinity"))observed[7].mpidr=7;
        if(!strcmp(argv[1],"midr"))observed[7].midr=observed[0].midr;
        if(!strcmp(argv[1],"identity"))observed[7].flags=0;
        assert(ss_cpu_topology_info(&info)==SS_TOPOLOGY_INVALID);
        assert(ss_work_split(1,255,1,ranges,SS_CPU_MAX,&count)==SS_TOPOLOGY_INVALID&&count==0);
        assert(ss_work_split_max(1,255,1,ranges,SS_CPU_MAX,&count)==SS_TOPOLOGY_INVALID&&count==0);
        prepare(); /* A captured identity mismatch remains a boot fault. */
        assert(ss_cpu_topology(out,SS_CPU_MAX,&count)==SS_TOPOLOGY_INVALID&&count==0);
        goto unchanged;
    }else if(!strcmp(argv[1],"invalid")){
        fixture.version=0;
        assert(ss_cpu_topology_info(&info)==SS_TOPOLOGY_INVALID);
        goto unchanged;
    }else if(!strcmp(argv[1],"uncalibrated")){
        for(unsigned c=0;c<fixture.cpu_count;++c){
            cpus[c].capacity_max=0;cpus[c].flags=0;
        }
        fixture.calibration_epoch=0;
        assert(ss_work_split_max(256,255,1,ranges,SS_CPU_MAX,&count)==SS_TOPOLOGY_UNCALIBRATED);
        goto unchanged;
    }else if(!strcmp(argv[1],"calibrated")){
        fixture.calibration_epoch=1;fixture.operating_epoch=1;
        fixture.calibration_source="synthetic wrapper fixture";
        fixture.capacity_workload="synthetic integer weights";
        for(unsigned c=0;c<fixture.cpu_count;++c){
            cpus[c].capacity_max=c<6?512:1024;cpus[c].capacity_effective=128;
            cpus[c].flags=SS_CPU_CAPACITY_MAX_VALID|SS_CPU_CAPACITY_EFFECTIVE_VALID;
        }
        assert(ss_work_split(100,255,1,ranges,SS_CPU_MAX,&count)==SS_TOPOLOGY_OK&&count==8);
        for(unsigned c=0;c<8;++c)assert(ranges[c].end-ranges[c].begin==(c<4?13:12));
        assert(ss_work_split_max(100,255,1,ranges,SS_CPU_MAX,&count)==SS_TOPOLOGY_OK&&count==8);
        for(unsigned c=0;c<8;++c)assert(ranges[c].end-ranges[c].begin==(c<6?10:20));
        assert(ss_work_split(100,255,1,ranges,7,&count)==SS_TOPOLOGY_SPACE&&count==8);
        puts("calibrated wrapper split conserves work and uses capacity weights");return 0;
    }else if(!strcmp(argv[1],"revision")){
        observed[6].midr^=UINT64_C(0x00f0000f); /* Variant/revision are not part identity. */
    }
    assert(ss_cpu_topology_info(&info)==SS_TOPOLOGY_OK);
    assert(info.cpu_count==8&&info.cache_count==25&&info.domain_count==2);
    assert(info.admitted==255&&info.observed==255&&info.calibration_epoch==fixture.calibration_epoch);
    assert(ss_cpu_topology(out,7,&count)==SS_TOPOLOGY_SPACE&&count==8);
    assert(!memcmp(out,saved,sizeof(out)));
    assert(ss_cpu_topology(out,SS_CPU_MAX,&count)==SS_TOPOLOGY_OK&&count==8);
    assert(out[7].affinity==0x700&&out[7].midr==0x414fd0b1);
    assert(ss_cpu_topology(NULL,0,&count)==SS_TOPOLOGY_SPACE&&count==8);
    assert(ss_work_split(100,255,1,ranges,SS_CPU_MAX,&count)==SS_TOPOLOGY_UNCALIBRATED);
    assert(!memcmp(ranges,saved_ranges,sizeof(ranges)));
    assert(ss_work_split_max(256,255,1,ranges,7,&count)==SS_TOPOLOGY_SPACE&&count==8);
    assert(!memcmp(ranges,saved_ranges,sizeof(ranges)));
    assert(ss_work_split_max(256,UINT64_C(1)<<8,1,ranges,SS_CPU_MAX,&count)==SS_TOPOLOGY_INVALID);
    assert(!memcmp(ranges,saved_ranges,sizeof(ranges)));
    assert(ss_work_split_max(256,255,1,ranges,SS_CPU_MAX,&count)==SS_TOPOLOGY_OK&&count==8);
    const unsigned lengths[]={20,20,19,19,19,19,70,70};
    for(unsigned c=0;c<8;++c)assert(ranges[c].end-ranges[c].begin==lengths[c]);
    assert(ss_cpu_topology_info(&info)==SS_TOPOLOGY_OK&&!info.operating_epoch);
    struct ss_cpu_observation observation;
    assert(ss_cpu_observations(7,&observation)&&observation.mpidr==0x80000700);
    assert(!ss_cpu_observations(SS_CPU_MAX,&observation)&&!ss_cpu_observations(7,NULL));
    puts("enabled wrappers validate identity, copy topology and refuse uncalibrated splits");
    return 0;
unchanged:
#endif
    assert(!memcmp(out,saved,sizeof(out)));
    assert(!memcmp(ranges,saved_ranges,sizeof(ranges)));
    puts("unavailable or invalid topology preserves caller arrays");return 0;
}
