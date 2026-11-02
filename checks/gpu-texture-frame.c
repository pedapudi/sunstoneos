/* Frame-path counter driver for checks/gpu-texture-frame.py.
 *
 * Links the actual render-service and texture code with stub owners,
 * clock, translation, cache maintenance and client rings. The harness runs it
 * under Unicorn and counts what the render service does to CPU and memory while
 * frame_mark(1) is in force. The GPU result is written by the harness from
 * gpu_model_render() and is not counted. Nothing here touches a device. */
#include "a733_gpu_graphics_memory.h"
#include "pvr_gpu_service.h"
#include "pvr_compute_queue.h"
#include "aarch64_device_memory.h"
#include "pvr_compute_fast.h"
#include <string.h>

#define SHARED_BYTES 65536u
static struct a733_gpu_graphics_memory owner;
static struct pvr_gpu_service service;
static uint64_t clock_us;
static unsigned device_pending, device_ready;
static unsigned scheduler_probe, probe_left, frame_complete, poll_cost = 50;
static unsigned turn_steps;
static uint64_t turn_start, task_last;
struct frame_scheduling {
    uint32_t max_burst_us, max_cpu_gap_us, cpu_blocks, task_runs, yields, submitting_turns;
};
volatile struct frame_scheduling frame_scheduling;
volatile struct frame_scheduling frame_probe_results[3];

/* Written by the harness before frame_main. */
struct frame_config {
    uint64_t payload, shared, input;
    uint32_t width, height, profile, state, frames, present;
    uint32_t vertex_count, index_count, input_bytes;
};
volatile struct frame_config frame_config;
/* Read by the harness: the layout offsets that classify stores. */
struct frame_layout {
    uint32_t output, output_allocation, image, geometry, depth, depth_bytes;
    uint32_t copy, upload, download, total, program_bytes, payload_bytes;
};
volatile struct frame_layout frame_layout;
volatile uint32_t frame_failure;
void frame_stub_reached(void);

/* Markers observed by the harness. 1 starts counting a request, 2 a worker
 * poll, and 0 stops counting; 3 records a worker tick wait between CPU-only
 * steps and 4 a wait for the GPU; 16 + n starts frame n; 32 + n starts the
 * read-back or present of frame n. */
__attribute__((noinline)) void frame_mark(unsigned what) { __asm__ volatile("" :: "r"(what) : "memory"); }
__attribute__((noinline)) void gpu_model_render(uint32_t sequence) { __asm__ volatile("" :: "r"(sequence) : "memory"); }
__attribute__((noinline, noreturn)) static void frame_fail(unsigned where)
{ frame_failure = where; for (;;) __asm__ volatile("brk #1"); }

void frame_stub_reached(void) { frame_fail(40); }
int a733_gpu_graphics_memory_render_admitted(struct a733_gpu_graphics_memory *s) { return s == &owner; }
uintptr_t a733_gpu_graphics_memory_render_shared(const struct a733_gpu_graphics_memory *s)
{ (void)s; return (uintptr_t)frame_config.shared; }
int pvr_compute_service_admit_mode(struct pvr_compute_service *s) { (void)s; return 1; }
/* Counted by the harness at entry. The image version takes whole 64-byte
 * lines and issues DSB SY before and after the maintenance. */
__attribute__((noinline)) int aarch64_device_cache(enum aarch64_cache_op op, uintptr_t at, size_t bytes)
{
    __asm__ volatile("" :: "r"(op), "r"(at), "r"(bytes) : "memory");
    if (!at || !bytes || (at & 63) || (bytes & 63)) frame_fail(3);
    return 1;
}
/* Counted by the harness at entry; the image version issues one AT. */
__attribute__((noinline)) int aarch64_device_translate(uintptr_t at, int write, struct aarch64_translation *t)
{
    (void)write;
    t->physical = at;
    t->attributes = at >= frame_config.shared && at < frame_config.shared + SHARED_BYTES ? 0x44u : 0xffu;
    t->shareability = 3; return 1;
}

/* Client rings: write each command header and payload in 256-byte chunks
 * through the worker's ring callbacks, then publish with one release
 * barrier, as pvr_cccb.c does. Completion is immediate. */
