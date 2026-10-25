#include "npu_session.h"
#include "a733_storage_digest.h"
#include "a733_range.h"
#include <string.h>

#define PAGE 4096u
#define SRAM_BASE 0x400000u
#define SRAM_BYTES 0x80000u

struct catalogue_entry {
    unsigned char digest[32];
    uint32_t model_bytes, inputs, outputs;
    uint32_t tensor_bytes[2 * NBG_ASSEMBLY_MAX_TENSORS];
    uint32_t nn_operations, descriptor_patches, state_patches, command_body_bytes;
};
/* Only privileged build-time entries are executable input. The digest fixes
 * descriptors, coefficients and relocations together; the counts are an
 * independent check of what the decoder and assembler derive from them. */
static const struct catalogue_entry catalogue[] = {
#include "npu_session_models.inc"
};
#define CATALOGUE_COUNT (sizeof(catalogue) / sizeof(catalogue[0]))

static size_t pages(size_t bytes) { return (bytes + PAGE - 1) & ~(size_t)(PAGE - 1); }
static int fault(struct npu_sessions *s)
{ atomic_store_explicit(&s->failed, 1, memory_order_release); return 0; }
static int good(const struct npu_sessions *s)
{ return s && s->prepared && s->count && s->count <= NPU_SESSION_MODELS &&
    !atomic_load_explicit(&s->failed, memory_order_acquire); }

/* Catalogue identity and the plan derived from an immutable model image. */
static int admit(const unsigned char *data, size_t bytes, unsigned *entry, struct nbg_model *model,
                 struct nbg_assembly_layout *layout)
{
    unsigned char digest[32];
    storage_digest_bytes(data, bytes, digest);
    unsigned e = 0;
    while (e < CATALOGUE_COUNT &&
           (catalogue[e].model_bytes != bytes || memcmp(catalogue[e].digest, digest, 32))) ++e;
    if (e == CATALOGUE_COUNT || nbg_model_open(data, bytes, model) != NBG_DECODE_OK ||
        !nbg_assembly_layout(model, layout)) return 0;
    const struct catalogue_entry *c = &catalogue[e];
    if (layout->inputs != c->inputs || layout->outputs != c->outputs ||
        layout->nn_operations != c->nn_operations ||
        layout->descriptor_patches != c->descriptor_patches ||
        layout->state_patches != c->state_patches ||
        layout->command_body_bytes != c->command_body_bytes) return 0;
    for (uint32_t i = 0; i < c->inputs + c->outputs; ++i)
        if (layout->buffers[NBG_ALLOC_INPUT + i].payload_bytes != c->tensor_bytes[i]) return 0;
    *entry = e;
    return 1;
}
static uint32_t buffer_count(const struct nbg_assembly_layout *l)
{ return NBG_ALLOC_INPUT + l->inputs + l->outputs; }
static enum a733_npu_buffer_role role(const struct nbg_assembly_layout *l, unsigned i)
{
    switch (i) {
    case NBG_ALLOC_NN: case NBG_ALLOC_COEFF: return A733_NPU_BUFFER_CONSTANT;
    case NBG_ALLOC_POOL: return A733_NPU_BUFFER_WRITE;
    case NBG_ALLOC_COMMAND: return A733_NPU_BUFFER_COMMAND;
    default: return i < NBG_ALLOC_INPUT + l->inputs ? A733_NPU_BUFFER_READ : A733_NPU_BUFFER_WRITE;
    }
}
/* The placement shared by sizing and preparation. Offsets are relative to
 * the parent allocation; devices start at the fixed base with one guard page
 * after each allocation. Returns the parent size, or 0 when it cannot fit. */
