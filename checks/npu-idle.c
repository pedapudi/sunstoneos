/* Production idle state machine; modeled MMIO and counter, no physical NPU. */
#include "a733_npu_idle.h"
#include "register_latency.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLOCK UINT32_C(0x00070100)
#define SHADER UINT32_C(0x015a0880)
#define FULL_IDLE UINT32_C(0x7fffffff)
#define TRACE_MAX 128u
struct access { unsigned write; uint32_t offset, value; uint64_t at; };
struct device {
    struct npu_idle idle;
    uint64_t ticks, frequency, quantum, settle, low_at;
    uint32_t clock, shader, status, high_value;
    unsigned gated, pulse, health, calls, fail_at, failed, accesses;
    unsigned failed_kind;
    uint32_t failed_offset, failed_value;
    unsigned writes, reads, resume_reads, completed_pulses;
    unsigned stuck, bad_clock, bad_shader, bad_idle, slow;
    uint32_t read_shader;
    struct register_latency effect;
    struct access trace[TRACE_MAX];
};
static void apply(struct device *d)
{
    if (!d->effect.pending) return;
    d->clock=reg_latency_apply(&d->effect,d->ticks,d->clock);
    if (!d->effect.pending) d->gated=!!(d->clock&1u);
}
static int access_ok(struct device *d,unsigned kind,uint32_t offset,uint32_t value)
{
    assert(!d->failed);
    ++d->calls;
    if(d->calls==d->fail_at){
        d->failed=1;d->failed_kind=kind;d->failed_offset=offset;d->failed_value=value;return 0;
    }
    return 1;
}
static void trace(struct device *d,unsigned write,uint32_t offset,uint32_t value)
{
    assert(d->accesses<TRACE_MAX);
    d->trace[d->accesses++]=(struct access){write,offset,value,d->ticks};
}
static int read32(void *cookie,uint32_t offset,uint32_t *value)
{
    struct device *d=cookie;
    if(!access_ok(d,NPU_IDLE_READ,offset,0))return 0;
    apply(d);
    /* The only source-backed suspended read is resume's clock control. */
    if(d->gated){assert(offset==0);++d->resume_reads;}
    assert(!d->pulse);
    ++d->reads;
    switch(offset){
    case 0:*value=d->clock;if(d->bad_clock&&!d->gated&&d->completed_pulses>=2)*value^=4;break;
    case 0x10c:*value=d->shader;if(d->bad_shader&&d->completed_pulses>=2)*value^=2;d->read_shader=*value;break;
    case 4:*value=d->bad_idle?0:d->status;break;
    default:assert(!"unexpected/destructive NPU register read");return 0;
    }
    trace(d,0,offset,*value);
    d->ticks+=d->slow==1?d->frequency/5000:d->slow==2?d->frequency/10000:1;
    return 1;
}
static int write32(void *cookie,uint32_t offset,uint32_t value)
{
    struct device *d=cookie;
    if(!access_ok(d,NPU_IDLE_WRITE,offset,value))return 0;
    apply(d);++d->writes;trace(d,1,offset,value);
    assert(!d->gated||offset==0);
    if(offset==0){
        /* The vendor writes back what it read: every bit but the two controls is the register's own. */
        assert((value&~UINT32_C(0x201))==(d->clock&~UINT32_C(0x201)));
        if(value&0x200){assert(!d->pulse);d->pulse=1;d->high_value=value;}
        else{
            assert(d->pulse&&value==(d->high_value&~UINT32_C(0x200)));
            d->pulse=0;++d->completed_pulses;
            d->low_at=d->ticks;
            reg_latency_schedule(&d->effect,d->ticks,d->settle,UINT32_MAX,value);
            d->effect.stuck=d->stuck;
        }
    }else if(offset==0x10c){
        assert(!d->gated&&!d->effect.pending);
        assert(d->ticks-d->low_at>=d->settle);
        assert((value&~1u)==(d->read_shader&~1u));
        if(value&1){assert(!d->pulse);d->pulse=1;}
        else{assert(d->pulse);d->pulse=0;}
        d->shader=value;
    }else assert(!"unexpected NPU register write");
    d->ticks+=d->slow==1?d->frequency/5000:d->slow==2?d->frequency/10000:1;
    return 1;
}
static uint64_t now(void *cookie)
{
    struct device *d=cookie;
    d->ticks+=d->quantum;apply(d);return d->ticks;
}
static int health(void *cookie)
{
    struct device *d=cookie;
    return access_ok(d,NPU_IDLE_HEALTH,0,0)&&d->health;
}
static const struct npu_idle_ops ops={read32,write32,now,health};
static unsigned strict_mode=1;
static void fresh(struct device *d,uint64_t frequency,unsigned enabled)
{
    memset(d,0,sizeof(*d));
    d->frequency=frequency;d->quantum=1;d->settle=(frequency+999999)/1000000;
    d->clock=CLOCK;d->shader=SHADER;d->status=FULL_IDLE;d->health=1;d->ticks=100;
    assert(npu_idle_init(&d->idle,&a733_npu_idle_part,&ops,d,frequency,enabled,strict_mode)==1);
    assert(d->idle.phase==NPU_IDLE_ON);
    npu_idle_completed(&d->idle,d->ticks);
}
static void mature(struct device *d)
{
    d->ticks=npu_idle_deadline_ticks(&d->idle);
    assert(npu_idle_eligible(&d->idle,d->ticks));
}
static void suspend(struct device *d)
{
    mature(d);assert(npu_idle_gate(&d->idle)==1);apply(d);
    assert(d->idle.phase==NPU_IDLE_SUSPENDED);
    assert(d->gated&&d->clock==(CLOCK|1u)&&!d->pulse);
    assert(d->ticks-d->low_at>=d->settle);
}
static void retained(struct device *d)
{
    assert(d->idle.phase==NPU_IDLE_RETAINED);
    const unsigned accesses=d->accesses,calls=d->calls;
    const struct npu_idle_failure failure=d->idle.failure;
    assert(failure.site);
    if(d->failed){
        assert(failure.site==d->failed_kind&&failure.offset==d->failed_offset);
        assert(failure.value==d->failed_value);
    }
    assert(npu_idle_gate(&d->idle)!=1&&npu_idle_resume(&d->idle)!=1);
    npu_idle_completed(&d->idle,d->ticks+1);
    npu_idle_retain(&d->idle,NPU_IDLE_OWNER,0xdead,0xbeef,0);
    assert(!npu_idle_eligible(&d->idle,UINT64_MAX));
    assert(d->accesses==accesses&&d->calls==calls);
    assert(!memcmp(&failure,&d->idle.failure,sizeof(failure)));
}
static void nominal(void)
{
    struct device d;fresh(&d,24000000,1);suspend(&d);
    const unsigned after_gate=d.accesses;
    assert(after_gate>=4&&d.trace[after_gate-1].write&&d.trace[after_gate-1].offset==0);
    assert(npu_idle_resume(&d.idle)==1);apply(&d);
    assert(d.idle.phase==NPU_IDLE_ON&&!d.gated&&!d.pulse);
    assert(d.clock==CLOCK&&d.shader==SHADER&&d.resume_reads==1);
    assert(d.idle.counters.gates==1&&d.idle.counters.resumes==1);
    assert(d.trace[after_gate].offset==0&&!d.trace[after_gate].write);
    assert(d.idle.counters.resume_max_ticks<=2400);
    assert(!npu_idle_eligible(&d.idle,d.ticks));
}
static void threshold(void)
{
    struct device d;fresh(&d,24000000,1);
    const uint64_t start=d.ticks;
    assert(npu_idle_gate(&d.idle)==0&&d.accesses==0);
    assert(!npu_idle_eligible(&d.idle,start+216000));
    assert(!npu_idle_eligible(&d.idle,start+239999));
    assert(npu_idle_eligible(&d.idle,start+240000));
    assert(npu_idle_eligible(&d.idle,start+264000));
    assert(npu_idle_deadline_ticks(&d.idle)==start+240000);
    fresh(&d,24000000,0);d.ticks+=2400000;
    assert(npu_idle_eligible(&d.idle,d.ticks));
    const unsigned accesses=d.accesses;
    assert(npu_idle_gate(&d.idle)==0);
    assert(d.accesses==accesses&&d.idle.phase==NPU_IDLE_ON&&!d.gated);
    assert(d.idle.counters.controls==1&&d.idle.counters.gates==0);
    assert(npu_idle_resume(&d.idle)==1&&d.accesses==accesses&&!d.gated);
}
static void cancellation(void)
{
    struct device d;fresh(&d,24000000,1);
    const uint64_t deadline=npu_idle_deadline_ticks(&d.idle),generation=d.idle.generation;
    d.ticks=deadline-1;npu_idle_completed(&d.idle,d.ticks);
    assert(d.idle.generation>generation);
    assert(!npu_idle_eligible(&d.idle,deadline));
    assert(npu_idle_deadline_ticks(&d.idle)==d.ticks+240000);
    assert(d.accesses==0);
}
static void admission(void)
{
    struct device d;fresh(&d,24000000,1);suspend(&d);
    const unsigned accesses=d.accesses;
    assert(!npu_idle_eligible(&d.idle,d.ticks+2400000));
    assert(npu_idle_gate(&d.idle)!=1&&d.accesses==accesses);
    assert(npu_idle_resume(&d.idle)==1);
    const unsigned awake_accesses=d.accesses;
    assert(npu_idle_resume(&d.idle)==1&&d.accesses==awake_accesses);
}
static void delayed(void)
{
    const uint64_t rates[]={24000000,25000001,1000000};
    for(unsigned i=0;i<sizeof(rates)/sizeof(rates[0]);++i){
        struct device d;fresh(&d,rates[i],1);suspend(&d);
        assert(npu_idle_resume(&d.idle)==1);
        assert(!d.gated&&d.ticks-d.low_at>=d.settle);
    }
}
static void failures(void)
{
    struct device control;fresh(&control,24000000,1);mature(&control);
    const unsigned start=control.calls;
    assert(npu_idle_gate(&control.idle)==1);
    const unsigned gate_calls=control.calls-start;
    const unsigned resume_start=control.calls;
    assert(npu_idle_resume(&control.idle)==1);
    const unsigned resume_calls=control.calls-resume_start;
    assert(gate_calls&&resume_calls);
    for(unsigned at=1;at<=gate_calls;++at){
        struct device d;fresh(&d,24000000,1);mature(&d);d.fail_at=d.calls+at;
        assert(npu_idle_gate(&d.idle)!=1&&d.failed);retained(&d);
    }
    for(unsigned at=1;at<=resume_calls;++at){
        struct device d;fresh(&d,24000000,1);suspend(&d);d.fail_at=d.calls+at;
        assert(npu_idle_resume(&d.idle)!=1&&d.failed);retained(&d);
    }
    printf("injected every callback failure: %u suspend, %u resume\n",gate_calls,resume_calls);
}
/* Values other than the expected ones are recorded and do not refuse; only a lost ungate
 * (the gate bit still set after the pulse), an idle word below the vendor's mask before
 * gating and the strict budget retain the queue. */
