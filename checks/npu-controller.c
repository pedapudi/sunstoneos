/* Stateful MMIO and noncoherent-buffer model; no physical NPU emulation. */
#include "a733_npu_controller.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define IDLE 0x7fffffffu
static _Alignas(4096) unsigned char tables[4096 + 2 * 16384];
static _Alignas(64) unsigned char bootstrap[256];
static struct a733_npu_mmu_image mmu;
static int boot_resource, task_resource;
static const struct a733_npu_command boot = {0x10000000, 524288, &boot_resource};
static const struct a733_npu_command task = {0x10080000, 524288, &task_resource};
struct device {
    uint32_t reg[0x800 / 4];
    uint64_t now, launched, event_after, idle_after;
    unsigned reads, writes, barriers, published, finished, triggers, resets;
    unsigned active, event_sent, cancel_pulse, cancel_done, refuse_cancel;
    unsigned io_fail_at, calls, failed, ignore_write, ignore_start, finish_fail, publish_fail;
    unsigned inject_late_fault;
    unsigned char cpu_input[64], device_input[64], cpu_output[64], device_output[64];
};

static void advance(struct device *d)
{
    if (!d->active) return;
    uint64_t elapsed = d->now - d->launched;
    if (!d->event_sent && elapsed >= d->event_after) {
        for (unsigned i = 0; i < 64; ++i) d->device_output[i] = d->device_input[i] ^ 0x5a;
        d->reg[0x10/4] |= 1; d->event_sent = 1;
    }
    if (elapsed >= d->idle_after) { d->reg[1] = IDLE; d->active = 0; }
}
static int access_ok(struct device *d)
{
    assert(!d->failed); /* No access may follow the first transport failure. */
    if (++d->calls == d->io_fail_at) { d->failed = 1; return 0; }
    return 1;
}
static int read32(void *cookie, unsigned at, uint32_t *value)
{
    struct device *d = cookie;
    if (!access_ok(d)) return 0;
    assert(at < sizeof(d->reg) && !(at & 3)); ++d->reads; advance(d);
    *value = d->reg[at/4];
    if (at == 0x10) d->reg[at/4] = 0; /* Event status acknowledges on read. */
    if (at == 4 && d->inject_late_fault && *value == IDLE) {
        d->reg[0x10/4] |= 0x40000000; d->inject_late_fault = 0;
    }
    return 1;
}
static int write32(void *cookie, unsigned at, uint32_t value)
{
    struct device *d = cookie;
    if (!access_ok(d)) return 0;
    assert(at < sizeof(d->reg) && !(at & 3)); ++d->writes;
    if (at == d->ignore_write) return 1;
    d->reg[at/4] = value;
    if (at == 0x3a8) {
        if (value & 1) { ++d->resets; d->reg[0x388/4] = 0; d->reg[1] = IDLE; }
        if (value & 8) d->cancel_pulse = 1;
        else if (d->cancel_pulse && !d->refuse_cancel) {
            d->cancel_done = 1; d->reg[at/4] |= 16; d->reg[1] = IDLE; d->active = 0;
        }
    }
    if (at == 0x3a4) {
        assert(value == 0x1ffff && d->reg[0x388/4] == 0x21);
        assert(d->reg[0x3b4/4] == mmu.pd_entry);
        assert(d->published && d->barriers);
        assert(d->reg[0x654/4] == boot.device || d->reg[0x654/4] == task.device);
        ++d->triggers;
        if (!d->ignore_start) {
            d->active = 1; d->event_sent = 0; d->launched = d->now; d->reg[1] = 0;
        }
    }
    return 1;
}
static uint64_t clock_us(void *cookie) { return ((struct device *)cookie)->now; }
static void delay_us(void *cookie, unsigned us) { ((struct device *)cookie)->now += us; }
static void barrier(void *cookie) { ++((struct device *)cookie)->barriers; }
static int publish(void *cookie, const struct a733_npu_mmu_image *selected,
                   const struct a733_npu_command *cmd, int init)
{
    struct device *d = cookie;
    assert(cmd->token == (init ? &boot_resource : &task_resource));
    assert(selected->physical == mmu.physical && selected->bytes == mmu.bytes &&
           selected->pd_entry == mmu.pd_entry);
    ++d->published;
    memcpy(d->device_input, d->cpu_input, 64);
    memcpy(d->device_output, d->cpu_output, 64);
    return !d->publish_fail;
}
static int finish(void *cookie, const struct a733_npu_command *cmd, int canceled)
{
    struct device *d = cookie;
    assert(cmd->token && d->reg[1] == IDLE);
    assert(canceled ? d->cancel_done : d->event_sent);
    if (d->finish_fail) return 0;
    ++d->finished; memcpy(d->cpu_output, d->device_output, 64); return 1;
}
static const struct a733_npu_ops ops = {read32, write32, clock_us, delay_us, barrier, publish, finish};