struct placement {
    size_t model_offset[NPU_SESSION_MODELS], bootstrap_offset, arena_offset, arena_bytes, bytes;
    size_t offset[NPU_SESSION_MODELS][NBG_LAYOUT_MAX_BUFFERS];
    uint32_t device[NPU_SESSION_MODELS][NBG_LAYOUT_MAX_BUFFERS], bootstrap_device;
    unsigned buffers;
};
static int place(const size_t *model_bytes, const struct nbg_assembly_layout *layouts, unsigned count,
                 struct placement *p)
{
    memset(p, 0, sizeof(*p));
    size_t offset = 0;
    for (unsigned m = 0; m < count; ++m) {
        p->model_offset[m] = offset;
        if (model_bytes[m] > NBG_MODEL_MAX_BYTES) return 0;
        offset += pages(model_bytes[m]);
    }
    uint64_t device = NPU_SESSION_DEVICE_BASE;
    p->bootstrap_offset = offset; p->bootstrap_device = (uint32_t)device;
    offset += NPU_SESSION_BOOTSTRAP_BYTES; device += NPU_SESSION_BOOTSTRAP_BYTES + PAGE;
    p->buffers = 1;
    for (unsigned m = 0; m < count; ++m)
        for (unsigned i = 0; i < buffer_count(&layouts[m]); ++i) {
            uint32_t bytes = layouts[m].buffers[i].allocation_bytes;
            if (!bytes) continue;
            if (offset > SIZE_MAX - bytes || device + bytes + PAGE > UINT32_MAX) return 0;
            p->offset[m][i] = offset; p->device[m][i] = (uint32_t)device;
            offset += bytes; device += bytes + PAGE; ++p->buffers;
        }
    /* One 16 KiB leaf table per 16 MiB root that the device range touches. */
    uint64_t roots = ((device - PAGE - 1) >> 24) - (NPU_SESSION_DEVICE_BASE >> 24) + 1;
    p->arena_offset = offset; p->arena_bytes = PAGE + roots * 16384u;
    if (p->buffers > A733_NPU_MEMORY_MAX_BUFFERS || offset > SIZE_MAX - p->arena_bytes) return 0;
    p->bytes = offset + p->arena_bytes;
    return 1;
}

size_t npu_session_requirements(const struct npu_session_model *models, unsigned count)
{
    struct nbg_assembly_layout layouts[NPU_SESSION_MODELS];
    size_t sizes[NPU_SESSION_MODELS];
    struct placement p;
    if (!models || !count || count > NPU_SESSION_MODELS) return 0;
    for (unsigned m = 0; m < count; ++m) {
        struct nbg_model model;
        unsigned entry;
        if (!a733_span_incl_nonempty(models[m].data, models[m].bytes) ||
            !admit(models[m].data, models[m].bytes, &entry, &model, &layouts[m])) return 0;
        sizes[m] = models[m].bytes;
    }
    return place(sizes, layouts, count, &p) ? p.bytes : 0;
}

/* Command streams are a short payload followed by END pairs to the end of
 * the mapped fetch span; the prefix digest plus the END check binds the
 * same bytes as a digest of the whole allocation. */
typedef uint64_t __attribute__((may_alias)) stream_word;
static int stream_intact(const struct a733_npu_buffer *b, size_t bytes, const unsigned char want[32])
{
    unsigned char actual[32];
    if (!bytes || (bytes & 7) || bytes > b->bytes) return 0;
    const stream_word *tail = (const stream_word *)b->cpu;
    for (size_t i = bytes / 8; i < b->bytes / 8; ++i) if (tail[i] != UINT64_C(0x10000000)) return 0;
    storage_digest_bytes((const void *)b->cpu, bytes, actual);
    return !memcmp(actual, want, 32);
}
/* A job publication checks the submitting model's device-read bytes; a
 * preparation-time check also covers its private model copy. */
