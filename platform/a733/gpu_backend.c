/* SPDX-License-Identifier: MIT
 * Identity decoding and register facts follow pvr_device.c, pvr_fw_startstop.c,
 * pvr_fw_mips.c and pvr_rogue_cr_defs.h.
 * Copyright (c) 2023 Imagination Technologies Ltd. See drivers/gpu/LICENSE.pvr.
 */
#include "a733_gpu_backend.h"
#include "aarch64_device_memory.h"
#include "a733_gpu_platform_native.h"

static int span(const void *p, size_t n)
{ return p && n && (uintptr_t)p <= UINTPTR_MAX - n; }
static int separate(const void *a, size_t n, const void *b, size_t m)
{ return span(a, n) && span(b, m) && ((uintptr_t)a + n <= (uintptr_t)b || (uintptr_t)b + m <= (uintptr_t)a); }
static int fail(struct a733_gpu_backend *b, enum a733_gpu_backend_error error)
{
    unsigned expected = 0;
    atomic_compare_exchange_strong_explicit(&b->error, &expected, error, memory_order_release, memory_order_relaxed);
    return 0;
}
static int healthy(struct a733_gpu_backend *b)
{ return b && b->initialized && b->service->phase != PVR_GPU_SERVICE_FAILED &&
    !atomic_load_explicit(&b->error, memory_order_acquire); }
