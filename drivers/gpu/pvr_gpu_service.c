#include "pvr_gpu_service.h"
#include "a733_range.h"
#include "pvr_compute_service.h"
#include "pvr_mimus_service.h"
#include "a733_gpu_graphics_memory.h"
#ifdef SUNSTONE_GPU_LIFECYCLE
#include "a733_gpu_disposal.h"
#endif
#include <string.h>

/* Only images with the mimus session link pvr_mimus_service.c. Elsewhere
 * config.mimus is null and these references stay unresolved and uncalled. */
#pragma weak pvr_mimus_service_prepare
#pragma weak pvr_mimus_service_publish_poll
#pragma weak pvr_mimus_service_poll
#pragma weak pvr_mimus_service_event
#pragma weak pvr_mimus_service_abort
#pragma weak pvr_mimus_service_close
#pragma weak pvr_mimus_service_deadline
#pragma weak pvr_mimus_service_submitting
#pragma weak pvr_mimus_service_progress
static struct pvr_gpu_service *instance;
static unsigned creating_worker;
static void worker(void *);
BaseType_t pvr_gpu_service_task_privileged(TaskFunction_t entry, void *argument)
{ return creating_worker && entry == worker && argument == instance ? pdTRUE : pdFALSE; }

static const struct pvr_mips_buffer *buffer(const struct pvr_gpu_service_config *c, unsigned i)
{ return i < 3 ? &c->firmware_buffers[i] : &c->resources[i - 3]; }
static int free_list_allocations(struct pvr_gpu_service *s, const struct pvr_gpu_service_config *c)
{
    const struct pvr_gpu_service_free_list_config *f = &c->free_lists;
    if (!f->count) return 1;
    if (f->count > PVR_FREE_LISTS || !f->minimum_pages ||
        !f->operation_timeout_us || f->operation_timeout_us > INT64_MAX ||
        !f->ops.allocate || !f->ops.publish_page || !f->ops.read || !f->ops.write ||
        !f->ops.publish || !f->ops.recover_targets ||
        !a733_span_excl_nonempty(f->lists, f->count * sizeof(*f->lists))) return 0;
    /* Include immutable list configs and the common ops/config object in the
     * cross-component checks before any firmware builder can modify backing. */
    for (unsigned i = 0; i <= f->count; ++i) {
        const void *ptr = i == f->count ? (const void *)f->lists : f->lists[i].pages;
        size_t bytes = i == f->count ? f->count * sizeof(*f->lists) :
            (size_t)f->lists[i].maximum_pages * sizeof(*f->lists[i].pages);
        if ((i < f->count && f->lists[i].maximum_pages > PVR_FREE_LIST_MAX_PAGES) ||
            !a733_span_excl_nonempty(ptr, bytes) || !a733_extents_disjoint_prechecked((uintptr_t)ptr, bytes, (uintptr_t)s, sizeof(*s)) ||
            !a733_extents_disjoint_prechecked((uintptr_t)ptr, bytes, (uintptr_t)c, sizeof(*c)) ||
            !a733_extents_disjoint_prechecked((uintptr_t)ptr, bytes, (uintptr_t)c->firmware, c->firmware_bytes)) return 0;
        if (i < f->count && !a733_extents_disjoint_prechecked((uintptr_t)ptr, bytes, (uintptr_t)f->lists, f->count * sizeof(*f->lists))) return 0;
        for (unsigned j = 0; j < i && j < f->count; ++j)
            if (!a733_extents_disjoint_prechecked((uintptr_t)ptr, bytes, (uintptr_t)f->lists[j].pages,
                    (size_t)f->lists[j].maximum_pages * sizeof(*f->lists[j].pages))) return 0;
        for (unsigned j = 0; j < 3 + PVR_FW_RESOURCE_COUNT; ++j) {
            const struct pvr_mips_buffer *b = buffer(c, j);
            if (!a733_extents_disjoint_prechecked((uintptr_t)ptr, bytes, (uintptr_t)b->storage.cpu, b->storage.bytes) ||
                !a733_extents_disjoint_prechecked((uintptr_t)ptr, bytes, (uintptr_t)b->pages, b->page_count * sizeof(uint64_t))) return 0;
        }
    }
    return 1;
}
static int allocations(struct pvr_gpu_service *s, const struct pvr_gpu_service_config *c)
{
    struct pvr_fw_plan plan;
    if (!a733_span_excl_nonempty(s, sizeof(*s)) || !a733_span_excl_nonempty(c, sizeof(*c)) ||
        !a733_extents_disjoint_prechecked((uintptr_t)s, sizeof(*s), (uintptr_t)c, sizeof(*c)) ||
        !a733_span_excl_nonempty(c->firmware, c->firmware_bytes) ||
        !a733_extents_disjoint_prechecked((uintptr_t)s, sizeof(*s), (uintptr_t)c->firmware, c->firmware_bytes) ||
        !pvr_fw_requirements(c->firmware, c->firmware_bytes, PVR_FW_A733_BVNC, &plan) ||
        c->state.bvnc != PVR_FW_A733_BVNC || !c->state.gpu_clock_hz ||
        c->state.clusters != 1 || c->state.slc_kilobytes != 16) return 0;
    /* Cross-component alias checks precede every write. The individual builders
     * validate their own subsets; neither alone knows all output allocations. */
    for (unsigned i = 0; i < 3 + PVR_FW_RESOURCE_COUNT; ++i) {
        const struct pvr_mips_buffer *b = buffer(c, i);
        size_t expected = i < 2 ? plan.allocation_bytes[i] : i == 2 ? PVR_MIPS_TABLE_BYTES :
            pvr_fw_state_layout((enum pvr_fw_resource)(i - 3))->allocation_bytes;
        if (b->storage.bytes != expected || b->page_count != expected / PVR_MIPS_PAGE_BYTES ||
            !a733_span_excl_nonempty(b->storage.cpu, expected) ||
            !a733_span_excl_nonempty(b->pages, b->page_count * sizeof(uint64_t)) ||
            !a733_extents_disjoint_prechecked((uintptr_t)s, sizeof(*s), (uintptr_t)b->pages, b->page_count * sizeof(uint64_t)) ||
            !a733_extents_disjoint_prechecked((uintptr_t)b->storage.cpu, expected, (uintptr_t)s, sizeof(*s)) ||
            !a733_extents_disjoint_prechecked((uintptr_t)b->storage.cpu, expected, (uintptr_t)c, sizeof(*c)) ||
            !a733_extents_disjoint_prechecked((uintptr_t)b->storage.cpu, expected, (uintptr_t)c->firmware, c->firmware_bytes)) return 0;
        for (unsigned j = 0; j < 3 + PVR_FW_RESOURCE_COUNT; ++j) {
            const struct pvr_mips_buffer *a = buffer(c, j);
            /* Validate lengths before pointer arithmetic even for later entries. */
            if (a->page_count > PVR_FW_MAX_ALLOCATION / PVR_MIPS_PAGE_BYTES ||
                !a733_span_excl_nonempty(a->pages, a->page_count * sizeof(uint64_t)) ||
                !a733_extents_disjoint_prechecked((uintptr_t)b->storage.cpu, expected, (uintptr_t)a->pages, a->page_count * sizeof(uint64_t))) return 0;
            if (j < i && !a733_extents_disjoint_prechecked((uintptr_t)b->storage.cpu, expected, (uintptr_t)a->storage.cpu, a->storage.bytes)) return 0;
        }
        for (unsigned p = 0; p < b->page_count; ++p) {
            uint64_t pa = b->pages[p];
            if (!pa || (pa & 4095) || pa >= (UINT64_C(1) << 36) ||
                (pa >= 0x1fc00000 && pa < 0x1fc04000)) return 0;
            for (unsigned j = 0; j <= i; ++j) {
                const struct pvr_mips_buffer *a = buffer(c, j);
                unsigned count = j == i ? p : a->page_count;
                for (unsigned q = 0; q < count; ++q) if (a->pages[q] == pa) return 0;
            }
        }
    }
    if (c->compute && (c->compute->service != s ||
        c->compute->phase != PVR_COMPUTE_SERVICE_BOUND)) return 0;
    if (c->render && (!c->graphics || c->render != &c->graphics->submission)) return 0;
    if (c->mimus && (c->compute || c->graphics || c->free_lists.count || c->mimus->service != s ||
        c->mimus->phase != PVR_MIMUS_SERVICE_BOUND)) return 0;
    if (c->graphics && !a733_gpu_graphics_memory_validate(c->graphics, c, s)) return 0;
    return free_list_allocations(s, c);
}
/* Interrupt state is shared with an ISR that may run on another core.
 * FreeRTOS SMP critical sections mask local API-safe interrupts and hold the
 * kernel lock, preventing a rearm from passing an in-flight mask operation. */