static int model_intact(const struct npu_sessions *s, unsigned m, int full)
{
    const struct npu_session *n = &s->session[m];
    unsigned char actual[32];
    if (full) {
        storage_digest_bytes(n->model, n->model_bytes, actual);
        if (n->model_bytes != catalogue[n->entry].model_bytes ||
            memcmp(actual, catalogue[n->entry].digest, 32)) return 0;
    }
    for (unsigned j = 0; j < 2; ++j) {
        const struct a733_npu_buffer *b = &n->buffers[j ? NBG_ALLOC_COEFF : NBG_ALLOC_NN];
        storage_digest_bytes((const void *)b->cpu, b->bytes, actual);
        if (memcmp(actual, n->constant_digest[j], 32)) return 0;
    }
    return stream_intact(&n->buffers[NBG_ALLOC_COMMAND], n->stream_bytes, n->stream_digest);
}
static int all_intact(struct npu_sessions *s)
{
    if (!good(s) || !stream_intact(&s->bootstrap, A733_NPU_BOOTSTRAP_BYTES, s->bootstrap_digest))
        return fault(s);
    for (unsigned m = 0; m < s->count; ++m) if (!model_intact(s, m, 1)) return fault(s);
    return 1;
}
static int bootstrap_image(struct a733_npu_buffer *b)
{
    unsigned char *p = (void *)b->cpu;
    for (size_t i = 0; i < b->bytes; i += 8) {
        static const unsigned char end[8] = {0, 0, 0, 0x10, 0, 0, 0, 0};
        memcpy(p + i, end, 8);
    }
    return a733_npu_bootstrap(p, b->bytes);
}

