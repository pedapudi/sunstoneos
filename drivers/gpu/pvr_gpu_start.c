/* SPDX-License-Identifier: MIT
 * Startup semantics adapted from pvr_fw_startstop.c, pvr_fw.c,
 * pvr_device_info.c and pvr_rogue_cr_defs.h.
 * Copyright (c) 2023 Imagination Technologies Ltd. See LICENSE.pvr.
 */
#include "pvr_gpu_start.h"
#include "a733_range.h"
#include <string.h>

enum { PUBLISH, CHECK_CLEAR, WRITE, READ_FENCE, READ_SLC, WRITE_SLC, DELAY, WAIT_BOOT };
static uint64_t read64(const unsigned char *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static int bit(const unsigned char *p, unsigned i) { return (p[i / 8] >> (i % 8)) & 1; }
static int features(const unsigned char *image, const struct pvr_fw_plan *p,
                    uint32_t *options, uint32_t *slc)
{
    static const unsigned char values[] = {
        1,3,9,10,11,12,13,14,20,21,23,24,25,26,28,29,30,31,36,42,43,44,46,51,52,57,60,63,66,67,68
    };
    const uint32_t *n = p->device_info_counts;
    const unsigned char *info = image + p->device_info_offset, *f = info + 48;
    if (n[0] != 1 || n[1] != 1 || n[2] != 2 || read64(f + 8) >> 7 ||
        read64(info + 32) >> 12 || read64(info + 40) >> 8 || !bit(info + 32, 9)) return 0;
    const unsigned char *params = f + 16;
    unsigned index = 0, found = 0;
    uint64_t line = 0;
    for (unsigned i = 0; i < sizeof(values); ++i) {
        unsigned id = values[i];
        if (!bit(f, id)) continue;
        if (index == n[3]) return 0;
        uint64_t value = read64(params + index++ * 8);
        switch (id) {
        case 28: if (value != 1) return 0; found |= 1; break;
        case 36: if (value != 36) return 0; found |= 2; break;
        case 44: line = value; found |= 4; break;
        case 46: if (value != 16) return 0; found |= 8; break;
        case 60: if (value != 40) return 0; found |= 16; break;
        }
    }
    if (index != n[3] || found != 31 || !line || line > UINT16_MAX || (line & (line - 1))) return 0;
    *options = (uint32_t)(bit(f, 32) | bit(f, 48) << 1 | bit(f, 65) << 2);
    *slc = 0x200000u | (line < 1024 ? 1u : 0u) |
        (bit(info + 32, 11) && !bit(f, 16) ? 0x1000000u : 0u);
    return 1;
}
static int valid_boot(const struct pvr_mips_boot *b)
{
    static const struct pvr_mips_write expected[PVR_MIPS_WRITES] = {
        {0x810,4,0x1cf80},{0x818,8,0x1fc00001},{0x820,8,0},
        {0x858,8,1},{0x860,8,0},{0x828,8,0x1fc01001},{0x830,8,0},
        {0x838,8,0x1fc02001},{0x840,8,0},{0xb50,8,0},{0x8c8,4,0}
    };
    if (b->physical_address_bits != 36 || b->write_count != PVR_MIPS_WRITES) return 0;
    for (unsigned i = 0; i < PVR_MIPS_WRITES; ++i) {
        const struct pvr_mips_write *w = &b->writes[i];
        if (w->offset != expected[i].offset || w->width != expected[i].width) return 0;
        if (i == 2 || i == 4 || i == 6 || i == 8) {
            uint64_t address = w->value & ~UINT64_C(4095);
            if ((w->value & 4095) != 12 || !address || address >= (UINT64_C(1) << 36) ||
                (address >= 0x1fc00000 && address < 0x1fc04000)) return 0;
        } else if (w->value != expected[i].value) return 0;
    }
    return b->writes[2].value == b->writes[4].value &&
        b->writes[2].value != b->writes[6].value && b->writes[2].value != b->writes[8].value &&
        b->writes[6].value != b->writes[8].value;
}
static void add(struct pvr_gpu_start *s, unsigned kind, unsigned off, unsigned width, uint64_t v)
{ s->steps[s->count++] = (struct pvr_gpu_start_step){kind, off, width, v}; }

int pvr_gpu_start_init(struct pvr_gpu_start *s, const void *image, size_t bytes,
                       const struct pvr_mips_boot *b, const struct pvr_gpu_start_ops *ops, void *ctx)
{
    struct pvr_fw_plan p;
    uint32_t options, slc;
    if (!a733_span_excl_nonempty(s, sizeof(*s)) || s->phase != PVR_GPU_START_NEW ||
        !a733_span_excl_nonempty(b, sizeof(*b)) || !a733_span_excl_nonempty(ops, sizeof(*ops)) ||
        !a733_extents_disjoint_prechecked((uintptr_t)s, sizeof(*s), (uintptr_t)b, sizeof(*b)) ||
        !a733_extents_disjoint_prechecked((uintptr_t)s, sizeof(*s), (uintptr_t)ops, sizeof(*ops)) ||
        !ops->now || !ops->read || !ops->write || !ops->started || !ops->publish ||
        !pvr_fw_requirements(image, bytes, PVR_FW_A733_BVNC, &p) ||
        !a733_extents_disjoint_prechecked((uintptr_t)s, sizeof(*s), (uintptr_t)image, bytes) ||
        !features(image, &p, &options, &slc) ||
        !valid_boot(b)) return 0;
    struct pvr_gpu_start result = {0};
    result.ops = *ops; result.context = ctx; result.phase = PVR_GPU_START_PREPARED;
    add(&result, PUBLISH, 0, 0, 0); add(&result, CHECK_CLEAR, 0, 0, 0);
    if (options & 2) {
        add(&result, WRITE, 0xa100, 4, 0); add(&result, READ_FENCE, 0xa100, 4, 0);
    }
    uint64_t mask = options & 1 ? UINT64_C(0xffeffffffffffc3d) : UINT64_C(0x00e7fffffffffc3d);
    const uint64_t reset[] = {mask, mask ^ UINT64_C(0x000001f8e0000000), UINT64_C(1) << 32};
    for (unsigned i = 0; i < 3; ++i) {
        add(&result, WRITE, 0x100, 8, reset[i]);
        if (options & 4) add(&result, WRITE, 0x108, 8, i ? 0 : 0x1fffff);
        add(&result, READ_FENCE, 0x100, 8, 0);
        if (options & 4) add(&result, READ_FENCE, 0x108, 8, 0);
    }
    add(&result, READ_SLC, 0x3800, 4, 0); add(&result, WRITE_SLC, 0x3800, 4, slc);
    for (unsigned i = 0; i < b->write_count; ++i)
        add(&result, WRITE, b->writes[i].offset, b->writes[i].width, b->writes[i].value);
    add(&result, WRITE, 0x38c0, 8, 0x8882af00);
    add(&result, DELAY, 0, 0, 3);
    add(&result, WRITE, 0x100, 8, 0); add(&result, READ_FENCE, 0x100, 8, 0);
    add(&result, DELAY, 0, 0, 3); add(&result, WAIT_BOOT, 0, 0, 0);
    *s = result;
    return 1;
}
static enum pvr_gpu_start_result fail(struct pvr_gpu_start *s, unsigned error)
{ s->error = error; s->phase = PVR_GPU_START_FAILED; return PVR_GPU_START_FAULT; }
static int clock_now(struct pvr_gpu_start *s, uint64_t *now)
{
    if (!s->ops.now(s->context, now)) { fail(s, PVR_GPU_START_IO); return 0; }
    if (*now < s->last_now) { fail(s, PVR_GPU_START_CLOCK); return 0; }
    s->last_now = *now;
    if (*now >= s->deadline) { fail(s, PVR_GPU_START_TIMEOUT); return 0; }
    return 1;
}
enum pvr_gpu_start_result pvr_gpu_start_begin(struct pvr_gpu_start *s, uint64_t deadline)
{
    if (!s || s->phase != PVR_GPU_START_PREPARED) return PVR_GPU_START_INVALID;
    uint64_t now;
    s->deadline = deadline;
    if (!clock_now(s, &now)) return PVR_GPU_START_FAULT;
    s->phase = PVR_GPU_START_RUNNING;
    return PVR_GPU_START_AGAIN;
}
enum pvr_gpu_start_result pvr_gpu_start_poll(struct pvr_gpu_start *s)
{
    if (!s) return PVR_GPU_START_INVALID;
    if (s->phase == PVR_GPU_START_FAILED) return PVR_GPU_START_FAULT;
    if (s->phase == PVR_GPU_START_BOOTED) return PVR_GPU_START_OK;
    if (s->phase != PVR_GPU_START_RUNNING) return PVR_GPU_START_INVALID;
    uint64_t now, value = 0;
    uint32_t started = 0;
    if (!clock_now(s, &now)) return PVR_GPU_START_FAULT;
    const struct pvr_gpu_start_step *op = &s->steps[s->index];
    int ok;
    switch (op->kind) {
    case PUBLISH: ok = s->ops.publish(s->context); break;
    case CHECK_CLEAR:
    case WAIT_BOOT: {
        /* Re-read the readiness byte for at most PVR_GPU_SPIN_US before
         * leaving the wait to the caller's next poll. */
        uint64_t start = now;
        for (;;) {
            ok = s->ops.started(s->context, &started);
            if (!ok || started || op->kind == CHECK_CLEAR) break;
            if (!clock_now(s, &now)) return PVR_GPU_START_FAULT;
            if (now - start >= PVR_GPU_SPIN_US) return PVR_GPU_START_AGAIN;
        }
        if (!ok) break;
        if (started > 1) return fail(s, PVR_GPU_START_BAD_FLAG);
        if (op->kind == CHECK_CLEAR) {
            if (started) return fail(s, PVR_GPU_START_STALE);
        } else {
            if (!clock_now(s, &now)) return PVR_GPU_START_FAULT;
            s->phase = PVR_GPU_START_BOOTED;
        }
        break;
    }
    case WRITE: ok = s->ops.write(s->context, op->offset, op->width, op->value); break;
    case READ_FENCE: ok = s->ops.read(s->context, op->offset, op->width, &value); break;
    case READ_SLC:
        ok = s->ops.read(s->context, op->offset, op->width, &value);
        if (ok) s->slc_value = (uint32_t)value & 2u;
        break;
    case WRITE_SLC:
        ok = s->ops.write(s->context, op->offset, op->width, op->value | s->slc_value);
        break;
    case DELAY: {
        /* A few microseconds: wait on the counter inside the step. The
         * deadline check in clock_now bounds the wait. */
        uint64_t until;
        if (!a733_deadline_after(now, op->value, &until)) return fail(s, PVR_GPU_START_TIMEOUT);
        while (now < until) if (!clock_now(s, &now)) return PVR_GPU_START_FAULT;
        ok = 1; break;
    }
    default: return fail(s, PVR_GPU_START_IO);
    }
    if (!ok) return fail(s, PVR_GPU_START_IO);
    ++s->index;
    return s->phase == PVR_GPU_START_BOOTED ? PVR_GPU_START_OK : PVR_GPU_START_AGAIN;
}
enum pvr_gpu_start_result pvr_gpu_start_cancel(struct pvr_gpu_start *s)
{
    if (!s || s->phase != PVR_GPU_START_RUNNING) return PVR_GPU_START_INVALID;
    return fail(s, PVR_GPU_START_CANCELLED);
}
