/* Fixed thermal policy driven by the actual controller through modeled MMIO.
 * Build with drivers/thermal/a733_thermal.c and drivers/clock/a733_clock_leases.c. */
#include "a733_thermal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CCU 0x02002000u
#define THS 0x02522000u
#define SID 0x03006244u
struct model {
    uint32_t ccu[0x2000/4],ths[0x100/4],sid[3],osc;
    uint64_t time;
    unsigned held,retained,fail_read;
    struct a733_thermal driver;
    struct a733_clock_leases registry;
    struct a733_thermal_policy policy;
    unsigned notified[3],answer,workers;
    enum a733_thermal_level last;
    uint64_t progress;
};
static int now(void *v,uint64_t *out) {struct model *m=v;*out=++m->time;return 1;}
static uint32_t *reg(struct model *m,uintptr_t a)
{
    if (!(a&3) && a>=CCU && a<CCU+0x2000) return &m->ccu[(a-CCU)/4];
    if (!(a&3) && a>=THS && a<THS+0x100) return &m->ths[(a-THS)/4];
    if (!(a&3) && a>=SID && a<SID+12) return &m->sid[(a-SID)/4];
    if (a==0x07090160) return &m->osc;
    assert(!"unexpected MMIO");return NULL;
}
static int read32(void *v,uintptr_t a,uint32_t *out)
{
    struct model *m=v;assert(m->held && !m->retained);
    if (m->fail_read && a==THS+0x20) return 0;
    *out=*reg(m,a);return 1;
}
static int write32(void *v,uintptr_t a,uint32_t value)
{
    struct model *m=v;assert(m->held && !m->retained);
    uint32_t *r=reg(m,a);
    if (a>=THS+0x20 && a<=THS+0x2c) *r&=~value;else *r=value;
    return 1;
}
static int enter(void *v,uint64_t deadline) {struct model *m=v;assert(!m->held && deadline>m->time);m->held=1;return 1;}
static void leave(void *v) {struct model *m=v;assert(m->held);m->held=0;}
static void retain(void *v) {struct model *m=v;assert(m->held);m->retained=1;}
static int authorize(void *v,const struct a733_thermal_config *c,const struct a733_clock_token *t)
{(void)v;(void)c;(void)t;return 1;}
/* Consumer: two workers normally, one while throttled so watchdog progress is
 * still credited, none after the critical stop. */