static void fresh(struct device *d, struct a733_npu_controller *c)
{
    memset(d, 0, sizeof(*d)); memset(c, 0, sizeof(*c));
    d->reg[0x20/4] = 0x9000; d->reg[0x24/4] = 0x9202;
    d->reg[0x30/4] = 0x1000003b; d->reg[0x28/4] = 0x20240101;
    d->reg[1] = IDLE; d->ignore_write = UINT32_MAX;
    d->event_after = 20; d->idle_after = 40;
    memset(d->cpu_input, 0x12, 64); memset(d->cpu_output, 0xa5, 64);
}
static void started(struct device *d, struct a733_npu_controller *c)
{
    fresh(d, c);
    assert(a733_npu_open(c, &ops, d, &mmu, &boot, 1000));
    assert(c->state == A733_NPU_BOOTING && d->resets == 2);
    d->now += 40;
    assert(a733_npu_poll(c) == A733_NPU_READY);
    assert(d->finished == 1);
}
static void retained(struct device *d, struct a733_npu_controller *c)
{
    assert(c->state == A733_NPU_FAULT);
    unsigned calls = d->calls;
    void *token = NULL; int canceled = -1;
    assert(a733_npu_poll(c) == A733_NPU_FAULT);
    assert(!a733_npu_cancel(c, 1000) && !a733_npu_close(c));
    assert(!a733_npu_submit(c, &task, 1000));
    assert(!a733_npu_open(c, &ops, d, &mmu, &boot, 1000));
    assert(!a733_npu_reap(c, &token, &canceled));
    assert(d->calls == calls && token == NULL && canceled == -1);
}
static uint32_t word(unsigned at)
{
    return bootstrap[at] | ((uint32_t)bootstrap[at+1] << 8) |
           ((uint32_t)bootstrap[at+2] << 16) | ((uint32_t)bootstrap[at+3] << 24);
}
static void check_bootstrap(void)
{
    memset(bootstrap, 0xa5, sizeof(bootstrap));
    assert(!a733_npu_bootstrap(bootstrap, A733_NPU_BOOTSTRAP_BYTES - 1));
    for (unsigned i = 0; i < sizeof(bootstrap); ++i) assert(bootstrap[i] == 0xa5);
    assert(a733_npu_bootstrap(bootstrap, sizeof(bootstrap)));
    unsigned banks = 0, sram = 0, event = 0, flush = 0, ended = 0, stalls = 0;
    for (unsigned at = 0; at < A733_NPU_BOOTSTRAP_BYTES; at += 8) {
        uint32_t op = word(at), data = word(at + 4);
        if (ended) { assert(op == 0 && data == 0); continue; }
        switch (op >> 27) {
        case 1: {
            assert(((op >> 16) & 0x3ff) == 1);
            unsigned reg = op & 0xffff;
            if (reg == 0x61) { assert(data == 0xffffff7f && at == 0); ++flush; }
            if (reg == 0xe55) { assert(data == (0x03010000 | banks)); ++banks; }
            if (reg == 0xe4e) { assert(data == 0x400000); ++sram; }
            if (reg == 0xe01) { assert(data == 0x40); ++event; }
            break;
        }
        case 9: assert(data == 0x701 || data == 0x30000701); ++stalls; break;
        case 2: assert(data == 0 && banks == 8 && sram == 1 && event == 1 && flush == 1); ended = 1; break;
        default: assert(0);
        }
    }
    assert(ended && stalls == 3);
    for (unsigned i = A733_NPU_BOOTSTRAP_BYTES; i < sizeof(bootstrap); ++i) assert(bootstrap[i] == 0xa5);
}