static int interrupt_mask(struct pvr_gpu_service *s)
{
    if (!s->config.irq.mask) return 1;
    taskENTER_CRITICAL();
    if (!s->irq_masked && !s->irq_fault) {
        s->irq_masked = 1;
        if (!s->config.irq.mask(s->config.context)) s->irq_fault = 1;
    }
    int ok = !s->irq_fault;
    taskEXIT_CRITICAL();
    return ok;
}
static int interrupt_rearm(struct pvr_gpu_service *s)
{
    if (!s->config.irq.rearm) return 1;
    taskENTER_CRITICAL();
    if (s->irq_masked && !s->irq_fault) {
        int result = s->config.irq.rearm(s->config.context);
        if (result < 0) s->irq_fault = 1;
        else if (result == 1) { s->irq_masked = 0; s->irq_deferred = 0; }
        else if (result != 0 || ++s->irq_deferred >= PVR_GPU_SERVICE_IRQ_REARM_ATTEMPTS) s->irq_fault = 1;
    }
    int ok = !s->irq_fault;
    taskEXIT_CRITICAL();
    return ok;
}
int pvr_gpu_service_interrupt(struct pvr_gpu_service *s, BaseType_t *woken)
{
    if (!s || s != instance || !s->initialized || !s->config.irq.mask || !woken) return 0;
    UBaseType_t saved = taskENTER_CRITICAL_FROM_ISR();
    if (!s->irq_masked && !s->irq_fault) {
        s->irq_masked = 1;
        if (!s->config.irq.mask(s->config.context)) s->irq_fault = 1;
    }
    if (!s->irq_wake_pending) {
        const struct pvr_gpu_service_message wake = {.client = PVR_GPU_SERVICE_CLIENTS};
        if (xQueueSendFromISR(s->requests, &wake, woken) == pdTRUE) s->irq_wake_pending = 1;
    }
    int ok = !s->irq_fault;
    taskEXIT_CRITICAL_FROM_ISR(saved);
    return ok;
}
static void fail(struct pvr_gpu_service *s, enum pvr_gpu_service_error error)
{
    if (!interrupt_mask(s)) error = PVR_GPU_SERVICE_INTERRUPT;
    s->phase = PVR_GPU_SERVICE_FAILED; s->error = error;
    if (s->config.compute) pvr_compute_service_abort(s->config.compute, PVR_COMPUTE_SERVICE_LIFECYCLE);
    if (s->config.mimus) pvr_mimus_service_abort(s->config.mimus);
    if (s->config.graphics) a733_gpu_graphics_memory_abort(s->config.graphics);
    (void)pvr_gpu_start_cancel(&s->start);
    (void)pvr_gpu_control_abort(&s->control);
    (void)pvr_ccb_abort(&s->queue);
}
/* The worker owns one monotonic clock. Startup, control, shutdown, the queue
 * users, free lists, compute and render all sample time through it, so
 * last_now is the latest sample of any component. A backward sample faults
 * here before the caller performs I/O. */
static int sample(struct pvr_gpu_service *s, uint64_t *now)
{
    if (s->phase == PVR_GPU_SERVICE_FAILED || s->config.start.now(s->config.context, now) != 1 ||
        *now < s->last_now) return 0;
    s->last_now = *now; return 1;
}
int pvr_gpu_service_now(struct pvr_gpu_service *s, uint64_t *now)
{ return s && s == instance && s->initialized && now && sample(s, now); }
static int service_clock(void *ctx, uint64_t *now)
{
    struct pvr_gpu_service *s = instance;
    return s && ctx == s->config.context && sample(s, now);
}
/* UPDATE_STATS reports a signed change to one firmware counter. The firmware
 * names process zero because no context carries a process identity. */
static int statistics(void *ctx, const struct pvr_gpu_event *e,
                      const struct pvr_gpu_event_resource *const *targets, unsigned count)
{
    struct pvr_gpu_service *s = ctx;
    if (s != instance || e->kind != PVR_GPU_EVENT_STATS || count != 1 ||
        targets[0]->kind != PVR_GPU_RESOURCE_PROCESS || !e->data.stats.statistic ||
        e->data.stats.statistic > PVR_GPU_SERVICE_STATISTICS) return 0;
    int64_t *value = &s->statistics[e->data.stats.statistic - 1];
    int32_t change = e->data.stats.adjustment;
    if ((change > 0 && *value > INT64_MAX - change) || (change < 0 && *value < INT64_MIN - change)) return 0;
    *value += change; return 1;
}
static int service_event(void *ctx, const unsigned char command[PVR_CCB_COMMAND_BYTES])
{
    struct pvr_gpu_service *s = instance;
    if (!s || ctx != s->config.context || s->phase == PVR_GPU_SERVICE_FAILED) return 0;
    if (s->config.compute && !pvr_compute_service_event(s->config.compute, command)) return 0;
    if (s->config.mimus && !pvr_mimus_service_event(s->config.mimus, command)) return 0;
    uint32_t kind = (uint32_t)command[0] | (uint32_t)command[1] << 8 |
                    (uint32_t)command[2] << 16 | (uint32_t)command[3] << 24;
    if (kind == (PVR_CCB_MAGIC | 107u))
        return pvr_gpu_events_dispatch(&s->statistics_events, s->epoch, command, PVR_CCB_COMMAND_BYTES);
    if (s->graphics_events_retired && (kind == (PVR_CCB_MAGIC | 103u) || kind == (PVR_CCB_MAGIC | 104u)))
        return 0; /* Includes synchronous event dispatch inside stop/control polls. */
    if (s->config.free_lists.count && (kind == (PVR_CCB_MAGIC | 103u) || kind == (PVR_CCB_MAGIC | 104u)))
        return pvr_gpu_events_dispatch(&s->free_list_events, s->epoch, command, PVR_CCB_COMMAND_BYTES);
    return s->config.control.event && s->config.control.event(ctx, command);
}
/* Deadline of the pending free-list operation, or 0 when none is pending. */
static uint64_t free_list_deadline(const struct pvr_gpu_service *s)
{
    if (s->free_lists.phase != PVR_FREE_LIST_IDLE) return s->free_lists.deadline;
    if (s->free_lists.request_count) return s->free_lists.requests[s->free_lists.request_head].deadline;
    return 0;
}
static enum pvr_free_list_result free_list_progress(struct pvr_gpu_service *s, int closing)
{
    uint64_t now;
    if (!sample(s, &now)) {
        fail(s, PVR_GPU_SERVICE_CLOCK); return PVR_FREE_LIST_FAULT;
    }
    if (closing && now >= s->deadline) {
        fail(s, PVR_GPU_SERVICE_STOP); return PVR_FREE_LIST_FAULT;
    }
    /* Capture the deadline before completion can move the manager to IDLE or
     * consume its request. A bounded callback can still finish too late. */
    uint64_t operation_deadline = free_list_deadline(s);
    enum pvr_free_list_result r = pvr_free_lists_poll(&s->free_lists, now);
    if (r == PVR_FREE_LIST_FAULT) { fail(s, PVR_GPU_SERVICE_FREE_LISTS); return r; }
    if (operation_deadline) {
        if (!sample(s, &now)) {
            fail(s, PVR_GPU_SERVICE_CLOCK); return PVR_FREE_LIST_FAULT;
        }
        if ((closing && now >= s->deadline) || now >= operation_deadline) {
            fail(s, closing && now >= s->deadline ? PVR_GPU_SERVICE_STOP : PVR_GPU_SERVICE_FREE_LISTS);
            return PVR_FREE_LIST_FAULT;
        }
    }
    return r;
}
static int deadline(struct pvr_gpu_service *s, uint64_t duration)
{
    uint64_t now;
    if (!sample(s, &now) || !a733_deadline_after(now, duration, &s->deadline)) {
        fail(s, PVR_GPU_SERVICE_CLOCK); return 0;
    }
    return 1;
}
static int opening_clock(struct pvr_gpu_service *s)
{
    uint64_t now;
    if (!sample(s, &now) || now >= s->deadline) { fail(s, PVR_GPU_SERVICE_CLOCK); return 0; }
    return 1;
}
static int start_firmware(struct pvr_gpu_service *s)
{
    if ((s->config.compute && pvr_compute_service_admit_mode(s->config.compute) != 1) ||
        (s->config.mimus && s->config.compute_mode(s->config.context) != 1)) {
        fail(s, PVR_GPU_SERVICE_COMPUTATION); return 0;
    }
    if (pvr_gpu_start_begin(&s->start, s->deadline) != PVR_GPU_START_AGAIN) {
        fail(s, PVR_GPU_SERVICE_START); return 0;
    }
    s->phase = PVR_GPU_SERVICE_STARTING; return 1;
}
static int enable_firmware(struct pvr_gpu_service *s)
{
    const struct pvr_gpu_service_config *c = &s->config;
    if (!opening_clock(s)) return 0;
    if (c->power.begin) {
        s->phase = PVR_GPU_SERVICE_ENABLING;
        if (c->power.begin(c->context, s->deadline) != 1) {
            fail(s, PVR_GPU_SERVICE_ENABLE); return 0;
        }
        return opening_clock(s);
    }
    if (!c->enable(c->context)) { fail(s, PVR_GPU_SERVICE_ENABLE); return 0; }
    return start_firmware(s);
}
static int publish_compute(struct pvr_gpu_service *s)
{
    if (s->config.mimus) {
        if (pvr_mimus_service_prepare(s->config.mimus, s->deadline) != 1) {
            fail(s, PVR_GPU_SERVICE_COMPUTATION); return 0;
        }
        s->phase = PVR_GPU_SERVICE_PUBLISHING; return 1;
    }
    if (!s->config.compute) return enable_firmware(s);
    if (pvr_compute_service_prepare(s->config.compute, s->deadline) != 1) {
        fail(s, PVR_GPU_SERVICE_COMPUTATION); return 0;
    }
    s->phase = PVR_GPU_SERVICE_PUBLISHING; return 1;
}
static int publish_resources(struct pvr_gpu_service *s)
{
    if (!s->config.graphics) return publish_compute(s);
    if (a733_gpu_graphics_memory_begin(s->config.graphics) != 1) {
        fail(s, PVR_GPU_SERVICE_GRAPHICS); return 0;
    }
    s->phase = PVR_GPU_SERVICE_PUBLISHING_GRAPHICS; return 1;
}
static int prepare(struct pvr_gpu_service *s)
{
    const struct pvr_gpu_service_config *c = &s->config;
    if (c->graphics && !a733_gpu_graphics_memory_validate(c->graphics, c, s)) {
        fail(s, PVR_GPU_SERVICE_GRAPHICS); return 0;
    }
    memset(&s->queue, 0, sizeof(s->queue)); memset(&s->start, 0, sizeof(s->start));
    memset(&s->control, 0, sizeof(s->control)); memset(&s->stop, 0, sizeof(s->stop));
    struct pvr_gpu_start_ops start_ops = c->start;
    start_ops.now = service_clock;
    if (!pvr_fw_state_prepare(c->resources, &c->state, s->mappings)) {
        fail(s, PVR_GPU_SERVICE_PREPARATION); return 0;
    }
    unsigned objects = PVR_FW_RESOURCE_COUNT;
    if (c->compute) {
        _Static_assert(PVR_FW_RESOURCE_COUNT + PVR_COMPUTE_SERVICE_OBJECTS <= PVR_MIPS_MAX_OBJECTS,
                       "firmware object capacity");
        memcpy(s->mappings + objects, c->compute->objects, sizeof(c->compute->objects));
        objects += PVR_COMPUTE_SERVICE_OBJECTS;
    }
    if (c->mimus) {
        _Static_assert(PVR_FW_RESOURCE_COUNT + PVR_MIMUS_SERVICE_OBJECTS <= PVR_MIPS_MAX_OBJECTS,
                       "mimus firmware object capacity");
        memcpy(s->mappings + objects, c->mimus->objects, sizeof(c->mimus->objects));
        objects += PVR_MIMUS_SERVICE_OBJECTS;
    }
    if (c->graphics) {
        _Static_assert(PVR_FW_RESOURCE_COUNT + PVR_COMPUTE_SERVICE_OBJECTS + 2 <= PVR_MIPS_MAX_OBJECTS,
                       "combined firmware object capacity");
        s->mappings[objects++] = c->graphics->object;
        if(c->render) s->mappings[objects++] = c->graphics->render_object;
    }
    if (!pvr_mips_prepare(c->firmware, c->firmware_bytes, c->firmware_buffers,
            s->mappings, objects, &s->boot) ||
        !pvr_gpu_start_init(&s->start, c->firmware, c->firmware_bytes, &s->boot,
            &start_ops, c->context) ||
        pvr_ccb_init(&s->queue, &c->queue, c->context, s->epoch) != PVR_CCB_OK) {
        fail(s, PVR_GPU_SERVICE_PREPARATION); return 0;
    }
    /* Each OPEN receives fresh statistics under its own queue epoch. */
    memset(&s->statistics_events, 0, sizeof(s->statistics_events));
    memset(s->statistics, 0, sizeof(s->statistics));
    const struct pvr_gpu_event_resource process = {PVR_GPU_RESOURCE_PROCESS, 0, (uintptr_t)s, s->epoch};
    const struct pvr_gpu_event_handlers recorded = {.stats = statistics};
    if (!pvr_gpu_events_init(&s->statistics_events, s->epoch, &process, 1, &recorded, s)) {
        fail(s, PVR_GPU_SERVICE_PREPARATION); return 0;
    }
    if (c->free_lists.count) {
        if (!opening_clock(s)) return 0;
        const struct pvr_gpu_service_free_list_config *f = &c->free_lists;
        struct pvr_gpu_event_resource resources[PVR_FREE_LISTS];
        for (unsigned i = 0; i < f->count; ++i) resources[i] = f->lists[i].resource;
        const struct pvr_gpu_event_handlers handlers = {
            .grow = pvr_free_lists_event, .reconstruct = pvr_free_lists_event};
        if (!pvr_free_lists_init(&s->free_lists, f->lists, f->count, f->minimum_pages,
                &f->ops, c->context, &s->queue, s->last_now, f->operation_timeout_us) ||
            !pvr_gpu_events_init(&s->free_list_events, s->epoch, resources, f->count,
                &handlers, &s->free_lists)) {
            fail(s, PVR_GPU_SERVICE_FREE_LISTS); return 0;
        }
        s->phase = PVR_GPU_SERVICE_BUILDING; return 1;
    }
    return publish_resources(s);
}
static int begin_stop(struct pvr_gpu_service *s)
{
    struct pvr_gpu_stop_ops ops = s->config.stop;
    ops.now = service_clock;
    if (!pvr_gpu_stop_init(&s->stop, &s->control, s->config.firmware,
            s->config.firmware_bytes, &ops, s->config.context, s->deadline)) {
        fail(s, PVR_GPU_SERVICE_STOP); return 0;
    }
    s->phase = PVR_GPU_SERVICE_STOPPING; return 1;
}
/* A compute job or render the worker polls, from its admission until it
 * completes or fails. These phases set the wait bound. */
