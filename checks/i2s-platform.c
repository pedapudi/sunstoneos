/* Clock/pin transaction model. No physical clocks, pin timing or microphone. */
#include "a733_i2s_platform.h"
#include "register_latency.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define C UINT32_C(0x02002000)
#define P UINT32_C(0x02000000)
#define B(n) (UINT32_C(1)<<(n))
#define READY A733_I2S_RESOURCE_READY
#define BUSY A733_I2S_RESOURCE_BUSY
#define PENDING A733_I2S_RESOURCE_PENDING
#define RETAINED A733_I2S_RESOURCE_RETAINED
static struct a733_audio_parent parent;
static struct a733_i2s_platform binding;
static struct a733_pins pins;
static struct a733_i2s_operations iops;
static struct a733_i2s_plan plan;
static uint32_t ccu[0x2000/4], pio[0x200/4], osc;
static uint64_t now;
static unsigned mutex, reads, writes, held[5], publish_count, releases;
static unsigned conflict_scope, late_acquire, late_publish, late_release, busy_lock;
static unsigned lease_pending, write_failure, lock_never, late_pin_release, late_write;
static uintptr_t failure_address;
static unsigned regress_after_enable, regression_phase;
static uint32_t saved_div2, pll_trace[8];
static unsigned pll_count;
static unsigned gate_status_reads, gate_status_hold, gate_status_fail, gate_status_late;
static uint64_t lock_after;
static unsigned lock_delay;
static unsigned stale_lock_reads;
static struct register_latency pll_lock, output_gate;
static unsigned fraction,write_cost;
static uint64_t lock_sample_ticks[3];
static int enter(void *cookie, uint64_t deadline)
{
    assert(cookie == &parent && !mutex && now < deadline);
    if (busy_lock) return 0;
    mutex=1; return 1;
}
static void leave(void *cookie)
{
    assert(cookie == &parent && mutex); mutex=0;
    if (regress_after_enable && (ccu[0x1200/4] & B(31))) {
        regress_after_enable=0; regression_phase=1;
    }
}
static int time_now(void *cookie, uint64_t *out)
{
    assert(cookie == &parent);
    if (late_pin_release && !binding.pin_lease.id) now=900;
    if (regression_phase == 1) { now=100; regression_phase=2; }
    else if (regression_phase == 2) { now=99; regression_phase=3; }
    *out=now; return 1;
}
static uint32_t *word(uintptr_t a)
{
    if (a >= C && a < C+sizeof(ccu)) return &ccu[(a-C)/4];
    if (a >= P && a < P+sizeof(pio)) return &pio[(a-P)/4];
    assert(a == 0x07090160); return &osc;
}
static int read_word(void *cookie, uintptr_t a, uint32_t *out)
{
    assert(cookie == &parent && mutex);
    ++reads;
    if(a==C+0x280){if(pll_lock.pending)++stale_lock_reads;ccu[0x280/4]=reg_latency_apply(&pll_lock,now,ccu[0x280/4]);}
    if(a==C+0x199c)ccu[0x199c/4]=reg_latency_apply(&output_gate,now,ccu[0x199c/4]);
    if (a == C+0x199c) {
        ++gate_status_reads;
        /* Cold status must not be sampled before PLL output enable. */
        assert(!parent.cold || (ccu[0x280/4] & B(27)));
        if (gate_status_late) now=parent.deadline;
        if (gate_status_fail) return 0;
    }
    *out=*word(a);
    if(a==C+0x280&&parent.stage==3&&(*out&B(28))&&parent.locked_samples<3){
        unsigned i=parent.locked_samples;lock_sample_ticks[i]=now*24+fraction;
        if(i)assert(lock_sample_ticks[i]-lock_sample_ticks[i-1]>=3*24);
    }
    return 1;
}
static int write_word(void *cookie, uintptr_t a, uint32_t value)
{
    assert(cookie == &parent && mutex);
    ++writes;
    if (a >= C) {
        assert(a == C+0x280 || a == C+0x1200 || a == C+0x120c);
        if (a == C+0x280) {
            assert(held[A733_AUDIO_CHANGE]);
            assert(pll_count < 8); pll_trace[pll_count++]=value;
            assert((value & 0x700000) == saved_div2);
            assert(!(value & B(27)) || (ccu[0x280/4] & B(28)));
            /* Manual4.1.4.1/.2 requires three lock samples and20us settle.
             * Constructed11us/7us hardware delays use explicit fixture time. */
            if((value&B(29))&&!(ccu[0x280/4]&B(29))){
                lock_after=now+lock_delay;reg_latency_schedule(&pll_lock,now,lock_delay,B(28),B(28));pll_lock.stuck=lock_never;
            }
            if ((value & B(27)) && !(ccu[0x280/4]&B(27))) {
                assert(!pll_lock.pending && parent.locked_samples==3 && now-parent.wait_since>=20);
                assert(now*24+fraction-lock_sample_ticks[2]>=20*24);
                if(!gate_status_hold)reg_latency_schedule(&output_gate,now,7,B(17)|B(18),B(17)|B(18));
            }
        } else assert(held[A733_AUDIO_LOCAL]);
    }
    *word(a)=value;
    if(a==C+0x280)now+=write_cost;
    if (late_write && a == failure_address) now=900;
    if (write_failure && a == failure_address) return 0; /* Write may have happened. */
    return 1;
}
static enum a733_i2s_resource_result acquire_lease(void *cookie, enum a733_audio_scope scope,
    struct a733_audio_token *token, uint64_t deadline)
{
    assert(cookie == &parent && !mutex);
    if (scope == conflict_scope) return BUSY;
    if (!token->id) {
        assert(!held[scope]); held[scope]=1;
        *token=(struct a733_audio_token){100u+scope,scope};
    } else assert(held[scope]);
    if (late_acquire == scope) now=deadline;
    if (lease_pending) { --lease_pending; return PENDING; }
    return READY;
}
static enum a733_i2s_resource_result publish_lease(void *cookie,
    const struct a733_audio_token *old, struct a733_audio_token *new_token, uint64_t deadline)
{
    assert(cookie == &parent && !mutex && old->scope == A733_AUDIO_CHANGE);
    assert((ccu[0x280/4] & 0xf807ff00u) == 0xf8047f00u);
    assert(held[A733_AUDIO_CHANGE]);
    assert((ccu[0x191c/4] & 0x60006) == 0x60006);
    assert((ccu[0x199c/4] & 0x60000) == 0x60000 && gate_status_reads);
    ++publish_count; held[A733_AUDIO_CHANGE]=0; held[A733_AUDIO_STABLE]=1;
    *new_token=(struct a733_audio_token){900,A733_AUDIO_STABLE};
    if (late_publish) now=deadline;
    return READY;
}
static enum a733_i2s_resource_result release_lease(void *cookie,
    const struct a733_audio_token *token, uint64_t deadline)
{
    assert(cookie == &parent && !mutex && token->scope == A733_AUDIO_LOCAL);
    assert(held[A733_AUDIO_LOCAL] && !binding.pin_lease.id);
    assert(!(ccu[0x1200/4] & B(31)) && !(ccu[0x120c/4] & 0x10001));
    ++releases; held[A733_AUDIO_LOCAL]=0;
    if (late_release) now=deadline;
    return READY;
}
static void setup(unsigned cold)
{
    memset(&parent,0,sizeof(parent)); memset(&binding,0,sizeof(binding));
    memset(&pins,0,sizeof(pins)); memset(ccu,0,sizeof(ccu)); memset(pio,0,sizeof(pio));
    memset(held,0,sizeof(held));
    now=1; mutex=reads=writes=publish_count=releases=0;
    pll_lock=(struct register_latency){0};output_gate=(struct register_latency){0};
    fraction=write_cost=0;memset(lock_sample_ticks,0,sizeof(lock_sample_ticks));
    conflict_scope=late_acquire=late_publish=late_release=busy_lock=0;
    lease_pending=write_failure=lock_never=late_pin_release=late_write=0; failure_address=0;
    osc=0x10000002; saved_div2=2u<<20; pll_count=0;
    regress_after_enable=regression_phase=0;
    gate_status_reads=gate_status_hold=gate_status_fail=gate_status_late=0;
    lock_after=0;lock_delay=11;stale_lock_reads=0;
    ccu[0]=(0xf8000000u | (99u<<8) | (99u<<16));
    ccu[0x280/4]=saved_div2 | B(7); /* Unowned field survives. */
    ccu[0x1200/4]=B(12); ccu[0x120c/4]=B(8);
    ccu[0x191c/4]=0x70007; /* AUDIO0 fields are unrelated and preserved. */
    ccu[0x199c/4]=cold ? B(16) : 0x70000;
    if (!cold) ccu[0x280/4] |= 0xf8047f00u;
    struct a733_audio_operations ops={acquire_lease,publish_lease,release_lease,
                                      enter,leave,time_now,read_word,write_word};
    assert(a733_audio_parent_init(&parent,&ops,&parent,cold,1000));
    struct a733_pins_operations pops={enter,leave,time_now,read_word,write_word};
    assert(a733_pins_init(&pins,&pops,&parent,A733_PINS_PB_MASK));
    const struct a733_pin_request req[]={{5,{0,0,0},{3,0,0}},
                                         {6,{0,0,0},{3,0,0}}, {8,{0,0,0},{3,0,0}}};
    assert(a733_i2s_platform_init(&binding,&parent,&pins,1,req,3));
    const struct a733_i2s_format f={24576000,16000,0,32,32,1,0};
    assert(a733_i2s_plan(&f,&plan)); iops=a733_i2s_platform_operations();
}
static void ready_parent(void)
{
    enum a733_i2s_resource_result r=PENDING;
    for (unsigned i=0;i<15 && r == PENDING;++i) {
        r=a733_audio_parent_poll(&parent); now+=10;
    }
    assert(r == READY && a733_audio_parent_ready(&parent));
}
static void ready_local(void)
{
    enum a733_i2s_resource_result r=iops.acquire(&binding,&plan,900);
    for (unsigned i=0;i<8 && r == PENDING;++i) { now+=10; r=iops.acquire_poll(&binding,900); }
    assert(r == READY && binding.local.id && binding.pin_lease.id);
    assert((ccu[0x1200/4] & 0x8700001f) == 0x82000018);
    assert((ccu[0x120c/4] & 0x10001) == 0x10001);
}
static void failed_parent_stays_quiet(void)
{
    unsigned total=reads+writes;
    assert(a733_audio_parent_poll(&parent) == RETAINED);
    assert(reads+writes == total);
}
static void failed_local_stays_quiet(void)
{
    unsigned total=reads+writes; uint32_t value;
    assert(iops.acquire_poll(&binding,900) == RETAINED);
    assert(iops.release(&binding,900) == RETAINED);
    assert(!iops.read(&binding,A733_I2S0_BASE,&value));
    assert(!iops.write(&binding,A733_I2S0_BASE,0));
    assert(reads+writes == total);
}
static void clock_failure_cases(void)
{
    for (unsigned late=0;late<2;++late) {
        setup(0); ready_parent();
        assert(iops.acquire(&binding,&plan,900) == PENDING);
        assert(iops.acquire_poll(&binding,900) == PENDING);
        assert(iops.acquire_poll(&binding,900) == PENDING && binding.pin_lease.id);
        now+=20; failure_address=C+0x1200; write_failure=!late; late_write=late;
        assert(iops.acquire_poll(&binding,900) == RETAINED);
        assert(binding.local.id && binding.pin_lease.id && parent.audio.id);
        failed_local_stays_quiet();
    }
    puts("I2S platform: uncertain and late module writes retain all tokens and stop I/O");
}
static uint32_t forbidden_dma_read(void *cookie, uintptr_t address)
{
    (void)cookie; (void)address;
    assert(!"clock regression must stop before any DMA claim access");
    return 0;
}
static enum a733_i2s_resource_result ready_health(void *cookie, uint64_t deadline)
{ (void)cookie; (void)deadline; return READY; }
static void monotonic_core_case(void)
{
    setup(0); ready_parent();
    /* A modeled DMA prerequisite; no DMA callback may execute in this case. */
    struct a733_dma_controller dma={.initialized=1,.base=0x04601000,
        .authorized_channels=1,.operations={.read=forbidden_dma_read}};
    /* A valid two-period receive catalogue; no transfer is ever submitted. */
    static _Alignas(64) unsigned char memory[3][128];
    struct a733_dma_mapping maps[2];struct a733_dma_segment segments[2];
    for (unsigned i=0;i<2;++i) {
        maps[i]=(struct a733_dma_mapping){memory[i],0x41000100u+i*128,128,A733_DMA_WRITE};
        segments[i]=(struct a733_dma_segment){{A733_I2S0_RXFIFO,3,4,8},{maps[i].bus,0,4,8},128};
    }
    const struct a733_dma_transfer catalogue={{memory[2],0x41000000,128,A733_DMA_DESCRIPTORS},segments,2,maps,2};
    const struct a733_i2s_stream_profile profile={plan.format,{iops,ready_health},&binding,
        A733_I2S_RECEIVE,100,0,A733_I2S_ROUTE_I2S0};
    static struct a733_i2s_stream stream;
    assert(a733_i2s_stream_open(&stream,&profile,&catalogue,&dma,0,900) == A733_I2S_OPENING);
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_OPENING);
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_OPENING && binding.pin_lease.id);
    now+=20; regress_after_enable=1;
    uint64_t core_before=stream.time;
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_RECOVERY);
    assert(regression_phase == 3 && now == 99 && parent.last_time == 100 && core_before < now);
    assert(stream.platform_held && !stream.dma_held && binding.local.id && binding.pin_lease.id);
    assert(binding.failed);
    unsigned io=reads+writes;
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_RECOVERY);
    assert(a733_i2s_stream_close(&stream,900) == A733_I2S_INVALID && io == reads+writes);
    failed_local_stays_quiet();
    puts("I2S acquisition: stream core rejects time below parent's latest observation before DMA or controller access");
}
static void audio_gate_cases(void)
{
    setup(0); ready_parent();
    assert(!writes && !publish_count && gate_status_reads == 1);
    /* Either automatic gating or a disabled software gate is inadmissible,
     * even when the effective-status register currently reports enabled. */
    const unsigned control_bits[]={1,2,17,18};
    for (unsigned cold=0;cold<2;++cold) for (unsigned i=0;i<4;++i) {
        setup(cold); ccu[0x191c/4] &= ~B(control_bits[i]); ccu[0x199c/4]=0x70000;
        assert(a733_audio_parent_poll(&parent) == PENDING);
        assert(a733_audio_parent_poll(&parent) == PENDING);
        assert(a733_audio_parent_poll(&parent) == RETAINED);
        assert(!writes && !gate_status_reads && !publish_count && parent.audio.id && parent.reference.id);
        failed_parent_stays_quiet();
    }
    for (unsigned bit=17;bit<=18;++bit) {
        setup(0); ccu[0x199c/4] &= ~B(bit);
        assert(a733_audio_parent_poll(&parent) == PENDING);
        assert(a733_audio_parent_poll(&parent) == PENDING);
        assert(a733_audio_parent_poll(&parent) == RETAINED && !writes && !publish_count);
        failed_parent_stays_quiet();
    }
    /* A cold output can take time to propagate. No status access occurs before
     * output enable, and no stable lease is published while either gate is off. */
    setup(1); gate_status_hold=1;
    for (unsigned i=0;!gate_status_reads;++i) {
        assert(i<15 && a733_audio_parent_poll(&parent) == PENDING); now+=10;
    }
    assert(!mutex && !publish_count && held[A733_AUDIO_CHANGE] && !held[A733_AUDIO_STABLE]);
    ccu[0x199c/4] |= B(17);
    assert(a733_audio_parent_poll(&parent) == PENDING && !publish_count);
    ccu[0x199c/4] |= B(18);
    assert(a733_audio_parent_poll(&parent) == PENDING && !publish_count);
    assert(a733_audio_parent_poll(&parent) == READY && publish_count == 1);
    assert(ccu[0x191c/4] == 0x70007 && ccu[0x199c/4] == 0x70000);
    for (unsigned failure=0;failure<4;++failure) {
        setup(1); gate_status_hold=1;
        for (unsigned i=0;!gate_status_reads;++i) {
            assert(i<15 && a733_audio_parent_poll(&parent) == PENDING); now+=10;
        }
        if (failure == 0) now=parent.deadline;
        if (failure == 1) gate_status_fail=1;
        if (failure == 2) gate_status_late=1;
        if (failure == 3) ccu[0x191c/4] &= ~B(2);
        assert(a733_audio_parent_poll(&parent) == RETAINED);
        assert(!mutex && !publish_count && held[A733_AUDIO_CHANGE] && parent.audio.id && parent.reference.id);
        failed_parent_stays_quiet();
    }
    puts("AUDIO1 gate admission: manual/software bits, effective status, cold propagation and retained failures passed; no gate writes");
}
/* Every integer rate that the planner accepts at the parent's 24.576 MHz
 * module clock with two 32-bit slots, offered to the binding from a fresh
 * parent: each acquires and releases the unchanged module divider. A plan
 * for the 44.1 kHz family's 22.5792 MHz module clock is refused without
 * effects. checks/i2s-rates-reference.py compares the log with the vendor's
 * rate table. */