static int ring_write(struct pvr_cccb *c, const struct pvr_cccb_command *cmd, unsigned count)
{
    unsigned char header[PVR_CCCB_HEADER_BYTES] = {0};
    uint32_t control[2];
    if (c->ops.barrier(c->context, PVR_CCB_ACQUIRE) != 1 ||
        c->ops.read(c->context, PVR_CCCB_CONTROL, 4, control, 4) != 1) return 0;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t words[4] = {cmd[i].type, cmd[i].bytes, cmd[i].external_reference, cmd[i].internal_reference};
        memcpy(header, words, sizeof(words));
        if (c->cursor + PVR_CCCB_HEADER_BYTES + cmd[i].bytes > c->size) c->cursor = 0;
        if (c->ops.write(c->context, PVR_CCCB_COMMANDS, c->cursor, header, sizeof(header)) != 1) return 0;
        c->cursor += PVR_CCCB_HEADER_BYTES;
        for (uint32_t done = 0; done < cmd[i].bytes;) {
            uint32_t n = cmd[i].bytes - done; if (n > PVR_CCCB_CHUNK_BYTES) n = PVR_CCCB_CHUNK_BYTES;
            if (c->ops.write(c->context, PVR_CCCB_COMMANDS, c->cursor, (const unsigned char *)cmd[i].payload + done, n) != 1)
                return 0;
            c->cursor += n; done += n;
        }
    }
    return c->ops.barrier(c->context, PVR_CCB_RELEASE) == 1;
}
int pvr_cccb_init(struct pvr_cccb *c, const struct pvr_cccb_ops *ops, void *context, struct pvr_ccb *queue,
    uint32_t size, uint32_t firmware_context, uint64_t deadline)
{
    memset(c, 0, sizeof(*c));
    c->ops = *ops; c->context = context; c->queue = queue; c->size = size;
    c->firmware_context = firmware_context; c->deadline = deadline; c->phase = PVR_CCCB_FORMAT; return 1;
}
enum pvr_cccb_result pvr_cccb_poll(struct pvr_cccb *c) { c->phase = PVR_CCCB_IDLE; return PVR_CCCB_OK; }
enum pvr_cccb_result pvr_cccb_abort(struct pvr_cccb *c) { c->phase = PVR_CCCB_FAILED; return PVR_CCCB_FAULT; }
int pvr_cccb_pair_reusable(const struct pvr_cccb_pair *p) { return p->phase == 3 && p->fragment_tail; }
int pvr_cccb_pair_begin(struct pvr_cccb_pair *p, struct pvr_cccb *g, struct pvr_cccb *f,
    const struct pvr_cccb_command *gc, unsigned gn, const struct pvr_cccb_command *fc, unsigned fn,
    const uint32_t *gclean, unsigned gcleans, const uint32_t *fclean, unsigned fcleans, uint64_t deadline)
{
    (void)gclean; (void)gcleans; (void)fclean; (void)fcleans;
    p->geometry = g; p->fragment = f; p->deadline = deadline; p->fragment_tail = 0; p->phase = 1;
    device_ready = 0;
    return ring_write(g, gc, gn) && ring_write(f, fc, fn);
}
int pvr_cccb_pair_fragment_tail_begin(struct pvr_cccb_pair *p, const struct pvr_cccb_command *c, unsigned n,
    const uint32_t *clean, unsigned cleans, uint64_t deadline)
{
    (void)clean; (void)cleans; (void)deadline;
    p->fragment_tail = 1; device_ready = 0; return ring_write(p->fragment, c, n);
}
/* A pair poll remains pending until the modeled device wait completes.
 * The fixture uses the 4 ms backup wait without delivering an interrupt. */
int pvr_cccb_pair_poll(struct pvr_cccb_pair *p)
{
    if (!device_ready) { device_pending = 1; return 0; }
    if (p->fragment_tail && p->phase != 3) {
        uint32_t sequence = owner.submission.sequence;
        gpu_model_render(sequence);
        p->geometry->serial = sequence; p->fragment->serial = (uint64_t)sequence * 2u; p->phase = 3;
    }
    return 1;
}

/* The real scheduler: drivers/gpu/pvr_gpu_service.c is compiled into this unit
 * untouched, so the worker task, its burst, its CPU-run block and its wait are the
 * image's own code. The fixture models only what lies outside it: device progress
 * (the render poll's cost and the GPU wait), time, and the RTOS calls at the kernel
 * boundary. taskYIELD is counted rather than trapped (the image traps with SVC). */
static void frame_yield(void);
#undef taskYIELD
#define taskYIELD() frame_yield()
#include PVR_GPU_SERVICE_SOURCE

int pvr_compute_queue_submitting(const struct pvr_compute_queue *q) { (void)q; return 0; }
uint64_t pvr_compute_queue_progress(const struct pvr_compute_queue *q) { (void)q; return 0; }
static int frame_clock(void *unused, uint64_t *out)
{ (void)unused; *out = clock_us; return 1; }
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) { (void)q; return 0; }
static void frame_yield(void) { ++frame_scheduling.yields; }
/* The worker is run until the frame completes: its first blocking receive after
 * that returns to scheduled_frame() through this buffer. */
