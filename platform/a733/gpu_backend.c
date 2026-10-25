/* SPDX-License-Identifier: MIT
 * Identity decoding and register facts follow pvr_device.c, pvr_fw_startstop.c,
 * pvr_fw_mips.c and pvr_rogue_cr_defs.h.
 * Copyright (c) 2023 Imagination Technologies Ltd. See drivers/gpu/LICENSE.pvr.
 */
#include "a733_gpu_backend.h"
#include "a733_range.h"
#include "a733_mmio.h"
#include "aarch64_device_memory.h"
#include "a733_mapping_check.h"
#include "a733_gpu_platform_native.h"

static int fail(struct a733_gpu_backend *b, enum a733_gpu_backend_error error)
{
    unsigned expected = 0;
    atomic_compare_exchange_strong_explicit(&b->error, &expected, error, memory_order_release, memory_order_relaxed);
    return 0;
}
static int healthy(struct a733_gpu_backend *b)
{
    if (!b || !b->initialized || b->service->phase == PVR_GPU_SERVICE_FAILED ||
        atomic_load_explicit(&b->error, memory_order_acquire)) return 0;
    if (b->external.revoked && atomic_load_explicit(b->external.revoked, memory_order_acquire))
        return fail(b, A733_GPU_BACKEND_REVOKED);
    if (b->external.live && b->external.live(b->external.context) != 1)
        return fail(b, A733_GPU_BACKEND_REVOKED);
    return 1;
}
static int device_page(uintptr_t address)
{
    static const struct a733_mapping_rule device = {A733_MAPPING_READ_WRITE, 1,
        {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}}, 0, 1};
    return a733_mapping_check(aarch64_device_translate, address, address, &device);
}
static int device_pages(void)
{
    static const uintptr_t pages[] = {0x01800000, 0x01801000, 0x01803000, 0x0180a000, 0x0180f000, 0x03400000};
    for (unsigned i = 0; i < sizeof(pages) / sizeof(*pages); ++i) {
        if (!device_page(pages[i]) || !device_page(pages[i] + 4092)) return 0;
    }
    return 1;
}
static int output_span(struct a733_gpu_backend *b, const void *p, size_t bytes, int queue_event)
{
    if (!a733_span_excl_nonempty(p, bytes) || !a733_ranges_disjoint_nonempty(b, sizeof(*b), p, bytes) ||
        (b->external.revoked && !a733_ranges_disjoint_nonempty(b->external.revoked, sizeof(*b->external.revoked), p, bytes)) ||
        !a733_ranges_disjoint_nonempty(b->memory, sizeof(*b->memory), p, bytes) ||
        !a733_ranges_disjoint_nonempty(b->power, sizeof(*b->power), p, bytes) ||
        !a733_ranges_disjoint_nonempty(b->power->manager, sizeof(*b->power->manager), p, bytes) ||
        (!queue_event && !a733_ranges_disjoint_nonempty(b->service, offsetof(struct pvr_gpu_service, worker_stack), p, bytes)) ||
        !a733_ranges_disjoint_nonempty(b->service->config.firmware, b->service->config.firmware_bytes, p, bytes)) return 0;
    for (unsigned i = 0; i < 3 + PVR_FW_RESOURCE_COUNT; ++i) {
        const struct pvr_mips_buffer *buffer = i < 3 ? &b->memory->firmware[i] : &b->memory->resources[i - 3];
        if (!a733_ranges_disjoint_nonempty(buffer->storage.cpu, buffer->storage.bytes, p, bytes) ||
            !a733_ranges_disjoint_nonempty(buffer->pages, buffer->page_count * sizeof(uint64_t), p, bytes)) return 0;
    }
    /* Every page of the span must be writable Normal memory, write-back or
     * non-cacheable; the physical address is not compared. */
    static const struct a733_mapping_rule normal = {A733_MAPPING_WRITE, 2,
        {{A733_MAIR_NORMAL_WB, A733_SH_ANY}, {A733_MAIR_NORMAL_NC, A733_SH_ANY}}, 0, 0};
    return a733_mapping_check_pages(aarch64_device_translate, (uintptr_t)p, 0, bytes, &normal);
}
static int output(struct a733_gpu_backend *b, const void *p, size_t bytes)
{ return output_span(b, p, bytes, 0); }
static int now(void *context, uint64_t *out)
{
    struct a733_gpu_backend *b = context;
    if (!healthy(b)) return 0;
    if (!output(b, out, sizeof(*out))) return fail(b, A733_GPU_BACKEND_ACCESS);
    uint64_t time;
    if (!aarch64_device_cache_stride() || !a733_gpu_platform_native_now(NULL, &time) || time < b->last_now ||
        time < b->power->last_now || time < b->power->power.last_now)
        return fail(b, A733_GPU_BACKEND_CLOCK);
    b->last_now = time; *out = time;
    return 1;
}
static int before(struct a733_gpu_backend *b, uint64_t deadline)
{
    uint64_t time;
    return now(b, &time) && (time < deadline || fail(b, A733_GPU_BACKEND_CLOCK));
}
static int active(struct a733_gpu_backend *b)
{
    if (!healthy(b)) return 0;
    return (b->phase == A733_GPU_BACKEND_ACTIVE && b->power->phase == A733_GPU_PLATFORM_ON &&
            b->power->token && b->power->delegated) || fail(b, A733_GPU_BACKEND_STATE);
}
static int raw_read(struct a733_gpu_backend *b, uint32_t off, uint32_t width, uint64_t *out)
{
    if (!device_page(A733_GPU_REGISTERS + off)) return fail(b, A733_GPU_BACKEND_MAPPING);
    uintptr_t address = (uintptr_t)(A733_GPU_REGISTERS + off);
    *out = width == 4 ? a733_mmio_read32_dsb_sy_both(address) : a733_mmio_read64_dsb_sy_both(address);
    return 1;
}
static int raw_write(struct a733_gpu_backend *b, uint32_t off, uint32_t width, uint64_t value)
{
    if (!device_page(A733_GPU_REGISTERS + off)) return fail(b, A733_GPU_BACKEND_MAPPING);
    uintptr_t address = (uintptr_t)(A733_GPU_REGISTERS + off);
    if (width == 4) a733_mmio_write32_dsb_sy_both(address, (uint32_t)value);
    else a733_mmio_write64_dsb_sy_both(address, value);
    return 1;
}
static int power_begin(void *context, uint64_t deadline)
{
    struct a733_gpu_backend *b = context;
    if (!healthy(b)) return 0;
    if ((b->phase != A733_GPU_BACKEND_BOUND && b->phase != A733_GPU_BACKEND_ISOLATED) ||
        b->memory->phase != A733_GPU_MEMORY_OFFLINE || !before(b, deadline)) return fail(b, A733_GPU_BACKEND_STATE);
    b->deadline = deadline; b->identity = 0; b->identity_step = 0;
    b->mode = (struct pvr_compute_mode){0};
    b->phase = A733_GPU_BACKEND_POWERING;
    return a733_gpu_platform_begin(b->power, deadline) || fail(b, A733_GPU_BACKEND_POWER);
}
static int power_poll(void *context)
{
    struct a733_gpu_backend *b = context;
    if (!healthy(b)) return -1;
    if (!before(b, b->deadline)) return -1;
    if (b->phase == A733_GPU_BACKEND_POWERING) {
        int result = a733_gpu_platform_poll(b->power);
        if (!before(b, b->deadline)) return -1;
        if (result == 0) return 0;
        if (result != 1 || b->power->phase != A733_GPU_PLATFORM_ON || !b->power->token || !b->power->delegated)
            return fail(b, A733_GPU_BACKEND_POWER), -1;
        b->phase = A733_GPU_BACKEND_IDENTIFY;
        return 0;
    }
    if (b->phase != A733_GPU_BACKEND_IDENTIFY || b->power->phase != A733_GPU_PLATFORM_ON ||
        !b->power->token || !b->power->delegated) return fail(b, A733_GPU_BACKEND_STATE), -1;
    uint64_t value;
    if (b->identity_step < 3) {
        if (b->identity_step == 0) {
            if (!raw_read(b, 0x20, 8, &value)) return -1;
            b->identity = value;
            b->identity_step = value >> 48 ? 3 : 1;
        } else if (b->identity_step == 1) {
            if (!raw_read(b, 0x20, 4, &value)) return -1;
            b->legacy_revision = (uint32_t)value; b->identity_step = 2;
        } else {
            if (!raw_read(b, 0x18, 4, &value)) return -1;
            b->identity = (uint64_t)((b->legacy_revision >> 16) & 255) << 48 |
                          (uint64_t)((b->legacy_revision >> 8) & 255) << 32 |
                          (uint64_t)((value >> 8) & 255) << 16 | (value & 255);
            b->identity_step = 3;
        }
        if (!before(b, b->deadline)) return -1;
        if (b->identity_step == 3 && b->identity != PVR_FW_A733_BVNC)
            return fail(b, A733_GPU_BACKEND_IDENTITY), -1;
        return 0;
    }
    if (b->identity_step == 3) {
        if (!raw_read(b, PVR_COMPUTE_MODE_SYSTEM, 4, &value) || !before(b, b->deadline)) return -1;
        if (!pvr_compute_mode_count(&b->mode, b->identity, b->power->token,
                                   b->service->epoch, (uint32_t)value))
            return fail(b, A733_GPU_BACKEND_COMPUTE_MODE), -1;
        b->identity_step = 4;
        return 0;
    }
    if (b->identity_step != 4 || !raw_read(b, PVR_COMPUTE_MODE_CAPABILITIES, 8, &value) ||
        !before(b, b->deadline)) return -1;
    if (!pvr_compute_mode_capabilities(&b->mode, value) ||
        !pvr_compute_mode_valid(&b->mode, b->identity, b->power->token, b->service->epoch))
        return fail(b, A733_GPU_BACKEND_COMPUTE_MODE), -1;
    b->phase = A733_GPU_BACKEND_ACTIVE; return 1;
}
static int compute_mode(void *context)
{
    struct a733_gpu_backend *b = context;
    if (!active(b)) return 0;
    return pvr_compute_mode_valid(&b->mode, b->identity, b->power->token, b->service->epoch) ||
        fail(b, A733_GPU_BACKEND_COMPUTE_MODE);
}

