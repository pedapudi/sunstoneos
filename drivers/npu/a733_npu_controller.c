/****************************************************************************
*
*    The MIT License (MIT)
*
*    Copyright (c) 2017 - 2024 Vivante Corporation
*
*    Permission is hereby granted, free of charge, to any person obtaining a
*    copy of this software and associated documentation files (the "Software"),
*    to deal in the Software without restriction, including without limitation
*    the rights to use, copy, modify, merge, publish, distribute, sublicense,
*    and/or sell copies of the Software, and to permit persons to whom the
*    Software is furnished to do so, subject to the following conditions:
*
*    The above copyright notice and this permission notice shall be included in
*    all copies or substantial portions of the Software.
*
*    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
*    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
*    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
*    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
*    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
*    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
*    DEALINGS IN THE SOFTWARE.
*
*****************************************************************************/
/* Register sequences adapted from the pinned VIPLite 2 hardware/device code.
 * The native state machine owns bounded polling and retains resources on fault. */
#include "a733_npu_controller.h"

#define IDLE_ALL UINT32_C(0x7fffffff)
#define EVENT_DONE 1u
#define IRQ_ENABLE UINT32_C(0xc0000001)
#define SRAM_BASE UINT32_C(0x400000)

static int fail(struct a733_npu_controller *c, enum a733_npu_fault why)
{
    c->state = A733_NPU_FAULT;
    c->fault = why;
    return 0;
}
static int read_reg(struct a733_npu_controller *c, unsigned at, uint32_t *value)
{
    if (c->state == A733_NPU_FAULT) return 0;
    return c->ops.read(c->cookie, at, value) ? 1 : fail(c, A733_NPU_IO);
}
static int write_reg(struct a733_npu_controller *c, unsigned at, uint32_t value)
{
    if (c->state == A733_NPU_FAULT) return 0;
    return c->ops.write(c->cookie, at, value) ? 1 : fail(c, A733_NPU_IO);
}
static int verify(struct a733_npu_controller *c, unsigned at, uint32_t expected, uint32_t mask)
{
    uint32_t value;
    if (!read_reg(c, at, &value)) return 0;
    return (value & mask) == expected ? 1 : fail(c, A733_NPU_REGISTER);
}
static int idle(struct a733_npu_controller *c)
{
    if (!read_reg(c, 4, &c->last_idle)) return 0;
    return c->last_idle == IDLE_ALL ? 1 : fail(c, A733_NPU_NOT_IDLE);
}
static int duration(uint64_t timeout)
{
    return timeout && timeout <= INT64_MAX;
}
static int command_valid(const struct a733_npu_command *cmd)
{
    return cmd && cmd->token && cmd->device && !(cmd->device & 63u) &&
        cmd->readable_bytes >= A733_NPU_FETCH_BYTES &&
        cmd->readable_bytes <= (UINT64_C(1) << 32) - cmd->device &&
        ((uint64_t)cmd->device + cmd->readable_bytes <= SRAM_BASE || cmd->device >= SRAM_BASE + 0x80000u);
}
static int deadline(struct a733_npu_controller *c)
{
    return c->ops.microseconds(c->cookie) - c->start < c->timeout ?
           1 : fail(c, A733_NPU_TIMEOUT);
}
static int check_configuration(struct a733_npu_controller *c)
{
    /* The pinned page-directory setup writes exactly 0x21. Reject unsupported
     * extra control state; this readback does not prove engine confinement. */
    return verify(c, 0x3b4, c->mmu.pd_entry, UINT32_MAX) &&
           verify(c, 0x388, 0x21, UINT32_MAX) && verify(c, 0x14, IRQ_ENABLE, UINT32_MAX);
}
static int trigger(struct a733_npu_controller *c)
{
    uint32_t pending;
    if (!check_configuration(c) || !idle(c) || !read_reg(c, 0x10, &pending)) return 0;
    if (pending) return fail(c, A733_NPU_IRQ);
    if (!deadline(c) || !write_reg(c, 0x654, c->job.device)) return 0;
    c->ops.barrier(c->cookie);
    return write_reg(c, 0x3a4, 0x1ffff);
}