static int compute_polled(const struct pvr_gpu_service *s)
{
    const struct pvr_compute_service *c = s->config.compute;
    return c && (c->phase == PVR_COMPUTE_SERVICE_FORMAT || c->phase == PVR_COMPUTE_SERVICE_RUNNING);
}
static int render_polled(const struct pvr_gpu_service *s)
{
    const struct pvr_render_service *r = s->config.render;
    return r && r->phase >= PVR_RENDER_SERVICE_FORMAT_GEOMETRY &&
        (r->phase < PVR_RENDER_SERVICE_COMPLETE || r->phase == PVR_RENDER_SERVICE_TEXTURE_PREPARE);
}
/* A compute job or render between the end of its preparation and its kick:
 * the ring writes, whose sizes are fixed. Only this window is exempt from the
 * CPU-run block. Texture preparation, which fills the whole output, and every
 * phase after the kick take the block. */
static int submitting(const struct pvr_gpu_service *s)
{
    const struct pvr_compute_service *c = s->config.compute;
    const struct pvr_render_service *r = s->config.render;
    return (c && (c->phase == PVR_COMPUTE_SERVICE_FORMAT ||
                  (c->phase == PVR_COMPUTE_SERVICE_RUNNING && c->job.phase <= PVR_JOB_KICK))) ||
        (r && r->phase >= PVR_RENDER_SERVICE_FORMAT_GEOMETRY && r->phase <= PVR_RENDER_SERVICE_SUBMIT) ||
        (s->config.mimus && pvr_mimus_service_submitting(s->config.mimus));
}
static void advance(struct pvr_gpu_service *s)
{
    if (s->phase == PVR_GPU_SERVICE_FAILED) return;
    if (!interrupt_mask(s)) { fail(s, PVR_GPU_SERVICE_INTERRUPT); return; }
    int rearm = 0;
    switch (s->phase) {
    case PVR_GPU_SERVICE_BUILDING: {
        if (!opening_clock(s)) return;
        enum pvr_free_list_result r = free_list_progress(s, 0);
        if (r == PVR_FREE_LIST_FAULT || !opening_clock(s)) return;
        if (r == PVR_FREE_LIST_OK) (void)publish_resources(s);
        return;
    }
    case PVR_GPU_SERVICE_PUBLISHING_GRAPHICS: {
        if (!opening_clock(s)) return;
        int r = a733_gpu_graphics_memory_poll(s->config.graphics);
        if (r < 0) { fail(s, PVR_GPU_SERVICE_GRAPHICS); return; }
        if (!opening_clock(s)) return;
        if (r == 1) (void)publish_compute(s);
        return;
    }
    case PVR_GPU_SERVICE_PUBLISHING: {
        if (!opening_clock(s)) return;
        int r = s->config.mimus ? pvr_mimus_service_publish_poll(s->config.mimus) :
            pvr_compute_service_poll(s->config.compute);
        if (r < 0) { fail(s, PVR_GPU_SERVICE_COMPUTATION); return; }
        if (!opening_clock(s)) return;
        if (r == 1) (void)enable_firmware(s);
        return;
    }
    case PVR_GPU_SERVICE_ENABLING: {
        if (!opening_clock(s)) return;
        int r = s->config.power.poll(s->config.context);
        if (r != 0 && r != 1) { fail(s, PVR_GPU_SERVICE_ENABLE); return; }
        if (!opening_clock(s)) return;
        if (r == 1 && !start_firmware(s)) return;
        return;
    }
    case PVR_GPU_SERVICE_STARTING: {
        enum pvr_gpu_start_result r = pvr_gpu_start_poll(&s->start);
        if (r == PVR_GPU_START_OK) {
            struct pvr_gpu_control_ops ops = s->config.control;
            ops.now = service_clock; ops.event = service_event;
            if (!pvr_gpu_control_init(&s->control, &s->start, &s->queue,
                    &ops, s->config.context, s->deadline)) {
                fail(s, PVR_GPU_SERVICE_CONTROL); return;
            }
            s->phase = PVR_GPU_SERVICE_CHECKING;
        } else if (r != PVR_GPU_START_AGAIN) { fail(s, PVR_GPU_SERVICE_START); return; }
        break;
    }
    case PVR_GPU_SERVICE_CHECKING:
    case PVR_GPU_SERVICE_READY: {
        enum pvr_gpu_control_result r = pvr_gpu_control_poll(&s->control, 4);
        if (r == PVR_GPU_CONTROL_OK) { s->phase = PVR_GPU_SERVICE_READY; rearm = 1; }
        else if (r != PVR_GPU_CONTROL_AGAIN) { fail(s, PVR_GPU_SERVICE_CONTROL); return; }
        if (s->config.free_lists.count) {
            if (free_list_progress(s, 0) == PVR_FREE_LIST_FAULT) return;
        }
        if (compute_polled(s) && pvr_compute_service_poll(s->config.compute) < 0) {
            fail(s, PVR_GPU_SERVICE_COMPUTATION); return;
        }
        if (render_polled(s) && pvr_render_service_poll(s->config.render) < 0) {
            fail(s, PVR_GPU_SERVICE_GRAPHICS); return;
        }
        if (s->config.mimus && s->phase == PVR_GPU_SERVICE_READY &&
            pvr_mimus_service_poll(s->config.mimus) < 0) {
            fail(s, PVR_GPU_SERVICE_COMPUTATION); return;
        }
        break;
    }
    case PVR_GPU_SERVICE_COMPUTE_CLEANUP: {
        enum pvr_gpu_control_result control=pvr_gpu_control_poll(&s->control,4);
        if(control!=PVR_GPU_CONTROL_OK&&control!=PVR_GPU_CONTROL_AGAIN) {
            fail(s,PVR_GPU_SERVICE_CONTROL);return;
        }
        int result=pvr_compute_service_cleanup_poll(s->config.compute);
        if(result<0){fail(s,PVR_GPU_SERVICE_COMPUTATION);return;}
        if(result==1)(void)begin_stop(s);
        return;
    }
    case PVR_GPU_SERVICE_GRAPHICS_CLEANUP: {
        uint64_t current;
        if(!sample(s,&current) || current>=s->deadline) {
            fail(s,PVR_GPU_SERVICE_STOP);return;
        }
        enum pvr_gpu_control_result control=pvr_gpu_control_poll(&s->control,4);
        if((control!=PVR_GPU_CONTROL_OK && control!=PVR_GPU_CONTROL_AGAIN) ||
           s->free_lists.phase!=PVR_FREE_LIST_IDLE || s->free_lists.request_count) {
            fail(s,PVR_GPU_SERVICE_CONTROL);return;
        }
        if(s->config.render) {
            int formatted=pvr_render_service_cleanup_format(s->config.render);
            if(formatted<0){fail(s,PVR_GPU_SERVICE_GRAPHICS);return;}
            if(!formatted)return;
        }
        unsigned contexts=s->config.render?2:0,index=s->graphics_cleanup_index;
        if(index==contexts && s->config.compute->phase!=PVR_COMPUTE_SERVICE_CLEANED) {
            if(pvr_compute_service_cleanup_poll(s->config.compute)<0)fail(s,PVR_GPU_SERVICE_COMPUTATION);
            return;
        }
        if(index==s->graphics_cleanup_count) {
            if(s->config.render)s->config.render->phase=PVR_RENDER_SERVICE_CLEANED;
            (void)begin_stop(s);return;
        }
        if(!s->graphics_cleanup_tokens[index].serial) {
            uint32_t command[PVR_CCB_COMMAND_BYTES/4]={PVR_CCB_MAGIC|106u,0,
                s->graphics_cleanup_type[index],s->graphics_cleanup_address[index]};
            enum pvr_ccb_result r=pvr_ccb_submit(&s->queue,command,s->last_now,s->deadline,
                &s->graphics_cleanup_tokens[index]);
            if(r!=PVR_CCB_OK && r!=PVR_CCB_AGAIN)fail(s,PVR_GPU_SERVICE_GRAPHICS);
            return;
        }
        enum pvr_ccb_result r=pvr_ccb_collect(&s->queue,s->graphics_cleanup_tokens[index],
            &s->graphics_cleanup_status[index]);
        if(r==PVR_CCB_AGAIN)return;
        if(r!=PVR_CCB_OK || s->graphics_cleanup_status[index]!=1 ||
           !sample(s,&current) || current>=s->deadline) {
            fail(s,PVR_GPU_SERVICE_GRAPHICS);return;
        }
        ++s->graphics_cleanup_index;return;
    }
#ifdef SUNSTONE_GPU_LIFECYCLE
    case PVR_GPU_SERVICE_DISPOSING: {
        int result=a733_gpu_disposal_poll(s);
        if(result<0){fail(s,PVR_GPU_SERVICE_COMPUTATION);return;}
        if(result==1)s->phase=PVR_GPU_SERVICE_DISPOSED;
        return;
    }
#endif
    case PVR_GPU_SERVICE_DRAINING: {
        uint64_t current;
        if (!sample(s, &current)) { fail(s, PVR_GPU_SERVICE_CLOCK); return; }
        if (current >= s->deadline) { fail(s, PVR_GPU_SERVICE_STOP); return; }
        enum pvr_gpu_control_result r = pvr_gpu_control_poll(&s->control, 4);
        if (r != PVR_GPU_CONTROL_OK && r != PVR_GPU_CONTROL_AGAIN) {
            fail(s, PVR_GPU_SERVICE_CONTROL); return;
        }
        if (free_list_progress(s, 1) == PVR_FREE_LIST_FAULT) return;
        if (r == PVR_GPU_CONTROL_OK && !s->free_lists.request_count && s->free_lists.phase==PVR_FREE_LIST_IDLE) {
            if(s->graphics_cleanup_count) {
                s->graphics_events_retired=1;s->phase=PVR_GPU_SERVICE_GRAPHICS_CLEANUP;
            } else (void)begin_stop(s);
        }
        return;
    }
    case PVR_GPU_SERVICE_STOPPING: {
        if (s->config.free_lists.count) {
            /* Stop polling can itself dispatch events. Pause before advancing
             * another shutdown step while any response remains owned. */
            uint64_t current;
            if (!sample(s, &current)) { fail(s, PVR_GPU_SERVICE_CLOCK); return; }
            if (current >= s->deadline) { fail(s, PVR_GPU_SERVICE_STOP); return; }
            unsigned had_pending = s->free_lists.request_count;
            if (had_pending || s->stop.phase == PVR_GPU_STOP_RETIRE) {
                enum pvr_gpu_control_result progress = pvr_gpu_control_poll(&s->control, 4);
                if (progress != PVR_GPU_CONTROL_OK && progress != PVR_GPU_CONTROL_AGAIN) {
                    fail(s, PVR_GPU_SERVICE_CONTROL); return;
                }
                if (free_list_progress(s, 1) == PVR_FREE_LIST_FAULT) return;
                if (had_pending || s->free_lists.request_count || progress == PVR_GPU_CONTROL_AGAIN) return;
            }
        }
        enum pvr_gpu_stop_result r = pvr_gpu_stop_poll(&s->stop);
        if (r == PVR_GPU_STOP_OK) {
            s->phase = PVR_GPU_SERVICE_STOPPED;
        } else if (r != PVR_GPU_STOP_AGAIN) { fail(s, PVR_GPU_SERVICE_STOP); return; }
        if (s->config.free_lists.count) {
            if (s->queue.phase == PVR_CCB_ACTIVE && free_list_progress(s, 1) == PVR_FREE_LIST_FAULT) return;
        }
        break;
    }
    default: return;
    }
    if (s->phase == PVR_GPU_SERVICE_STOPPED) s->owner = PVR_GPU_SERVICE_CLIENTS;
    if (rearm && !interrupt_rearm(s)) fail(s, PVR_GPU_SERVICE_INTERRUPT);
}
static enum pvr_gpu_service_result execute(struct pvr_gpu_service *s,
                                           const struct pvr_gpu_service_message *m)
{
    const struct pvr_gpu_service_request *r = &m->request;
    if ((unsigned)r->operation > PVR_GPU_SERVICE_DISPOSE ||
        ((r->operation == PVR_GPU_SERVICE_STATUS || r->operation == PVR_GPU_SERVICE_RENDER_OUTPUT ||
          r->operation == PVR_GPU_SERVICE_COMPUTE_OUTPUT || r->operation == PVR_GPU_SERVICE_COMPUTE_CANCEL) ? r->timeout_us != 0 :
         (!r->timeout_us || r->timeout_us > INT64_MAX))) return PVR_GPU_SERVICE_INVALID;
    if (r->operation == PVR_GPU_SERVICE_STATUS)
        return s->phase == PVR_GPU_SERVICE_FAILED ? PVR_GPU_SERVICE_FAULT : PVR_GPU_SERVICE_OK;
    if(m->epoch!=s->epoch)return PVR_GPU_SERVICE_INVALID;
#ifdef SUNSTONE_GPU_LIFECYCLE
    if(r->operation==PVR_GPU_SERVICE_DISPOSE) {
        if(s->phase==PVR_GPU_SERVICE_FAILED)return PVR_GPU_SERVICE_FAULT;
        if(m->client!=s->closed_owner || !s->closed_epoch || m->epoch!=s->closed_epoch)return PVR_GPU_SERVICE_DENIED;
        if(s->phase!=PVR_GPU_SERVICE_STOPPED)return PVR_GPU_SERVICE_BUSY;
        if(!deadline(s,r->timeout_us))return PVR_GPU_SERVICE_FAULT;
        if(a733_gpu_disposal_begin(s,m->epoch)!=1)return PVR_GPU_SERVICE_INVALID;
        s->phase=PVR_GPU_SERVICE_DISPOSING;return PVR_GPU_SERVICE_OK;
    }
#else
    if(r->operation==PVR_GPU_SERVICE_DISPOSE)return PVR_GPU_SERVICE_INVALID;
#endif
    if (r->operation == PVR_GPU_SERVICE_OPEN) {
        if (s->phase == PVR_GPU_SERVICE_FAILED) return PVR_GPU_SERVICE_FAULT;
        if (s->owner != PVR_GPU_SERVICE_CLIENTS) return PVR_GPU_SERVICE_BUSY;
#ifdef SUNSTONE_GPU_LIFECYCLE
        if(s->phase==PVR_GPU_SERVICE_DISPOSED) {
            if(m->client!=s->closed_owner || s->epoch==UINT64_MAX)return PVR_GPU_SERVICE_DENIED;
            if(!deadline(s,r->timeout_us))return PVR_GPU_SERVICE_FAULT;
            struct a733_gpu_disposal *d=a733_gpu_disposal_owner(s);
            if(!d)return PVR_GPU_SERVICE_INVALID;
            if(s->deadline>d->limit)s->deadline=d->limit;
            s->phase=PVR_GPU_SERVICE_RECONSTRUCTING;
            if(a733_gpu_disposal_rebuild(s)!=1){fail(s,PVR_GPU_SERVICE_COMPUTATION);return PVR_GPU_SERVICE_FAULT;}
            memset(&s->free_lists,0,sizeof(s->free_lists));memset(&s->free_list_events,0,sizeof(s->free_list_events));
            memset(s->graphics_cleanup_tokens,0,sizeof(s->graphics_cleanup_tokens));
            memset(s->graphics_cleanup_status,0,sizeof(s->graphics_cleanup_status));
            memset(s->graphics_cleanup_address,0,sizeof(s->graphics_cleanup_address));
            memset(s->graphics_cleanup_type,0,sizeof(s->graphics_cleanup_type));
            s->graphics_cleanup_count=s->graphics_cleanup_index=s->graphics_events_retired=0;
            s->owner=m->client;++s->epoch;
            atomic_store_explicit(&s->published_epoch,s->epoch,memory_order_release);
            if(!prepare(s))return PVR_GPU_SERVICE_FAULT;
            return PVR_GPU_SERVICE_OK;
        }
#endif
        if ((s->phase != PVR_GPU_SERVICE_COLD && s->phase != PVR_GPU_SERVICE_STOPPED) ||
            s->epoch == UINT64_MAX) return PVR_GPU_SERVICE_BUSY;
        if ((s->config.free_lists.count || s->config.compute) && s->phase == PVR_GPU_SERVICE_STOPPED)
            return PVR_GPU_SERVICE_BUSY;
        s->owner = m->client; ++s->epoch;
        atomic_store_explicit(&s->published_epoch,s->epoch,memory_order_release);
        if (!deadline(s, r->timeout_us) || !prepare(s)) return PVR_GPU_SERVICE_FAULT;
        return PVR_GPU_SERVICE_OK;
    }
    if (s->owner != m->client) return PVR_GPU_SERVICE_DENIED;
    if (s->phase == PVR_GPU_SERVICE_FAILED) return PVR_GPU_SERVICE_FAULT;
    if (s->phase != PVR_GPU_SERVICE_READY) return PVR_GPU_SERVICE_BUSY;
    if(r->operation==PVR_GPU_SERVICE_TEXTURE) {
        struct pvr_render_service *d=s->config.render;
        if(!d || !d->owner || d->owner->draw<2 || !s->config.compute ||
            atomic_load_explicit(&d->texture_state,memory_order_acquire)!=2 ||
            d->texture_ticket!=m->ticket || d->texture_client!=m->client || d->texture_epoch!=s->epoch)return PVR_GPU_SERVICE_INVALID;
        if((d->phase!=PVR_RENDER_SERVICE_READY && d->phase!=PVR_RENDER_SERVICE_COMPLETE) ||
            (s->config.compute->phase!=PVR_COMPUTE_SERVICE_READY && s->config.compute->phase!=PVR_COMPUTE_SERVICE_COMPLETE))
            return PVR_GPU_SERVICE_BUSY;
        if(!deadline(s,r->timeout_us))return PVR_GPU_SERVICE_FAULT;
        int result=pvr_render_service_texture_request(d,s->deadline);
        if(result<0){fail(s,PVR_GPU_SERVICE_GRAPHICS);return PVR_GPU_SERVICE_FAULT;}
        return result?PVR_GPU_SERVICE_OK:PVR_GPU_SERVICE_INVALID;
    }
    if(r->operation==PVR_GPU_SERVICE_CLOSE && s->config.render &&
        atomic_load_explicit(&s->config.render->texture_output_state,memory_order_acquire))return PVR_GPU_SERVICE_BUSY;
    if (r->operation == PVR_GPU_SERVICE_RENDER_OUTPUT) {
        if (!s->config.render || (s->config.render->owner && s->config.render->owner->draw>=2) || m->render_first_pixel >= 4096 || (m->render_first_pixel & 63))
            return PVR_GPU_SERVICE_INVALID;
        if(m->render_sequence && m->render_sequence!=s->config.render->sequence)return PVR_GPU_SERVICE_INVALID;
        return s->config.render->phase == PVR_RENDER_SERVICE_COMPLETE ? PVR_GPU_SERVICE_OK : PVR_GPU_SERVICE_BUSY;
    }
    if (r->operation == PVR_GPU_SERVICE_COMPUTE_OUTPUT) {
        if (!s->config.compute || m->compute_first_word >= pvr_compute_job_result_words(s->config.compute->config->job)) return PVR_GPU_SERVICE_INVALID;
        if(m->compute_sequence && m->compute_sequence!=s->config.compute->job.sequence)
            return PVR_GPU_SERVICE_INVALID;
        return s->config.compute->phase == PVR_COMPUTE_SERVICE_COMPLETE ? PVR_GPU_SERVICE_OK : PVR_GPU_SERVICE_BUSY;
    }
    if(r->operation==PVR_GPU_SERVICE_COMPUTE_INPUT) {
        if(s->config.render&&s->config.render->phase!=PVR_RENDER_SERVICE_READY&&
           s->config.render->phase!=PVR_RENDER_SERVICE_COMPLETE)return PVR_GPU_SERVICE_BUSY;
        struct pvr_compute_service *c=s->config.compute;
        if(!c||atomic_load_explicit(&c->input_state,memory_order_acquire)!=2||
           c->input_ticket!=m->ticket||c->input_client!=m->client)return PVR_GPU_SERVICE_INVALID;
        if(!deadline(s,r->timeout_us))return PVR_GPU_SERVICE_FAULT;
        int accepted=pvr_compute_service_replace(c,m->compute_sequence,c->input_copy,c->input_bytes,s->deadline);
        if(accepted<0){fail(s,PVR_GPU_SERVICE_COMPUTATION);return PVR_GPU_SERVICE_FAULT;}
        return accepted?PVR_GPU_SERVICE_OK:PVR_GPU_SERVICE_INVALID;
    }
    if (r->operation == PVR_GPU_SERVICE_COMPUTE_CANCEL) {
        if (!s->config.compute) return PVR_GPU_SERVICE_INVALID;
        pvr_compute_service_abort(s->config.compute,PVR_COMPUTE_SERVICE_CANCELLED);
        fail(s,PVR_GPU_SERVICE_COMPUTATION);
        return PVR_GPU_SERVICE_OK;
    }
    if (s->config.render && s->config.render->phase != PVR_RENDER_SERVICE_READY &&
        s->config.render->phase != PVR_RENDER_SERVICE_COMPLETE) return PVR_GPU_SERVICE_BUSY;
    if(r->operation==PVR_GPU_SERVICE_DRAW) {
        struct pvr_render_service *d=s->config.render;
        if(!d || !s->config.compute || !d->owner || d->owner->draw!=1 ||
            atomic_load_explicit(&d->draw_state,memory_order_acquire)!=2 ||
            d->draw_ticket!=m->ticket || d->draw_client!=m->client || d->draw_epoch!=s->epoch)return PVR_GPU_SERVICE_INVALID;
        if((d->phase!=PVR_RENDER_SERVICE_READY && d->phase!=PVR_RENDER_SERVICE_COMPLETE) ||
            (s->config.compute->phase!=PVR_COMPUTE_SERVICE_READY &&
             s->config.compute->phase!=PVR_COMPUTE_SERVICE_COMPLETE))return PVR_GPU_SERVICE_BUSY;
        if(!deadline(s,r->timeout_us))return PVR_GPU_SERVICE_FAULT;
        int result=pvr_render_service_draw_next(d,d->draw_previous,d->draw_copy,d->draw_bytes,d->draw_count,s->deadline);
        if(result<0){fail(s,PVR_GPU_SERVICE_GRAPHICS);return PVR_GPU_SERVICE_FAULT;}
        return result?PVR_GPU_SERVICE_OK:PVR_GPU_SERVICE_INVALID;
    }
    if (r->operation == PVR_GPU_SERVICE_RENDER) {
        if (!s->config.render) return PVR_GPU_SERVICE_INVALID;
        if(s->config.render->owner && s->config.render->owner->draw)return PVR_GPU_SERVICE_INVALID;
        if (s->config.render->phase != PVR_RENDER_SERVICE_READY ||
            (s->config.compute->phase != PVR_COMPUTE_SERVICE_READY &&
             s->config.compute->phase != PVR_COMPUTE_SERVICE_COMPLETE)) return PVR_GPU_SERVICE_BUSY;
        if (!deadline(s,r->timeout_us)) return PVR_GPU_SERVICE_FAULT;
        if (pvr_render_service_begin(s->config.render,s->deadline) != 1) {
            fail(s,PVR_GPU_SERVICE_GRAPHICS); return PVR_GPU_SERVICE_FAULT;
        }
        return PVR_GPU_SERVICE_OK;
    }
    if (r->operation == PVR_GPU_SERVICE_COMPUTE) {
        if (!s->config.compute) return PVR_GPU_SERVICE_INVALID;
        if (s->config.compute->phase != PVR_COMPUTE_SERVICE_READY &&
            s->config.compute->phase != PVR_COMPUTE_SERVICE_COMPLETE) return PVR_GPU_SERVICE_BUSY;
        if (!deadline(s, r->timeout_us)) return PVR_GPU_SERVICE_FAULT;
        if (pvr_compute_service_begin(s->config.compute, s->deadline) != 1) {
            fail(s, PVR_GPU_SERVICE_COMPUTATION); return PVR_GPU_SERVICE_FAULT;
        }
        return PVR_GPU_SERVICE_OK;
    }
    if (s->config.compute && s->config.compute->phase != PVR_COMPUTE_SERVICE_READY &&
        s->config.compute->phase != PVR_COMPUTE_SERVICE_COMPLETE) return PVR_GPU_SERVICE_BUSY;
    if(s->config.compute&&
       atomic_load_explicit(&s->config.compute->input_state,memory_order_acquire))return PVR_GPU_SERVICE_BUSY;
    if(s->config.render) {
        struct pvr_render_service *d=s->config.render;
        if(atomic_load_explicit(&d->draw_state,memory_order_acquire) ||
           atomic_load_explicit(&d->texture_state,memory_order_acquire) ||
           atomic_load_explicit(&d->texture_output_state,memory_order_acquire) ||
           d->texture_draft || d->texture_preparing)return PVR_GPU_SERVICE_BUSY;
    }
    if (s->config.mimus && !pvr_mimus_service_close(s->config.mimus)) return PVR_GPU_SERVICE_BUSY;
    s->closed_owner=m->client;s->closed_epoch=s->epoch;
    if (!interrupt_mask(s)) { fail(s, PVR_GPU_SERVICE_INTERRUPT); return PVR_GPU_SERVICE_FAULT; }
    if (!deadline(s, r->timeout_us)) return PVR_GPU_SERVICE_FAULT;
    if(s->config.graphics) {
        if(!a733_gpu_graphics_memory_cleanup_records(s->config.graphics,s->graphics_cleanup_address,
                s->graphics_cleanup_type,&s->graphics_cleanup_count) ||
           (s->config.render && pvr_render_service_cleanup_prepare(s->config.render,s->deadline)!=1) ||
           pvr_compute_service_cleanup_begin(s->config.compute,s->deadline)!=1) {
            fail(s,PVR_GPU_SERVICE_GRAPHICS);return PVR_GPU_SERVICE_FAULT;
        }
        s->phase=PVR_GPU_SERVICE_DRAINING;return PVR_GPU_SERVICE_OK;
    }
    if(s->config.compute&&!s->config.graphics&&!s->config.render&&!s->config.free_lists.count) {
        if(pvr_compute_service_cleanup_begin(s->config.compute,s->deadline)!=1) {
            fail(s,PVR_GPU_SERVICE_COMPUTATION);return PVR_GPU_SERVICE_FAULT;
        }
        s->phase=PVR_GPU_SERVICE_COMPUTE_CLEANUP;return PVR_GPU_SERVICE_OK;
    }
    if (s->config.free_lists.count) { s->phase = PVR_GPU_SERVICE_DRAINING; return PVR_GPU_SERVICE_OK; }
    return begin_stop(s) ? PVR_GPU_SERVICE_OK : PVR_GPU_SERVICE_FAULT;
}
/* Coarse progress identity of the controllers the worker advances. It only
 * decides whether another step may follow without waiting. A missed change
 * falls back to the worker's wait and never affects a protocol decision. */