static void check_mmu_control(void)
{
    /* Every single-bit deviation must refuse submission, visibility and close.
     * Device behavior is modeled; these checks do not identify bypass fields. */
    for (unsigned bit = 0; bit < 32; ++bit) {
        struct device d; struct a733_npu_controller c;
        started(&d, &c); d.reg[0x388/4] ^= UINT32_C(1) << bit;
        assert(!a733_npu_submit(&c, &task, 1000));
        assert(c.fault == A733_NPU_REGISTER && d.triggers == 1 && d.finished == 1);
        retained(&d, &c);
        started(&d, &c); assert(a733_npu_submit(&c, &task, 1000));
        d.reg[0x388/4] ^= UINT32_C(1) << bit; d.now += 40;
        assert(a733_npu_poll(&c) == A733_NPU_FAULT);
        assert(c.fault == A733_NPU_REGISTER && d.finished == 1);
        retained(&d, &c);
        started(&d, &c); d.reg[0x388/4] ^= UINT32_C(1) << bit;
        unsigned writes = d.writes;
        assert(!a733_npu_close(&c) && c.fault == A733_NPU_REGISTER);
        assert(d.writes == writes); retained(&d, &c);
    }
    puts("PASS NPU MMU control: all single-bit deviations retain submission, completion and close");
}