static void readback(void)
{
#ifdef NPU_IDLE_VALUE_RECORDING
    const unsigned dev[]={NPU_IDLE_DEV_RESUME_CLOCK,NPU_IDLE_DEV_RESUME_SHADER,NPU_IDLE_DEV_RESUME_STATUS};
#endif
    for(unsigned at=0;at<3;++at){
        struct device d;fresh(&d,24000000,1);suspend(&d);
        if(at==0)d.bad_clock=1;
        if(at==1)d.bad_shader=1;
        if(at==2)d.bad_idle=1;
        assert(npu_idle_resume(&d.idle)==1&&d.idle.phase==NPU_IDLE_ON);
        assert(d.idle.counters.resumes==1);
#ifdef NPU_IDLE_VALUE_RECORDING
        assert(d.idle.counters.value_deviations==1&&d.idle.first_deviation.deviation&dev[at]);
#endif
    }
    /* A cycle that gates with unexpected register values writes them back, records them and
     * resumes. */
    for(unsigned at=0;at<4;++at){
        struct device d;fresh(&d,24000000,1);mature(&d);
        if(at==0)d.clock^=4;
        if(at==1)d.shader^=2;
        if(at==2)d.clock|=0x200;
        if(at==3)d.shader|=1;
        assert(npu_idle_gate(&d.idle)==1&&d.writes==2);
#ifdef NPU_IDLE_VALUE_RECORDING
        assert(d.idle.observed.deviation&(at%2?NPU_IDLE_DEV_GATE_SHADER:NPU_IDLE_DEV_GATE_CLOCK));
#endif
        d.pulse=0;apply(&d);
        assert(npu_idle_resume(&d.idle)==1);
#ifdef NPU_IDLE_VALUE_RECORDING
        assert(d.idle.counters.value_deviations==1);
        assert(d.idle.first_deviation.deviation&(at%2?NPU_IDLE_DEV_GATE_SHADER:NPU_IDLE_DEV_GATE_CLOCK));
#endif
    }
    /* The idle word compares under the vendor's mask: bit 31 is ignored; a unit not idle refuses. */
    struct device masked;fresh(&masked,24000000,1);mature(&masked);masked.status=FULL_IDLE|UINT32_C(0x80000000);
    assert(npu_idle_gate(&masked.idle)==1);
    struct device busy;fresh(&busy,24000000,1);mature(&busy);busy.bad_idle=1;
    assert(npu_idle_gate(&busy.idle)!=1&&busy.writes==0&&busy.idle.failure.site==NPU_IDLE_FULL_IDLE);retained(&busy);
    /* A lost suspend write shows at resume as an entry value without the gate bit: recorded,
     * and the ungate sequence is harmless. */
    struct device lost;fresh(&lost,24000000,1);mature(&lost);lost.stuck=1;
    assert(npu_idle_gate(&lost.idle)==1);
    lost.stuck=0;lost.effect.pending=0;lost.pulse=0; /* only the suspend write was lost */
    assert(npu_idle_resume(&lost.idle)==1);
#ifdef NPU_IDLE_VALUE_RECORDING
    assert(lost.idle.first_deviation.deviation&NPU_IDLE_DEV_RESUME_ENTRY);
#endif
    /* A lost resume write leaves the gate bit set: the queue is retained before +10c is read.
     * read32 asserts immediately if production performs a forbidden read. */
    struct device stuck;fresh(&stuck,24000000,1);suspend(&stuck);stuck.stuck=1;
    assert(npu_idle_resume(&stuck.idle)!=1);
    assert(stuck.gated&&stuck.idle.failure.site==NPU_IDLE_GATE_VALUE);retained(&stuck);
    struct device d;fresh(&d,24000000,1);suspend(&d);d.slow=1;
    assert(npu_idle_resume(&d.idle)!=1);retained(&d);
    assert(d.idle.failure.site==NPU_IDLE_SETTLE||d.idle.failure.site==NPU_IDLE_RESUME_BUDGET);
}
/* An over-budget resume that completes is retained only in strict mode. The
 * elapsed time is recorded in both modes. */