static int read_allowed(uint32_t off, uint32_t width, int stopping)
{
    if (!stopping) return (off == 0x100 && width == 8) || ((off == 0xa100 || off == 0x3800) && width == 4);
    return (off == 0x3870 && width == 8) || (width == 4 && (off == 0x8a8 || off == 0x3c8 || off == 0x1358 ||
        off == 0x1350 || off == 0x1320 || off == 0x1338 || off == 0x3898));
}
static int start_read(void *context, uint32_t off, uint32_t width, uint64_t *out)
{
    struct a733_gpu_backend *b = context;
    if (!active(b)) return 0;
    const struct pvr_gpu_start *s = &b->service->start;
    if (!read_allowed(off, width, 0) || !output(b, out, sizeof(*out)) || s->phase != PVR_GPU_START_RUNNING ||
        s->index >= s->count || s->steps[s->index].offset != off || s->steps[s->index].width != width)
        return fail(b, A733_GPU_BACKEND_ACCESS);
    if (!before(b, s->deadline)) return 0;
    uint64_t value;
    if (!raw_read(b, off, width, &value) || !before(b, s->deadline)) return 0;
    *out = value; return 1;
}
static int write_allowed(uint32_t off, uint32_t width, uint64_t value)
{
    if (width == 4 && value > UINT32_MAX) return 0;
    switch (off) {
    case 0xa100: return width == 4 && value == 0;
    case 0x100: return width == 8 && (value == UINT64_C(0xffeffffffffffc3d) || value == UINT64_C(0x00e7fffffffffc3d) ||
        value == UINT64_C(0xffeffe071ffffc3d) || value == UINT64_C(0x00e7fe071ffffc3d) || value == UINT64_C(0x100000000) || value == 0);
    case 0x3800: return width == 4 && (value & ~UINT64_C(0x1000003)) == 0x200000;
    case 0x38c0: return width == 8 && value == 0x8882af00;
    case 0x810: return width == 4 && value == 0x1cf80;
    case 0x818: return width == 8 && value == 0x1fc00001;
    case 0x858: return width == 8 && value == 1;
    case 0x828: return width == 8 && value == 0x1fc01001;
    case 0x838: return width == 8 && value == 0x1fc02001;
    case 0x820: case 0x860: case 0x830: case 0x840:
        return width == 8 && (value & 4095) == 12 && value >= 4096 && value < (UINT64_C(1) << 36) &&
            (value < 0x1fc00000 || value >= 0x1fc04000);
    case 0xb50: return width == 8 && value == 0;
    case 0x8c8: return width == 4 && value == 0;
    default: return 0;
    }
}
static int start_write(void *context, uint32_t off, uint32_t width, uint64_t value)
{
    struct a733_gpu_backend *b = context;
    if (!active(b)) return 0;
    const struct pvr_gpu_start *s = &b->service->start;
    if (!write_allowed(off, width, value) || s->phase != PVR_GPU_START_RUNNING || s->index >= s->count ||
        s->steps[s->index].offset != off || s->steps[s->index].width != width ||
        value != (s->steps[s->index].value | (off == 0x3800 ? s->slc_value & 2u : 0)))
        return fail(b, A733_GPU_BACKEND_ACCESS);
    return before(b, s->deadline) && raw_write(b, off, width, value) && before(b, s->deadline);
}
static int stop_read(void *context, uint32_t off, uint32_t width, uint64_t *out)
{
    struct a733_gpu_backend *b = context;
    if (!active(b)) return 0;
    const struct pvr_gpu_stop *s = &b->service->stop;
    if (!read_allowed(off, width, 1) || !output(b, out, sizeof(*out)) || s->phase != PVR_GPU_STOP_REGISTERS ||
        s->index >= s->count || s->steps[s->index].offset != off || s->steps[s->index].width != width)
        return fail(b, A733_GPU_BACKEND_ACCESS);
    if (!before(b, s->deadline)) return 0;
    uint64_t value;
    if (!raw_read(b, off, width, &value) || !before(b, s->deadline)) return 0;
    *out = value; return 1;
}
static int stop_write(void *context, uint32_t off, uint32_t width, uint64_t value)
{
    struct a733_gpu_backend *b = context;
    if (!active(b)) return 0;
    const struct pvr_gpu_stop *s = &b->service->stop;
    int allowed = ((off == 0xb40 || off == 0xb30) && width == 4 && !value) ||
        (off == 0x8b0 && width == 4 && value == 1 && (s->irq_status & 1u));
    if (!allowed || s->phase != PVR_GPU_STOP_REGISTERS || s->index >= s->count || s->steps[s->index].offset != off ||
        s->steps[s->index].width != width || s->steps[s->index].value != value) return fail(b, A733_GPU_BACKEND_ACCESS);
    return before(b, s->deadline) && raw_write(b, off, width, value) && before(b, s->deadline);
}
static int control_live(struct a733_gpu_backend *b)
{
    return active(b) && ((b->service->control.phase >= PVR_GPU_CONTROL_SUBMIT_HEALTH &&
        b->service->control.phase <= PVR_GPU_CONTROL_READY && b->memory->phase == A733_GPU_MEMORY_PUBLISHED) ||
        fail(b, A733_GPU_BACKEND_STATE));
}
static int control_read(void *context, uint32_t off, uint32_t *out)
{
    struct a733_gpu_backend *b = context;
    if (!control_live(b)) return 0;
    if (off != 0x8a8 || !output(b, out, sizeof(*out))) return fail(b, A733_GPU_BACKEND_ACCESS);
    uint64_t value;
    if (!raw_read(b, off, 4, &value)) return 0;
    *out = (uint32_t)value; return 1;
}
static int control_write(void *context, uint32_t off, uint32_t value)
{
    struct a733_gpu_backend *b = context;
    if (!control_live(b)) return 0;
    if (off != 0x8b0 || value != 1) return fail(b, A733_GPU_BACKEND_ACCESS);
    return raw_write(b, off, 4, value);
}
static int kick(void *context, uint32_t off, uint32_t value)
{
    struct a733_gpu_backend *b = context;
    if (!control_live(b)) return 0;
    if (off != 0xb00 || value != 0 || b->memory->phase != A733_GPU_MEMORY_PUBLISHED)
        return fail(b, A733_GPU_BACKEND_ACCESS);
    return raw_write(b, off, 4, value);
}
static int memory_result(struct a733_gpu_backend *b, int result)
{ return result || fail(b, A733_GPU_BACKEND_MEMORY); }
static int queue_read(void *context, enum pvr_ccb_region region, uint32_t off, void *out, size_t bytes)
{
    struct a733_gpu_backend *b = context;
    if (!healthy(b)) return 0;
    const struct pvr_ccb *q = &b->service->queue;
    /* Receive retains its event in queue-owned storage before acknowledging
     * the firmware slot. Only this exact destination can overlap service state. */
    int queue_event = region == PVR_CCB_FIRMWARE_COMMANDS && bytes == PVR_CCB_COMMAND_BYTES &&
        out == q->last_event && q->phase == PVR_CCB_ACTIVE && q->context == b && q->ops.read == queue_read &&
        q->firmware_read < PVR_CCB_FIRMWARE_SLOTS && off == q->firmware_read * PVR_CCB_COMMAND_BYTES;
    return (output_span(b, out, bytes, queue_event) || fail(b, A733_GPU_BACKEND_ACCESS)) &&
        memory_result(b, a733_gpu_memory_read(b->memory, region, off, out, bytes));
}
static int queue_write(void *context, enum pvr_ccb_region region, uint32_t off, const void *in, size_t bytes)
{
    struct a733_gpu_backend *b = context;
    return healthy(b) && memory_result(b, a733_gpu_memory_write(b->memory, region, off, in, bytes));
}
static int barrier(void *context, enum pvr_ccb_barrier operation)
{ struct a733_gpu_backend *b = context; return healthy(b) && memory_result(b, a733_gpu_memory_barrier(b->memory, operation)); }
static int publish(void *context)
{ struct a733_gpu_backend *b = context; return active(b) && memory_result(b, a733_gpu_memory_publish(b->memory)); }
static int started(void *context, uint32_t *out)
{
    struct a733_gpu_backend *b = context;
    return active(b) && (output(b, out, sizeof(*out)) || fail(b, A733_GPU_BACKEND_ACCESS)) &&
        memory_result(b, a733_gpu_memory_started(b->memory, out));
}
static int compat(void *context, uint32_t off, void *out, size_t bytes)
{
    struct a733_gpu_backend *b = context;
    return active(b) && (output(b, out, bytes) || fail(b, A733_GPU_BACKEND_ACCESS)) &&
        memory_result(b, a733_gpu_memory_compat(b->memory, off, out, bytes));
}
static int sync_read(void *context, uint32_t *out)
{
    struct a733_gpu_backend *b = context;
    if (!active(b) || !(output(b,out,sizeof(*out)) || fail(b,A733_GPU_BACKEND_ACCESS))) return 0;
    uint32_t ack;
    if (!memory_result(b,a733_gpu_memory_sync_read(b->memory,&ack))) return 0;
    /* The firmware can acknowledge a denied power transition. The board
     * driver independently requires POW_OFF before hardware teardown. */
    if (ack) {
        unsigned phase=b->service->stop.phase;
        uint32_t state;
        if (ack!=1 || (phase!=PVR_GPU_STOP_WAIT_IDLE && phase!=PVR_GPU_STOP_WAIT_OFF))
            return fail(b,A733_GPU_BACKEND_STATE);
        if (!memory_result(b,a733_gpu_memory_power_state(b->memory,&state))) return 0;
        if (state!=(phase==PVR_GPU_STOP_WAIT_IDLE ? 2u : 0u))
            return fail(b,A733_GPU_BACKEND_STATE);
    }
    *out=ack; return 1;
}
static int sync_write(void *context, uint32_t value)
{
    struct a733_gpu_backend *b = context;
    if (!active(b)) return 0;
    return memory_result(b, a733_gpu_memory_sync_write(b->memory, value));
}
static int event(void *context, const unsigned char command[PVR_CCB_COMMAND_BYTES])
{
    struct a733_gpu_backend *b = context;
    return active(b) && ((b->external.event && b->external.event(b->external.context, command)) || fail(b, A733_GPU_BACKEND_EVENT));
}
static int isolate(void *context)
{
    struct a733_gpu_backend *b = context;
    if (!active(b)) return 0;
    uint64_t deadline = b->service->stop.deadline;
    if (b->service->stop.phase != PVR_GPU_STOP_ISOLATE || !before(b, deadline) ||
        (b->external.isolate ?
         b->external.isolate(b->external.context, b->power, deadline) != 1 :
         !a733_gpu_platform_off_begin(b->power, deadline)))
        return fail(b, A733_GPU_BACKEND_ISOLATION);
    if (!b->external.isolate) return 1;
    if (b->power->phase != A733_GPU_PLATFORM_OFF || b->power->token || b->power->delegated || !before(b, deadline))
        return fail(b, A733_GPU_BACKEND_ISOLATION);
    if (!memory_result(b, a733_gpu_memory_reclaim(b->memory)) || !before(b, deadline)) return 0;
    b->phase = A733_GPU_BACKEND_ISOLATED; return 1;
}
static int isolate_poll(void *context)
{
    struct a733_gpu_backend *b = context;
    if (!healthy(b)) return -1;
    uint64_t deadline=b->service->stop.deadline;
    if (b->phase!=A733_GPU_BACKEND_ACTIVE || b->service->stop.phase!=PVR_GPU_STOP_ISOLATE ||
        !b->service->stop.isolation_started || !before(b,deadline))
        return fail(b,A733_GPU_BACKEND_ISOLATION), -1;
    int result=a733_gpu_platform_off_poll(b->power);
    if (result!=0 && result!=1) return fail(b,A733_GPU_BACKEND_ISOLATION), -1;
    if (!before(b,deadline)) return -1;
    if (!result) return 0;
    if (b->power->phase!=A733_GPU_PLATFORM_OFF || b->power->token || b->power->delegated)
        return fail(b,A733_GPU_BACKEND_ISOLATION), -1;
    if (!memory_result(b,a733_gpu_memory_reclaim(b->memory)) || !before(b,deadline)) return -1;
    b->phase=A733_GPU_BACKEND_ISOLATED;
    return 1;
}
static int irq_mask(void *context)
{
    struct a733_gpu_backend *b = context;
    /* Masking remains available after a worker fault. It does not access GPU
     * registers or release resources and runs under service IRQ serialization. */
    if (!b || !b->initialized) return 0;
    if (!device_page(A733_GPU_GICD)) return fail(b, A733_GPU_BACKEND_MAPPING);
    return gicv3_spi_mask(&b->irq) || fail(b, A733_GPU_BACKEND_INTERRUPT);
}
static int irq_rearm(void *context)
{
    struct a733_gpu_backend *b = context;
    if (!healthy(b)) { (void)irq_mask(b); return -1; }
    if (!device_page(A733_GPU_GICD)) return fail(b, A733_GPU_BACKEND_MAPPING), -1;
    int result = gicv3_spi_rearm(&b->irq);
    if (result < 0) fail(b, A733_GPU_BACKEND_INTERRUPT);
    return result;
}
static int free_allocate(void *context, uint64_t epoch, uint64_t owner, uint64_t token,
                         unsigned slot, struct pvr_free_list_page *out)
{
    struct a733_gpu_backend *b = context;
    if (!healthy(b)) return -1;
    return b->free_list_ops.allocate(b->free_list_context, epoch, owner, token, slot, out);
}
static int free_page(void *context, const struct pvr_free_list_page *page)
{ struct a733_gpu_backend *b = context; return healthy(b) ? b->free_list_ops.publish_page(b->free_list_context, page) : 0; }
static int free_read(void *context, uint64_t token, enum pvr_free_list_memory memory,
                     uint32_t off, void *out, size_t bytes)
{
    struct a733_gpu_backend *b = context;
    return healthy(b) ? b->free_list_ops.read(b->free_list_context, token, memory, off, out, bytes) : 0;
}
static int free_write(void *context, uint64_t token, enum pvr_free_list_memory memory,
                      uint32_t off, const void *in, size_t bytes)
{
    struct a733_gpu_backend *b = context;
    return healthy(b) ? b->free_list_ops.write(b->free_list_context, token, memory, off, in, bytes) : 0;
}
static int free_publish(void *context, uint64_t token, enum pvr_free_list_memory memory,
                        uint32_t off, size_t bytes)
{
    struct a733_gpu_backend *b = context;
    return healthy(b) ? b->free_list_ops.publish(b->free_list_context, token, memory, off, bytes) : 0;
}
static int free_recover(void *context, uint64_t token)
{ struct a733_gpu_backend *b = context; return healthy(b) ? b->free_list_ops.recover_targets(b->free_list_context, token) : 0; }
int a733_gpu_backend_bind(struct a733_gpu_backend *b, struct pvr_gpu_service *s,
    struct pvr_gpu_service_config *c, struct a733_gpu_memory *m, struct a733_gpu_platform *p,
    const struct a733_gpu_backend_external *external)
{
    if (!b || !s || !c || !m || !p || !external || b->phase != A733_GPU_BACKEND_NEW ||
        s->initialized || m->phase != A733_GPU_MEMORY_OFFLINE || !p->initialized || !p->manager ||
        p->phase != A733_GPU_PLATFORM_NEW || c->state.gpu_clock_hz != A733_GPU_INITIAL_HZ ||
        c->state.bvnc != PVR_FW_A733_BVNC || !a733_span_excl_nonempty(c->firmware, c->firmware_bytes)) return 0;
    if (c->free_lists.count && (!c->free_lists.ops.allocate || !c->free_lists.ops.publish_page ||
        !c->free_lists.ops.read || !c->free_lists.ops.write || !c->free_lists.ops.publish ||
        !c->free_lists.ops.recover_targets)) return 0;
    const void *objects[] = {b, s, c, m, p, p->manager, external};
    const size_t sizes[] = {sizeof(*b), sizeof(*s), sizeof(*c), sizeof(*m), sizeof(*p), sizeof(*p->manager), sizeof(*external)};
    if (external->revoked) {
        static const struct a733_mapping_rule write_back = {A733_MAPPING_READ, 1,
            {{A733_MAIR_NORMAL_WB, A733_SH_ANY}}, 0, 0};
        if ((uintptr_t)external->revoked % _Alignof(atomic_uint) ||
            !a733_mapping_check(aarch64_device_translate, (uintptr_t)external->revoked, 0, &write_back) ||
            !a733_ranges_disjoint_nonempty(external->revoked, sizeof(*external->revoked), c->firmware, c->firmware_bytes)) return 0;
        for (unsigned i = 0; i < 7; ++i)
            if (!a733_ranges_disjoint_nonempty(external->revoked, sizeof(*external->revoked), objects[i], sizes[i])) return 0;
    }
    for (unsigned i = 0; i < 7; ++i) {
        if (!a733_ranges_disjoint_nonempty(objects[i], sizes[i], c->firmware, c->firmware_bytes)) return 0;
        for (unsigned j = 0; j < i; ++j) if (!a733_ranges_disjoint_nonempty(objects[i], sizes[i], objects[j], sizes[j])) return 0;
    }
    for (unsigned i = 0; i < 3 + PVR_FW_RESOURCE_COUNT; ++i) {
        const struct pvr_mips_buffer *a = i < 3 ? &c->firmware_buffers[i] : &c->resources[i - 3];
        const struct pvr_mips_buffer *d = i < 3 ? &m->firmware[i] : &m->resources[i - 3];
        if (a->storage.cpu != d->storage.cpu || a->storage.bytes != d->storage.bytes || a->pages != d->pages || a->page_count != d->page_count) return 0;
        if (external->revoked && (!a733_ranges_disjoint_nonempty(external->revoked, sizeof(*external->revoked), a->storage.cpu, a->storage.bytes) ||
            !a733_ranges_disjoint_nonempty(external->revoked, sizeof(*external->revoked), a->pages, a->page_count * sizeof(uint64_t)))) return 0;
        for (unsigned j = 0; j < 7; ++j) if (!a733_ranges_disjoint_nonempty(objects[j], sizes[j], a->storage.cpu, a->storage.bytes) ||
            !a733_ranges_disjoint_nonempty(objects[j], sizes[j], a->pages, a->page_count * sizeof(uint64_t))) return 0;
    }
    uint64_t frequency;
    if (!aarch64_device_cache_stride() || !device_pages()) return 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    if (frequency != 24000000) return 0;
    *b = (struct a733_gpu_backend){.service = s, .memory = m, .power = p, .external = *external,
        .free_list_ops = c->free_lists.ops, .free_list_context = c->context,
        .irq = {A733_GPU_GICD, A733_GPU_INTID}, .phase = A733_GPU_BACKEND_BOUND, .initialized = 1};
    atomic_init(&b->error, 0);
    c->queue = (struct pvr_ccb_ops){queue_read, queue_write, barrier, kick};
    c->start = (struct pvr_gpu_start_ops){now, start_read, start_write, started, publish};
    c->control = (struct pvr_gpu_control_ops){now, control_read, control_write, compat, event};
    c->stop = (struct pvr_gpu_stop_ops){now, stop_read, stop_write, sync_write, sync_read, isolate, external->isolate ? NULL : isolate_poll};
    c->irq = (struct pvr_gpu_service_irq_ops){irq_mask, irq_rearm};
    c->power = (struct pvr_gpu_service_enable_ops){power_begin, power_poll};
    if (c->free_lists.count) c->free_lists.ops = (struct pvr_free_list_ops){free_allocate, free_page,
        free_read, free_write, free_publish, free_recover};
    c->enable = NULL; c->context = b; c->compute_mode = compute_mode;
    return 1;
}