static int reset(struct a733_npu_controller *c)
{
    /* The reference requires two successful reset passes. A failed pass ends
     * this attempt; there is no reset retry loop around an unresolved failure. */
    for (unsigned pass = 0; pass < 2; ++pass) {
        if (!deadline(c) || !write_reg(c, 0x00, 0x70900) ||
            !write_reg(c, 0x104, 0) || !write_reg(c, 0x10c, 0x15a0881) ||
            !write_reg(c, 0x10c, 0x15a0880) || !write_reg(c, 0, 0x70b00)) return 0;
        c->ops.delay(c->cookie, 5);
        if (!write_reg(c, 0, 0x70900)) return 0;
        c->ops.delay(c->cookie, 20);
        if (!write_reg(c, 0, 0xf0900) || !write_reg(c, 0x3a8, 1)) return 0;
        c->ops.delay(c->cookie, 20);
        if (!write_reg(c, 0x3a8, 0)) return 0;
        c->ops.delay(c->cookie, 40);
        if (!write_reg(c, 0, 0xf0900) || !write_reg(c, 0, 0x70900)) return 0;
        c->ops.delay(c->cookie, 50);
        if (!verify(c, 4, 1, 1) || !verify(c, 0, 0x30000, 0x30000) ||
            !verify(c, 0x388, 0, 1)) return 0;
    }
    uint32_t value;
    if (!write_reg(c, 0x55c, 0xffffff) || !read_reg(c, 0x90, &value) ||
        !write_reg(c, 0x90, value & ~64u)) return 0;
    return idle(c);
}

int a733_npu_open(struct a733_npu_controller *c, const struct a733_npu_ops *ops, void *cookie,
                  const struct a733_npu_mmu_image *mmu, const struct a733_npu_command *boot,
                  uint64_t timeout)
{
    if (!c || (c->state != A733_NPU_COLD && c->state != A733_NPU_CLOSED) || !ops ||
        !ops->read || !ops->write || !ops->microseconds || !ops->delay ||
        !ops->barrier || !ops->publish || !ops->finish || !mmu ||
        !command_valid(boot) || !duration(timeout) || mmu->physical >= (UINT64_C(1) << 40) ||
        (mmu->physical & 4095u) || mmu->bytes < 4096 || mmu->bytes > A733_NPU_MMU_MAX_BYTES ||
        (mmu->bytes & 63u) || (mmu->physical >> 32) != ((mmu->physical + mmu->bytes - 1) >> 32) ||
        mmu->pd_entry != ((uint32_t)((mmu->physical >> 12) << 4) | 1u)) return 0;
    c->ops = *ops; c->cookie = cookie; c->job = *boot; c->mmu = *mmu;
    c->state = A733_NPU_BOOTING; c->fault = A733_NPU_NO_FAULT; c->bootstrap = 1;
    c->irq_seen = 0; c->start = ops->microseconds(cookie); c->timeout = timeout;
    if (!write_reg(c, 0, 0x70900) || !read_reg(c, 0x20, &c->chip_model) ||
        !read_reg(c, 0x24, &c->chip_revision) || !read_reg(c, 0x30, &c->chip_product) ||
        !read_reg(c, 0x28, &c->chip_date)) return 0;
    if (c->chip_product != 0x1000003b || c->chip_revision != 0x9202)
        return fail(c, A733_NPU_IDENTITY);
    if (!c->ops.publish(cookie, &c->mmu, &c->job, 1)) return fail(c, A733_NPU_MEMORY);
    c->ops.barrier(cookie);
    if (!reset(c)) return 0;
    uint32_t value;
    if (!write_reg(c, 0, 0x70900) || !write_reg(c, 0, 0x70100) ||
        !write_reg(c, 0x3a8, 2) || !write_reg(c, 0x3c, UINT32_MAX) ||
        !write_reg(c, 0x3c, 0) || !write_reg(c, 0x100, 0x140021) ||
        !write_reg(c, 0x3b4, c->mmu.pd_entry) || !write_reg(c, 0x388, 0x21) ||
        !write_reg(c, 0x104, 0) || !read_reg(c, 0x10c, &value) ||
        !write_reg(c, 0x10c, (value & ~0x10000u) | 0x20000u) ||
        !read_reg(c, 0x90, &value) || !write_reg(c, 0x90, value | 64u) ||
        !read_reg(c, 0x414, &value) || !write_reg(c, 0x414, value & ~1u) ||
        !write_reg(c, 0x14, IRQ_ENABLE) || !read_reg(c, 0x10, &value)) return 0;
    /* Drain reset-era status once. trigger requires a subsequent clear read. */
    c->ops.delay(cookie, 10);
    return trigger(c);
}