static uint64_t progress(const struct pvr_gpu_service *s)
{
    const struct pvr_compute_service *c = s->config.compute;
    const struct pvr_render_service *r = s->config.render;
    const uint64_t values[] = {s->phase, s->start.phase, s->start.index, s->control.phase,
        s->control.health_done, s->stop.phase, s->stop.index, s->queue.serial, s->queue.host_read,
        s->queue.firmware_read, s->free_lists.phase,
        s->free_lists.stage, s->free_lists.list, s->free_lists.cursor, s->free_lists.request_count,
        s->graphics_cleanup_index,
        c ? c->phase : 0, c ? c->job.phase : 0, c ? c->client.phase : 0, c ? c->client.index : 0,
        c ? c->client.cursor : 0,
        r ? r->phase : 0, r ? r->checked : 0, r ? r->texture_cursor : 0, r ? r->pair.phase : 0,
        r ? r->geometry.phase : 0, r ? r->geometry.index : 0, r ? r->geometry.cursor : 0,
        r ? r->fragment.phase : 0, r ? r->fragment.index : 0, r ? r->fragment.cursor : 0,
        s->config.mimus ? pvr_mimus_service_progress(s->config.mimus) : 0};
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (unsigned i = 0; i < sizeof(values) / sizeof(*values); ++i)
        hash = (hash ^ values[i]) * UINT64_C(0x100000001b3);
    return hash;
}
/* Return 1 when the step progressed and another may follow at once. OPEN
 * preparation and disposal perform CPU-only work and never wait on the GPU. */
