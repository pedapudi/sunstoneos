/* Reuse the controller model; do not replay its conversion/protocol suite. */
#define main thermal_existing_suite
#include "a733-thermal.c"
#undef main

static int retained_enter(void *v,uint64_t deadline)
{
    struct model *m=v;
    assert(enter(v,deadline)==1);
    assert(a733_clock_retain(&m->registry,&m->driver.token)==A733_CLOCK_FAULT);
    return 1;
}
static int contended_enter(void *v,uint64_t deadline)
{
    struct model *m=v;
    assert(enter(v,deadline)==1);
    atomic_store(&m->registry.busy,1);
    return 1;
}
int main(void)
{
    for (unsigned active=0;active<2;++active) {
        for (unsigned contention=0;contention<2;++contention) {
            struct model m;
            setup(&m,1,0);
            assert(a733_thermal_poll(&m.driver)==A733_THERMAL_PENDING);
            if (active) assert(a733_thermal_poll(&m.driver)==A733_THERMAL_READY);
            m.driver.ops.enter=contention ? contended_enter : retained_enter;
            unsigned reads=m.reads,writes=m.writes;
            struct a733_thermal_sample sample;
            memset(&sample,0xa5,sizeof(sample));
            struct a733_thermal_sample before=sample;
            enum a733_thermal_result result=active ?
                a733_thermal_read(&m.driver,100000,&sample) : a733_thermal_poll(&m.driver);
            assert(m.reads==reads && m.writes==writes);
            assert(!memcmp(&sample,&before,sizeof(sample)));
            if (contention) {
                assert(result==A733_THERMAL_BUSY && !m.held && !m.retained && !m.driver.fault);
                atomic_store(&m.registry.busy,0);
                m.driver.ops.enter=enter;
                result=active ? a733_thermal_read(&m.driver,100000,&sample) :
                    a733_thermal_poll(&m.driver);
                assert(result==(active ? A733_THERMAL_AGAIN : A733_THERMAL_READY));
                assert(!m.held && !m.retained);
            } else {
                assert(result==A733_THERMAL_RETAINED && m.held && m.retained);
                inert(&m);
            }
        }
    }
    puts("PASS thermal lease loss and transient registry contention after mutex wait, during startup and sampling");
    return 0;
}
