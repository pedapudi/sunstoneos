#include "a733_gpu_nsi_pmu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
/* The model's live counters and immediate clear are deliberate hypotheses.
 * It checks software behavior under those hypotheses, not A733 semantics. */
struct model {
    uint64_t ticks;
    uint32_t enable,clear,cycle,rd,wr,writes,stall,fail_write,latch_on_disable,delayed_clear;
    uint64_t clear_at;
    unsigned log[16],n;
};
static int read_reg(void *ctx,enum a733_gpu_nsi_register reg,uint32_t *out)
{
    struct model *m=ctx;
    switch(reg) {
    case A733_NSI_ENABLE:*out=m->enable;return 1;
    case A733_NSI_CLEAR:*out=m->clear;return 1;
    case A733_NSI_CYCLE:*out=m->cycle;return 1;
    case A733_NSI_READ:*out=m->rd;return 1;
    case A733_NSI_WRITE:*out=m->wr;return 1;
    }return 0;
}
static int write_reg(void *ctx,enum a733_gpu_nsi_register reg,uint32_t v)
{
    struct model *m=ctx;++m->writes;
    if(m->fail_write==m->writes)return 0;
    if(m->n<16)m->log[m->n++]=reg;
    switch(reg) {
    case A733_NSI_ENABLE:
        if(m->latch_on_disable && !(v&1) && (m->enable&1)){m->rd=500;m->wr=50;}
        m->enable=v;return 1;
    case A733_NSI_CLEAR:
        m->clear=(m->stall||m->delayed_clear)?v:0;m->clear_at=m->ticks+120;
        if(!m->delayed_clear){m->rd=0;m->wr=0;}return 1;
    case A733_NSI_CYCLE:m->cycle=v;return 1;
    default:assert(0);return 0;
    }
}
static uint64_t ticks(void *ctx)
{
    struct model *m=ctx;m->ticks+=24;
    if(m->delayed_clear && m->clear && m->ticks>=m->clear_at){m->rd=0;m->wr=0;m->clear=0;}
    return m->ticks;
}
static const struct a733_gpu_nsi_ops ops={read_reg,write_reg,ticks};
static void begin(struct a733_gpu_nsi_pmu *p,struct model *m,struct a733_gpu_nsi_record *r)
{a733_gpu_nsi_begin(p,&ops,m,696000000,0xc5000000,r);}
int main(void)
{
    const uint32_t oscillator=0x10000002,reference=0xf8636300,peri=0xfe106300;
    assert(a733_gpu_nsi_hz(oscillator,reference,peri,0,0,0xf4025600,0,0,0xc5000000)==696000000);
    assert(a733_gpu_nsi_hz(oscillator,reference,peri,0,0,0,0,0,0xc3000000)==600000000);
    assert(a733_gpu_nsi_hz(oscillator,reference,peri,0,0,0,0,0,0xc3000001)==300000000);
    assert(!a733_gpu_nsi_hz(oscillator,reference,peri,0,0,0,0,0,0xc4000000));
    assert(a733_gpu_nsi_hz(oscillator,reference,peri|16,0,0,0,0,0,0xc4000000)==480000000);
    assert(!a733_gpu_nsi_hz(oscillator,reference,peri,0x80000000,0,0,0,0,0xc3000000));
    assert(!a733_gpu_nsi_hz(oscillator,reference,peri,0,0,0,0,0,0xd3000000));
    assert(!a733_gpu_nsi_hz(oscillator,reference,peri,0,0,0,0,0,0xcb000000));
    assert(!a733_gpu_nsi_hz(oscillator,reference,peri,0,0,0xf4025600,0x80000000,0,0xc5000000));
    assert(!a733_gpu_nsi_hz(oscillator,reference,peri,0,0,0,0,0,0xc1000000));
    struct a733_gpu_nsi_pmu p={0};struct a733_gpu_nsi_record r;
    struct model m={.enable=0x10,.cycle=100,.rd=7,.wr=3};
    begin(&p,&m,&r);
    assert(p.owned && r.status==A733_NSI_SAMPLED && r.read[0]==7 && r.write[0]==3);
    assert(m.n==4 && m.log[0]==A733_NSI_CYCLE && m.log[1]==A733_NSI_ENABLE &&
        m.log[2]==A733_NSI_CLEAR && m.log[3]==A733_NSI_ENABLE);
    m.rd=100;m.wr=10;a733_gpu_nsi_after_kick(&p,&r);
    m.rd=200;m.wr=20;a733_gpu_nsi_end(&p,&r);
    assert(!p.owned && m.enable==0x10 && m.cycle==100 && r.samples==63);
    assert(r.read[2]==100 && r.read[3]==200 && r.read[4]==200 && r.read[5]==200);
    assert(r.status==A733_NSI_SAMPLED);
    p=(struct a733_gpu_nsi_pmu){0};m=(struct model){.cycle=42,.rd=37,.delayed_clear=1,.latch_on_disable=1};
    begin(&p,&m,&r);assert(r.read[0]==37 && r.read[1]==0);
    m.rd=10;m.wr=1;a733_gpu_nsi_end(&p,&r);
    assert(r.read[3]==10 && r.read[4]==500 && r.read[5]==500 && !p.owned && m.cycle==42);
    p=(struct a733_gpu_nsi_pmu){0};m=(struct model){.enable=1,.cycle=42,.rd=17};
    begin(&p,&m,&r);assert(r.status==A733_NSI_BUSY && m.writes==0 && m.rd==17 && m.cycle==42);
    p=(struct a733_gpu_nsi_pmu){0};m=(struct model){.stall=1,.cycle=42};
    begin(&p,&m,&r);assert(r.status==A733_NSI_CLEAR_PENDING && !p.owned && !m.enable && m.cycle==42);
    p=(struct a733_gpu_nsi_pmu){0};m=(struct model){.cycle=42};
    begin(&p,&m,&r);m.ticks+=UINT64_C(24000000)*7;a733_gpu_nsi_end(&p,&r);
    assert(r.status==A733_NSI_PERIOD_EXPIRED && !p.owned && !m.enable && m.cycle==42);
    p=(struct a733_gpu_nsi_pmu){0};m=(struct model){.cycle=42};
    begin(&p,&m,&r);
    m.ticks=p.enabled+UINT64_C(24000000)*UINT32_MAX/r.hz-100;
    a733_gpu_nsi_end(&p,&r);
    assert(r.status==A733_NSI_PERIOD_EXPIRED && !p.owned && !m.enable && m.cycle==42);
    p=(struct a733_gpu_nsi_pmu){0};m=(struct model){.cycle=42};
    begin(&p,&m,&r);r.read[1]=10;m.rd=1;a733_gpu_nsi_end(&p,&r);
    assert(r.status==A733_NSI_NONMONOTONIC && !p.owned);
    p=(struct a733_gpu_nsi_pmu){0};m=(struct model){.cycle=42};
    begin(&p,&m,&r);m.fail_write=m.writes+2;a733_gpu_nsi_end(&p,&r);
    assert(r.status==A733_NSI_IO && p.failed);
    const unsigned writes=m.writes;begin(&p,&m,&r);assert(m.writes==writes && r.status==A733_NSI_IO);
    puts("NSI PMU software ownership, clock decoding, vendor ordering, raw sampling and refusal checks passed");
}
