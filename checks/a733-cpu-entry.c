#include "a733_cpu_entry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct model {
    struct a733_cpu_slot slot;
    struct a733_cpu_entry entry;
    struct a733_cpu_start power;
    uint64_t now;
    uint32_t arch, lo, hi, control, mode, policy, powered;
    unsigned published;
    unsigned calls, mailbox_calls, fail_at, late_at, power_fail, suppress, report_bad;
    unsigned release_count, arm_count;
};
static void arrival(struct model *m)
{
    if(m->slot.report.state || m->suppress)return;
#ifdef SUNSTONE_BOARD_RESIDENT
    assert(m->published && m->arm_count==1 && m->slot.command.state==A733_ENTRY_ARMED);
    assert(!m->release_count); /* Immediate wake can arrive, never continue. */
#endif
    unsigned core=m->power.config.core;
    m->slot.report=(struct a733_cpu_report){.epoch=m->entry.config.epoch,.core=core,
        .midr=core<6?0x412fd052:0x413fd0b1,.mpidr=UINT64_C(0x81000000)+(core<<8),
        .state=A733_ENTRY_ARRIVED};
    switch(m->report_bad) {
    case 1:++m->slot.report.epoch;break;
    case 2:--m->slot.report.core;break;
    case 3:m->slot.report.mpidr|=1;break;
    case 4:m->slot.report.midr=0x410fd030;break;
    case 5:m->slot.report.sctlr=1;break;
    case 6:m->slot.report.state=A733_ENTRY_RELEASED;break;
    case 7:m->slot.report.state=A733_ENTRY_FAULT;m->slot.report.reason=2;break;
    case 8:m->slot.report.reserved[4]=1;break;
    case 9:m->slot.report.far=0x1234;break;
    }
}
static int clock_read(void *ctx,uint64_t *out)
{struct model *m=ctx;++m->calls;*out=m->now;return 1;}
static uint32_t *reg(struct model *m,uintptr_t address)
{
    uintptr_t core=m->power.config.core;
    if(address==0x08001000+core*0x1000)return &m->arch;
    if(address==0x08001004+core*0x1000)return &m->lo;
    if(address==0x08001008+core*0x1000)return &m->hi;
    if(address==0x07050200+core*4)return &m->control;
    if(address==0x07050220+core*4)return &m->mode;
#ifdef SUNSTONE_BOARD_RESIDENT
    if(address==0x07052000+core*0x1000)return &m->policy;
#endif
    assert(0);return NULL;
}
static int read_reg(void *ctx,uintptr_t address,uint32_t *value)
{
    struct model *m=ctx;++m->calls;
    if(m->power_fail)return 0;
    if(address==0x07052008+(uintptr_t)m->power.config.core*0x1000) {
#ifdef SUNSTONE_BOARD_RESIDENT
        *value=m->powered|(m->policy&0x100);if(m->powered)arrival(m);
#else
        *value=(m->control&1)?8:0;if(*value)arrival(m);
#endif
    } else *value=*reg(m,address);
    return 1;
}
static int write_reg(void *ctx,uintptr_t address,uint32_t value)
{
    struct model *m=ctx;++m->calls;*reg(m,address)=value;
#ifdef SUNSTONE_BOARD_RESIDENT
    if(address==0x07052000+(uintptr_t)m->power.config.core*0x1000) {
        assert(value==0x100 && m->published && m->arm_count==1);
        assert(m->lo==m->power.config.entry && !m->hi && (m->arch&1));
        m->powered=8;arrival(m); /* Wake before the explicit hotplug request. */
    }
#endif
    return 1;
}
static int barrier(void *ctx) {struct model *m=ctx;++m->calls;m->published=1;return 1;}
static int mailbox_result(struct model *m)
{
    ++m->calls;++m->mailbox_calls;
    /* Reentrant polling cannot issue another operation. */
    unsigned calls=m->calls;
    assert(a733_cpu_entry_poll(&m->entry)==A733_CPU_ENTRY_PENDING);
    assert(m->calls==calls);
    if(m->mailbox_calls==m->late_at)m->now=1000;
    return m->mailbox_calls!=m->fail_at;
}
static int read_slot(void *ctx,unsigned core,struct a733_cpu_slot *out)
{struct model *m=ctx;assert(core==m->power.config.core);*out=m->slot;return mailbox_result(m);}
static int arm(void *ctx,unsigned core,const struct a733_cpu_command *command)
{
    struct model *m=ctx;assert(core==m->power.config.core);
    assert(!m->power.step && !m->power.writes_attempted);
    ++m->arm_count;m->slot.command=*command;
    return mailbox_result(m);
}
static int release(void *ctx,unsigned core,uint64_t epoch)
{
    struct model *m=ctx;assert(core==m->power.config.core && epoch==m->slot.command.epoch);
    assert(m->power.state==A733_CPU_START_POWERED && !(m->control&1) && !m->mode);
    assert(m->entry.report.state==A733_ENTRY_ARRIVED);
#ifdef SUNSTONE_BOARD_RESIDENT
    assert(m->control==2); /* Continuation may clear wake-disable only now. */
#endif
    ++m->release_count;m->slot.command.state=A733_ENTRY_RELEASE;
    m->slot.report.state=A733_ENTRY_RELEASED;
    return mailbox_result(m);
}
static void setup(struct model *m,unsigned core)
{
    memset(m,0,sizeof(*m));m->now=1;
#ifdef SUNSTONE_BOARD_RESIDENT
    m->control=2;
#endif
    struct a733_cpu_start_config p={.ops={read_reg,write_reg,barrier,clock_read},
        .context=m,.boot_mpidr=0x81000000,.entry=0x48000000,.lease=17,.deadline=1000,.core=core};
    assert(a733_cpu_start_init(&m->power,&p));
    struct a733_cpu_entry_config c={.power=&m->power,.ops={read_slot,arm,release,clock_read},
        .context=m,.epoch=UINT64_C(0x0123456789abcdef),.entry=p.entry};
    assert(a733_cpu_entry_init(&m->entry,&c));
    memset(&c,0,sizeof(c));
}
static enum a733_cpu_entry_state run(struct model *m)
{
    for(unsigned i=0;i<100 && m->entry.state==A733_CPU_ENTRY_PENDING;++i)
        (void)a733_cpu_entry_poll(&m->entry);
    return m->entry.state;
}
static void quiet(struct model *m)
{
    unsigned calls=m->calls;enum a733_cpu_entry_state state=m->entry.state;
    for(unsigned i=0;i<4;++i)assert(a733_cpu_entry_poll(&m->entry)==state);
    assert(m->calls==calls);
}
int main(void)
{
    struct model m;
    for(unsigned c=1;c<8;++c) {
        setup(&m,c);assert(run(&m)==A733_CPU_ENTRY_DISPATCHED);
        assert(m.arm_count==1 && m.release_count==1 && m.mailbox_calls==5);
        assert(m.entry.report.core==c && m.entry.report.state==A733_ENTRY_RELEASED);
        assert(m.entry.writes_attempted==2 && m.power.writes_attempted==
#ifdef SUNSTONE_BOARD_RESIDENT
        8
#else
        7
#endif
        );quiet(&m);
    }
    for(unsigned op=1;op<=5;++op) {
        setup(&m,3);m.fail_at=op;assert(run(&m)==A733_CPU_ENTRY_RETAINED);
        assert(m.entry.error==A733_CPU_ENTRY_IO);quiet(&m);
        if(op>=4)assert(m.slot.command.state==A733_ENTRY_RELEASE);
        setup(&m,7);m.late_at=op;assert(run(&m)==A733_CPU_ENTRY_RETAINED);
        assert(m.entry.error==A733_CPU_ENTRY_TIME);quiet(&m);
    }
    for(unsigned bad=1;bad<=9;++bad) {
        setup(&m,1);m.report_bad=bad;assert(run(&m)==A733_CPU_ENTRY_RETAINED);
        assert(m.entry.error==A733_CPU_ENTRY_REPORT && !m.release_count);quiet(&m);
    }
    setup(&m,2);m.slot.report.reserved[4]=1;
    assert(run(&m)==A733_CPU_ENTRY_RETAINED && m.entry.error==A733_CPU_ENTRY_DIRTY_SLOT);
    assert(!m.arm_count && !m.power.writes_attempted);quiet(&m);
    setup(&m,2);m.slot.command.state=A733_ENTRY_RELEASE;
    assert(run(&m)==A733_CPU_ENTRY_RETAINED && !m.arm_count);quiet(&m);
    setup(&m,2);m.power_fail=1;assert(run(&m)==A733_CPU_ENTRY_RETAINED);
    assert(m.entry.error==A733_CPU_ENTRY_POWER && !m.release_count);quiet(&m);
    setup(&m,2);m.suppress=1;assert(run(&m)==A733_CPU_ENTRY_PENDING);
    m.now=1000;assert(run(&m)==A733_CPU_ENTRY_RETAINED);
    assert(m.entry.error==A733_CPU_ENTRY_TIME && !m.release_count);quiet(&m);
    setup(&m,2);assert(a733_cpu_entry_poll(&m.entry)==A733_CPU_ENTRY_PENDING);
    m.now=0;assert(run(&m)==A733_CPU_ENTRY_RETAINED && m.entry.error==A733_CPU_ENTRY_TIME);quiet(&m);
    setup(&m,2);++m.power.config.lease;
    assert(run(&m)==A733_CPU_ENTRY_RETAINED && !m.arm_count);quiet(&m);
    setup(&m,2);struct a733_cpu_entry saved=m.entry;
    assert(!a733_cpu_entry_init(&m.entry,&m.entry.config));assert(!memcmp(&saved,&m.entry,sizeof(saved)));
    struct a733_cpu_entry zero={0};struct a733_cpu_entry_config c=saved.config;c.epoch=0;
    assert(!a733_cpu_entry_init(&zero,&c) && !zero.initialized);
    c=saved.config;c.entry+=4;assert(!a733_cpu_entry_init(&zero,&c));
    c=saved.config;m.power.step=1;assert(!a733_cpu_entry_init(&zero,&c));
    assert(a733_cpu_entry_poll(NULL)==A733_CPU_ENTRY_RETAINED);
    puts("PASS seven secondary owners, power composition, mailbox failures, deadlines, stale records and report refusal");
}