static void budget(void)
{
    struct device d;
    strict_mode=0;fresh(&d,24000000,1);suspend(&d);d.slow=2;
    assert(npu_idle_resume(&d.idle)==1&&d.idle.phase==NPU_IDLE_ON);
    assert(d.idle.counters.resume_over_budget==1&&d.idle.counters.resume_max_ticks>2400);
    assert(d.idle.counters.resumes==1&&!d.gated);
    strict_mode=1;
}
/* Bit 31 of the idle word is outside the vendor's mask and does not prevent gating. */
static void idle_mask(void)
{
    struct device d;fresh(&d,24000000,1);mature(&d);d.status=FULL_IDLE|UINT32_C(0x80000000);
    assert(npu_idle_gate(&d.idle)==1&&d.idle.phase==NPU_IDLE_SUSPENDED);
    assert(npu_idle_resume(&d.idle)==1);
}
static void retention(void)
{
    struct device d;fresh(&d,24000000,1);suspend(&d);
    npu_idle_retain(&d.idle,NPU_IDLE_UNEXPECTED_IRQ,0x10,2,0);retained(&d);
    fresh(&d,24000000,1);mature(&d);d.health=0;
    assert(npu_idle_gate(&d.idle)!=1);retained(&d);
    fresh(&d,24000000,1);d.idle.generation=UINT64_MAX;
    npu_idle_completed(&d.idle,d.ticks);retained(&d);
}
int main(int argc,char **argv)
{
    assert(argc==2);
    if(!strcmp(argv[1],"nominal"))nominal();
    else if(!strcmp(argv[1],"threshold"))threshold();
    else if(!strcmp(argv[1],"cancellation"))cancellation();
    else if(!strcmp(argv[1],"admission"))admission();
    else if(!strcmp(argv[1],"delayed-effects"))delayed();
    else if(!strcmp(argv[1],"partial-failure"))failures();
    else if(!strcmp(argv[1],"readback"))readback();
    else if(!strcmp(argv[1],"retention"))retention();
    else if(!strcmp(argv[1],"budget"))budget();
    else if(!strcmp(argv[1],"idle-mask"))idle_mask();
    else assert(!"unknown case");
    printf("PASS %s\n",argv[1]);return 0;
}