int npu_session_prepare(struct npu_sessions *s, const struct npu_session_config *config)
{
    if (!s || !config || s->prepared || atomic_load(&s->failed) ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) return 0;
    const struct npu_session_config v = *config;
    if (!v.count || v.count > NPU_SESSION_MODELS || v.register_physical != 0x03600000 ||
        !a733_span_incl_nonempty((const void *)v.cpu, v.bytes) ||
        ((v.cpu | v.physical | v.bytes) & (PAGE - 1)) ||
        !a733_span_incl_nonempty(s, sizeof(*s)) ||
        a733_extents_intersect((uintptr_t)s, sizeof(*s), v.cpu, v.bytes) ||
        a733_extents_intersect((uintptr_t)config, sizeof(*config), v.cpu, v.bytes) ||
        a733_extents_intersect((uintptr_t)config, sizeof(*config), (uintptr_t)s, sizeof(*s)) ||
        a733_extents_intersect(v.register_cpu, PAGE, v.cpu, v.bytes)) return 0;
    for (unsigned m = 0; m < v.count; ++m)
        if (!a733_span_incl_nonempty(v.models[m].data, v.models[m].bytes) ||
            v.models[m].bytes > NBG_MODEL_MAX_BYTES ||
            a733_extents_intersect((uintptr_t)v.models[m].data, v.models[m].bytes, v.cpu, v.bytes) ||
            a733_extents_intersect((uintptr_t)v.models[m].data, v.models[m].bytes, (uintptr_t)s, sizeof(*s)))
            return 0;
    /* Copy first; everything below reads only the private copies. */
    atomic_init(&s->failed, 0);
    size_t offset = 0, sizes[NPU_SESSION_MODELS];
    for (unsigned m = 0; m < v.count; ++m) {
        if (offset > v.bytes || pages(v.models[m].bytes) > v.bytes - offset) return 0;
        unsigned char *copy = (unsigned char *)v.cpu + offset;
        memset(copy, 0, pages(v.models[m].bytes));
        memcpy(copy, v.models[m].data, v.models[m].bytes);
        s->session[m].model = copy; s->session[m].model_bytes = sizes[m] = v.models[m].bytes;
        offset += pages(v.models[m].bytes);
    }
    struct nbg_model models[NPU_SESSION_MODELS];
    struct nbg_assembly_layout layouts[NPU_SESSION_MODELS];
    struct placement p;
    for (unsigned m = 0; m < v.count; ++m)
        if (!admit(s->session[m].model, sizes[m], &s->session[m].entry, &models[m], &layouts[m]))
            return fault(s);
    if (!place(sizes, layouts, v.count, &p) || p.bytes != v.bytes) return fault(s);
    s->count = v.count; s->parent_cpu = v.cpu; s->parent_bytes = v.bytes;
    struct a733_npu_buffer all[A733_NPU_MEMORY_MAX_BUFFERS];
    unsigned n = 0;
    s->bootstrap = all[n++] = (struct a733_npu_buffer){v.cpu + p.bootstrap_offset,
        v.physical + p.bootstrap_offset, NPU_SESSION_BOOTSTRAP_BYTES, p.bootstrap_device,
        A733_NPU_BUFFER_BOOTSTRAP};
    for (unsigned m = 0; m < v.count; ++m) {
        struct npu_session *session = &s->session[m];
        session->layout = layouts[m];
        for (unsigned i = 0; i < buffer_count(&layouts[m]); ++i) {
            uint32_t bytes = layouts[m].buffers[i].allocation_bytes;
            if (!bytes) continue;
            session->buffers[i] = all[n++] = (struct a733_npu_buffer){v.cpu + p.offset[m][i],
                v.physical + p.offset[m][i], bytes, p.device[m][i], role(&layouts[m], i)};
        }
    }
    if (!a733_npu_native_init(&s->native, v.register_cpu, v.register_physical,
            (void *)(v.cpu + p.arena_offset), p.arena_bytes, v.physical + p.arena_offset,
            all, n, v.fatal, v.fatal_cookie)) return fault(s);
    for (unsigned m = 0; m < v.count; ++m) {
        struct npu_session *session = &s->session[m];
        struct nbg_assembly_buffer targets[NBG_LAYOUT_MAX_BUFFERS] = {0};
        for (unsigned i = 0; i < buffer_count(&layouts[m]); ++i) {
            const struct a733_npu_buffer *b = &session->buffers[i];
            if (b->bytes && !a733_npu_memory_group(&s->native.memory, b->device, m + 1)) return fault(s);
            if (b->bytes) targets[i] = (struct nbg_assembly_buffer){(void *)b->cpu, b->bytes, b->device};
        }
        struct nbg_assembly_layout assembled;
        if (!nbg_assemble_layout(&models[m], targets, buffer_count(&layouts[m]), &assembled) ||
            memcmp(&assembled, &layouts[m], sizeof(assembled)) ||
            !a733_npu_memory_command(&s->native.memory, session->buffers[NBG_ALLOC_COMMAND].device,
                                     &s->commands[m + 1])) return fault(s);
        for (unsigned j = 0; j < 2; ++j) {
            const struct a733_npu_buffer *b = &session->buffers[j ? NBG_ALLOC_COEFF : NBG_ALLOC_NN];
            storage_digest_bytes((const void *)b->cpu, b->bytes, session->constant_digest[j]);
        }
        session->stream_bytes = layouts[m].buffers[NBG_ALLOC_COMMAND].payload_bytes;
        const struct a733_npu_buffer *command = &session->buffers[NBG_ALLOC_COMMAND];
        if ((session->stream_bytes & 7) || session->stream_bytes > command->bytes) return fault(s);
        storage_digest_bytes((const void *)command->cpu, session->stream_bytes, session->stream_digest);
    }
    if (!bootstrap_image(&s->bootstrap) ||
        !a733_npu_memory_command(&s->native.memory, s->bootstrap.device, &s->commands[0]))
        return fault(s);
    storage_digest_bytes((const void *)s->bootstrap.cpu, A733_NPU_BOOTSTRAP_BYTES, s->bootstrap_digest);
    s->prepared = 1;
    return all_intact(s);
}

/* NPU register access only while the bound startup is READY, which requires
 * the shared SRAM routed to the NPU; a refusal makes no bus access. */
