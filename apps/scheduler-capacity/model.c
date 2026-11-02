/* SPDX-License-Identifier: GPL-2.0-only */
/* Host-only producer exercises the native result layout and arithmetic.
 * Its durations and register values are synthetic and prove no board result. */
#include "result.h"
#include <stdio.h>
#include <string.h>
int main(void)
{
    static struct capacity_result r;
    static uint32_t data[CAPACITY_WORDS];
    const uint64_t integer=capacity_integer(data),neon=capacity_neon(data);
    if(integer!=neon)return 1;
    r.magic=CAPACITY_MAGIC;r.counter_hz=24000000;
    r.words=CAPACITY_WORDS;r.rounds=CAPACITY_ROUNDS;r.warm=CAPACITY_WARM;r.reps=CAPACITY_REPS;
    for(unsigned core=0;core<CAPACITY_CORES;++core){
        struct capacity_core *c=&r.core[core];c->observed_core=core;c->readiness=15;
        struct ss_cpu_observation *o=&c->observation;
        o->core=core;o->mpidr=UINT64_C(0x81000000)|(core<<8);
        o->midr=core<6?0x412fd050:0x414fd0b1;o->isar0=2<<20;
        o->flags=15;o->atomic=2;o->clidr=3|(4<<3)|(4<<6);o->cache_count=4;
        for(unsigned i=0;i<4;++i){
            struct ss_cpu_cache_observation *cache=&o->cache[i];
            cache->level=i<2?1:i;cache->kind=i==1?1:i>=2?4:2;
            cache->line_bytes=64;cache->ways=4;cache->sets=128;
            cache->bytes=32768;cache->ccsidr=2|(3<<3)|(127<<13);
        }
        snprintf(c->clock_before,sizeof(c->clock_before),"CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 transitions=3 dsu_mhz=1274 sid=12340400 bin=0 core=6 pattern=0 parked_max_us=40\n");
        memcpy(c->clock_after,c->clock_before,sizeof(c->clock_before));
        for(unsigned k=0;k<2;++k)for(unsigned j=0;j<CAPACITY_REPS;++j)
            c->samples[k][j]=(struct capacity_sample){1000+j,integer};
    }
    return fwrite(&r,sizeof(r),1,stdout)!=1;
}