static uint64_t worker_exit[13];
int frame_setjmp(uint64_t *buffer) __attribute__((returns_twice));
void frame_longjmp(uint64_t *buffer) __attribute__((noreturn));
__asm__(".text\n.global frame_setjmp\nframe_setjmp:\n"
    " stp x19, x20, [x0]\n stp x21, x22, [x0, #16]\n stp x23, x24, [x0, #32]\n stp x25, x26, [x0, #48]\n"
    " stp x27, x28, [x0, #64]\n stp x29, x30, [x0, #80]\n mov x1, sp\n str x1, [x0, #96]\n mov x0, #0\n ret\n"
    ".global frame_longjmp\nframe_longjmp:\n"
    " ldp x19, x20, [x0]\n ldp x21, x22, [x0, #16]\n ldp x23, x24, [x0, #32]\n ldp x25, x26, [x0, #48]\n"
    " ldp x27, x28, [x0, #64]\n ldp x29, x30, [x0, #80]\n ldr x1, [x0, #96]\n mov sp, x1\n mov x0, #1\n ret\n");
BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t ticks)
{
    (void)q; (void)out;
    uint64_t elapsed = clock_us - turn_start;
    if (elapsed > frame_scheduling.max_burst_us) frame_scheduling.max_burst_us = elapsed;
    /* A final atomic poll may cross 200 us; no further poll may start there. */
    if (elapsed > 200u + poll_cost) frame_fail(20);
    /* The frame is done: the worker's wait for the next request is outside the check. */
    if (frame_complete) frame_longjmp(worker_exit);
    if (submitting(&service)) {
        ++frame_scheduling.submitting_turns;
        if (ticks && !device_pending) frame_fail(21);
    }
    if (ticks) {
        if (ticks != (device_pending ? 4u : 1u)) frame_fail(22);
        if (device_pending) { frame_mark(4); device_ready = 1; device_pending = 0; }
        else { frame_mark(3); ++frame_scheduling.cpu_blocks; }
        ++frame_scheduling.task_runs;
        /* The lower-priority task runs only while the worker blocks, through
         * the next tick. taskYIELD cannot schedule it while the worker is ready. */
        clock_us = (clock_us / 1000u + ticks) * 1000u;
        task_last = clock_us;
    }
    turn_start = clock_us; turn_steps = 0;
    return pdFALSE;
}
/* The render poll is the device boundary: each call costs poll_cost, and the
 * probes stand in for CPU-only render phases. */
int __real_pvr_render_service_poll(struct pvr_render_service *r);
int __wrap_pvr_render_service_poll(struct pvr_render_service *r)
{
    struct pvr_gpu_service *s = &service;
    if (frame_complete) return 0;
    if (turn_steps++ && clock_us - turn_start >= 200u) frame_fail(23);
    clock_us += poll_cost;
    int result = 0;
    if (scheduler_probe) {
        if (!probe_left--) frame_fail(24);
        if (scheduler_probe == 2) ++r->geometry.index;
        else ++r->texture_cursor;
        if (!probe_left) frame_complete = 1;
    } else {
        frame_mark(2);
        result = __real_pvr_render_service_poll(r);
        frame_mark(0);
        if (result < 0) frame_fail(13);
        if (result == 1) frame_complete = 1;
        result = 0;
    }
    if (!submitting(s)) {
        uint64_t gap = clock_us - task_last;
        if (gap > frame_scheduling.max_cpu_gap_us) frame_scheduling.max_cpu_gap_us = gap;
        /* The CPU run may end one burst and one atomic poll past 1 ms. */
        if (gap > 1000u + 200u + poll_cost) frame_fail(25);
    }
    return result;
}
static void scheduled_frame(void)
{
    frame_complete = 0; service.yield_running = 0; task_last = clock_us;
    turn_start = clock_us; turn_steps = 0;
    if (!frame_setjmp(worker_exit)) worker(&service);
}
void frame_scheduling_check(void)
{
    service.config.render = &owner.submission;
    service.config.start.now = frame_clock;
    service.phase = PVR_GPU_SERVICE_READY;
    instance = &service; service.initialized = 1;
    for (unsigned mode = 1; mode <= 3; ++mode) {
        scheduler_probe = mode; probe_left = 80; poll_cost = mode == 3 ? 75 : 50;
        frame_scheduling = (struct frame_scheduling){0};
        owner.submission.phase = mode == 2 ? PVR_RENDER_SERVICE_FORMAT_GEOMETRY : PVR_RENDER_SERVICE_TEXTURE_PREPARE;
        scheduled_frame();
        if (mode == 2) {
            if (frame_scheduling.cpu_blocks || frame_scheduling.task_runs || !frame_scheduling.submitting_turns) frame_fail(27);
        } else if (!frame_scheduling.cpu_blocks || !frame_scheduling.task_runs || !frame_scheduling.max_cpu_gap_us) frame_fail(28);
        frame_probe_results[mode - 1] = frame_scheduling;
    }
    scheduler_probe = 0; poll_cost = 50;
}

