/* SPDX-License-Identifier: Apache-2.0 */
/* Count actual-provider polls per PMIC snapshot when each wire event takes
 * modeled bus time and the provider task sleeps one 1 ms tick between polls.
 * A concurrent a733_pmic_live() consumer runs at every bus access and between
 * polls; the check bounds how long each poll and each snapshot keep the
 * provider BUSY to it. Arguments: poll limit, longest-poll and total BUSY
 * limits in microseconds.
 * Build with checks/a733-pmic.c's provider, rail, clock, GPU and pin units. */
#define PMIC_MODEL_NO_MAIN
#include "a733-pmic.c"
#include <stdlib.h>
static struct pmic_model m;
struct busy { uint64_t held,longest;unsigned accesses,observed; };
static unsigned snapshot_polls(enum a733_rail rail,unsigned owner,unsigned expected_uv,struct busy *b)
{
    struct a733_rail_token t={0};
    assert(a733_rail_acquire(&m.rails,owner,A733_RAIL_BIT(rail),0,m.now+1000000,&t)==A733_RAIL_PENDING);
    for(unsigned i=0;i<16 && a733_rail_poll(&m.rails,&t)!=A733_RAIL_READY;++i){}
    struct a733_pmic_request q;struct a733_pmic_snapshot s;
    assert(a733_pmic_submit(&m.provider,owner,&t,rail,m.now+1000000,&q)==A733_PMIC_PENDING);
    unsigned polls=0;
    *b=(struct busy){0};m.probe_live=1;m.live_busy=0;
    while(a733_pmic_result(&m.provider,&q,&s)==A733_PMIC_PENDING) {
        uint64_t start=m.now;unsigned accesses=m.reads+m.writes;
        assert(++polls<1000 && pmic_model_step(&m)!=A733_PMIC_RETAINED);
        uint64_t held=m.now-start;accesses=m.reads+m.writes-accesses;
        b->held+=held;if(held>b->longest)b->longest=held;
        if(accesses>b->accesses)b->accesses=accesses;
        /* Between polls the consumer always sees the provider. */
        assert(a733_pmic_live(&m.provider,&m.lifetime)==A733_PMIC_READY);
        m.now+=1000;
    }
    m.probe_live=0;b->observed=m.live_busy;
    assert(a733_pmic_result(&m.provider,&q,&s)==A733_PMIC_COMPLETE);
    assert(s.rail==rail && s.enabled && s.decoded && s.programmed_uv==expected_uv);
    assert(s.chip==3 && !s.page_before && !s.page_after && !s.revision && !m.i2c_held && !m.retained);
    assert(a733_pmic_reap(&m.provider,&q)==A733_PMIC_COMPLETE);
    return polls;
}
int main(int argc,char **argv)
{
    unsigned limit=argc>1?(unsigned)strtoul(argv[1],NULL,10):48;
    uint64_t longest_limit=argc>2?strtoull(argv[2],NULL,10):64;
    uint64_t total_limit=argc>3?strtoull(argv[3],NULL,10):1000;
    pmic_model_setup(&m);
    for(unsigned i=0;i<16 && a733_pmic_live(&m.provider,&m.lifetime)!=A733_PMIC_READY;++i)
        assert(pmic_model_step(&m)!=A733_PMIC_RETAINED);
    /* 100 kHz: a START or a byte plus acknowledgment takes about 90 us. Each
     * time read advances the model clock by 1 us. */
    m.now_step=1;m.wire_us=90;
    const enum a733_rail rails[]={A733_RAIL_DCDC1,A733_RAIL_DCDC2};
    const unsigned uv[]={3300000,800000};
    for(unsigned i=0;i<2;++i) {
        struct busy b;unsigned polls=snapshot_polls(rails[i],10+i,uv[i],&b);
        printf("PMIC rail %u: %u polls (limit %u); BUSY %llu us in total (limit %llu), "
            "%llu us and %u accesses in the longest poll (limit %llu us); %u BUSY live() observations\n",
            (unsigned)rails[i],polls,limit,(unsigned long long)b.held,(unsigned long long)total_limit,
            (unsigned long long)b.longest,b.accesses,(unsigned long long)longest_limit,b.observed);
        fflush(stdout);
        /* The consumer did run while the provider was busy. */
        assert(b.observed && polls<=limit && b.longest<=longest_limit && b.held<=total_limit);
    }
    puts("PASS PMIC snapshot poll count and bounded BUSY with modeled bus time");
    return 0;
}