int main(int argc, char **argv)
{
    struct a733_npu_region region = {0x51000000, 1048576, 0x10000000, A733_NPU_PAGE_4K, 0};
    assert(a733_npu_mmu_build(tables, sizeof(tables), 0x49000000, &region, 1, &mmu));
    if (argc == 2 && !strcmp(argv[1], "--mmu-control-only")) {
        check_mmu_control(); return 0;
    }
    assert(argc == 1);
    check_bootstrap();
    struct device d; struct a733_npu_controller c;
    started(&d, &c);
    memset(d.cpu_output, 0xa5, 64); memset(d.cpu_input, 0x37, 64);
    assert(a733_npu_submit(&c, &task, 1000));
    assert(!a733_npu_submit(&c, &task, 1000) && !a733_npu_close(&c));
    d.now += 20; assert(a733_npu_poll(&c) == A733_NPU_RUNNING);
    assert(d.finished == 1 && d.cpu_output[0] == 0xa5); /* event precedes bus drain */
    d.now += 20; assert(a733_npu_poll(&c) == A733_NPU_DONE);
    assert(d.cpu_output[0] == (0x37 ^ 0x5a) && d.finished == 2);
    assert(a733_npu_poll(&c) == A733_NPU_DONE && d.finished == 2);
    void *token = NULL; int canceled = -1;
    assert(a733_npu_reap(&c, &token, &canceled) && token == &task_resource && !canceled);
    assert(!a733_npu_reap(&c, &token, &canceled));
    assert(a733_npu_close(&c) && c.state == A733_NPU_CLOSED);

    started(&d, &c); d.now = UINT64_MAX - 10;
    assert(a733_npu_submit(&c, &task, 1000)); d.now += 40;
    assert(a733_npu_poll(&c) == A733_NPU_DONE); /* wrapping monotonic clock */

    started(&d, &c); d.ignore_start = 1;
    assert(a733_npu_submit(&c, &task, 100)); d.now += 50;
    assert(a733_npu_poll(&c) == A733_NPU_RUNNING); /* idle alone is insufficient */
    d.now += 50; assert(a733_npu_poll(&c) == A733_NPU_FAULT);
    assert(c.fault == A733_NPU_TIMEOUT && d.finished == 1); retained(&d, &c);

    started(&d, &c); assert(a733_npu_submit(&c, &task, 1000));
    d.reg[0x10/4] = 0x80000001; assert(a733_npu_poll(&c) == A733_NPU_FAULT);
    assert(c.fault == A733_NPU_IRQ && d.finished == 1); retained(&d, &c);
    started(&d, &c); assert(a733_npu_submit(&c, &task, 1000));
    d.inject_late_fault = 1; d.now += 40;
    assert(a733_npu_poll(&c) == A733_NPU_FAULT && d.finished == 1); retained(&d, &c);

    started(&d, &c); d.reg[0x10/4] = 1;
    assert(!a733_npu_submit(&c, &task, 1000));
    assert(c.fault == A733_NPU_IRQ && d.triggers == 1); retained(&d, &c);
    started(&d, &c); d.reg[0x3b4/4] ^= 16;
    assert(!a733_npu_submit(&c, &task, 1000));
    assert(c.fault == A733_NPU_REGISTER && d.triggers == 1); retained(&d, &c);

    started(&d, &c); d.event_after = d.idle_after = 10000;
    assert(a733_npu_submit(&c, &task, 1000)); assert(a733_npu_cancel(&c, 500));
    unsigned writes = d.writes; d.now += 99;
    assert(a733_npu_poll(&c) == A733_NPU_CANCEL_ASSERTED && d.writes == writes);
    d.now++; assert(a733_npu_poll(&c) == A733_NPU_CANCEL_WAIT);
    assert(a733_npu_poll(&c) == A733_NPU_CANCELED);
    assert(a733_npu_reap(&c, &token, &canceled) && canceled && token == &task_resource);
    assert(c.state == A733_NPU_DRAINED && !a733_npu_submit(&c, &task, 1000));
    assert(a733_npu_close(&c));

    started(&d, &c); d.event_after = d.idle_after = 10000;
    assert(a733_npu_submit(&c, &task, 1000) && a733_npu_cancel(&c, 500));
    d.now += 100; assert(a733_npu_poll(&c) == A733_NPU_CANCEL_WAIT);
    d.reg[1] = 0; /* Cancellation acknowledged, but memory is still busy. */
    assert(a733_npu_poll(&c) == A733_NPU_CANCEL_WAIT && d.finished == 1);
    d.now += 400; assert(a733_npu_poll(&c) == A733_NPU_FAULT); retained(&d, &c);

    started(&d, &c); d.refuse_cancel = 1; d.event_after = d.idle_after = 10000;
    assert(a733_npu_submit(&c, &task, 1000) && a733_npu_cancel(&c, 500));
    d.now += 100; assert(a733_npu_poll(&c) == A733_NPU_CANCEL_WAIT);
    d.now += 400; assert(a733_npu_poll(&c) == A733_NPU_FAULT);
    assert(d.finished == 1); retained(&d, &c);

    started(&d, &c); d.io_fail_at = d.calls + 2;
    assert(!a733_npu_submit(&c, &task, 1000));
    assert(c.fault == A733_NPU_IO); retained(&d, &c);
    started(&d, &c); assert(a733_npu_submit(&c, &task, 1000));
    d.finish_fail = 1; d.now += 40;
    assert(a733_npu_poll(&c) == A733_NPU_FAULT && d.finished == 1); retained(&d, &c);
    started(&d, &c); d.publish_fail = 1;
    assert(!a733_npu_submit(&c, &task, 1000) && d.triggers == 1); retained(&d, &c);
    started(&d, &c); d.ignore_write = 0x388;
    assert(!a733_npu_close(&c) && c.fault == A733_NPU_REGISTER); retained(&d, &c);

    fresh(&d, &c); d.reg[0x30/4]++;
    assert(!a733_npu_open(&c, &ops, &d, &mmu, &boot, 1000));
    assert(c.fault == A733_NPU_IDENTITY && !d.resets && !d.triggers); retained(&d, &c);
    fresh(&d, &c); assert(!a733_npu_open(&c, &ops, &d, &mmu, &boot, 1));
    assert(c.fault == A733_NPU_TIMEOUT && !d.triggers); retained(&d, &c);
    fresh(&d, &c); struct a733_npu_command bad = boot;
    bad.readable_bytes = A733_NPU_FETCH_BYTES - 1;
    assert(!a733_npu_open(&c, &ops, &d, &mmu, &bad, 1000));
    assert(!d.calls && c.state == A733_NPU_COLD);
    bad = boot; bad.device = 0x400000;
    assert(!a733_npu_open(&c, &ops, &d, &mmu, &bad, 1000) && !d.calls);
    puts("PASS NPU controller: startup, event and idle completion, cancellation, retained faults");
    return 0;
}
