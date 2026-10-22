/* SPDX-License-Identifier: MIT
 * Firmware compatibility, MIPS interrupt and command semantics adapted from
 * pvr_rogue_fwif.h, pvr_fw_mips.c, pvr_device.c, pvr_power.c and pvr_ccb.c.
 * Copyright (c) 2023 Imagination Technologies Ltd. See LICENSE.pvr.
 */
#include "pvr_gpu_control.h"
#include "a733_range.h"
#include <string.h>

static int outside(const struct pvr_gpu_control *s, const void *p, size_t n)
{
    return a733_span_excl_nonempty(p, n) &&
        a733_extents_disjoint_prechecked((uintptr_t)s, sizeof(*s), (uintptr_t)p, n);
}
static uint64_t le(const unsigned char *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static enum pvr_gpu_control_result fail(struct pvr_gpu_control *s, unsigned reason)
{
    s->error = reason; s->phase = PVR_GPU_CONTROL_FAILED;
    (void)pvr_ccb_abort(s->queue);
    return PVR_GPU_CONTROL_FAULT;
}
static int clock_now(struct pvr_gpu_control *s, uint64_t *now)
{
    if (!s->ops.now(s->context, now)) { fail(s, PVR_GPU_CONTROL_IO); return 0; }
    if (*now < s->last_now) { fail(s, PVR_GPU_CONTROL_CLOCK); return 0; }
    s->last_now = *now;
    if (s->phase != PVR_GPU_CONTROL_READY && *now >= s->deadline) {
        fail(s, PVR_GPU_CONTROL_TIMEOUT); return 0;
    }
    return 1;
}
int pvr_gpu_control_init(struct pvr_gpu_control *s, const struct pvr_gpu_start *start,
                         struct pvr_ccb *q, const struct pvr_gpu_control_ops *ops,
                         void *ctx, uint64_t deadline)
{
    if (!a733_span_excl_nonempty(s, sizeof(*s)) || s->phase != PVR_GPU_CONTROL_NEW ||
        !outside(s, start, sizeof(*start)) || !outside(s, q, sizeof(*q)) ||
        !outside(s, ops, sizeof(*ops)) || start->phase != PVR_GPU_START_BOOTED ||
        q->phase != PVR_CCB_PREPARED || !deadline || !ops->now || !ops->read32 ||
        !ops->write32 || !ops->compat) return 0;
    struct pvr_gpu_control result = {0};
    result.ops = *ops; result.queue = q; result.context = ctx; result.deadline = deadline;
    result.last_now = start->last_now; result.phase = PVR_GPU_CONTROL_COMPAT;
    *s = result; return 1;
}
static int compatible(const unsigned char *b)
{
    return b[56] == 1 && le(b, 4) == 3 && le(b + 16, 4) == 3 &&
        le(b + 8, 8) == PVR_FW_A733_BVNC && le(b + 24, 8) == PVR_FW_A733_BVNC;
}
static int pending(struct pvr_gpu_control *s, uint32_t *value)
{
    if (!s->ops.read32(s->context, 0x8a8, value) || *value & ~1u) {
        fail(s, PVR_GPU_CONTROL_IO); return 0;
    }
    return 1;
}
static int known_event(uint32_t type)
{ return type >= (PVR_CCB_MAGIC | 101u) && type <= (PVR_CCB_MAGIC | 108u); }

enum pvr_gpu_control_result pvr_gpu_control_poll(struct pvr_gpu_control *s, unsigned budget)
{
    if (!s || !budget || budget >= PVR_CCB_FIRMWARE_SLOTS) return PVR_GPU_CONTROL_INVALID;
    if (s->phase == PVR_GPU_CONTROL_FAILED) return PVR_GPU_CONTROL_FAULT;
    if (s->phase < PVR_GPU_CONTROL_COMPAT || s->phase > PVR_GPU_CONTROL_READY)
        return PVR_GPU_CONTROL_INVALID;
    if (s->queue->phase != (s->phase == PVR_GPU_CONTROL_COMPAT ?
                           PVR_CCB_PREPARED : PVR_CCB_ACTIVE))
        return fail(s, PVR_GPU_CONTROL_QUEUE);
    uint64_t now;
    if (!clock_now(s, &now)) return PVR_GPU_CONTROL_FAULT;
    if (s->phase == PVR_GPU_CONTROL_COMPAT) {
        unsigned char updated = 0;
        if (!s->ops.compat(s->context, 56, &updated, 1)) return fail(s, PVR_GPU_CONTROL_IO);
        if (!updated) return PVR_GPU_CONTROL_AGAIN;
        if (updated != 1) return fail(s, PVR_GPU_CONTROL_INCOMPATIBLE);
        if (!s->ops.compat(s->context, 0, s->compatibility, sizeof(s->compatibility)))
            return fail(s, PVR_GPU_CONTROL_IO);
        if (!compatible(s->compatibility)) return fail(s, PVR_GPU_CONTROL_INCOMPATIBLE);
        if (!clock_now(s, &now)) return PVR_GPU_CONTROL_FAULT;
        if (pvr_ccb_start(s->queue, now) != PVR_CCB_OK) return fail(s, PVR_GPU_CONTROL_QUEUE);
        s->phase = PVR_GPU_CONTROL_SUBMIT_HEALTH;
        return PVR_GPU_CONTROL_AGAIN;
    }
    uint32_t irq;
    if (!pending(s, &irq)) return PVR_GPU_CONTROL_FAULT;
    if (irq && !s->ops.write32(s->context, 0x8b0, 1)) return fail(s, PVR_GPU_CONTROL_IO);
    int more = 1;
    for (unsigned i = 0; i < budget; ++i) {
        enum pvr_ccb_result result = pvr_ccb_receive(s->queue, s->last_event);
        if (result == PVR_CCB_AGAIN) { more = 0; break; }
        if (result == PVR_CCB_RESTART) return fail(s, PVR_GPU_CONTROL_RESTART);
        if (result != PVR_CCB_OK) return fail(s, PVR_GPU_CONTROL_QUEUE);
        if (!known_event((uint32_t)le(s->last_event, 4)) || !s->ops.event ||
            !s->ops.event(s->context, s->last_event)) return fail(s, PVR_GPU_CONTROL_EVENT);
    }
    if (!clock_now(s, &now)) return PVR_GPU_CONTROL_FAULT;
    if (pvr_ccb_poll(s->queue, now) != PVR_CCB_OK) return fail(s, PVR_GPU_CONTROL_QUEUE);
    if (!pending(s, &irq)) return PVR_GPU_CONTROL_FAULT;
    more |= irq != 0;
    if (s->phase == PVR_GPU_CONTROL_SUBMIT_HEALTH) {
        unsigned char command[PVR_CCB_COMMAND_BYTES] = {0};
        uint32_t type = PVR_CCB_HEALTH;
        for (unsigned i = 0; i < 4; ++i) command[i] = (unsigned char)(type >> (8 * i));
        if (!clock_now(s, &now)) return PVR_GPU_CONTROL_FAULT;
        enum pvr_ccb_result result = pvr_ccb_submit(s->queue, command, now, s->deadline, &s->health_token);
        if (result == PVR_CCB_AGAIN) return PVR_GPU_CONTROL_AGAIN;
        if (result != PVR_CCB_OK) return fail(s, PVR_GPU_CONTROL_QUEUE);
        s->phase = PVR_GPU_CONTROL_WAIT_HEALTH;
    } else if (s->phase == PVR_GPU_CONTROL_WAIT_HEALTH) {
        if (!s->health_done) {
            enum pvr_ccb_result result = pvr_ccb_collect(s->queue, s->health_token, &s->health_status);
            if (result == PVR_CCB_AGAIN) return PVR_GPU_CONTROL_AGAIN;
            if (result != PVR_CCB_OK) return fail(s, PVR_GPU_CONTROL_QUEUE);
            if (s->health_status != 1) return fail(s, PVR_GPU_CONTROL_HEALTH);
            s->health_done = 1;
        }
        if (!clock_now(s, &now)) return PVR_GPU_CONTROL_FAULT;
        if (!more) s->phase = PVR_GPU_CONTROL_READY;
    }
    return s->phase == PVR_GPU_CONTROL_READY && !more ? PVR_GPU_CONTROL_OK : PVR_GPU_CONTROL_AGAIN;
}

enum pvr_gpu_control_result pvr_gpu_control_retire(struct pvr_gpu_control *s)
{
    if (!s || s->phase != PVR_GPU_CONTROL_READY) return PVR_GPU_CONTROL_INVALID;
    if (pvr_ccb_retire(s->queue) != PVR_CCB_OK) return fail(s, PVR_GPU_CONTROL_QUEUE);
    s->phase = PVR_GPU_CONTROL_RETIRED;
    return PVR_GPU_CONTROL_OK;
}

enum pvr_gpu_control_result pvr_gpu_control_abort(struct pvr_gpu_control *s)
{
    if (!s || s->phase < PVR_GPU_CONTROL_COMPAT || s->phase > PVR_GPU_CONTROL_RETIRED)
        return PVR_GPU_CONTROL_INVALID;
    if (s->phase == PVR_GPU_CONTROL_FAILED) return PVR_GPU_CONTROL_FAULT;
    return fail(s, PVR_GPU_CONTROL_EXTERNAL);
}