static int read_reg(void *cookie, unsigned offset, uint32_t *out)
{
    struct npu_sessions *s = cookie;
    return good(s) && a733_npu_startup_window_open(s->startup) &&
        a733_npu_native_operations.read(&s->native, offset, out);
}
static int write_reg(void *cookie, unsigned offset, uint32_t value)
{
    struct npu_sessions *s = cookie;
    return good(s) && a733_npu_startup_window_open(s->startup) &&
        a733_npu_native_operations.write(&s->native, offset, value);
}
static uint64_t now(void *cookie)
{ struct npu_sessions *s = cookie; return a733_npu_native_operations.microseconds(&s->native); }
static void delay(void *cookie, unsigned us)
{ struct npu_sessions *s = cookie; a733_npu_native_operations.delay(&s->native, us); }
static void barrier(void *cookie)
{ struct npu_sessions *s = cookie; a733_npu_native_operations.barrier(&s->native); }
static int same_command(const struct a733_npu_command *a, const struct a733_npu_command *b)
{ return a->device == b->device && a->readable_bytes == b->readable_bytes && a->token == b->token; }
static int publish(void *cookie, const struct a733_npu_mmu_image *image,
                   const struct a733_npu_command *command, int bootstrap)
{
    struct npu_sessions *s = cookie;
    if (!good(s) || (bootstrap != 0 && bootstrap != 1)) return fault(s);
    if (bootstrap) {
        if (!same_command(command, &s->commands[0]) || !all_intact(s)) return fault(s);
    } else {
        unsigned m = 0;
        while (m < s->count && !same_command(command, &s->commands[m + 1])) ++m;
        if (m == s->count || !model_intact(s, m, 0)) return fault(s);
    }
    return a733_npu_native_operations.publish(&s->native, image, command, bootstrap);
}
static int finish(void *cookie, const struct a733_npu_command *command, int canceled)
{ struct npu_sessions *s = cookie; return good(s) && a733_npu_native_operations.finish(&s->native, command, canceled); }
static const struct a733_npu_ops operations = {read_reg, write_reg, now, delay, barrier, publish, finish};