static int notify(void *v,enum a733_thermal_level level)
{
    struct model *m=v;assert(level<=A733_THERMAL_CRITICAL && level!=m->last);
    assert(a733_thermal_policy_level(&m->policy)==level);
    ++m->notified[level];
    if (level!=A733_THERMAL_CRITICAL) {m->last=level;m->workers=level==A733_THERMAL_NORMAL ? 2 : 1;return 1;}
    if (m->answer==1) {m->last=level;m->workers=0;}
    return (int)m->answer;
}
static void setup(struct model *m)
{
    memset(m,0,sizeof(*m));m->osc=0x10000002;m->workers=2;m->answer=1;
    m->ccu[0]=0xf8000000u|(99u<<8)|(99u<<16);m->ccu[0x518/4]=1;m->ccu[0xfc0/4]=0x81000040;m->ccu[0xfe4/4]=0x20;
    m->sid[0]=250; /* Factory 25.0 C, every sensor code 2400. */
    for (unsigned i=0;i<5;++i) for (unsigned j=0;j<12;++j)
        if (2400u&(1u<<j)) m->sid[(12+12*i+j)/32]|=1u<<((12+12*i+j)%32);
    assert(a733_clock_leases_init(&m->registry,now,m));
    struct a733_clock_token token;
    assert(a733_clock_acquire(&m->registry,23,A733_CLOCK_FABRIC|A733_CLOCK_GPADC24M,A733_CLOCK_THS,100000,&token)==A733_CLOCK_OK);
    /* The session's hardware alarm thresholds are deliberately unrelated to the policy trips. */
    struct a733_thermal_config c={.owner=23,.handoff=91,.startup_deadline_us=100000,.apb1=1,.apb1_hz=12000000,
        .alarm_mask=31,.critical_mask=31};
    for (unsigned i=0;i<5;++i) c.limits[i]=(struct a733_thermal_limits){1400,1600,1000};
    const struct a733_thermal_ops ops={now,authorize,enter,leave,retain,read32,write32};
    assert(a733_thermal_init(&m->driver,&c,&ops,m,&m->registry,&token));
    assert(a733_thermal_poll(&m->driver)==A733_THERMAL_PENDING && a733_thermal_poll(&m->driver)==A733_THERMAL_READY);
    assert(a733_thermal_policy_init(&m->policy,m->time,notify,m) && !a733_thermal_policy_init(&m->policy,m->time,notify,m));
    assert(a733_thermal_policy_level(&m->policy)==A733_THERMAL_NORMAL);
}
/* One owner iteration: read, take the clock afterwards, update, do the work. */
static enum a733_thermal_level step(struct model *m,uint32_t ready,const uint16_t code[5],uint32_t events)
{
    m->ths[0x20/4]=ready;m->ths[0x24/4]=events;m->ths[0x2c/4]=events;
    for (unsigned i=0;i<5;++i) if (ready&(1u<<i)) m->ths[(0xc0+4*i)/4]=code[i];
    struct a733_thermal_sample s;
    enum a733_thermal_result r=a733_thermal_read(&m->driver,m->time+100000,&s);
    uint64_t t;now(m,&t);
    enum a733_thermal_level level=a733_thermal_policy_update(&m->policy,t,r,&s);
    assert(level==a733_thermal_policy_level(&m->policy));
    m->progress+=m->workers;return level;
}
static void codes(uint16_t out[5],uint16_t value) {for (unsigned i=0;i<5;++i) out[i]=value;}
int main(void)
{
    /* Converted trip boundaries lie in the code<=1769 branch. */
    int32_t t;
    assert(a733_thermal_temperature(1309,&t) && t==90034);
    assert(a733_thermal_temperature(1310,&t) && t==89975);
    assert(a733_thermal_temperature(1343,&t) && t==88028);
    assert(a733_thermal_temperature(1344,&t) && t==87969);
    assert(a733_thermal_temperature(970,&t) && t==110035);
    assert(a733_thermal_temperature(971,&t) && t==109976);
    assert(a733_thermal_temperature(2338,&t) && t==30008);
    struct model m;uint16_t c[5];
    setup(&m);codes(c,2338);
    /* Normal readings; hardware alarm and critical events alone change nothing. */
    assert(step(&m,31,c,31)==A733_THERMAL_NORMAL && m.policy.seen==31 && !m.notified[1]);
    assert(step(&m,0,c,0)==A733_THERMAL_NORMAL && step(&m,0,c,31)==A733_THERMAL_NORMAL);
    /* CPUB at its passive trip throttles, not below it; DDR/NPU have no passive trip. */
    c[0]=1310;assert(step(&m,1,c,0)==A733_THERMAL_NORMAL);
    c[1]=c[2]=1000;assert(step(&m,6,c,0)==A733_THERMAL_NORMAL);
    c[0]=1309;uint64_t before=m.progress;
    assert(step(&m,1,c,0)==A733_THERMAL_THROTTLE && m.notified[1]==1 && m.policy.passive==1 && m.workers==1);
    /* Hysteresis holds the trip until the reading falls below 88 C. */
    c[0]=1310;assert(step(&m,1,c,0)==A733_THERMAL_THROTTLE);
    c[0]=1343;assert(step(&m,1,c,0)==A733_THERMAL_THROTTLE);
    assert(m.progress>before+1); /* Throttled work still progresses. */
    c[0]=1344;assert(step(&m,1,c,0)==A733_THERMAL_NORMAL && m.notified[0]==1 && m.workers==2 && !m.policy.passive);
    /* GPU has no hysteresis; CPUL keeps the level while GPU releases. */
    c[4]=1309;c[3]=1300;assert(step(&m,24,c,0)==A733_THERMAL_THROTTLE && m.policy.passive==24);
    c[4]=1310;assert(step(&m,16,c,0)==A733_THERMAL_THROTTLE && m.policy.passive==8);
    c[3]=1344;assert(step(&m,8,c,0)==A733_THERMAL_NORMAL && m.notified[0]==2);
    /* Repeated BUSY/AGAIN keeps values; a channel without readings for 1 s throttles. */
    codes(c,2338);assert(step(&m,31,c,0)==A733_THERMAL_NORMAL);
    m.time+=900000;assert(step(&m,15,c,0)==A733_THERMAL_NORMAL);
    m.time+=200000;assert(step(&m,15,c,0)==A733_THERMAL_THROTTLE && m.policy.stale==16 && !m.policy.passive);
    assert(step(&m,31,c,0)==A733_THERMAL_NORMAL && !m.policy.stale);
    /* Critical trip on one channel: one stop request, terminal and frozen. */
    unsigned notified=m.notified[2];
    c[1]=971;assert(step(&m,2,c,0)==A733_THERMAL_NORMAL);
    c[1]=970;before=m.progress;
    assert(step(&m,2,c,0)==A733_THERMAL_CRITICAL && m.notified[2]==notified+1 && m.policy.critical==2 &&
        m.policy.request==2 && !m.workers);
    codes(c,2338);assert(step(&m,31,c,0)==A733_THERMAL_CRITICAL && m.notified[2]==notified+1 && m.policy.critical==2);
    assert(m.progress==before);
    /* A busy stop request is asked again once per update; an uncertain answer is retained. */
    setup(&m);codes(c,2338);m.answer=0;
    c[2]=900;assert(step(&m,31,c,0)==A733_THERMAL_CRITICAL && m.notified[2]==1 && m.policy.request==1);
    assert(step(&m,0,c,0)==A733_THERMAL_CRITICAL && m.notified[2]==2 && m.policy.request==1);
    m.answer=7;assert(step(&m,0,c,0)==A733_THERMAL_CRITICAL && m.notified[2]==3 && m.policy.request==3);
    m.answer=1;assert(step(&m,0,c,0)==A733_THERMAL_CRITICAL && m.notified[2]==3);
    /* No reading for 5 s, including before any first sample, is critical. */
    setup(&m);codes(c,2338);
    m.time+=1000001;assert(step(&m,0,c,0)==A733_THERMAL_THROTTLE && m.policy.stale==31);
    m.time+=4000000;assert(step(&m,0,c,0)==A733_THERMAL_CRITICAL && m.policy.critical==31 && m.policy.stale==31);
    /* A controller failure ends supervision: driver retains, policy stops work. */
    setup(&m);codes(c,2338);assert(step(&m,31,c,0)==A733_THERMAL_NORMAL);
    m.fail_read=1;assert(step(&m,31,c,0)==A733_THERMAL_CRITICAL && m.retained && m.policy.sensor && m.notified[2]==1);
    /* Unexpected results, replayed samples and backward time are sensor failures. */
    setup(&m);struct a733_thermal_sample s={0};
    assert(a733_thermal_policy_update(&m.policy,m.time,A733_THERMAL_INVALID,&s)==A733_THERMAL_CRITICAL && m.policy.sensor);
    setup(&m);codes(c,2338);assert(step(&m,31,c,0)==A733_THERMAL_NORMAL);
    s.sequence=m.policy.sequence;s.valid=31;s.observed_us=m.time;
    assert(a733_thermal_policy_update(&m.policy,m.time+1,A733_THERMAL_SAMPLE,&s)==A733_THERMAL_CRITICAL && m.policy.sensor);
    setup(&m);codes(c,2338);assert(step(&m,31,c,0)==A733_THERMAL_NORMAL);
    assert(a733_thermal_policy_update(&m.policy,m.policy.last_us-1,A733_THERMAL_AGAIN,NULL)==A733_THERMAL_CRITICAL);
    setup(&m);s=(struct a733_thermal_sample){.sequence=1,.valid=1,.observed_us=m.time+10};
    assert(a733_thermal_policy_update(&m.policy,m.time+5,A733_THERMAL_SAMPLE,&s)==A733_THERMAL_CRITICAL);
    setup(&m);assert(a733_thermal_policy_update(&m.policy,m.time,A733_THERMAL_SAMPLE,NULL)==A733_THERMAL_CRITICAL);
    /* Uninitialized policy objects report CRITICAL. */
    struct a733_thermal_policy none={0};
    assert(a733_thermal_policy_level(&none)==A733_THERMAL_CRITICAL && a733_thermal_policy_level(NULL)==A733_THERMAL_CRITICAL);
    assert(a733_thermal_policy_update(&none,1,A733_THERMAL_AGAIN,NULL)==A733_THERMAL_CRITICAL && !none.initialized);
    assert(!a733_thermal_policy_init(&none,1,NULL,NULL));
    puts("PASS thermal policy trips, hysteresis, stale and lost readings, sensor failure and stop requests");
    return 0;
}
