/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "a733_dsi_video.h"
#include "a733_dsi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

void video_reference(const struct a733_dsi_video_timing *, const uint32_t *, const uint32_t *);
void video_reference_start(const uint32_t *, const uint32_t *);
void video_reference_stop(const uint32_t *, const uint32_t *);
struct model {
    uint32_t regs[256];
    uint64_t now;
    unsigned controller, calls, fail_at, late_at, reenter_at, corrupt_at, stop_reads;
    unsigned start_writes, stop_writes, ready, producer, stopped, stuck;
    struct a733_dsi_video *driver;
};
static struct a733_dsi_video_timing timing(unsigned burst, unsigned format)
{
    return (struct a733_dsi_video_timing){
        .pixel_hz = burst ? 72000000 : 80000000, .hs_hz = 480000000, .ls_hz = 80000000,
        .width = 1280, .height = 720, .hsync = 40, .hback = 100, .hfront = 80,
        .vsync = 5, .vback = 20, .vfront = 5, .lanes = 4, .mode = burst, .format = format};
}
static int operation(struct model *m)
{
    ++m->calls;
    if (m->calls == m->late_at) m->now = m->driver->deadline;
    if (m->calls == m->reenter_at) assert(a733_dsi_video_poll(m->driver) == A733_DSI_VIDEO_RETAINED);
    return m->calls != m->fail_at;
}
static int admit(void *arg, const struct a733_dsi_video_config *c, enum a733_dsi_video_action action)
{
    struct model *m = arg;
    assert(c->controller == m->controller && c->lease == 23 && c->timing.lanes == 4);
    int permitted = action == A733_DSI_VIDEO_PREPARE ? m->ready : m->producer;
    return operation(m) && permitted;
}
static unsigned index_of(struct model *m, uintptr_t addr)
{
    uintptr_t base = 0x05506000u + m->controller * 0x2000u;
    assert(addr >= base && addr < base + 1024 && !(addr & 3));
    return (unsigned)(addr - base) / 4;
}
static int read32(void *arg, uintptr_t addr, uint32_t *v)
{
    struct model *m = arg;
    unsigned i = index_of(m, addr);
    if (i == 4 && m->stop_writes && !m->stuck && ++m->stop_reads >= 3) {
        m->regs[4] &= ~1u; m->stopped = 1;
    }
    *v = m->regs[i];
    int ok = operation(m);
    if (m->calls == m->corrupt_at) *v ^= 0x80000000;
    return ok;
}
static int write32(void *arg, uintptr_t addr, uint32_t v)
{
    struct model *m = arg;
    unsigned i = index_of(m, addr);
    assert(i != 0 && i != 1); /* No reset, module gating or IRQ writes. */
    m->regs[i] = v;
    if (i == 4 && (v & 1)) { assert(m->producer); ++m->start_writes; }
    if (i == 0x4c / 4 && (v & 0x10000000)) { assert(m->producer); ++m->stop_writes; }
    return operation(m);
}
static int barrier(void *arg) { return operation(arg); }
static int now(void *arg, uint64_t *v) { struct model *m = arg; *v = m->now++; return 1; }
static void setup(struct model *m, struct a733_dsi_video *d, unsigned controller, unsigned burst, unsigned format)
{
    memset(m, 0, sizeof(*m)); memset(d, 0, sizeof(*d));
    m->controller = controller; m->driver = d; m->ready = 1;
    m->regs[0] = 1; m->regs[4] = 0x30000;
    struct a733_dsi_video_config c = {.ops = {admit, read32, write32, barrier, now},
        .context = m, .lease = 23, .controller = controller, .timing = timing(burst, format)};
    assert(a733_dsi_video_init(d, &c, 1000000));
}
static void drive(struct model *m, struct a733_dsi_video *d)
{
    for (unsigned n = 0; d->state == A733_DSI_VIDEO_PENDING; ++n) {
        assert(n < 1000);
        unsigned before = m->calls;
        a733_dsi_video_poll(d);
        assert(m->calls - before <= 1);
    }
    unsigned before = m->calls; uint64_t t = m->now;
    assert(a733_dsi_video_poll(d) == d->state);
    assert(before == m->calls && t == m->now);
}
static void start(struct model *m, struct a733_dsi_video *d)
{
    m->producer = 1;
    assert(a733_dsi_video_request(d, A733_DSI_VIDEO_START, m->now + 10000));
    drive(m, d);
}
static void check_graph(uint32_t jumps)
{
    unsigned id = 0, saw_exit = 0;
    for (unsigned i = 0; i != 16; ++i) {
        assert(id < 8);
        if (id == 6) id = 5; /* Conditional source stop branch. */
        else id = (jumps >> (id * 4)) & 15;
        if (id == 5) saw_exit = 1;
        if (id == 15) { assert(saw_exit); return; }
    }
    assert(!"continuous clock stop must reach END");
}
static void plans(void)
{
    struct model m; struct a733_dsi_video d;
    for (unsigned controller = 0; controller != 2; ++controller)
    for (unsigned burst = 0; burst != 2; ++burst)
    for (unsigned format = 0; format != 2; ++format) {
        setup(&m, &d, controller, burst, format);
        uint32_t before[256]; memcpy(before, m.regs, sizeof(before));
        drive(&m, &d); assert(d.state == A733_DSI_VIDEO_READY);
        assert(d.plan.count <= A733_DSI_VIDEO_WORDS);
        video_reference(&d.config.timing, before, m.regs);
        memcpy(before, m.regs, sizeof(before));
        start(&m, &d); assert(d.state == A733_DSI_VIDEO_ACTIVE && m.start_writes == 1);
        video_reference_start(before, m.regs); check_graph(m.regs[0x48 / 4]);
        assert(a733_dsi_video_request(&d, A733_DSI_VIDEO_OBSERVE, m.now + 10000));
        drive(&m, &d); assert(d.state == A733_DSI_VIDEO_ACTIVE);
        memcpy(before, m.regs, sizeof(before));
        assert(a733_dsi_video_request(&d, A733_DSI_VIDEO_STOP, m.now + 10000));
        drive(&m, &d); assert(d.state == A733_DSI_VIDEO_STOPPED && m.stopped && m.stop_reads == 3);
        video_reference_stop(before, m.regs);
        assert(!a733_dsi_video_request(&d, A733_DSI_VIDEO_START, m.now + 10000));
    }
    struct a733_dsi_video_timing t = timing(0, 0);
    struct a733_dsi_video_plan out, prior;
    memset(&out, 0x5a, sizeof(out)); prior = out;
    for (unsigned i = 0; i != 11; ++i) {
        t = timing(0, 0);
        switch (i) {
        case 0: t.lanes = 3; break;
        case 1: t.format = 2; break;
        case 2: t.mode = 2; break;
        case 3: t.hsync = 3; break;
        case 4: t.hback = 4; break;
        case 5: t.hfront = 19; break;
        case 6: t.hs_hz++; break;
        case 7: t.ls_hz++; break;
        case 8: t.vback = 65535; break;
        case 9: t.hfront = 65535; break;
        default: t.pixel_hz++; break;
        }
        assert(!a733_dsi_video_plan(&t, &out)); assert(!memcmp(&out, &prior, sizeof(out)));
    }
    /* Exercise the smallest admitted porches and DRQ boundary with source math. */
    for (unsigned burst = 0; burst != 2; ++burst) {
        setup(&m, &d, 0, burst, 0);
        t = timing(burst, 0); t.hsync = 4; t.hback = 5; t.hfront = 20;
        d.config.timing = t; assert(a733_dsi_video_plan(&t, &d.plan));
        uint32_t before[256]; memcpy(before, m.regs, sizeof(before));
        drive(&m, &d); assert(d.state == A733_DSI_VIDEO_READY);
        video_reference(&t, before, m.regs);
    }
}
static void faults(void)
{
    struct model m; struct a733_dsi_video d;
    /* Admission, baseline, read/write/readback, barriers, active and stop polls. */
    static const unsigned boundaries[] = {1, 4, 10, 11, 12, 134, 135, 136};
    for (unsigned kind = 0; kind != 3; ++kind)
    for (unsigned i = 0; i != sizeof(boundaries)/sizeof(*boundaries); ++i) {
        setup(&m, &d, 0, 0, 0);
        unsigned target = boundaries[i];
        if (kind == 0) m.fail_at = target;
        if (kind == 1) m.late_at = target;
        if (kind == 2) m.reenter_at = target;
        drive(&m, &d); assert(d.state == A733_DSI_VIDEO_RETAINED && d.config.lease == 23);
    }
    for (unsigned action = A733_DSI_VIDEO_START; action <= A733_DSI_VIDEO_OBSERVE; ++action) {
        unsigned count = action == A733_DSI_VIDEO_START ? 12 : action == A733_DSI_VIDEO_STOP ? 12 : 3;
        for (unsigned kind = 0; kind != 2; ++kind)
        for (unsigned n = 1; n <= count; ++n) {
            setup(&m, &d, 1, 1, 1); drive(&m, &d);
            if (action != A733_DSI_VIDEO_START) { start(&m, &d); assert(d.state == A733_DSI_VIDEO_ACTIVE); }
            m.producer = 1;
            if (kind == 0) m.fail_at = m.calls + n; else m.late_at = m.calls + n;
            assert(a733_dsi_video_request(&d, action, m.now + 10000));
            drive(&m, &d); assert(d.state == A733_DSI_VIDEO_RETAINED);
        }
    }
    setup(&m, &d, 0, 0, 0); m.ready = 0; drive(&m, &d);
    assert(d.state == A733_DSI_VIDEO_RETAINED && !d.writes_attempted);
    setup(&m, &d, 0, 0, 0); m.regs[4] |= 1; drive(&m, &d);
    assert(d.state == A733_DSI_VIDEO_RETAINED && !d.writes_attempted);
    setup(&m, &d, 0, 0, 0); m.corrupt_at = 12; drive(&m, &d);
    assert(d.error == A733_DSI_VIDEO_VERIFY);
    setup(&m, &d, 0, 0, 0); drive(&m, &d);
    assert(a733_dsi_video_request(&d, A733_DSI_VIDEO_START, m.now + 10000));
    drive(&m, &d); assert(d.error == A733_DSI_VIDEO_AUTHORITY && !m.start_writes);
    setup(&m, &d, 0, 0, 0); drive(&m, &d); start(&m, &d);
    m.regs[0x2f0 / 4] = 2;
    assert(a733_dsi_video_request(&d, A733_DSI_VIDEO_OBSERVE, m.now + 1000));
    drive(&m, &d); assert(d.error == A733_DSI_VIDEO_ENGINE);
    setup(&m, &d, 0, 0, 0); drive(&m, &d); start(&m, &d); m.stuck = 1;
    assert(a733_dsi_video_request(&d, A733_DSI_VIDEO_STOP, m.now + 100));
    drive(&m, &d); assert(d.error == A733_DSI_VIDEO_TIME && !m.stopped);
}
static void admission(void)
{
    struct model m; struct a733_dsi_video d;
    static const unsigned bits[] = {16, 23, 24, 26, 28};
    for (unsigned i = 0; i != sizeof(bits)/sizeof(*bits); ++i) {
        setup(&m, &d, 0, 0, 0);
        m.regs[(i == 4 ? 0x2f8 : 0xc) / 4] |= 1u << bits[i];
        drive(&m, &d);
        assert(d.error == A733_DSI_VIDEO_INITIAL_STATE && !d.writes_attempted);
    }
    setup(&m, &d, 0, 0, 0); drive(&m, &d);
    assert(d.state == A733_DSI_VIDEO_READY && m.calls == 136);
    for (unsigned kind = 0; kind != 3; ++kind) {
        setup(&m, &d, 0, 0, 0);
        if (!kind) m.fail_at = 136;
        else if (kind == 1) m.late_at = 136;
        else m.reenter_at = 136;
        drive(&m, &d); assert(d.state == A733_DSI_VIDEO_RETAINED);
    }
    puts("PASS: ready threshold/selector and FIFO BIST refused before writes; accepted baseline and final barrier fault/deadline/reentry");
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "admission")) { admission(); return 0; }
    plans(); faults();
    puts("PASS: ten source timing images; both controllers/formats/modes; continuous stop graph; producer authority; bounded prepare/start/observe/stop and failure retention");
    return 0;
}
