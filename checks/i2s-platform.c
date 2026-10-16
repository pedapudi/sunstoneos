/* Clock/pin transaction model. No physical clocks, pin timing or microphone. */
#include "a733_i2s_platform.h"
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
    *out=*word(a);
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
            if ((value & B(31)) && !lock_never) value |= B(28);
        } else assert(held[A733_AUDIO_LOCAL]);
    }
    *word(a)=value;
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
    conflict_scope=late_acquire=late_publish=late_release=busy_lock=0;
    lease_pending=write_failure=lock_never=late_pin_release=late_write=0; failure_address=0;
    osc=0; saved_div2=2u<<20; pll_count=0;
    regress_after_enable=regression_phase=0;
    ccu[0]=(0xf8000000u | (99u<<8) | (99u<<16));
    ccu[0x280/4]=saved_div2 | B(7); /* Unowned field survives. */
    ccu[0x1200/4]=B(12); ccu[0x120c/4]=B(8);
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
static void monotonic_core_case(void)
{
    setup(0); ready_parent();
    /* A modeled DMA prerequisite; no DMA callback may execute in this case. */
    struct a733_dma_controller dma={.initialized=1,.base=0x04601000,
        .authorized_channels=1,.operations={.read=forbidden_dma_read}};
    struct a733_i2s_capture capture={0};
    assert(a733_i2s_open(&capture,&plan.format,&iops,&binding,&dma,0,900) == A733_I2S_OPENING);
    assert(a733_i2s_poll(&capture) == A733_I2S_OPENING);
    assert(a733_i2s_poll(&capture) == A733_I2S_OPENING && binding.pin_lease.id);
    now+=20; regress_after_enable=1;
    uint64_t core_before=capture.resource_time;
    assert(a733_i2s_poll(&capture) == A733_I2S_RECOVERY);
    assert(regression_phase == 3 && now == 99 && parent.last_time == 100 && core_before < now);
    assert(capture.platform_held && !capture.dma_held && binding.local.id && binding.pin_lease.id);
    assert(binding.failed);
    unsigned io=reads+writes;
    assert(a733_i2s_poll(&capture) == A733_I2S_RECOVERY);
    assert(a733_i2s_close(&capture,900) == A733_I2S_RECOVERY && io == reads+writes);
    failed_local_stays_quiet();
    puts("I2S acquisition: core rejects time below parent's latest observation before DMA or controller access");
}
int main(int argc, char **argv)
{
    if (argc == 2) {
        if (strcmp(argv[1],"--monotonic-core") == 0) monotonic_core_case();
        else { assert(strcmp(argv[1],"--clock-faults") == 0); clock_failure_cases(); }
        return 0;
    }
    assert(argc == 1);
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
