#include "pvr_gpu_service.h"
#include "pvr_compute_service.h"
#include "a733_gpu_graphics_memory.h"
#include <string.h>

static struct pvr_gpu_service *instance;
static unsigned creating_worker;
static void worker(void *);
BaseType_t pvr_gpu_service_task_privileged(TaskFunction_t entry, void *argument)
{ return creating_worker && entry == worker && argument == instance ? pdTRUE : pdFALSE; }

static int span(const void *p, size_t n)
{ return p && n && n <= UINTPTR_MAX - (uintptr_t)p; }
static int separate(const void *a, size_t n, const void *b, size_t m)
{ return (uintptr_t)a + n <= (uintptr_t)b || (uintptr_t)b + m <= (uintptr_t)a; }
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
        !span(f->lists, f->count * sizeof(*f->lists))) return 0;
    /* Include immutable list configs and the common ops/config object in the
     * cross-component checks before any firmware builder can modify backing. */
    for (unsigned i = 0; i <= f->count; ++i) {
        const void *ptr = i == f->count ? (const void *)f->lists : f->lists[i].pages;
        size_t bytes = i == f->count ? f->count * sizeof(*f->lists) :
            (size_t)f->lists[i].maximum_pages * sizeof(*f->lists[i].pages);
        if ((i < f->count && f->lists[i].maximum_pages > PVR_FREE_LIST_MAX_PAGES) ||
            !span(ptr, bytes) || !separate(ptr, bytes, s, sizeof(*s)) ||
            !separate(ptr, bytes, c, sizeof(*c)) ||
            !separate(ptr, bytes, c->firmware, c->firmware_bytes)) return 0;
        if (i < f->count && !separate(ptr, bytes, f->lists, f->count * sizeof(*f->lists))) return 0;
        for (unsigned j = 0; j < i && j < f->count; ++j)
            if (!separate(ptr, bytes, f->lists[j].pages,
                    (size_t)f->lists[j].maximum_pages * sizeof(*f->lists[j].pages))) return 0;
        for (unsigned j = 0; j < 3 + PVR_FW_RESOURCE_COUNT; ++j) {
            const struct pvr_mips_buffer *b = buffer(c, j);
            if (!separate(ptr, bytes, b->storage.cpu, b->storage.bytes) ||
                !separate(ptr, bytes, b->pages, b->page_count * sizeof(uint64_t))) return 0;
        }
    }
    return 1;
}
static int allocations(struct pvr_gpu_service *s, const struct pvr_gpu_service_config *c)
{
    struct pvr_fw_plan plan;
    if (!span(s, sizeof(*s)) || !span(c, sizeof(*c)) ||
        !separate(s, sizeof(*s), c, sizeof(*c)) ||
        !span(c->firmware, c->firmware_bytes) ||
        !separate(s, sizeof(*s), c->firmware, c->firmware_bytes) ||
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
            !span(b->storage.cpu, expected) ||
            !span(b->pages, b->page_count * sizeof(uint64_t)) ||
            !separate(s, sizeof(*s), b->pages, b->page_count * sizeof(uint64_t)) ||
            !separate(b->storage.cpu, expected, s, sizeof(*s)) ||
            !separate(b->storage.cpu, expected, c, sizeof(*c)) ||
            !separate(b->storage.cpu, expected, c->firmware, c->firmware_bytes)) return 0;
        for (unsigned j = 0; j < 3 + PVR_FW_RESOURCE_COUNT; ++j) {
            const struct pvr_mips_buffer *a = buffer(c, j);
            /* Validate lengths before pointer arithmetic even for later entries. */
            if (a->page_count > PVR_FW_MAX_ALLOCATION / PVR_MIPS_PAGE_BYTES ||
                !span(a->pages, a->page_count * sizeof(uint64_t)) ||
                !separate(b->storage.cpu, expected, a->pages, a->page_count * sizeof(uint64_t))) return 0;
            if (j < i && !separate(b->storage.cpu, expected, a->storage.cpu, a->storage.bytes)) return 0;
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
    if (s->config.graphics) a733_gpu_graphics_memory_abort(s->config.graphics);
    (void)pvr_gpu_start_cancel(&s->start);
    (void)pvr_gpu_control_abort(&s->control);
    (void)pvr_ccb_abort(&s->queue);
}
/* The optional resource manager advances the shared clock between controller
 * calls. Its controllers use this adapter to reject a backward clock before I/O. */
static int free_list_clock(void *ctx, uint64_t *now)
{
    struct pvr_gpu_service *s = instance;
    if (!s || ctx != s->config.context || s->phase == PVR_GPU_SERVICE_FAILED ||
        !s->config.start.now(ctx, now) || *now < s->last_now || *now < s->queue.last_now ||
        *now < s->start.last_now || *now < s->control.last_now || *now < s->stop.last_now ||
        *now < s->free_lists.last_now ||
        (s->config.compute && *now < s->config.compute->last_now)) return 0;
    s->last_now = *now; return 1;
}
static int free_list_event(void *ctx, const unsigned char command[PVR_CCB_COMMAND_BYTES])
{
    struct pvr_gpu_service *s = instance;
    if (!s || ctx != s->config.context || s->phase == PVR_GPU_SERVICE_FAILED) return 0;
    if (s->config.compute && !pvr_compute_service_event(s->config.compute, command)) return 0;
    uint32_t kind = (uint32_t)command[0] | (uint32_t)command[1] << 8 |
                    (uint32_t)command[2] << 16 | (uint32_t)command[3] << 24;
    if (s->config.free_lists.count && (kind == (PVR_CCB_MAGIC | 103u) || kind == (PVR_CCB_MAGIC | 104u)))
        return pvr_gpu_events_dispatch(&s->free_list_events, s->epoch, command, PVR_CCB_COMMAND_BYTES);
    return s->config.control.event && s->config.control.event(ctx, command);
}
static enum pvr_free_list_result free_list_progress(struct pvr_gpu_service *s, int closing)
{
    uint64_t now;
    if (!free_list_clock(s->config.context, &now)) {
        fail(s, PVR_GPU_SERVICE_CLOCK); return PVR_FREE_LIST_FAULT;
    }
    if (closing && now >= s->deadline) {
        fail(s, PVR_GPU_SERVICE_STOP); return PVR_FREE_LIST_FAULT;
    }
    /* Capture the deadline before completion can move the manager to IDLE or
     * consume its request. A bounded callback can still finish too late. */
    uint64_t operation_deadline = 0;
    if (s->free_lists.phase != PVR_FREE_LIST_IDLE) operation_deadline = s->free_lists.deadline;
    else if (s->free_lists.request_count)
        operation_deadline = s->free_lists.requests[s->free_lists.request_head].deadline;
    enum pvr_free_list_result r = pvr_free_lists_poll(&s->free_lists, now);
    if (r == PVR_FREE_LIST_FAULT) { fail(s, PVR_GPU_SERVICE_FREE_LISTS); return r; }
    if (operation_deadline) {
        if (!free_list_clock(s->config.context, &now)) {
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
    if (!((s->config.free_lists.count || s->config.compute) ? free_list_clock(s->config.context, &now) :
          s->config.start.now(s->config.context, &now)) || now < s->last_now ||
        duration > UINT64_MAX - now) { fail(s, PVR_GPU_SERVICE_CLOCK); return 0; }
    s->last_now = now; s->deadline = now + duration; return 1;
}
static int opening_clock(struct pvr_gpu_service *s)
{
    uint64_t now;
    if (!((s->config.free_lists.count || s->config.compute) ? free_list_clock(s->config.context, &now) :
          s->config.start.now(s->config.context, &now)) || now < s->last_now || now >= s->deadline) {
        fail(s, PVR_GPU_SERVICE_CLOCK); return 0;
    }
    s->last_now = now; return 1;
}
static int start_firmware(struct pvr_gpu_service *s)
{
    if (s->config.compute && pvr_compute_service_admit_mode(s->config.compute) != 1) {
        fail(s, PVR_GPU_SERVICE_COMPUTATION); return 0;
    }
    if (pvr_gpu_start_begin(&s->start, s->deadline) != PVR_GPU_START_AGAIN ||
        s->start.last_now < s->last_now) { fail(s, PVR_GPU_SERVICE_START); return 0; }
    s->last_now = s->start.last_now; s->phase = PVR_GPU_SERVICE_STARTING; return 1;
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
    if (c->free_lists.count || c->compute) start_ops.now = free_list_clock;
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
    if (c->graphics) {
        _Static_assert(PVR_FW_RESOURCE_COUNT + PVR_COMPUTE_SERVICE_OBJECTS + 1 <= PVR_MIPS_MAX_OBJECTS,
                       "combined firmware object capacity");
        s->mappings[objects++] = c->graphics->object;
    }
    if (!pvr_mips_prepare(c->firmware, c->firmware_bytes, c->firmware_buffers,
            s->mappings, objects, &s->boot) ||
        !pvr_gpu_start_init(&s->start, c->firmware, c->firmware_bytes, &s->boot,
            &start_ops, c->context) ||
        pvr_ccb_init(&s->queue, &c->queue, c->context, s->epoch) != PVR_CCB_OK) {
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
    if (s->config.free_lists.count || s->config.compute) ops.now = free_list_clock;
    if (!pvr_gpu_stop_init(&s->stop, &s->control, s->config.firmware,
            s->config.firmware_bytes, &ops, s->config.context, s->deadline)) {
        fail(s, PVR_GPU_SERVICE_STOP); return 0;
    }
    s->phase = PVR_GPU_SERVICE_STOPPING; return 1;
}
static void advance(struct pvr_gpu_service *s)
{
    if (s->phase == PVR_GPU_SERVICE_FAILED) return;
    if (!interrupt_mask(s)) { fail(s, PVR_GPU_SERVICE_INTERRUPT); return; }
    uint64_t now = s->last_now;
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
        int r = pvr_compute_service_poll(s->config.compute);
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
        now = s->start.last_now;
        if (r == PVR_GPU_START_OK) {
            struct pvr_gpu_control_ops ops = s->config.control;
            if (s->config.free_lists.count || s->config.compute) { ops.now = free_list_clock; ops.event = free_list_event; }
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
        now = s->control.last_now;
        if (r == PVR_GPU_CONTROL_OK) { s->phase = PVR_GPU_SERVICE_READY; rearm = 1; }
        else if (r != PVR_GPU_CONTROL_AGAIN) { fail(s, PVR_GPU_SERVICE_CONTROL); return; }
        if (s->config.free_lists.count) {
            if (free_list_progress(s, 0) == PVR_FREE_LIST_FAULT) return;
            now = s->last_now;
        }
        if (s->config.compute) {
            unsigned phase = s->config.compute->phase;
            if (phase == PVR_COMPUTE_SERVICE_FORMAT || phase == PVR_COMPUTE_SERVICE_RUNNING) {
                if (pvr_compute_service_poll(s->config.compute) < 0) {
                    fail(s, PVR_GPU_SERVICE_COMPUTATION); return;
                }
            }
            now = s->last_now;
        }
        break;
    }
    case PVR_GPU_SERVICE_DRAINING: {
        uint64_t current;
        if (!free_list_clock(s->config.context, &current)) { fail(s, PVR_GPU_SERVICE_CLOCK); return; }
        if (current >= s->deadline) { fail(s, PVR_GPU_SERVICE_STOP); return; }
        enum pvr_gpu_control_result r = pvr_gpu_control_poll(&s->control, 4);
        if (r != PVR_GPU_CONTROL_OK && r != PVR_GPU_CONTROL_AGAIN) {
            fail(s, PVR_GPU_SERVICE_CONTROL); return;
        }
        if (free_list_progress(s, 1) == PVR_FREE_LIST_FAULT) return;
        if (r == PVR_GPU_CONTROL_OK && !s->free_lists.request_count) (void)begin_stop(s);
        return;
    }
    case PVR_GPU_SERVICE_STOPPING: {
        if (s->config.free_lists.count) {
            /* Stop polling can itself dispatch events. Pause before advancing
             * another shutdown step while any response remains owned. */
            uint64_t current;
            if (!free_list_clock(s->config.context, &current)) { fail(s, PVR_GPU_SERVICE_CLOCK); return; }
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
        now = s->stop.last_now;
        if (r == PVR_GPU_STOP_OK) {
            s->phase = PVR_GPU_SERVICE_STOPPED;
        } else if (r != PVR_GPU_STOP_AGAIN) { fail(s, PVR_GPU_SERVICE_STOP); return; }
        if (s->config.free_lists.count) {
            if (s->queue.phase == PVR_CCB_ACTIVE && free_list_progress(s, 1) == PVR_FREE_LIST_FAULT) return;
            now = s->last_now;
        }
        break;
    }
    default: return;
    }
    if (now < s->last_now) { fail(s, PVR_GPU_SERVICE_CLOCK); return; }
    s->last_now = now;
    if (s->phase == PVR_GPU_SERVICE_STOPPED) s->owner = PVR_GPU_SERVICE_CLIENTS;
    if (rearm && !interrupt_rearm(s)) fail(s, PVR_GPU_SERVICE_INTERRUPT);
}
static enum pvr_gpu_service_result execute(struct pvr_gpu_service *s,
                                           const struct pvr_gpu_service_message *m)
{
    const struct pvr_gpu_service_request *r = &m->request;
    if ((unsigned)r->operation > PVR_GPU_SERVICE_COMPUTE ||
        (r->operation == PVR_GPU_SERVICE_STATUS ? r->timeout_us != 0 :
         (!r->timeout_us || r->timeout_us > INT64_MAX))) return PVR_GPU_SERVICE_INVALID;
    if (r->operation == PVR_GPU_SERVICE_STATUS)
        return s->phase == PVR_GPU_SERVICE_FAILED ? PVR_GPU_SERVICE_FAULT : PVR_GPU_SERVICE_OK;
    if (r->operation == PVR_GPU_SERVICE_OPEN) {
        if (s->phase == PVR_GPU_SERVICE_FAILED) return PVR_GPU_SERVICE_FAULT;
        if (s->owner != PVR_GPU_SERVICE_CLIENTS) return PVR_GPU_SERVICE_BUSY;
        if ((s->phase != PVR_GPU_SERVICE_COLD && s->phase != PVR_GPU_SERVICE_STOPPED) ||
            s->epoch == UINT64_MAX) return PVR_GPU_SERVICE_BUSY;
        if ((s->config.free_lists.count || s->config.compute) && s->phase == PVR_GPU_SERVICE_STOPPED)
            return PVR_GPU_SERVICE_BUSY;
        s->owner = m->client; ++s->epoch;
        if (!deadline(s, r->timeout_us) || !prepare(s)) return PVR_GPU_SERVICE_FAULT;
        return PVR_GPU_SERVICE_OK;
    }
    if (s->owner != m->client) return PVR_GPU_SERVICE_DENIED;
    if (s->phase == PVR_GPU_SERVICE_FAILED) return PVR_GPU_SERVICE_FAULT;
    if (s->phase != PVR_GPU_SERVICE_READY) return PVR_GPU_SERVICE_BUSY;
    if (r->operation == PVR_GPU_SERVICE_COMPUTE) {
        if (!s->config.compute) return PVR_GPU_SERVICE_INVALID;
        if (s->config.compute->phase != PVR_COMPUTE_SERVICE_READY) return PVR_GPU_SERVICE_BUSY;
        if (!deadline(s, r->timeout_us)) return PVR_GPU_SERVICE_FAULT;
        if (pvr_compute_service_begin(s->config.compute, s->deadline) != 1) {
            fail(s, PVR_GPU_SERVICE_COMPUTATION); return PVR_GPU_SERVICE_FAULT;
        }
        return PVR_GPU_SERVICE_OK;
    }
    if (s->config.compute && s->config.compute->phase != PVR_COMPUTE_SERVICE_READY &&
        s->config.compute->phase != PVR_COMPUTE_SERVICE_COMPLETE) return PVR_GPU_SERVICE_BUSY;
    if (!interrupt_mask(s)) { fail(s, PVR_GPU_SERVICE_INTERRUPT); return PVR_GPU_SERVICE_FAULT; }
    if (!deadline(s, r->timeout_us)) return PVR_GPU_SERVICE_FAULT;
    if (s->config.free_lists.count) { s->phase = PVR_GPU_SERVICE_DRAINING; return PVR_GPU_SERVICE_OK; }
    return begin_stop(s) ? PVR_GPU_SERVICE_OK : PVR_GPU_SERVICE_FAULT;
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
        advance(s); /* One bounded step before each request; traffic cannot starve it. */
        int active = (s->phase >= PVR_GPU_SERVICE_STARTING && s->phase <= PVR_GPU_SERVICE_STOPPING) ||
                     s->phase == PVR_GPU_SERVICE_ENABLING || s->phase == PVR_GPU_SERVICE_BUILDING ||
                     s->phase == PVR_GPU_SERVICE_DRAINING || s->phase == PVR_GPU_SERVICE_PUBLISHING ||
                     s->phase == PVR_GPU_SERVICE_PUBLISHING_GRAPHICS;
        struct pvr_gpu_service_message message;
        if (xQueueReceive(s->requests, &message, active ? 1 : portMAX_DELAY) != pdTRUE) continue;
        if (message.client == PVR_GPU_SERVICE_CLIENTS && !message.ticket) {
            taskENTER_CRITICAL(); s->irq_wake_pending = 0; taskEXIT_CRITICAL();
            continue;
        }
        configASSERT(message.client < PVR_GPU_SERVICE_CLIENTS && message.ticket);
        struct pvr_gpu_service_reply reply = {.ticket = message.ticket};
        reply.result = execute(s, &message);
        reply.phase = s->phase; reply.error = s->error; reply.epoch = s->epoch;
        reply.owns_session = s->owner == message.client;
        reply.start_error = s->start.error; reply.control_error = s->control.error;
        reply.stop_error = s->stop.error;
        if (s->config.graphics) reply.graphics_phase = s->config.graphics->phase;
        reply.free_list_error = s->free_lists.error; reply.free_list_pending = s->free_lists.request_count;
        if (s->config.compute) {
            reply.compute_phase = s->config.compute->phase; reply.compute_error = s->config.compute->error;
            reply.compute_job_error = s->config.compute->job.error;
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
        (c->compute && !c->compute_mode) ||
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
int pvr_gpu_service_send(struct pvr_gpu_service *s, const struct pvr_gpu_service_request *r,
                         TickType_t wait, uint64_t *ticket)
{
    if (!r || !ticket || wait == portMAX_DELAY) return 0;
    unsigned slot; struct pvr_gpu_service_client *c = caller(s, &slot);
    if (!c || c->pending || c->next_ticket == UINT64_MAX) return 0;
    struct pvr_gpu_service_message message = {*r, c->next_ticket + 1, slot};
    c->pending = message.ticket;
    if (xQueueSend(s->requests, &message, wait) != pdTRUE) { c->pending = 0; return 0; }
    c->next_ticket = message.ticket; *ticket = message.ticket; return 1;
}
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
