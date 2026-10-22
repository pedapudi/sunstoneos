/* SPDX-License-Identifier: Apache-2.0 */
/* Count the bus work of one PMIC snapshot through the actual provider: worker
 * polls, register reads and writes by address, and the reads that are not
 * part of a bus transaction (setup reads). The model clock advances 1 us per
 * time read and each wire event completes 90 us after the write that starts
 * it, as in checks/a733-pmic-polls.c. The output is one JSON object, so the
 * same file measures the merge base and the current tree.
 * Build with checks/a733-pmic.c's provider, rail, clock, GPU and pin units. */
#define PMIC_MODEL_NO_MAIN
#include "a733-pmic.c"
static struct pmic_model m;
static unsigned reads_at[8],other_reads,writes_total;
static int counted_read(void *v,uintptr_t a,uint32_t *out)
{
    if(a>=0x07083008 && a<=0x07083020 && !(a&3))++reads_at[(a-0x07083008)/4];else ++other_reads;
    return pmic_model_read(v,a,out);
}
static int counted_write(void *v,uintptr_t a,uint32_t value){++writes_total;return pmic_model_write(v,a,value);}
int main(void)
{
    pmic_model_setup(&m);
    for(unsigned i=0;i<64 && a733_pmic_live(&m.provider,&m.lifetime)!=A733_PMIC_READY;++i)
        assert(pmic_model_step(&m)!=A733_PMIC_RETAINED);
    assert(a733_pmic_live(&m.provider,&m.lifetime)==A733_PMIC_READY);
    m.provider.ops.read=counted_read;m.provider.ops.write=counted_write;
    m.now_step=1;m.wire_us=90;
    struct a733_rail_token t={0};
    assert(a733_rail_acquire(&m.rails,10,A733_RAIL_BIT(A733_RAIL_DCDC2),0,m.now+1000000,&t)==A733_RAIL_PENDING);
    for(unsigned i=0;i<16 && a733_rail_poll(&m.rails,&t)!=A733_RAIL_READY;++i){}
    struct a733_pmic_request q;struct a733_pmic_snapshot s;
    assert(a733_pmic_submit(&m.provider,10,&t,A733_RAIL_DCDC2,m.now+1000000,&q)==A733_PMIC_PENDING);
    unsigned polls=0;
    while(a733_pmic_result(&m.provider,&q,&s)==A733_PMIC_PENDING) {
        assert(++polls<1000 && pmic_model_step(&m)!=A733_PMIC_RETAINED);
        m.now+=1000;
    }
    assert(a733_pmic_result(&m.provider,&q,&s)==A733_PMIC_COMPLETE && s.programmed_uv==800000);
    assert(a733_pmic_reap(&m.provider,&q)==A733_PMIC_COMPLETE);
    /* Bus-idle reads the extended status word (+0x20) with control and status. */
    const unsigned idle=reads_at[6],setup=other_reads+reads_at[(0x07083014-0x07083008)/4]+
        reads_at[(0x07083018-0x07083008)/4]+reads_at[(0x0708301c-0x07083008)/4]+3*idle;
    const unsigned transfer_reads=reads_at[0]+reads_at[1]+reads_at[2]-2*idle;
    printf("{\"polls\":%u,\"ticks\":%u,\"transactions\":6,\"writes\":%u,\"transfer_reads\":%u,"
        "\"setup_reads\":%u,\"bus_idle_checks\":%u}\n",polls,polls,writes_total,transfer_reads,setup,idle);
    return 0;
}
