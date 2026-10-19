/* Reuse the accepted source-derived startup fixture and its register model.
 * These cases exercise only the added local stop branch after READY. */
#define main startup_fixture_main
#include "npu-platform.c"
#undef main

static void ready(struct model *m)
{
    setup(m,2);
    assert(run(m)==READY);
}
static enum a733_npu_platform_result stop(struct model *m)
{
    enum a733_npu_platform_result r=PENDING;
    for(unsigned i=0;i<10 && r!=READY && r!=RETAINED;++i)
        r=a733_npu_platform_stop_poll(&m->p);
    return r;
}
static void no_later_access(struct model *m)
{
    unsigned calls=m->calls;
    assert(a733_npu_platform_stop_poll(&m->p)==RETAINED);
    assert(a733_npu_platform_poll(&m->p)==RETAINED);
    assert(m->calls==calls);
    assert(a733_clock_token_status(&m->clocks,&m->p.resources.clock)==A733_CLOCK_OK);
}
int main(void)
{
    struct model m;
    setup(&m,2);assert(a733_npu_platform_stop_poll(&m.p)==RETAINED && !m.reads && !m.nwrite);
    ready(&m);unsigned before=m.nwrite;uint64_t token=m.p.resources.clock.id;
    m.lock_busy=1;assert(a733_npu_platform_stop_poll(&m.p)==BUSY && m.nwrite==before);
    assert(!a733_npu_platform_ready(&m.p));
    m.lock_busy=0;assert(stop(&m)==READY && m.p.stopped && m.nwrite==before+5);
    const uintptr_t addresses[]={CCU+0xb00,CCU+0xb04,CCU+0xb04,CCU+0xb04,CCU+0xb04};
    const uint32_t values[]={0x00200001,0xa00f0040,0xa00b0040,0xa0090040,0xa0080040};
    for(unsigned i=0;i<5;++i)assert(m.writes[before+i].address==addresses[i] && m.writes[before+i].value==values[i]);
    assert(*reg(&m,PCK)==0x80000008 && *reg(&m,PCK+8)==8);
    assert(*reg(&m,CCU+0x5c0)==0x123456ff && *reg(&m,CCU+0x5e0)==0x41055800);
    assert(*reg(&m,0x070101ac)==0xaa000001 && m.p.resources.clock.id==token);
    assert(a733_clock_token_status(&m.clocks,&m.p.resources.clock)==A733_CLOCK_OK);
    unsigned calls=m.calls;assert(a733_npu_platform_stop_poll(&m.p)==READY && m.calls==calls);
    for(unsigned at=1;at<=5;++at) {
        ready(&m);before=m.nwrite;m.fail_write=before+at;
        assert(stop(&m)==RETAINED && m.nwrite==before+at);no_later_access(&m);
    }
    ready(&m);before=m.nwrite;m.now=m.config.deadline_us;
    assert(stop(&m)==RETAINED && m.nwrite==before);no_later_access(&m);
    ready(&m);before=m.nwrite;*reg(&m,CCU+0x2a0)^=1u<<31;
    assert(stop(&m)==RETAINED && m.nwrite==before);no_later_access(&m);
    ready(&m);before=m.nwrite;m.no_ack=1;*reg(&m,PCK+8)=0;
    assert(stop(&m)==RETAINED && m.nwrite==before);no_later_access(&m);
    ready(&m);before=m.nwrite;*reg(&m,CCU+0xb04)&=~(1u<<19);
    assert(stop(&m)==RETAINED && m.nwrite==before);no_later_access(&m);
    ready(&m);m.fail_unlock=1;
    assert(stop(&m)==RETAINED);no_later_access(&m);
    puts("PASS NPU local stop: source order, unchanged shared/SRAM/domain state, leases retained, bounded faults and idempotent completion");
    return 0;
}