static int metadata(const struct npu_sessions *s, const struct a733_npu_ops *ops, void *cookie,
                    const struct a733_npu_mmu_image *image, const struct a733_npu_command *commands,
                    unsigned count)
{
    const struct a733_npu_mmu_image *own = &s->native.memory.image;
    if (!ops || !image || !commands || count != s->count + 1 || cookie != s ||
        ops->read != read_reg || ops->write != write_reg || ops->microseconds != now ||
        ops->delay != delay || ops->barrier != barrier || ops->publish != publish ||
        ops->finish != finish || image->physical != own->physical || image->bytes != own->bytes ||
        image->pd_entry != own->pd_entry || image->tables != own->tables) return 0;
    for (unsigned i = 0; i < count; ++i) if (!same_command(&commands[i], &s->commands[i])) return 0;
    return 1;
}
static int private_overlap(const struct npu_sessions *s, uintptr_t p, size_t n)
{
    return !a733_span_incl_nonempty((const void *)p, n) ||
        a733_extents_intersect(p, n, (uintptr_t)s, sizeof(*s)) ||
        a733_extents_intersect(p, n, s->parent_cpu, s->parent_bytes) ||
        a733_extents_intersect(p, n, s->native.registers, PAGE) ||
        (s->startup && a733_extents_intersect(p, n, (uintptr_t)s->startup, sizeof(*s->startup)));
}
static int prepared(void *cookie, const struct a733_npu_ops *ops, void *backend,
                    const struct a733_npu_mmu_image *image, const struct a733_npu_command *commands,
                    unsigned count)
{
    struct npu_sessions *s = cookie;
    if (!good(s) || !metadata(s, ops, backend, image, commands, count)) {
        fault(s); return -1;
    }
    int result = a733_npu_startup_prepared(s->startup);
    if (result < 0) fault(s);
    return result;
}
int npu_session_bind(struct npu_sessions *s, struct a733_npu_startup *startup,
                     const TaskHandle_t clients[], UBaseType_t priority, UBaseType_t affinity)
{
    if (!good(s) || s->bound || !startup || !startup->initialized || !startup->task || !clients ||
        private_overlap(s, (uintptr_t)startup, sizeof(*startup)) ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
        return 0;
    for (unsigned m = 0; m < s->count; ++m) {
        if (!clients[m] || clients[m] == startup->task) return 0;
        for (unsigned j = 0; j < m; ++j) if (clients[j] == clients[m]) return 0;
    }
    s->startup = startup;
    const struct npu_service_activation activation = {startup->task, prepared, s};
    if (!npu_service_init_deferred(&s->service, &operations, s, &s->native.memory.image, s->commands,
            s->count + 1, priority, affinity, &activation) || !npu_service_share(&s->service))
        return fault(s);
    for (unsigned m = 0; m < s->count; ++m) {
        s->session[m].client = clients[m];
        if (!npu_service_bind(&s->service, m, clients[m], UINT32_C(1) << (m + 1))) return fault(s);
    }
    s->bound = 1;
    return 1;
}
int npu_session_bind_irq(struct npu_sessions *s, const struct npu_service_irq_ops *ops, void *cookie,
                         UBaseType_t route)
{
    return good(s) && s->bound && npu_service_bind_irq(&s->service, ops, cookie, route);
}
static enum a733_npu_platform_result platform_step(struct npu_sessions *s, int stop)
{
    if (!s || !s->bound || !s->startup || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING ||
        xTaskGetCurrentTaskHandle() != s->startup->task) return A733_NPU_PLATFORM_RETAINED;
    uint64_t level;
    __asm__ volatile("mrs %0,CurrentEL" : "=r"(level));
    if (level != 4) return A733_NPU_PLATFORM_RETAINED;
    if (stop)
        for (unsigned m = 0; m < s->count; ++m)
            if (!atomic_load_explicit(&s->session[m].closed, memory_order_acquire))
                return A733_NPU_PLATFORM_RETAINED;
    if (!good(s)) { fault(s); return A733_NPU_PLATFORM_RETAINED; }
    enum a733_npu_platform_result result = stop ? a733_npu_startup_stop(s->startup) :
        a733_npu_startup_step(s->startup);
    if (result == A733_NPU_PLATFORM_RETAINED) fault(s);
    return result;
}
enum a733_npu_platform_result npu_session_startup_poll(struct npu_sessions *s)
{ return platform_step(s, 0); }
/* Every model's copied successful CLOSE reply: the last one followed the
 * worker's completed controller close. */
enum a733_npu_platform_result npu_session_stop_poll(struct npu_sessions *s)
{ return platform_step(s, 1); }

int npu_session_tensor(const struct npu_sessions *s, unsigned model, enum nbg_section_id section,
                       unsigned index, struct nbg_tensor *out)
{
    struct nbg_model view;
    if (!good(s) || model >= s->count || (section != NBG_INPUT && section != NBG_OUTPUT) || !out ||
        private_overlap(s, (uintptr_t)out, sizeof(*out)) ||
        nbg_model_open(s->session[model].model, s->session[model].model_bytes, &view) != NBG_DECODE_OK)
        return 0;
    return nbg_model_tensor(&view, section, index, out);
}
static struct npu_session *client(struct npu_sessions *s, unsigned model)
{
    if (!good(s) || !s->bound || model >= s->count ||
        xTaskGetSchedulerState() != taskSCHEDULER_RUNNING ||
        xTaskGetCurrentTaskHandle() != s->session[model].client) return NULL;
    return &s->session[model];
}
int npu_session_send(struct npu_sessions *s, unsigned model, enum npu_service_operation op,
                     const struct nbg_span *inputs, unsigned count, uint64_t timeout,
                     TickType_t wait, uint64_t *ticket)
{
    struct npu_session *n = client(s, model);
    if (!n || atomic_load_explicit(&n->closed, memory_order_acquire) || n->pending || !ticket ||
        private_overlap(s, (uintptr_t)ticket, sizeof(*ticket)) || wait == portMAX_DELAY ||
        (op != NPU_SERVICE_SUBMIT && (inputs || count))) return 0;
    struct npu_service_request r = {op, 0, timeout};
    switch (op) {
    case NPU_SERVICE_OPEN:
        if (n->opened || !timeout || timeout > INT64_MAX) return 0;
        break;
    case NPU_SERVICE_STATUS: case NPU_SERVICE_WAIT:
        if (!n->opened || timeout) return 0;
        break;
    case NPU_SERVICE_SUBMIT:
        if (!n->ready || n->job || !inputs || count != n->layout.inputs || !timeout ||
            timeout > INT64_MAX || private_overlap(s, (uintptr_t)inputs, count * sizeof(*inputs)))
            return 0;
        for (unsigned i = 0; i < count; ++i)
            if (inputs[i].bytes != n->layout.buffers[NBG_ALLOC_INPUT + i].payload_bytes ||
                private_overlap(s, (uintptr_t)inputs[i].data, inputs[i].bytes)) return 0;
        /* This model has no job, so the device does not use its pool. Its job
         * publication cleans and invalidates these CPU writes; other models'
         * completions never touch this session group. */
        if (n->buffers[NBG_ALLOC_POOL].bytes)
            memset((void *)n->buffers[NBG_ALLOC_POOL].cpu, 0, n->buffers[NBG_ALLOC_POOL].bytes);
        for (unsigned i = 0; i < count; ++i)
            memcpy((void *)n->buffers[NBG_ALLOC_INPUT + i].cpu, inputs[i].data, inputs[i].bytes);
        r.command = model + 1;
        break;
    case NPU_SERVICE_CANCEL:
        if (!n->job || n->output_ready || !timeout || timeout > INT64_MAX) return 0;
        break;
    case NPU_SERVICE_REAP:
        if (!n->job || timeout) return 0;
        break;
    case NPU_SERVICE_CLOSE:
        if (!n->opened || !n->ready || n->job || timeout) return 0;
        break;
    default: return 0;
    }
    uint64_t t;
    if (!npu_service_send(&s->service, &r, wait, &t)) return 0;
    n->pending = t; n->pending_operation = op;
    if (op == NPU_SERVICE_OPEN) n->opened = 1;
    if (op == NPU_SERVICE_SUBMIT) { n->job = t; n->ready = 0; n->output_ready = 0; }
    *ticket = t;
    return 1;
}
int npu_session_collect(struct npu_sessions *s, unsigned model, uint64_t ticket,
                        struct npu_service_reply *out, TickType_t wait)
{
    struct npu_session *n = client(s, model);
    if (!n || !ticket || n->pending != ticket || !out ||
        private_overlap(s, (uintptr_t)out, sizeof(*out)) || wait == portMAX_DELAY) return 0;
    struct npu_service_reply reply;
    if (!npu_service_collect(&s->service, ticket, &reply, wait)) return 0;
    enum npu_service_operation op = n->pending_operation;
    n->pending = 0; n->observed = reply;
    if (op == NPU_SERVICE_CLOSE) {
        if (reply.result != NPU_SERVICE_OK || reply.fault != A733_NPU_NO_FAULT ||
            reply.state != A733_NPU_CLOSED || reply.owns_session || reply.canceled || reply.job_ticket)
            fault(s);
        else { n->ready = 0; atomic_store_explicit(&n->closed, 1, memory_order_release); }
    } else if (reply.result != NPU_SERVICE_OK || reply.fault != A733_NPU_NO_FAULT ||
               !reply.owns_session || (n->job && reply.job_ticket != n->job)) fault(s);
    else if (op == NPU_SERVICE_REAP) {
        if (reply.state != A733_NPU_READY) fault(s);
        else { n->job = 0; n->output_ready = 0; n->ready = 1; }
    } else if (reply.state == A733_NPU_READY && !n->job) n->ready = 1;
    else if (reply.state == A733_NPU_DONE && n->job) n->output_ready = 1;
    else if (reply.state == A733_NPU_CANCELED && n->job) {
        /* A queued job was withdrawn; only REAP follows and no output exists. */
    }
    else if (reply.state != A733_NPU_BOOTING && reply.state != A733_NPU_RUNNING) fault(s);
    *out = reply;
    return 1;
}
int npu_session_output(struct npu_sessions *s, unsigned model, unsigned index, void *out, size_t bytes)
{
    struct npu_session *n = client(s, model);
    if (!n || n->pending || !n->output_ready || !n->job || index >= n->layout.outputs ||
        bytes != n->layout.buffers[NBG_ALLOC_INPUT + n->layout.inputs + index].payload_bytes ||
        private_overlap(s, (uintptr_t)out, bytes)) return 0;
    memcpy(out, (const void *)n->buffers[NBG_ALLOC_INPUT + n->layout.inputs + index].cpu, bytes);
    return 1;
}
