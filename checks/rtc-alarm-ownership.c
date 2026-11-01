#include "a733_rtc.h"
#include <assert.h>
#include <stdio.h>

struct model {
    uint32_t reg[0x54 / 4];
    uint64_t now;
    unsigned calls, writes, acks, fail_at;
};
static int now(void *context, uint64_t *value)
{
    struct model *m = context;
    ++m->calls;
    *value = ++m->now;
    return m->calls != m->fail_at;
}
static int read32(void *context, uint32_t offset, uint32_t *value)
{
    struct model *m = context;
    assert(offset <= 0x50 && !(offset & 3));
    ++m->calls;
    *value = m->reg[offset / 4];
    return m->calls != m->fail_at;
}
static int write32(void *context, uint32_t offset, uint32_t value)
{
    struct model *m = context;
    assert(offset == 0x20 || offset == 0x24 || offset == 0x28 ||
           offset == 0x2c || offset == 0x30);
    ++m->calls;
    ++m->writes;
    /* A failed store may already have reached the peripheral. */
    if (offset == 0x30) {
        assert(value == 1);
        ++m->acks;
        m->reg[offset / 4] &= ~value;
    } else {
        m->reg[offset / 4] = value;
    }
    return m->calls != m->fail_at;
}
static void open_rtc(struct model *m, struct a733_rtc *r)
{
    *m = (struct model){.reg = {[0] = 0x4011, [1] = 1, [4] = 20000,
        [8] = 0xa5000020, [9] = 0x80000000}};
    *r = (struct a733_rtc){0};
    const struct a733_rtc_ops ops = {now, read32, write32};
    assert(a733_rtc_open(r, &ops, m, 1000000));
}
static void no_irq_io(struct model *m, struct a733_rtc *r)
{
    /* Model a pending event belonging to the next owner. Even a clock
     * callback would be an effect after this instance lost IRQ authority. */
    m->reg[0x28 / 4] = m->reg[0x2c / 4] = m->reg[0x30 / 4] = 1;
    unsigned calls = m->calls, writes = m->writes, acks = m->acks;
    enum a733_rtc_state state = r->state;
    assert(!a733_rtc_irq(r) && !a733_rtc_irq(r));
    assert(m->calls == calls && m->writes == writes && m->acks == acks);
    assert(m->reg[0x28 / 4] == 1 && m->reg[0x2c / 4] == 1 &&
           m->reg[0x30 / 4] == 1 && r->state == state);
}
static void fired(struct model *m)
{
    m->reg[0x14 / 4] = 2;
    m->reg[0x30 / 4] = 1;
}
int main(void)
{
    struct model m;
    struct a733_rtc r;
    open_rtc(&m, &r);
    no_irq_io(&m, &r); /* READY has never owned an alarm. */

    open_rtc(&m, &r);
    assert(a733_rtc_arm(&r, 2));
    fired(&m);
    assert(a733_rtc_irq(&r) && m.acks == 1);
    unsigned calls = m.calls;
    assert(!a733_rtc_irq(&r) && m.calls == calls);
    assert(a733_rtc_finish(&r, 0) && r.state == A733_RTC_FIRED);
    assert(!atomic_load(&r.owned));
    no_irq_io(&m, &r);

    open_rtc(&m, &r);
    assert(a733_rtc_arm(&r, 2) && a733_rtc_finish(&r, 1));
    assert(r.state == A733_RTC_CANCELLED && !atomic_load(&r.owned));
    no_irq_io(&m, &r);

    /* Inject one failure at every callback in arm, IRQ and finish. Faults
     * retain ownership and subsequent notifications must perform no I/O. */
    unsigned tested[3] = {0};
    for (unsigned phase = 0; phase < 3; ++phase) {
        open_rtc(&m, &r);
        if (phase) assert(a733_rtc_arm(&r, 2));
        if (phase) fired(&m);
        if (phase == 2) assert(a733_rtc_irq(&r));
        unsigned start = m.calls;
        assert(phase == 0 ? a733_rtc_arm(&r, 2) :
               phase == 1 ? a733_rtc_irq(&r) : a733_rtc_finish(&r, 0));
        unsigned count = m.calls - start;
        for (unsigned i = 1; i <= count; ++i) {
            open_rtc(&m, &r);
            if (phase) assert(a733_rtc_arm(&r, 2));
            if (phase) fired(&m);
            if (phase == 2) assert(a733_rtc_irq(&r));
            m.fail_at = m.calls + i;
            assert(!(phase == 0 ? a733_rtc_arm(&r, 2) :
                     phase == 1 ? a733_rtc_irq(&r) : a733_rtc_finish(&r, 0)));
            if (phase == 1) assert(atomic_load(&r.irq_fault));
            else assert(r.state == A733_RTC_RETAINED);
            m.fail_at = 0;
            no_irq_io(&m, &r);
            ++tested[phase];
        }
    }
    printf("PASS: RTC stale notifications and %u arm, %u IRQ, %u finish callback failures\n",
           tested[0], tested[1], tested[2]);
    return 0;
}