static int device_page(uintptr_t address)
{
    struct aarch64_translation r, w;
    return aarch64_device_translate(address, 0, &r) && aarch64_device_translate(address, 1, &w) &&
        r.physical == address && w.physical == address && r.attributes == 0 && w.attributes == 0;
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
    if (!span(p, bytes) || !separate(b, sizeof(*b), p, bytes) ||
        !separate(b->memory, sizeof(*b->memory), p, bytes) ||
        !separate(b->power, sizeof(*b->power), p, bytes) ||
        !separate(b->power->manager, sizeof(*b->power->manager), p, bytes) ||
        (!queue_event && !separate(b->service, offsetof(struct pvr_gpu_service, worker_stack), p, bytes)) ||
        !separate(b->service->config.firmware, b->service->config.firmware_bytes, p, bytes)) return 0;
    for (unsigned i = 0; i < 3 + PVR_FW_RESOURCE_COUNT; ++i) {
        const struct pvr_mips_buffer *buffer = i < 3 ? &b->memory->firmware[i] : &b->memory->resources[i - 3];
        if (!separate(buffer->storage.cpu, buffer->storage.bytes, p, bytes) ||
            !separate(buffer->pages, buffer->page_count * sizeof(uint64_t), p, bytes)) return 0;
    }
    uintptr_t first = (uintptr_t)p, end = first + bytes - 1;
    for (uintptr_t address = first;; address = (address & ~(uintptr_t)4095) + 4096) {
        struct aarch64_translation t;
        if (!aarch64_device_translate(address, 1, &t) || (t.attributes != 0xff && t.attributes != 0x44)) return 0;
        if ((address >> 12) == (end >> 12)) break;
    }
    return 1;
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
    __asm__ volatile("dsb sy" ::: "memory");
    uint64_t value = width == 4 ? *(volatile uint32_t *)(uintptr_t)(A733_GPU_REGISTERS + off) :
                                 *(volatile uint64_t *)(uintptr_t)(A733_GPU_REGISTERS + off);
    __asm__ volatile("dsb sy" ::: "memory");
    *out = value; return 1;
}
static int raw_write(struct a733_gpu_backend *b, uint32_t off, uint32_t width, uint64_t value)
{
    if (!device_page(A733_GPU_REGISTERS + off)) return fail(b, A733_GPU_BACKEND_MAPPING);
    __asm__ volatile("dsb sy" ::: "memory");
    if (width == 4) *(volatile uint32_t *)(uintptr_t)(A733_GPU_REGISTERS + off) = (uint32_t)value;
    else *(volatile uint64_t *)(uintptr_t)(A733_GPU_REGISTERS + off) = value;
    __asm__ volatile("dsb sy" ::: "memory"); return 1;
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
    if (off == 0x100 && width == 8) return 1;
    if (!stopping) return (off == 0x108 && width == 8) || ((off == 0xa100 || off == 0x3800) && width == 4);
    return (off == 0x3870 && width == 8) || (width == 4 && (off == 0x3c8 || off == 0x1358 ||
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
    case 0x108: return width == 8 && (value == 0 || value == 0x1fffff);
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
    int allowed = ((off == 0xb40 || off == 0xb30 || off == 0xb48 || off == 0xb38) && width == 4 && !value) ||
        (off == 0x100 && width == 8 && (value == UINT64_C(0xffeffffffffffc3d) || value == UINT64_C(0x00e7fffffffffc3d)));
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
    return active(b) && (output(b, out, sizeof(*out)) || fail(b, A733_GPU_BACKEND_ACCESS)) &&
        memory_result(b, a733_gpu_memory_sync_read(b->memory, out));
}
static int sync_write(void *context, uint32_t value)
{ struct a733_gpu_backend *b = context; return active(b) && memory_result(b, a733_gpu_memory_sync_write(b->memory, value)); }
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
        b->external.isolate(b->external.context, b->power, deadline) != 1 ||
        b->power->phase != A733_GPU_PLATFORM_OFF || b->power->token || b->power->delegated || !before(b, deadline))
        return fail(b, A733_GPU_BACKEND_ISOLATION);
    if (!memory_result(b, a733_gpu_memory_reclaim(b->memory)) || !before(b, deadline)) return 0;
    b->phase = A733_GPU_BACKEND_ISOLATED; return 1;
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
    if (!b || !s || !c || !m || !p || !external || !external->isolate || b->phase != A733_GPU_BACKEND_NEW ||
        s->initialized || m->phase != A733_GPU_MEMORY_OFFLINE || !p->initialized || !p->manager ||
        p->phase != A733_GPU_PLATFORM_NEW || c->state.gpu_clock_hz != A733_GPU_INITIAL_HZ ||
        c->state.bvnc != PVR_FW_A733_BVNC || !span(c->firmware, c->firmware_bytes)) return 0;
    if (c->free_lists.count && (!c->free_lists.ops.allocate || !c->free_lists.ops.publish_page ||
        !c->free_lists.ops.read || !c->free_lists.ops.write || !c->free_lists.ops.publish ||
        !c->free_lists.ops.recover_targets)) return 0;
    const void *objects[] = {b, s, c, m, p, p->manager, external};
    const size_t sizes[] = {sizeof(*b), sizeof(*s), sizeof(*c), sizeof(*m), sizeof(*p), sizeof(*p->manager), sizeof(*external)};
    for (unsigned i = 0; i < 7; ++i) {
        if (!separate(objects[i], sizes[i], c->firmware, c->firmware_bytes)) return 0;
        for (unsigned j = 0; j < i; ++j) if (!separate(objects[i], sizes[i], objects[j], sizes[j])) return 0;
    }
    for (unsigned i = 0; i < 3 + PVR_FW_RESOURCE_COUNT; ++i) {
        const struct pvr_mips_buffer *a = i < 3 ? &c->firmware_buffers[i] : &c->resources[i - 3];
        const struct pvr_mips_buffer *d = i < 3 ? &m->firmware[i] : &m->resources[i - 3];
        if (a->storage.cpu != d->storage.cpu || a->storage.bytes != d->storage.bytes || a->pages != d->pages || a->page_count != d->page_count) return 0;
        for (unsigned j = 0; j < 7; ++j) if (!separate(objects[j], sizes[j], a->storage.cpu, a->storage.bytes) ||
            !separate(objects[j], sizes[j], a->pages, a->page_count * sizeof(uint64_t))) return 0;
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
    c->stop = (struct pvr_gpu_stop_ops){now, stop_read, stop_write, sync_write, sync_read, isolate};
    c->irq = (struct pvr_gpu_service_irq_ops){irq_mask, irq_rearm};
    c->power = (struct pvr_gpu_service_enable_ops){power_begin, power_poll};
    if (c->free_lists.count) c->free_lists.ops = (struct pvr_free_list_ops){free_allocate, free_page,
        free_read, free_write, free_publish, free_recover};
    c->enable = NULL; c->context = b; c->compute_mode = compute_mode;
    return 1;
}
