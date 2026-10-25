/* SPDX-License-Identifier: Apache-2.0 */
/* When the provider task admits a snapshot request, its pin check waits for
 * REGISTER_LOCK and its bus access waits for the I2C mutex. A consumer that
 * holds one of those locks and calls a733_pmic_live() must not see the
 * provider object BUSY because of those waits, or each side waits for the
 * other until a deadline. The lock callbacks here call live() while the
 * worker waits. The check also covers another caller holding the object when
 * the poll takes it back, and a client fault latched during the wait.
 * Build with checks/a733-pmic.c's provider, rail, clock, GPU and pin units. */
#define PMIC_MODEL_NO_MAIN
#include "a733-pmic.c"
static struct pmic_model m;
static int (*pin_enter)(void *,uint64_t);
static int (*i2c_enter)(void *,uint64_t);
static unsigned pin_waits,i2c_waits,busy_seen,steal,fail_in_wait;
static void observe(void)
{if(a733_pmic_live(&m.provider,&m.lifetime)!=A733_PMIC_READY)++busy_seen;}
static int pin_wait(void *v,uint64_t deadline)
{if(m.worker){++pin_waits;observe();}return pin_enter(v,deadline);}
static int i2c_wait(void *v,uint64_t deadline)
{
    ++i2c_waits;observe();
    /* A client fault latched while the provider object was free. */
    if(fail_in_wait){fail_in_wait=0;m.provider.failed=1;}
    int r=i2c_enter(v,deadline);
    /* Another caller holds the object when the poll tries to take it back. */
    if(steal){unsigned zero=0;assert(atomic_compare_exchange_strong(&m.provider.busy,&zero,1));}
    return r;
}
static void submit(unsigned owner,struct a733_pmic_request *q)
{
    struct a733_rail_token t={0};
    assert(a733_rail_acquire(&m.rails,owner,A733_RAIL_BIT(A733_RAIL_DCDC1),0,m.now+1000000,&t)==A733_RAIL_PENDING);
    for(unsigned i=0;i<16 && a733_rail_poll(&m.rails,&t)!=A733_RAIL_READY;++i){}
    assert(a733_pmic_submit(&m.provider,owner,&t,A733_RAIL_DCDC1,m.now+1000000,q)==A733_PMIC_PENDING);
}
static void complete(const struct a733_pmic_request *q)
{
    struct a733_pmic_snapshot s;
    for(unsigned i=0;i<150 && a733_pmic_result(&m.provider,q,&s)==A733_PMIC_PENDING;++i)
        assert(pmic_model_step(&m)!=A733_PMIC_RETAINED);
    assert(a733_pmic_result(&m.provider,q,&s)==A733_PMIC_COMPLETE);
    assert(s.enabled && s.decoded && s.programmed_uv==3300000 && !m.i2c_held && !m.retained);
    assert(a733_pmic_reap(&m.provider,q)==A733_PMIC_COMPLETE);
}
static void setup(void)
{
    pmic_model_setup(&m);
    for(unsigned i=0;i<16 && a733_pmic_live(&m.provider,&m.lifetime)!=A733_PMIC_READY;++i)
        assert(pmic_model_step(&m)!=A733_PMIC_RETAINED);
    assert(a733_pmic_live(&m.provider,&m.lifetime)==A733_PMIC_READY);
    pin_enter=m.pins.operations.enter;m.pins.operations.enter=pin_wait;
    i2c_enter=m.provider.pmic_bus_ops.enter;m.provider.pmic_bus_ops.enter=i2c_wait;
    pin_waits=i2c_waits=busy_seen=0;
}
int main(void)
{
    struct a733_pmic_request q;
    /* Both lock waits see a free provider object. */
    setup();submit(10,&q);complete(&q);
    printf("PMIC request admission: %u REGISTER_LOCK and %u I2C mutex waits, %u BUSY live() observations\n",
        pin_waits,i2c_waits,busy_seen);fflush(stdout);
    assert(pin_waits==1 && i2c_waits==1 && !busy_seen);
    /* The object is taken when the poll returns from the I2C wait: the poll
     * releases the untouched mutex, returns BUSY and admits on the next poll. */
    setup();submit(10,&q);
    unsigned writes=m.writes,reads=m.reads;steal=1;
    assert(pmic_model_step(&m)==A733_PMIC_BUSY);steal=0;
    assert(!m.i2c_held && !m.retained && !m.provider.failed && !m.provider.held);
    assert(m.writes==writes && m.reads==reads);
    atomic_store(&m.provider.busy,0);complete(&q);
    /* A fault latched during the wait releases the untouched mutex and
     * retains the request without any bus access. */
    setup();submit(10,&q);
    writes=m.writes;reads=m.reads;fail_in_wait=1;
    assert(pmic_model_step(&m)==A733_PMIC_RETAINED);
    assert(!m.i2c_held && !m.retained && !m.provider.held && m.writes==writes && m.reads==reads);
    struct a733_pmic_snapshot s;
    assert(a733_pmic_result(&m.provider,&q,&s)==A733_PMIC_RETAINED);
    puts("PASS PMIC request admission waits for REGISTER_LOCK and the I2C mutex without holding the provider object");
    return 0;
}