static uintptr_t base(void) { return (uintptr_t)frame_config.payload + owner.render.payload_bytes; }
static int request(const struct pvr_render_texture_request *q)
{
    owner.submission.texture_request = *q;
    frame_mark(1);
    int r = pvr_render_service_texture_request(&owner.submission, UINT64_C(1) << 40);
    frame_mark(0);
    return r;
}
void frame_main(void)
{
    struct pvr_render_service *s = &owner.submission;
    struct pvr_render_texture_layout l;
    const volatile struct frame_config *c = &frame_config;
    if (!pvr_render_texture_layout_profile(c->width, c->height, c->profile, &l)) frame_fail(1);
    owner.self = (uintptr_t)&owner; owner.service = &service; owner.phase = A733_GPU_GRAPHICS_READY;
    owner.vm.phase = A733_GPU_VM_MEMORY_PUBLISHED; owner.draw = c->profile; owner.fixed_clear = 1;
    owner.profile.render.width = c->width; owner.profile.render.height = c->height;
    owner.render.payload_bytes = 0;
    owner.payload_extent.cpu = owner.payload_extent.physical = (uintptr_t)c->payload;
    owner.payload_extent.bytes = l.total;
    owner.target.data_firmware[0] = 0xc0b00000u; owner.target.data_firmware[1] = 0xc0b01000u;
    service.config.render = s; service.config.graphics = &owner; service.phase = PVR_GPU_SERVICE_READY;
    service.config.start.now = frame_clock;
    instance = &service; service.initialized = 1; /* The service clock is the real one. */
    service.queue.phase = PVR_CCB_ACTIVE; service.epoch = 1;
    s->owner = &owner;
    if (!pvr_render_texture_programs_build_profile((void *)base(), PVR_RENDER_PROGRAM_BYTES, c->width, c->height,
        c->profile, &s->programs)) frame_fail(2);
    s->phase = PVR_RENDER_SERVICE_READY;
    frame_layout = (struct frame_layout){l.output, l.output_allocation, l.image, l.geometry, l.depth, l.depth_bytes,
        l.copy, l.upload, l.download, l.total, PVR_RENDER_PROGRAM_BYTES, owner.render.payload_bytes};
    struct pvr_render_texture_input input = {c->width, c->height, (c->width + 1u) & ~1u, c->vertex_count, c->index_count};
    for (uint32_t frame = 1; frame <= c->frames; ++frame) {
        frame_mark(16 + frame);
        struct pvr_render_texture_request q = {.operation = PVR_TEXTURE_BEGIN, .previous = frame - 1,
            .input = input, .state = c->state};
        if (request(&q) != 1) frame_fail(10);
        uint64_t generation = s->texture_generation;
        for (uint32_t offset = 0; offset < c->input_bytes;) {
            uint32_t n = c->input_bytes - offset; if (n > PVR_RENDER_TEXTURE_CHUNK) n = PVR_RENDER_TEXTURE_CHUNK;
            /* The runtime's copy of the caller's chunk; counted separately. */
            memcpy((void *)(base() + l.upload), (const void *)(uintptr_t)(c->input + offset), n);
            q = (struct pvr_render_texture_request){.operation = PVR_TEXTURE_WRITE, .previous = frame - 1,
                .generation = generation, .offset = offset, .bytes = n};
            if (request(&q) != 1) frame_fail(11);
            offset += n;
        }
        q = (struct pvr_render_texture_request){.operation = PVR_TEXTURE_SUBMIT, .previous = frame - 1,
            .generation = generation};
        if (request(&q) != 1) frame_fail(12);
        frame_scheduling = (struct frame_scheduling){0};
        scheduled_frame();
        frame_mark(32 + frame);
        uint32_t bytes = l.output_bytes - 128;
        if (c->present) {
            q = (struct pvr_render_texture_request){.operation = PVR_TEXTURE_PRESENT, .previous = frame};
            if (request(&q) != 1) frame_fail(14);
            q = (struct pvr_render_texture_request){.operation = PVR_TEXTURE_RELEASE, .previous = frame,
                .generation = s->texture_output_ticket};
            if (request(&q) != 1) frame_fail(15);
        } else for (uint32_t offset = 0; offset < bytes;) {
            uint32_t n = bytes - offset; if (n > PVR_RENDER_TEXTURE_CHUNK) n = PVR_RENDER_TEXTURE_CHUNK;
            q = (struct pvr_render_texture_request){.operation = PVR_TEXTURE_READ, .previous = frame,
                .offset = offset, .bytes = n};
            if (request(&q) != 1) frame_fail(16);
            q = (struct pvr_render_texture_request){.operation = PVR_TEXTURE_RELEASE, .previous = frame,
                .generation = s->texture_output_ticket, .offset = offset};
            if (request(&q) != 1) frame_fail(17);
            offset += n;
        }
    }
    frame_mark(15);
}