int a733_gpu_backend_stopped(struct a733_gpu_backend *b,struct pvr_gpu_service *s)
{
    return b && s && healthy(b) && b->service==s && s->config.context==b &&
        s->config.start.now==now && s->config.stop.isolate==isolate && s->config.stop.isolate_poll==isolate_poll &&
        b->memory && b->memory->phase==A733_GPU_MEMORY_OFFLINE && b->power &&
        b->power->phase==A733_GPU_PLATFORM_OFF && !b->power->token && !b->power->delegated &&
        b->phase==A733_GPU_BACKEND_ISOLATED && s->stop.phase==PVR_GPU_STOP_STOPPED &&
        s->stop.isolation_observed && s->stop.acknowledgement[0]==1 && s->stop.acknowledgement[1]==1 &&
        s->queue.phase==PVR_CCB_RETIRED && s->control.phase==PVR_GPU_CONTROL_RETIRED &&
        s->irq_masked && !s->irq_fault;
}
int a733_gpu_backend_rebind_children(struct a733_gpu_backend *b)
{
    if(!b || !healthy(b) || b->phase!=A733_GPU_BACKEND_ISOLATED ||
       b->service->phase!=PVR_GPU_SERVICE_RECONSTRUCTING)return 0;
    struct pvr_gpu_service_config *c=&b->service->config;
    if(c->free_lists.count) {
        b->free_list_ops=c->free_lists.ops;b->free_list_context=c->context;
        c->free_lists.ops=(struct pvr_free_list_ops){free_allocate,free_page,free_read,free_write,free_publish,free_recover};
    }
    c->context=b;return 1;
}