static int step(struct pvr_gpu_service *s)
{
    enum pvr_gpu_service_phase phase = s->phase;
    uint64_t before = progress(s);
    advance(s);
    if (s->phase == PVR_GPU_SERVICE_FAILED) return 0;
    /* A ready mimus session whose step rearmed the interrupt has no CPU work
     * until the next slot wake, the interrupt or the job poll bound. */
    if (phase == PVR_GPU_SERVICE_READY && s->phase == PVR_GPU_SERVICE_READY && s->config.mimus &&
        !s->irq_masked && !pvr_mimus_service_submitting(s->config.mimus)) return 0;
    if (phase == PVR_GPU_SERVICE_BUILDING || phase == PVR_GPU_SERVICE_PUBLISHING_GRAPHICS ||
        phase == PVR_GPU_SERVICE_PUBLISHING || phase == PVR_GPU_SERVICE_DISPOSING) return 1;
    return progress(s) != before;
}
static TickType_t ticks(uint64_t us)
{
    const uint64_t period = 1000000u / configTICK_RATE_HZ;
    uint64_t n = (us + period - 1) / period;
    return n ? (TickType_t)n : 1;
}
/* The worker's one wait when no step is ready to run. A lifecycle phase waits
 * one tick: its hardware waits shorter than A733_GPU_SPIN_US end inside the
 * step. A ready session waits one tick while a masked event batch remains.
 * While a job, render or free-list operation is in flight it waits until the
 * earliest deadline of that work or PVR_GPU_SERVICE_JOB_POLL_US, whichever is
 * sooner, and otherwise until a request or interrupt. Without interrupt
 * delivery an idle session also wakes every IDLE_POLL_US. */
