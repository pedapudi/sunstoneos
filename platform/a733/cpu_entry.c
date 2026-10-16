#include "a733_cpu_entry.h"

enum { CHECK_SLOT, ARM_SLOT, POWER, WAIT_ARRIVAL, RELEASE, WAIT_RELEASE };
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y && (x+an<=y || y+bn<=x);
}
int a733_cpu_entry_init(struct a733_cpu_entry *s,const struct a733_cpu_entry_config *c)
{
    if(!separate(s,sizeof(*s),c,sizeof(*c)) || s->initialized || !c->power ||
       !separate(s,sizeof(*s),c->power,sizeof(*c->power)) ||
       !separate(c,sizeof(*c),c->power,sizeof(*c->power)) ||
       !c->ops.read_slot || !c->ops.arm || !c->ops.release || !c->ops.now_us ||
       !c->epoch || c->entry<UINT32_C(0x40000000) || c->entry>UINT32_MAX || (c->entry&3))return 0;
    const struct a733_cpu_start *p=c->power;
    if(!p->initialized || p->entered || p->step || p->writes_attempted ||
       p->state!=A733_CPU_START_PENDING || p->error!=A733_CPU_START_NO_ERROR ||
       p->config.entry!=c->entry || !p->config.core || p->config.core>=8 ||
       !p->config.lease || !p->config.deadline)return 0;
    *s=(struct a733_cpu_entry){.config=*c,.deadline=p->config.deadline,
        .lease=p->config.lease,.core=p->config.core,.initialized=1,.state=A733_CPU_ENTRY_PENDING};
    return 1;
}
static void fail(struct a733_cpu_entry *s,enum a733_cpu_entry_error e)
{s->error=e;s->state=A733_CPU_ENTRY_RETAINED;}
static int time_ok(struct a733_cpu_entry *s)
{
    uint64_t now;
    if(s->config.ops.now_us(s->config.context,&now)!=1 || now<s->last_now ||
       now<s->config.power->last_now || now>=s->deadline) {
        fail(s,A733_CPU_ENTRY_TIME);return 0;
    }
    s->last_now=now;return 1;
}
static int valid_report(struct a733_cpu_entry *s,const struct a733_cpu_report *r)
{
    unsigned core;
    /* MIDR implementer/architecture/part identify the expected processor;
     * variant and revision are retained for later errata selection. */
    uint64_t expected=s->core<6 ? UINT64_C(0x410fd050) : UINT64_C(0x410fd0b0);
    if(r->epoch!=s->config.epoch || r->core!=s->core ||
       !a733_cpu_index(r->mpidr,&core) || core!=s->core ||
       (r->midr&UINT64_C(0xffffffffff0ffff0))!=expected ||
       (r->sctlr&A733_ENTRY_SCTLR_REJECT) || r->reason || r->esr || r->far || r->elr || r->spsr)return 0;
    for(unsigned i=0;i<5;++i)if(r->reserved[i])return 0;
    return 1;
}
enum a733_cpu_entry_state a733_cpu_entry_poll(struct a733_cpu_entry *s)
{
    if(!s || !s->initialized)return A733_CPU_ENTRY_RETAINED;
    if(s->entered || s->state!=A733_CPU_ENTRY_PENDING)return s->state;
    s->entered=1;
    if(!time_ok(s))goto out;
    struct a733_cpu_start *p=s->config.power;
    if(p->config.core!=s->core || p->config.entry!=s->config.entry ||
       p->config.lease!=s->lease || p->config.deadline!=s->deadline) {
        fail(s,A733_CPU_ENTRY_POWER);goto out;
    }
    struct a733_cpu_slot slot;
    int advance=1;
    switch(s->phase) {
    case CHECK_SLOT: {
        if(s->config.ops.read_slot(s->config.context,s->core,&slot)!=1) {fail(s,A733_CPU_ENTRY_IO);break;}
        const unsigned char *b=(const unsigned char *)&slot;
        for(size_t i=0;i<sizeof(slot);++i)if(b[i]) {fail(s,A733_CPU_ENTRY_DIRTY_SLOT);break;}
        break;
    }
    case ARM_SLOT: {
        struct a733_cpu_command command={.magic=A733_ENTRY_MAGIC,.epoch=s->config.epoch,
            .core=s->core,.state=A733_ENTRY_ARMED,.inverse_epoch=~s->config.epoch};
        ++s->writes_attempted;
        if(s->config.ops.arm(s->config.context,s->core,&command)!=1)fail(s,A733_CPU_ENTRY_IO);
        break;
    }
    case POWER:
        if(a733_cpu_start_poll(p)==A733_CPU_START_RETAINED)fail(s,A733_CPU_ENTRY_POWER);
        else advance=p->state==A733_CPU_START_POWERED;
        break;
    case WAIT_ARRIVAL: case WAIT_RELEASE:
        if(s->config.ops.read_slot(s->config.context,s->core,&slot)!=1) {fail(s,A733_CPU_ENTRY_IO);break;}
        s->report=slot.report;
        if(!s->report.state) {advance=0;break;}
        if(!valid_report(s,&s->report) ||
           (s->phase==WAIT_ARRIVAL && s->report.state!=A733_ENTRY_ARRIVED) ||
           (s->phase==WAIT_RELEASE && s->report.state!=A733_ENTRY_ARRIVED && s->report.state!=A733_ENTRY_RELEASED))
            fail(s,A733_CPU_ENTRY_REPORT);
        else if(s->phase==WAIT_RELEASE)advance=s->report.state==A733_ENTRY_RELEASED;
        break;
    case RELEASE:
        if(p->state!=A733_CPU_START_POWERED) {fail(s,A733_CPU_ENTRY_POWER);break;}
        ++s->writes_attempted;
        if(s->config.ops.release(s->config.context,s->core,s->config.epoch)!=1)fail(s,A733_CPU_ENTRY_IO);
        break;
    default:fail(s,A733_CPU_ENTRY_REPORT);break;
    }
    if(s->state==A733_CPU_ENTRY_PENDING && time_ok(s) && advance) {
        if(s->phase==WAIT_RELEASE)s->state=A733_CPU_ENTRY_DISPATCHED;
        else ++s->phase;
    }
out:
    s->entered=0;return s->state;
}
