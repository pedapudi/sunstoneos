/* SPDX-License-Identifier: Apache-2.0 */
/* Count actual-provider polls per PMIC snapshot when each wire event takes
 * modeled bus time and the provider task sleeps one 1 ms tick between polls.
 * Build with checks/a733-pmic.c's provider, rail, clock, GPU and pin units. */
#define PMIC_MODEL_NO_MAIN
#include "a733-pmic.c"
#include <stdlib.h>
static struct pmic_model m;
static unsigned snapshot_polls(enum a733_rail rail,unsigned owner,unsigned expected_uv)
{
    struct a733_rail_token t={0};
    assert(a733_rail_acquire(&m.rails,owner,A733_RAIL_BIT(rail),0,m.now+1000000,&t)==A733_RAIL_PENDING);
    for(unsigned i=0;i<16 && a733_rail_poll(&m.rails,&t)!=A733_RAIL_READY;++i){}
    struct a733_pmic_request q;struct a733_pmic_snapshot s;
    assert(a733_pmic_submit(&m.provider,owner,&t,rail,m.now+1000000,&q)==A733_PMIC_PENDING);
    unsigned polls=0;
    while(a733_pmic_result(&m.provider,&q,&s)==A733_PMIC_PENDING) {
        assert(++polls<1000 && pmic_model_step(&m)!=A733_PMIC_RETAINED);
        m.now+=1000;
    }
    assert(a733_pmic_result(&m.provider,&q,&s)==A733_PMIC_COMPLETE);
    assert(s.rail==rail && s.enabled && s.decoded && s.programmed_uv==expected_uv);
    assert(s.chip==3 && !s.page_before && !s.page_after && !s.revision && !m.i2c_held && !m.retained);
    assert(a733_pmic_reap(&m.provider,&q)==A733_PMIC_COMPLETE);
    return polls;
}
int main(int argc,char **argv)
{
    unsigned limit=argc>1?(unsigned)strtoul(argv[1],NULL,10):12;
    pmic_model_setup(&m);
    for(unsigned i=0;i<16 && a733_pmic_live(&m.provider,&m.lifetime)!=A733_PMIC_READY;++i)
        assert(pmic_model_step(&m)!=A733_PMIC_RETAINED);
    /* 100 kHz: a START or a byte plus acknowledgment takes about 90 us. Each
     * time read advances the model clock by 1 us. */
    m.now_step=1;m.wire_us=90;
    unsigned dcdc1=snapshot_polls(A733_RAIL_DCDC1,10,3300000);
    unsigned dcdc2=snapshot_polls(A733_RAIL_DCDC2,11,800000);
    printf("PMIC polls per snapshot: DCDC1 %u, DCDC2 %u (limit %u)\n",dcdc1,dcdc2,limit);
    fflush(stdout);
    assert(dcdc1<=limit && dcdc2<=limit);
    puts("PASS PMIC snapshot poll count with modeled bus time");
    return 0;
}