static TickType_t wait_ticks(struct pvr_gpu_service *s)
{
    switch (s->phase) {
    case PVR_GPU_SERVICE_COLD: case PVR_GPU_SERVICE_STOPPED: case PVR_GPU_SERVICE_FAILED:
    case PVR_GPU_SERVICE_DISPOSED: case PVR_GPU_SERVICE_RECONSTRUCTING: return portMAX_DELAY;
    case PVR_GPU_SERVICE_READY: break;
    default: return 1;
    }
    if (s->config.irq.mask && s->irq_masked) return 1;
    /* The job and the render keep their own deadlines. s->deadline belongs
     * to the latest client request, which the worker may have refused while
     * the job runs. A free-list operation carries its own deadline. */
    const struct pvr_compute_service *c = s->config.compute;
    const struct pvr_render_service *r = s->config.render;
    uint64_t earliest = UINT64_MAX;
    int busy = 0;
    if (compute_polled(s)) { busy = 1; earliest = c->deadline; }
    if (render_polled(s)) { busy = 1; if (r->deadline < earliest) earliest = r->deadline; }
    uint64_t job;
    if (s->config.mimus && pvr_mimus_service_deadline(s->config.mimus, &job)) { busy = 1; if (job < earliest) earliest = job; }
    if (s->config.free_lists.count &&
        (s->free_lists.phase != PVR_FREE_LIST_IDLE || s->free_lists.request_count)) {
        uint64_t operation = free_list_deadline(s);
        busy = 1;
        if (operation && operation < earliest) earliest = operation;
    }
    if (!busy) return s->config.irq.mask ? portMAX_DELAY : ticks(PVR_GPU_SERVICE_IDLE_POLL_US);
    uint64_t now, bound = PVR_GPU_SERVICE_JOB_POLL_US;
    if (sample(s, &now)) bound = earliest <= now ? 0 : earliest - now < bound ? earliest - now : bound;
    return ticks(bound);
}
/* Steps that progress continue while nothing is queued, for at most
 * PVR_GPU_SERVICE_BURST_US. Return 1 when CPU work remains. */