int a733_npu_submit(struct a733_npu_controller *c, const struct a733_npu_command *cmd,
                    uint64_t timeout)
{
    if (!c || c->state != A733_NPU_READY || !command_valid(cmd) || !duration(timeout)) return 0;
    c->job = *cmd; c->bootstrap = 0; c->irq_seen = 0;
    c->start = c->ops.microseconds(c->cookie); c->timeout = timeout;
    c->state = A733_NPU_RUNNING;
    if (!c->ops.publish(c->cookie, &c->mmu, &c->job, 0)) return fail(c, A733_NPU_MEMORY);
    c->ops.barrier(c->cookie);
    return trigger(c);
}

int a733_npu_cancel(struct a733_npu_controller *c, uint64_t timeout)
{
    if (!c || (c->state != A733_NPU_RUNNING && c->state != A733_NPU_BOOTING) ||
        !duration(timeout) || timeout <= 100) return 0;
    c->cancel_start = c->ops.microseconds(c->cookie); c->cancel_timeout = timeout;
    c->state = A733_NPU_CANCEL_ASSERTED;
    uint32_t value;
    if (!read_reg(c, 0x3a8, &value) || !write_reg(c, 0x3a8, value | 8u)) return 0;
    return 1;
}

enum a733_npu_state a733_npu_check_timeout(struct a733_npu_controller *c)
{
    if(!c)return A733_NPU_FAULT;
    int canceled=c->state==A733_NPU_CANCEL_ASSERTED || c->state==A733_NPU_CANCEL_WAIT;
    if(canceled || c->state==A733_NPU_BOOTING || c->state==A733_NPU_RUNNING) {
        uint64_t elapsed=c->ops.microseconds(c->cookie)-(canceled?c->cancel_start:c->start);
        if(elapsed >= (canceled?c->cancel_timeout:c->timeout))fail(c,A733_NPU_TIMEOUT);
    }
    return c->state;
}

enum a733_npu_state a733_npu_advance(struct a733_npu_controller *c, int sample_events)
{
    if (!c) return A733_NPU_FAULT;
    if (c->state != A733_NPU_BOOTING && c->state != A733_NPU_RUNNING &&
        c->state != A733_NPU_CANCEL_ASSERTED && c->state != A733_NPU_CANCEL_WAIT) {
        /* An unexpected event between jobs is never attributed to the next job. */
        if (sample_events && c->state != A733_NPU_FAULT && c->state != A733_NPU_CLOSED) {
            uint32_t pending;
            if (c->state == A733_NPU_COLD) fail(c, A733_NPU_IRQ);
            else if (read_reg(c, 0x10, &pending) && pending) fail(c, A733_NPU_IRQ);
        }
        return c->state;
    }
    int canceled = c->state == A733_NPU_CANCEL_ASSERTED || c->state == A733_NPU_CANCEL_WAIT;
    uint64_t elapsed = c->ops.microseconds(c->cookie) - (canceled ? c->cancel_start : c->start);
    if (elapsed >= (canceled ? c->cancel_timeout : c->timeout)) {
        fail(c, A733_NPU_TIMEOUT); return c->state;
    }
    if (!check_configuration(c)) return c->state;
    uint32_t value;
    if (sample_events) {
        if (!read_reg(c, 0x10, &value)) return c->state;
        c->irq_seen |= value;
    }
    if (c->irq_seen & ~EVENT_DONE) { fail(c, A733_NPU_IRQ); return c->state; }
    if (c->state == A733_NPU_CANCEL_ASSERTED) {
        if (elapsed < 100) return c->state;
        if (!read_reg(c, 0x3a8, &value) || !write_reg(c, 0x3a8, value & ~8u)) return c->state;
        c->state = A733_NPU_CANCEL_WAIT;
        return c->state;
    }
    if (canceled) {
        if (!read_reg(c, 0x3a8, &value) || !(value & 16u)) return c->state;
    } else if (!(c->irq_seen & EVENT_DONE)) return c->state;
    if (!read_reg(c, 4, &c->last_idle) || c->last_idle != IDLE_ALL) return c->state;
    if (!read_reg(c, 0x10, &value)) return c->state;
    c->irq_seen |= value;
    if (c->irq_seen & ~EVENT_DONE) { fail(c, A733_NPU_IRQ); return c->state; }
    c->ops.barrier(c->cookie);
    if (!c->ops.finish(c->cookie, &c->job, canceled)) {
        fail(c, A733_NPU_MEMORY); return c->state;
    }
    if (canceled) c->state = A733_NPU_CANCELED;
    else if (c->bootstrap) { c->job.token = 0; c->state = A733_NPU_READY; }
    else c->state = A733_NPU_DONE;
    return c->state;
}