static void rate_cases(void)
{
    printf("{\"module_hz\":24576000,\"slot_bits\":32,\"rates\":[");
    unsigned accepted=0;
    for (uint32_t rate=1; rate<=400000; ++rate) {
        const struct a733_i2s_format f={24576000,rate,0,32,32,1,0};
        struct a733_i2s_plan p;
        if (!a733_i2s_plan(&f,&p)) continue;
        setup(0); ready_parent(); plan=p; ready_local();
        enum a733_i2s_resource_result r=iops.release(&binding,900);
        while (r == PENDING) r=iops.release(&binding,900);
        assert(r == READY && !binding.local.id && !binding.pin_lease.id);
        printf("%s%u",accepted++ ? "," : "",rate);
    }
    const struct a733_i2s_format f={22579200,44100,0,32,32,1,0};
    struct a733_i2s_plan p;
    assert(a733_i2s_plan(&f,&p));
    setup(0); ready_parent();
    unsigned before=reads+writes;
    assert(iops.acquire(&binding,&p,900) == BUSY && reads+writes == before && !binding.local.id);
    puts("],\"refused_module_hz\":[22579200]}");
}
static void unstable_lock(void)
{
    setup(1);
    for(unsigned i=0;i<40&&!parent.locked_samples;++i){assert(a733_audio_parent_poll(&parent)==PENDING);now+=3;}
    assert(parent.locked_samples==1&&!publish_count);
    ccu[0x280/4]&=~B(28);
    ++now;
    assert(a733_audio_parent_poll(&parent)==PENDING&&!parent.locked_samples&&!publish_count);
    reg_latency_schedule(&pll_lock,now,11,B(28),B(28));
    ready_parent();assert(publish_count==1&&!output_gate.pending);
}
static void minimum_intervals(void)
{
    for(unsigned costly=0;costly<2;++costly){
        setup(1);fraction=23;write_cost=costly?17:0;
        while(parent.stage!=3)assert(a733_audio_parent_poll(&parent)==PENDING);
        uint64_t anchor=parent.wait_since;
        now=anchor+3;fraction=0;
        assert(a733_audio_parent_poll(&parent)==PENDING&&!parent.locked_samples);
        now=anchor+20;fraction=23;
        assert(a733_audio_parent_poll(&parent)==PENDING&&parent.locked_samples==1);
        now+=3;fraction=0;
        assert(a733_audio_parent_poll(&parent)==PENDING&&parent.locked_samples==1);
        ++now;assert(a733_audio_parent_poll(&parent)==PENDING&&parent.locked_samples==2);
        now+=4;fraction=23;assert(a733_audio_parent_poll(&parent)==PENDING&&parent.locked_samples==3);
        anchor=parent.wait_since;now=anchor+20;fraction=0;
        assert(a733_audio_parent_poll(&parent)==PENDING&&!(ccu[0x280/4]&B(27))&&!publish_count);
        ++now;assert(a733_audio_parent_poll(&parent)==PENDING&&(ccu[0x280/4]&B(27)));
        ready_parent();assert(publish_count==1);
    }
}
int main(int argc, char **argv)
{
    if (argc == 2) {
        if(strcmp(argv[1],"--observed-audio1") == 0){
            setup(1);saved_div2=1u<<20;ccu[0x280/4]=UINT32_C(0x41147f00);
            ccu[0x28c/4]=UINT32_C(0x30000000);lock_delay=50;ready_parent();
            assert(publish_count==1&&(ccu[0x280/4]&B(24))&&stale_lock_reads);
            for(unsigned i=0;i<pll_count;++i)assert(pll_trace[i]&B(24));
            assert(ccu[0x28c/4]==UINT32_C(0x30000000));
            setup(0);ccu[0x280/4]|=B(24);ready_parent();assert(!pll_count);
            puts("PASS observed AUDIO1 reserved bit survives cold preparation and warm adoption");
            return 0;
        }
        if(strcmp(argv[1],"--reset-ldo") == 0){
            setup(1);saved_div2=1u<<20;ccu[0x280/4]=UINT32_C(0x40147f00);lock_delay=50;ready_parent();
            assert(publish_count==1&&(ccu[0x280/4]&B(30))&&gate_status_reads&&stale_lock_reads&&now>=lock_after+20);
            puts("PASS AUDIO1 disabled PLL with enabled reset-default LDO waits for lock, settle and effective output gates");
            return 0;
        }
        if (strcmp(argv[1],"--audio-gates") == 0) audio_gate_cases();
        else if (strcmp(argv[1],"--rates") == 0) rate_cases();
        else if (strcmp(argv[1],"--monotonic-core") == 0) monotonic_core_case();
        else { assert(strcmp(argv[1],"--clock-faults") == 0); clock_failure_cases(); }
        return 0;
    }
    assert(argc == 1);
    minimum_intervals();
    unstable_lock();
    setup(1); lease_pending=1; ready_parent();
    assert(publish_count == 1 && held[A733_AUDIO_REFERENCE] && held[A733_AUDIO_STABLE]);
    assert(!held[A733_AUDIO_CHANGE] && (ccu[0x280/4] & B(7)));
    ready_local();
    printf("{\"audio\":%u,\"module\":%u,\"pll_writes\":[",ccu[0x280/4],ccu[0x1200/4]);
    for (unsigned i=0;i<pll_count;++i) printf("%s%u",i ? "," : "",pll_trace[i]);
    puts("]}");
    enum a733_i2s_resource_result r=iops.release(&binding,900);
    while (r == PENDING) r=iops.release(&binding,900);
    assert(r == READY && releases == 1 && !binding.local.id && !binding.pin_lease.id);
    assert(held[A733_AUDIO_REFERENCE] && held[A733_AUDIO_STABLE] && parent.audio.id);
    assert(ccu[0x1200/4] == B(12) && ccu[0x120c/4] == B(8));
    assert(pio[0x100/4] == 0 && pio[0x104/4] == 0);

    setup(0); ready_parent(); assert(writes == 0 && publish_count == 0);
    ready_local(); late_release=1;
    assert(iops.release(&binding,900) == PENDING);
    assert(iops.release(&binding,900) == PENDING);
    assert(iops.release(&binding,900) == RETAINED && binding.local.id);
    failed_local_stays_quiet();

    setup(0); ready_parent(); ready_local();
    assert(iops.release(&binding,900) == PENDING);
    late_pin_release=1;
    assert(iops.release(&binding,900) == RETAINED && binding.pin_lease.id && binding.local.id);
    failed_local_stays_quiet();

    setup(1); conflict_scope=A733_AUDIO_REFERENCE;
    assert(a733_audio_parent_poll(&parent) == PENDING && !reads && !writes);
    conflict_scope=0; ready_parent();
    setup(1); late_acquire=A733_AUDIO_REFERENCE;
    assert(a733_audio_parent_poll(&parent) == RETAINED && parent.reference.id && !reads);
    failed_parent_stays_quiet();

    setup(1); ccu[0] ^= 1u<<16;
    assert(a733_audio_parent_poll(&parent) == PENDING);
    assert(a733_audio_parent_poll(&parent) == PENDING);
    assert(a733_audio_parent_poll(&parent) == RETAINED && writes == 0);
    failed_parent_stays_quiet();
    for (unsigned i=0;i<8;++i) {
        static const unsigned consumers[]={0x1200,0x1210,0x1220,0x1224,0x1230,0x1240,0x1280,0x12c0};
        setup(1); ccu[consumers[i]/4] |= B(31);
        assert(a733_audio_parent_poll(&parent) == PENDING);
        assert(a733_audio_parent_poll(&parent) == PENDING);
        assert(a733_audio_parent_poll(&parent) == RETAINED && writes == 0);
    }
    setup(0); ccu[0x280/4] ^= 1u<<8;
    assert(a733_audio_parent_poll(&parent) == PENDING);
    assert(a733_audio_parent_poll(&parent) == PENDING);
    assert(a733_audio_parent_poll(&parent) == RETAINED && writes == 0);

    setup(1); lock_never=1;
    for (unsigned i=0;i<8;++i) { assert(a733_audio_parent_poll(&parent) == PENDING); now+=10; }
    now=1000; assert(a733_audio_parent_poll(&parent) == RETAINED && parent.audio.id);
    failed_parent_stays_quiet();
    setup(1); late_publish=1;
    r=PENDING;
    for (unsigned i=0;i<15 && r == PENDING;++i) { r=a733_audio_parent_poll(&parent); now+=10; }
    assert(r == RETAINED && parent.audio.scope == A733_AUDIO_CHANGE && parent.published.id);
    failed_parent_stays_quiet();

    setup(0); ready_parent();
    assert(iops.acquire(&binding,&plan,900) == PENDING);
    assert(iops.acquire_poll(&binding,900) == PENDING);
    write_failure=1; failure_address=P+0x100;
    assert(iops.acquire_poll(&binding,900) == RETAINED && binding.pin_lease.id && binding.local.id);
    failed_local_stays_quiet();

    setup(0); ready_parent(); busy_lock=1;
    assert(iops.acquire(&binding,&plan,900) == PENDING);
    assert(iops.acquire_poll(&binding,900) == PENDING && !writes);
    busy_lock=0; now=0;
    assert(iops.acquire_poll(&binding,900) == RETAINED && binding.local.id);
    failed_local_stays_quiet();
    clock_failure_cases();
    monotonic_core_case();
    puts("I2S platform: parent authorization, clock sequence, shared lifetime, pin retention and deadlines passed");
    return 0;
}