static int burst(struct pvr_gpu_service *s, uint64_t *start)
{
    uint64_t now;
    int clocked = sample(s, start), more = step(s);
    if (!clocked) *start = s->last_now;
    while (more && clocked && !uxQueueMessagesWaiting(s->requests) &&
           sample(s, &now) && now - *start < PVR_GPU_SERVICE_BURST_US) more = step(s);
    return more;
}
static void worker(void *argument)
{
    struct pvr_gpu_service *s = argument;
    uint64_t level, stack;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, spsel" : "=r"(level), "=r"(stack));
    configASSERT(level == 4);
#if configNUMBER_OF_CORES > 1
    configASSERT(stack == 0);
#else
    (void)stack;
#endif
    for (;;) {
        /* One bounded burst before each request; traffic cannot starve it,
         * and the burst bound keeps requests from waiting behind it. With CPU
         * work left, the worker takes a queued request without waiting or
         * yields to tasks of its priority. */
        struct pvr_gpu_service_message message;
        uint64_t started;
        if (burst(s, &started)) {
            /* Bursts that end with CPU work left keep tasks below the worker's
             * priority off its core. Once such a run reaches CPU_RUN_US the
             * worker blocks for one tick, so those tasks run for the rest of
             * the tick; otherwise it yields only to tasks of its priority.
             * Ring writes before a kick are exempt, so no tick wait lies
             * between them and the kick; the fixed ring sizes bound them. */
            TickType_t block = 0;
            if (submitting(s)) s->yield_running = 0;
            else {
                if (!s->yield_running) { s->yield_running = 1; s->yield_start = started; }
                uint64_t run = s->last_now - s->yield_start;
                if (run > s->longest_yield_us) s->longest_yield_us = run;
                if (run >= PVR_GPU_SERVICE_CPU_RUN_US) { s->yield_running = 0; ++s->yield_blocks; block = 1; }
            }
            if (xQueueReceive(s->requests, &message, block) != pdTRUE) {
                if (!block) taskYIELD();
                continue;
            }
        } else {
            s->yield_running = 0;
            int ready = s->phase == PVR_GPU_SERVICE_READY;
            TickType_t wait = wait_ticks(s);
            if (wait != portMAX_DELAY) ++*(ready ? &s->ready_timed_waits : &s->lifecycle_timed_waits);
            if (xQueueReceive(s->requests, &message, wait) != pdTRUE) {
                if (wait != portMAX_DELAY)
                    ++*(ready ? &s->ready_wait_expiries : &s->lifecycle_wait_expiries);
                continue;
            }
        }
        if (message.client == PVR_GPU_SERVICE_CLIENTS && !message.ticket) {
            taskENTER_CRITICAL(); s->irq_wake_pending = 0; taskEXIT_CRITICAL();
            continue;
        }
        /* The mimus job wake: the next step takes the slot. */
        if (message.client == PVR_GPU_SERVICE_CLIENTS && message.ticket == PVR_GPU_SERVICE_MIMUS_WAKE &&
            s->config.mimus) continue;
        configASSERT(message.client < PVR_GPU_SERVICE_CLIENTS && message.ticket);
        struct pvr_gpu_service_reply reply = {.ticket = message.ticket};
        reply.result = execute(s, &message);
        if(message.request.operation==PVR_GPU_SERVICE_COMPUTE_INPUT) {
            struct pvr_compute_service *c=s->config.compute;
            if(c&&atomic_load_explicit(&c->input_state,memory_order_acquire)==2&&
               c->input_ticket==message.ticket&&c->input_client==message.client)
                atomic_store_explicit(&c->input_state,0,memory_order_release);
        }
        if(message.request.operation==PVR_GPU_SERVICE_DRAW) {
            struct pvr_render_service *d=s->config.render;
            if(d && atomic_load_explicit(&d->draw_state,memory_order_acquire)==2 &&
                d->draw_ticket==message.ticket && d->draw_client==message.client)
                atomic_store_explicit(&d->draw_state,0,memory_order_release);
        }
        if(message.request.operation==PVR_GPU_SERVICE_TEXTURE) {
            struct pvr_render_service *d=s->config.render;
            if(d && atomic_load_explicit(&d->texture_state,memory_order_acquire)==2 &&
                d->texture_ticket==message.ticket && d->texture_client==message.client) {
                reply.texture_generation=d->texture_generation;reply.texture_received=d->texture_received;
                atomic_store_explicit(&d->texture_state,0,memory_order_release);
            }
        }
        reply.phase = s->phase; reply.error = s->error; reply.epoch = s->epoch;
        reply.owns_session = s->owner == message.client;
        reply.start_error = s->start.error; reply.control_error = s->control.error;
        reply.stop_error = s->stop.error;
        if (s->config.graphics) reply.graphics_phase = s->config.graphics->phase;
        if (s->config.render) {
            reply.render_phase=s->config.render->phase; reply.render_error=s->config.render->error;
            reply.render_checked=s->config.render->checked;
            reply.render_sequence=s->config.render->sequence;
            if (message.request.operation==PVR_GPU_SERVICE_RENDER_OUTPUT && reply.result==PVR_GPU_SERVICE_OK) {
                reply.render_first_pixel=message.render_first_pixel; reply.render_pixel_count=64;
                memcpy(reply.render_pixels,s->config.render->pixels+message.render_first_pixel,sizeof(reply.render_pixels));
            }
        }
        reply.free_list_error = s->free_lists.error; reply.free_list_pending = s->free_lists.request_count;
        if (s->config.compute) {
            reply.compute_phase = s->config.compute->phase; reply.compute_error = s->config.compute->error;
            reply.compute_job_error = s->config.compute->job.error;
            reply.compute_sequence = s->config.compute->job.sequence;
            if (message.request.operation == PVR_GPU_SERVICE_COMPUTE_OUTPUT && reply.result == PVR_GPU_SERVICE_OK) {
                reply.compute_first_word=message.compute_first_word;
                reply.compute_total_words=pvr_compute_job_result_words(s->config.compute->config->job);
                reply.compute_word_count=reply.compute_total_words-reply.compute_first_word;
                if(reply.compute_word_count>32)reply.compute_word_count=32;
                memcpy(reply.compute_words,s->config.compute->job.results+reply.compute_first_word,reply.compute_word_count*4);
            }
        }
        configASSERT(xQueueSend(s->clients[message.client].replies, &reply, 0) == pdTRUE);
    }
}
int pvr_gpu_service_init(struct pvr_gpu_service *s, const struct pvr_gpu_service_config *c,
                         UBaseType_t priority, UBaseType_t affinity)
{
    if (!s || instance || s->initialized || !c || !priority || priority >= configMAX_PRIORITIES ||
        !affinity || (affinity >> configNUMBER_OF_CORES) ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED ||
        ((c->compute || c->mimus) && !c->compute_mode) ||
        (!!c->power.begin != !!c->power.poll) || (!!c->enable == !!c->power.begin) ||
        !c->queue.read || !c->queue.write || !c->queue.barrier || !c->queue.kick ||
        !c->start.now || !c->start.read || !c->start.write || !c->start.started || !c->start.publish ||
        c->control.now != c->start.now || !c->control.read32 || !c->control.write32 || !c->control.compat ||
        c->stop.now != c->start.now || !c->stop.read || !c->stop.write ||
        !c->stop.sync_write || !c->stop.sync_read || !c->stop.isolate ||
        (!!c->irq.mask != !!c->irq.rearm) || !allocations(s, c)) return 0;
    s->config = *c; s->owner = PVR_GPU_SERVICE_CLIENTS; s->irq_masked = 1;
    s->requests = xQueueCreateStatic(PVR_GPU_SERVICE_CLIENTS, sizeof(struct pvr_gpu_service_message),
                                     s->request_storage, &s->request_state);
    configASSERT(s->requests);
    for (unsigned i = 0; i < PVR_GPU_SERVICE_CLIENTS; ++i) {
        struct pvr_gpu_service_client *client = &s->clients[i];
        client->replies = xQueueCreateStatic(1, sizeof(struct pvr_gpu_service_reply),
                                            client->reply_storage, &client->reply_state);
        configASSERT(client->replies);
    }
    instance = s; s->initialized = 1; creating_worker = 1;
#if configNUMBER_OF_CORES > 1
    s->worker = xTaskCreateStaticAffinitySet(worker, "gpu-service", PVR_GPU_SERVICE_STACK_WORDS,
        s, priority, s->worker_stack, &s->worker_state, affinity);
#else
    s->worker = xTaskCreateStatic(worker, "gpu-service", PVR_GPU_SERVICE_STACK_WORDS,
        s, priority, s->worker_stack, &s->worker_state);
#endif
    creating_worker = 0; configASSERT(s->worker); return 1;
}
int pvr_gpu_service_bind(struct pvr_gpu_service *s, unsigned slot, TaskHandle_t task)
{
    if (!s || s != instance || !s->initialized || slot >= PVR_GPU_SERVICE_CLIENTS || !task ||
        task == s->worker || s->clients[slot].task ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) return 0;
    for (unsigned i = 0; i < PVR_GPU_SERVICE_CLIENTS; ++i)
        if (s->clients[i].task == task) return 0;
    s->clients[slot].task = task; return 1;
}
static struct pvr_gpu_service_client *caller(struct pvr_gpu_service *s, unsigned *slot)
{
    if (!s || s != instance || !s->initialized ||
        xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return NULL;
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    for (unsigned i = 0; i < PVR_GPU_SERVICE_CLIENTS; ++i)
        if (s->clients[i].task == task) { *slot = i; return &s->clients[i]; }
    return NULL;
}
int pvr_gpu_service_send_epoch(struct pvr_gpu_service *s,uint64_t epoch, const struct pvr_gpu_service_request *r,
                         TickType_t wait, uint64_t *ticket)
{
    if (!r || !ticket || wait == portMAX_DELAY || r->operation==PVR_GPU_SERVICE_COMPUTE_INPUT ||
        r->operation==PVR_GPU_SERVICE_DRAW) return 0;
    unsigned slot; struct pvr_gpu_service_client *c = caller(s, &slot);
    if (!c || c->pending || c->next_ticket == UINT64_MAX) return 0;
    struct pvr_gpu_service_message message = {.request=*r, .ticket=c->next_ticket+1, .epoch=epoch, .client=slot};
    c->pending = message.ticket;
    if (xQueueSend(s->requests, &message, wait) != pdTRUE) { c->pending = 0; return 0; }
    c->next_ticket = message.ticket; *ticket = message.ticket; return 1;
}
static uint64_t request_epoch(struct pvr_gpu_service *s)
{ return s && s==instance?atomic_load_explicit(&s->published_epoch,memory_order_acquire):0; }
int pvr_gpu_service_send(struct pvr_gpu_service *s,const struct pvr_gpu_service_request *r,
    TickType_t wait,uint64_t *ticket)
{ return pvr_gpu_service_send_epoch(s,request_epoch(s),r,wait,ticket); }
int pvr_gpu_service_collect(struct pvr_gpu_service *s, uint64_t ticket,
                            struct pvr_gpu_service_reply *reply, TickType_t wait)
{
    if (!reply || !ticket || wait == portMAX_DELAY) return 0;
    unsigned slot; struct pvr_gpu_service_client *c = caller(s, &slot); (void)slot;
    if (!c || c->pending != ticket) return 0;
    struct pvr_gpu_service_reply value;
    if (xQueueReceive(c->replies, &value, wait) != pdTRUE) return 0;
    configASSERT(value.ticket == ticket); c->pending = 0; *reply = value; return 1;
}

static int render_read(struct pvr_gpu_service *s,uint64_t epoch,uint32_t sequence,uint32_t first_pixel,
    TickType_t wait,uint64_t *ticket)
{
    if (!ticket || wait==portMAX_DELAY || first_pixel>=4096 || (first_pixel&63)) return 0;
    unsigned slot;struct pvr_gpu_service_client *c=caller(s,&slot);
    if(!c || c->pending || c->next_ticket==UINT64_MAX)return 0;
    struct pvr_gpu_service_message message={.request={PVR_GPU_SERVICE_RENDER_OUTPUT,0},
        .ticket=c->next_ticket+1,.epoch=epoch,.client=slot,.render_first_pixel=first_pixel,.render_sequence=sequence};
    c->pending=message.ticket;
    if(xQueueSend(s->requests,&message,wait)!=pdTRUE){c->pending=0;return 0;}
    c->next_ticket=message.ticket;*ticket=message.ticket;return 1;
}
int pvr_gpu_service_render_read(struct pvr_gpu_service *s,uint32_t first,TickType_t wait,uint64_t *ticket)
{ return render_read(s,request_epoch(s),0,first,wait,ticket); }
int pvr_gpu_service_render_read_sequence(struct pvr_gpu_service *s,uint32_t frame,uint32_t first,TickType_t wait,uint64_t *ticket)
{ return frame && render_read(s,request_epoch(s),frame,first,wait,ticket); }

static int compute_read(struct pvr_gpu_service *s,uint64_t epoch,uint32_t sequence,uint32_t first,
    TickType_t wait,uint64_t *ticket)
{
    if(!ticket||wait==portMAX_DELAY||first>=PVR_COMPUTE_PACKAGE_OUTPUT/4)return 0;
    unsigned slot;struct pvr_gpu_service_client *c=caller(s,&slot);
    if(!c||c->pending||c->next_ticket==UINT64_MAX)return 0;
    struct pvr_gpu_service_message m={.request={PVR_GPU_SERVICE_COMPUTE_OUTPUT,0},
        .ticket=c->next_ticket+1,.epoch=epoch,.client=slot,.compute_first_word=first,.compute_sequence=sequence};
    c->pending=m.ticket;
    if(xQueueSend(s->requests,&m,wait)!=pdTRUE){c->pending=0;return 0;}
    c->next_ticket=m.ticket;*ticket=m.ticket;return 1;
}

int pvr_gpu_service_compute_read_sequence(struct pvr_gpu_service *s,uint32_t sequence,uint32_t first,
    TickType_t wait,uint64_t *ticket)
{ return sequence&&compute_read(s,request_epoch(s),sequence,first,wait,ticket); }
int pvr_gpu_service_compute_read(struct pvr_gpu_service *s,uint32_t first,
    TickType_t wait,uint64_t *ticket)
{ return compute_read(s,request_epoch(s),0,first,wait,ticket); }
int pvr_gpu_service_compute_input_epoch(struct pvr_gpu_service *s,uint64_t epoch,uint32_t sequence,
    const void *input,size_t bytes,uint64_t timeout,TickType_t wait,uint64_t *ticket)
{
    if(!ticket||!sequence||!a733_span_excl_nonempty(input,bytes)||bytes>PVR_COMPUTE_PACKAGE_INPUT||
       !timeout||timeout>INT64_MAX||wait==portMAX_DELAY)return 0;
    unsigned slot;struct pvr_gpu_service_client *client=caller(s,&slot);
    if(!client||client->pending||client->next_ticket==UINT64_MAX||!s->config.compute)return 0;
    struct pvr_compute_service *c=s->config.compute;
    const struct pvr_compute_package *p=c->config->job->package;
    if(!p||bytes!=p->input_bytes||!a733_extents_disjoint_prechecked((uintptr_t)input,bytes,(uintptr_t)c,sizeof(*c)))return 0;
    unsigned empty=0;
    if(!atomic_compare_exchange_strong_explicit(&c->input_state,&empty,1,
        memory_order_acquire,memory_order_relaxed))return 0;
    struct pvr_gpu_service_message m={.request={PVR_GPU_SERVICE_COMPUTE_INPUT,timeout},
        .ticket=client->next_ticket+1,.epoch=epoch,.client=slot,.compute_sequence=sequence};
    memcpy(c->input_copy,input,bytes);c->input_bytes=(uint32_t)bytes;
    c->input_ticket=m.ticket;c->input_client=slot;
    client->pending=m.ticket;
    atomic_store_explicit(&c->input_state,2,memory_order_release);
    if(xQueueSend(s->requests,&m,wait)!=pdTRUE) {
        client->pending=0;atomic_store_explicit(&c->input_state,0,memory_order_release);return 0;
    }
    client->next_ticket=m.ticket;*ticket=m.ticket;return 1;
}
int pvr_gpu_service_draw_next(struct pvr_gpu_service *s,uint64_t epoch,uint32_t previous,const void *vertices,
    size_t bytes,uint32_t count,uint64_t timeout,TickType_t wait,uint64_t *ticket)
{
    if(previous>=PVR_RENDER_DRAW_MAX_FRAMES || !epoch || !ticket ||
        !a733_span_excl_nonempty(vertices,bytes) || !pvr_render_draw_vertices_valid(vertices,bytes,count) ||
        !timeout || timeout>INT64_MAX || wait==portMAX_DELAY)return 0;
    unsigned slot;struct pvr_gpu_service_client *client=caller(s,&slot);
    if(!client || client->pending || client->next_ticket==UINT64_MAX || !s->config.render)return 0;
    struct pvr_render_service *d=s->config.render;
    if(!a733_extents_disjoint_prechecked((uintptr_t)vertices,bytes,(uintptr_t)d,sizeof(*d)))return 0;
    unsigned empty=0;
    if(!atomic_compare_exchange_strong_explicit(&d->draw_state,&empty,1,memory_order_acquire,memory_order_relaxed))return 0;
    struct pvr_gpu_service_message m={.request={PVR_GPU_SERVICE_DRAW,timeout},.ticket=client->next_ticket+1,.epoch=epoch,.client=slot};
    memcpy(d->draw_copy,vertices,bytes);d->draw_count=count;d->draw_bytes=(uint32_t)bytes;
    d->draw_ticket=m.ticket;d->draw_client=slot;d->draw_epoch=epoch;d->draw_previous=previous;
    client->pending=m.ticket;
    atomic_store_explicit(&d->draw_state,2,memory_order_release);
    if(xQueueSend(s->requests,&m,wait)!=pdTRUE) {
        client->pending=0;atomic_store_explicit(&d->draw_state,0,memory_order_release);return 0;
    }
    client->next_ticket=m.ticket;*ticket=m.ticket;return 1;
}
int pvr_gpu_service_draw(struct pvr_gpu_service *s,uint64_t epoch,const void *vertices,
    size_t bytes,uint32_t count,uint64_t timeout,TickType_t wait,uint64_t *ticket)
{ return pvr_gpu_service_draw_next(s,epoch,0,vertices,bytes,count,timeout,wait,ticket); }

int pvr_gpu_service_texture(struct pvr_gpu_service *s,uint64_t epoch,const struct pvr_render_texture_request *request,
    const void *data,uint64_t timeout,TickType_t wait,uint64_t *ticket)
{
    if(!epoch || !ticket || !a733_span_excl_nonempty(request,sizeof(*request)) || !timeout || timeout>INT64_MAX || wait==portMAX_DELAY ||
        request->operation>PVR_TEXTURE_PRESENT || request->bytes>PVR_RENDER_TEXTURE_CHUNK ||
        (request->operation==PVR_TEXTURE_WRITE && !a733_span_excl_nonempty(data,request->bytes)))return 0;
    unsigned slot;struct pvr_gpu_service_client *client=caller(s,&slot);
    if(!client || client->pending || client->next_ticket==UINT64_MAX || !s->config.render || !s->config.graphics ||
        s->config.graphics->draw<2 || s->config.graphics!=s->config.render->owner)return 0;
    struct pvr_render_service *d=s->config.render;struct pvr_render_texture_layout l;
    if(!pvr_render_texture_layout_profile(d->owner->profile.render.width,d->owner->profile.render.height,d->owner->draw,&l) ||
        !a733_extents_disjoint_prechecked((uintptr_t)request,sizeof(*request),(uintptr_t)d,sizeof(*d)))return 0;
    uintptr_t base=d->owner->payload_extent.cpu+d->owner->render.payload_bytes;
    if(request->operation==PVR_TEXTURE_WRITE &&
        (!a733_extents_disjoint_prechecked((uintptr_t)data,request->bytes,(uintptr_t)d,sizeof(*d)) ||
         !a733_extents_disjoint_prechecked((uintptr_t)data,request->bytes,d->owner->payload_extent.cpu,d->owner->payload_extent.bytes)))return 0;
    unsigned empty=0;
    if(!atomic_compare_exchange_strong_explicit(&d->texture_state,&empty,1,memory_order_acquire,memory_order_relaxed))return 0;
    d->texture_request=*request;
    if(request->operation==PVR_TEXTURE_WRITE)memcpy((void *)(base+l.upload),data,request->bytes);
    struct pvr_gpu_service_message m={.request={PVR_GPU_SERVICE_TEXTURE,timeout},.ticket=client->next_ticket+1,.epoch=epoch,.client=slot};
    d->texture_ticket=m.ticket;d->texture_client=slot;d->texture_epoch=epoch;client->pending=m.ticket;
    atomic_store_explicit(&d->texture_state,2,memory_order_release);
    if(xQueueSend(s->requests,&m,wait)!=pdTRUE) {
        client->pending=0;atomic_store_explicit(&d->texture_state,0,memory_order_release);return 0;
    }
    client->next_ticket=m.ticket;*ticket=m.ticket;return 1;
}
int pvr_gpu_service_texture_output(struct pvr_gpu_service *s,uint64_t ticket,const void **data,uint32_t *bytes)
{
    if(!ticket || !data || !bytes)return 0;
    unsigned slot;struct pvr_gpu_service_client *c=caller(s,&slot);
    if(!c || c->pending || !s->config.render || s->owner!=slot || s->phase!=PVR_GPU_SERVICE_READY)return 0;
    struct pvr_render_service *d=s->config.render;
    if(atomic_load_explicit(&d->texture_output_state,memory_order_acquire)!=1 ||
        d->texture_output_ticket!=ticket || d->texture_output_client!=slot)return 0;
    struct pvr_render_texture_layout l;
    if(!pvr_render_texture_layout_profile(d->owner->profile.render.width,d->owner->profile.render.height,d->owner->draw,&l))return 0;
    *data=(const void *)(d->owner->payload_extent.cpu+d->owner->render.payload_bytes+l.download);
    *bytes=d->texture_output_bytes;return 1;
}
int pvr_gpu_service_texture_present(struct pvr_gpu_service *s,uint64_t ticket,struct pvr_render_texture_frame *out)
{
    if(!ticket || !out)return 0;
    unsigned slot;struct pvr_gpu_service_client *c=caller(s,&slot);
    if(!c || c->pending || !s->config.render || s->owner!=slot || s->phase!=PVR_GPU_SERVICE_READY)return 0;
    struct pvr_render_service *d=s->config.render;struct pvr_render_texture_layout l;uint32_t stride,bytes;
    if(atomic_load_explicit(&d->texture_output_state,memory_order_acquire)!=2 ||
        d->texture_output_ticket!=ticket || d->texture_output_client!=slot ||
        !pvr_render_texture_layout_profile(d->owner->profile.render.width,d->owner->profile.render.height,d->owner->draw,&l) ||
        !pvr_render_texture_geometry(d->owner->profile.render.width,d->owner->profile.render.height,&stride,&bytes) ||
        d->texture_output_bytes!=bytes)return 0;
    uint64_t offset=d->owner->render.payload_bytes+l.copy;
    *out=(struct pvr_render_texture_frame){(const void *)(d->owner->payload_extent.cpu+offset),
        d->owner->payload_extent.physical+offset,bytes,d->owner->profile.render.width,
        d->owner->profile.render.height,stride,d->texture_output_frame};
    return 1;
}

int pvr_gpu_service_compute_input(struct pvr_gpu_service *s,uint32_t sequence,
    const void *input,size_t bytes,uint64_t timeout,TickType_t wait,uint64_t *ticket)
{ return pvr_gpu_service_compute_input_epoch(s,request_epoch(s),sequence,input,bytes,timeout,wait,ticket); }
int pvr_gpu_service_compute_read_epoch(struct pvr_gpu_service *s,uint64_t epoch,uint32_t sequence,
    uint32_t first,TickType_t wait,uint64_t *ticket)
{ return epoch && sequence && compute_read(s,epoch,sequence,first,wait,ticket); }
int pvr_gpu_service_render_read_epoch(struct pvr_gpu_service *s,uint64_t epoch,uint32_t frame,
    uint32_t first,TickType_t wait,uint64_t *ticket)
{ return epoch && frame && render_read(s,epoch,frame,first,wait,ticket); }
