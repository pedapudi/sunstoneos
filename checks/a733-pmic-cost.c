/* SPDX-License-Identifier: Apache-2.0 */
/* Count the work of one PMIC snapshot and one selector write through the
 * actual provider: worker polls, register reads and writes, the reads that are
 * not part of a bus transaction (setup reads), time reads (the now op) and I2C
 * bus acquisitions. In production the native adapter answers a time read from
 * the architectural counter with no lock, and queries the kernel's mutex
 * holder once per bus acquisition and never per register access, so the
 * acquisitions also count those queries; checks/pmic-tree.py keeps both
 * properties in the adapter's source. The model clock advances 1 us per time
 * read and each wire event completes 90 us after the write that starts it, as
 * in checks/a733-pmic-polls.c. The output is one JSON object per request;
 * --limits also asserts the budgets below.
 * Build with checks/a733-pmic.c's provider, rail, clock, GPU and pin units. */
#define PMIC_MODEL_NO_MAIN
#include "a733-pmic.c"
static struct pmic_model m;
struct count { unsigned polls,reads_at[8],other_reads,writes,time_reads,acquisitions; };
static struct count c;
static int counted_read(void *v,uintptr_t a,uint32_t *out)
{
    if(a>=0x07083008 && a<=0x07083020 && !(a&3))++c.reads_at[(a-0x07083008)/4];else ++c.other_reads;
    return pmic_model_read(v,a,out);
}
static int counted_write(void *v,uintptr_t a,uint32_t value){++c.writes;return pmic_model_write(v,a,value);}
static int counted_now(void *v,uint64_t *out){++c.time_reads;return pmic_model_now(v,out);}
static int counted_enter(void *v,uint64_t d){++c.acquisitions;return pmic_model_enter(v,d);}
/* Run one request from submit to publication and print its counts. */
static struct count request(const char *name,enum a733_rail rail,unsigned write,unsigned transactions)
{
    struct a733_rail_token t={0};
    const a733_rail_mask_t bit=A733_RAIL_BIT(rail);
    assert(a733_rail_acquire(&m.rails,10,write ? 0 : bit,write ? bit : 0,m.now+1000000,&t)==A733_RAIL_PENDING);
    for(unsigned i=0;i<16 && a733_rail_poll(&m.rails,&t)!=A733_RAIL_READY;++i){}
    struct a733_pmic_request q;struct a733_pmic_snapshot s;
    c=(struct count){0};
    assert((write ? a733_pmic_submit_write(&m.provider,10,&t,rail,0,30,m.now+1000000,&q) :
        a733_pmic_submit(&m.provider,10,&t,rail,m.now+1000000,&q))==A733_PMIC_PENDING);
    while(a733_pmic_result(&m.provider,&q,&s)==A733_PMIC_PENDING) {
        assert(++c.polls<1000 && pmic_model_step(&m)!=A733_PMIC_RETAINED);
        m.now+=1000;
    }
    assert(a733_pmic_result(&m.provider,&q,&s)==A733_PMIC_COMPLETE && s.enabled && s.decoded);
    assert(s.programmed_uv==800000u);
    assert(a733_pmic_reap(&m.provider,&q)==A733_PMIC_COMPLETE);
    assert(a733_rail_release(&m.rails,&t,m.now+1000000)==A733_RAIL_READY);
    /* Bus-idle reads the extended status word (+0x20) with control and status. */
    const unsigned idle=c.reads_at[6],setup=c.other_reads+c.reads_at[(0x07083014-0x07083008)/4]+
        c.reads_at[(0x07083018-0x07083008)/4]+c.reads_at[(0x0708301c-0x07083008)/4]+3*idle;
    const unsigned transfer_reads=c.reads_at[0]+c.reads_at[1]+c.reads_at[2]-2*idle;
    printf("{\"request\":\"%s\",\"polls\":%u,\"transactions\":%u,\"writes\":%u,\"transfer_reads\":%u,"
        "\"setup_reads\":%u,\"bus_idle_checks\":%u,\"time_reads\":%u,\"bus_acquisitions\":%u}\n",
        name,c.polls,transactions,c.writes,transfer_reads,setup,idle,c.time_reads,c.acquisitions);
    return c;
}
/* Budgets: time reads only around register accesses, one per poll, one at
 * submit and two at publication; one bus acquisition, and so one
 * mutex-holder query, per request. */
static void limits(struct count k)
{
    unsigned accesses=k.other_reads+k.writes;
    for(unsigned i=0;i<8;++i)accesses+=k.reads_at[i];
    assert(k.time_reads<=2*accesses+k.polls+3);
    assert(k.acquisitions==1);
}
int main(int argc,char **argv)
{
    const int check=argc==2 && !strcmp(argv[1],"--limits");
    assert(argc==1 || check);
    pmic_model_setup(&m);
    for(unsigned i=0;i<64 && a733_pmic_live(&m.provider,&m.lifetime)!=A733_PMIC_READY;++i)
        assert(pmic_model_step(&m)!=A733_PMIC_RETAINED);
    assert(a733_pmic_live(&m.provider,&m.lifetime)==A733_PMIC_READY);
    m.provider.ops.read=counted_read;m.provider.ops.write=counted_write;
    m.provider.ops.now=counted_now;m.provider.ops.enter=counted_enter;
    m.now_step=1;m.wire_us=90;
    /* DCDC3 starts at selector 0; the write programs 800 mV (selector 30). */
    const struct count snapshot=request("DCDC2 snapshot",A733_RAIL_DCDC2,0,6);
    const struct count write=request("DCDC3 selector write",A733_RAIL_DCDC3,1,8);
    if(check){limits(snapshot);limits(write);puts("PASS PMIC cost budgets");}
    return 0;
}
