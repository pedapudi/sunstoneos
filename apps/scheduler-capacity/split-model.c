/* SPDX-License-Identifier: GPL-2.0-only */
#include "split-result.h"
#include <stdio.h>
#include <string.h>
int main(void)
{
    static struct split_result result;
    static uint32_t data[CAPACITY_WORDS];
    uint64_t digest=capacity_integer(data);
    result.magic=SPLIT_MAGIC;result.counter_hz=24000000;
    result.units=SPLIT_UNITS;result.warm=SPLIT_WARM;result.reps=SPLIT_REPS;result.cores=CAPACITY_CORES;
    struct ss_cpu_info cpus[CAPACITY_CORES]={0};
    struct ss_cpu_policy policy={.application=255};
    struct ss_cpu_topology view={.cpus=cpus,.cpu_count=CAPACITY_CORES,.policy=&policy};
    for(unsigned c=0;c<CAPACITY_CORES;++c){
        result.capacity[c]=c<6?285:1024;
        cpus[c].capacity_effective=result.capacity[c];cpus[c].flags=SS_CPU_CAPACITY_EFFECTIVE_VALID;
        cpus[c].siblings=UINT64_C(1)<<c;
    }
    snprintf(result.clock_before,256,"CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 transitions=3 dsu_mhz=1274 sid=00000495 bin=4 core=6 pattern=0 parked_max_us=184\n");
    memcpy(result.clock_after,result.clock_before,256);
    struct ss_work_range ranges[CAPACITY_CORES];size_t count;
    if(ss_cpu_work_split(&view,SPLIT_UNITS,255,1,ranges,CAPACITY_CORES,&count)||count!=CAPACITY_CORES)return 1;
    for(unsigned mode=0;mode<2;++mode)for(unsigned rep=0;rep<SPLIT_REPS;++rep){
        struct split_phase *p=&result.samples[mode][rep];p->launch=100000;p->readiness=3;
        for(unsigned c=0;c<CAPACITY_CORES;++c){
            struct split_core *s=&p->core[c];s->observed_core=c;
            s->begin=mode?ranges[c].begin:SPLIT_UNITS*c/CAPACITY_CORES;
            s->end=mode?ranges[c].end:SPLIT_UNITS*(c+1)/CAPACITY_CORES;
            s->started=p->launch+c;
            s->finished=s->started+(s->end-s->begin)*(c<6?36:10)+rep;
            for(uint64_t tile=s->begin;tile<s->end;++tile)s->digest+=digest^tile;
        }
    }
    return fwrite(&result,sizeof(result),1,stdout)!=1;
}