enum a733_npu_state a733_npu_poll(struct a733_npu_controller *c)
{
    if(!c)return A733_NPU_FAULT;
    if(c->state!=A733_NPU_BOOTING && c->state!=A733_NPU_RUNNING &&
       c->state!=A733_NPU_CANCEL_ASSERTED && c->state!=A733_NPU_CANCEL_WAIT)return c->state;
    return a733_npu_advance(c,1);
}

int a733_npu_reap(struct a733_npu_controller *c, void **token, int *canceled)
{
    if (!c || !token || !canceled || (c->state != A733_NPU_DONE && c->state != A733_NPU_CANCELED)) return 0;
    *token = c->job.token; *canceled = c->state == A733_NPU_CANCELED;
    c->job.token = 0; c->state = *canceled ? A733_NPU_DRAINED : A733_NPU_READY;
    return 1;
}

int a733_npu_close(struct a733_npu_controller *c)
{
    if (!c || (c->state != A733_NPU_READY && c->state != A733_NPU_DRAINED)) return 0;
    uint32_t pending;
    if (!check_configuration(c) || !idle(c) || !read_reg(c, 0x10, &pending)) return 0;
    if (pending) return fail(c, A733_NPU_IRQ);
    if (!write_reg(c, 0x14, 0) ||
        !verify(c, 0x14, 0, UINT32_MAX) || !write_reg(c, 0x388, 0) ||
        !verify(c, 0x388, 0, 1)) return 0;
    c->ops.barrier(c->cookie);
    if (!idle(c)) return 0;
    c->state = A733_NPU_CLOSED;
    return 1;
}

int a733_npu_bootstrap(void *memory, size_t capacity)
{
    if (!memory || ((uintptr_t)memory & 63u) || capacity < A733_NPU_BOOTSTRAP_BYTES ||
        A733_NPU_BOOTSTRAP_BYTES - 1 > UINTPTR_MAX - (uintptr_t)memory) return 0;
    const uint32_t words[] = {
        0x08010061, 0xffffff7f, 0x08010e02, 0x30000701, 0x48000000, 0x30000701,
        0x0801028a, 0x11, 0x08010e13, 2, 0x08010e12, 0x18010000,
        0x08010e55, 0x03010000, 0x08010e55, 0x03010001,
        0x08010e55, 0x03010002, 0x08010e55, 0x03010003,
        0x08010e55, 0x03010004, 0x08010e55, 0x03010005,
        0x08010e55, 0x03010006, 0x08010e55, 0x03010007,
        0x08010e21, 0x20000, 0x08010e02, 0x701, 0x48000000, 0x701,
        0x0801022c, 0x1f, 0x08010e4e, SRAM_BASE,
        0x08010e02, 0x30000701, 0x48000000, 0x30000701,
        0x08010e01, 0x40, 0x10000000, 0,
        0, 0 /* Prefetch padding after END. */
    };
    _Static_assert(sizeof(words) == A733_NPU_BOOTSTRAP_BYTES, "bootstrap size");
    uint8_t *out = memory;
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        out[4*i] = (uint8_t)words[i]; out[4*i+1] = (uint8_t)(words[i] >> 8);
        out[4*i+2] = (uint8_t)(words[i] >> 16); out[4*i+3] = (uint8_t)(words[i] >> 24);
    }
    return 1;
}
