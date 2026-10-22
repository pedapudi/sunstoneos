/* Frame-path counter driver for checks/gpu-texture-frame.py.
 *
 * Links the actual render-service worker and texture code with stub owners,
 * clock, translation, cache maintenance and client rings. The harness runs it
 * under Unicorn and counts what the worker does to the CPU and memory while
 * frame_mark(1) is in force. The GPU result is written by the harness from
 * gpu_model_render() and is not counted. Nothing here touches a device. */
#include "a733_gpu_graphics_memory.h"
#include "pvr_gpu_service.h"
#include "aarch64_device_memory.h"
#include <string.h>

#define SHARED_BYTES 65536u
static struct a733_gpu_graphics_memory owner;
static struct pvr_gpu_service service;
static uint64_t clock_us;

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

/* Markers observed by the harness. 1 starts counting a request, 2 a worker
 * poll, and 0 stops counting;
 * 16 + n starts frame n; 32 + n starts the read-back or present of frame n. */
__attribute__((noinline)) void frame_mark(unsigned what) { __asm__ volatile("" :: "r"(what) : "memory"); }
__attribute__((noinline)) void gpu_model_render(uint32_t sequence) { __asm__ volatile("" :: "r"(sequence) : "memory"); }
__attribute__((noinline, noreturn)) static void fail(unsigned where)
{ frame_failure = where; for (;;) __asm__ volatile("brk #1"); }

int a733_gpu_graphics_memory_render_admitted(struct a733_gpu_graphics_memory *s) { return s == &owner; }
uintptr_t a733_gpu_graphics_memory_render_shared(const struct a733_gpu_graphics_memory *s)
{ (void)s; return (uintptr_t)frame_config.shared; }
int pvr_gpu_service_now(struct pvr_gpu_service *s, uint64_t *out) { (void)s; *out = ++clock_us; return 1; }
int pvr_compute_service_admit_mode(struct pvr_compute_service *s) { (void)s; return 1; }
/* Counted by the harness at entry. The image version takes whole 64-byte
 * lines and issues DSB SY before and after the maintenance. */
__attribute__((noinline)) int aarch64_device_cache(enum aarch64_cache_op op, uintptr_t at, size_t bytes)
{
    __asm__ volatile("" :: "r"(op), "r"(at), "r"(bytes) : "memory");
    if (!at || !bytes || (at & 63) || (bytes & 63)) fail(3);
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
    return ring_write(g, gc, gn) && ring_write(f, fc, fn);
}
int pvr_cccb_pair_fragment_tail_begin(struct pvr_cccb_pair *p, const struct pvr_cccb_command *c, unsigned n,
    const uint32_t *clean, unsigned cleans, uint64_t deadline)
{
    (void)clean; (void)cleans; (void)deadline;
    p->fragment_tail = 1; return ring_write(p->fragment, c, n);
}
int pvr_cccb_pair_poll(struct pvr_cccb_pair *p)
{
    if (p->fragment_tail && p->phase != 3) {
        uint32_t sequence = owner.submission.sequence;
        gpu_model_render(sequence);
        p->geometry->serial = sequence; p->fragment->serial = (uint64_t)sequence * 2u; p->phase = 3;
    }
    return 1;
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
    if (!pvr_render_texture_layout_profile(c->width, c->height, c->profile, &l)) fail(1);
    owner.self = (uintptr_t)&owner; owner.service = &service; owner.phase = A733_GPU_GRAPHICS_READY;
    owner.vm.phase = A733_GPU_VM_MEMORY_PUBLISHED; owner.draw = c->profile; owner.fixed_clear = 1;
    owner.profile.render.width = c->width; owner.profile.render.height = c->height;
    owner.render.payload_bytes = 0;
    owner.payload_extent.cpu = owner.payload_extent.physical = (uintptr_t)c->payload;
    owner.payload_extent.bytes = l.total;
    owner.target.data_firmware[0] = 0xc0b00000u; owner.target.data_firmware[1] = 0xc0b01000u;
    service.config.render = s; service.config.graphics = &owner; service.phase = PVR_GPU_SERVICE_READY;
    service.queue.phase = PVR_CCB_ACTIVE; service.epoch = 1;
    s->owner = &owner;
    if (!pvr_render_texture_programs_build_profile((void *)base(), PVR_RENDER_PROGRAM_BYTES, c->width, c->height,
        c->profile, &s->programs)) fail(2);
    s->phase = PVR_RENDER_SERVICE_READY;
    frame_layout = (struct frame_layout){l.output, l.output_allocation, l.image, l.geometry, l.depth, l.depth_bytes,
        l.copy, l.upload, l.download, l.total, PVR_RENDER_PROGRAM_BYTES, owner.render.payload_bytes};
    struct pvr_render_texture_input input = {c->width, c->height, (c->width + 1u) & ~1u, c->vertex_count, c->index_count};
    for (uint32_t frame = 1; frame <= c->frames; ++frame) {
        frame_mark(16 + frame);
        struct pvr_render_texture_request q = {.operation = PVR_TEXTURE_BEGIN, .previous = frame - 1,
            .input = input, .state = c->state};
        if (request(&q) != 1) fail(10);
        uint64_t generation = s->texture_generation;
        for (uint32_t offset = 0; offset < c->input_bytes;) {
            uint32_t n = c->input_bytes - offset; if (n > PVR_RENDER_TEXTURE_CHUNK) n = PVR_RENDER_TEXTURE_CHUNK;
            /* The runtime's copy of the caller's chunk; counted separately. */
            memcpy((void *)(base() + l.upload), (const void *)(uintptr_t)(c->input + offset), n);
            q = (struct pvr_render_texture_request){.operation = PVR_TEXTURE_WRITE, .previous = frame - 1,
                .generation = generation, .offset = offset, .bytes = n};
            if (request(&q) != 1) fail(11);
            offset += n;
        }
        q = (struct pvr_render_texture_request){.operation = PVR_TEXTURE_SUBMIT, .previous = frame - 1,
            .generation = generation};
        if (request(&q) != 1) fail(12);
        for (unsigned i = 0;; ++i) {
            frame_mark(2);
            int r = pvr_render_service_poll(s);
            frame_mark(0);
            if (r == 1) break;
            if (r < 0 || i > 1000000) fail(13);
        }
        frame_mark(32 + frame);
        uint32_t bytes = l.output_bytes - 128;
        if (c->present) {
            q = (struct pvr_render_texture_request){.operation = PVR_TEXTURE_PRESENT, .previous = frame};
            if (request(&q) != 1) fail(14);
            q = (struct pvr_render_texture_request){.operation = PVR_TEXTURE_RELEASE, .previous = frame,
                .generation = s->texture_output_ticket};
            if (request(&q) != 1) fail(15);
        } else for (uint32_t offset = 0; offset < bytes;) {
            uint32_t n = bytes - offset; if (n > PVR_RENDER_TEXTURE_CHUNK) n = PVR_RENDER_TEXTURE_CHUNK;
            q = (struct pvr_render_texture_request){.operation = PVR_TEXTURE_READ, .previous = frame,
                .offset = offset, .bytes = n};
            if (request(&q) != 1) fail(16);
            q = (struct pvr_render_texture_request){.operation = PVR_TEXTURE_RELEASE, .previous = frame,
                .generation = s->texture_output_ticket, .offset = offset};
            if (request(&q) != 1) fail(17);
            offset += n;
        }
    }
    frame_mark(15);
}
