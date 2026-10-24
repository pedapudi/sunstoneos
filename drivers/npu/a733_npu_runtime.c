/* Compiled models loaded into fixed slots at runtime and run concurrently on
 * the one NPU. Design, budgets and failure classes:
 * docs/designs/npu-breadth.md. */
#include "npu_runtime.h"
#include "a733_range.h"
#include <string.h>

#define PAGE 4096u
#define END_PAIR UINT64_C(0x10000000)

struct catalogue_entry {
    unsigned char digest[32];
    uint32_t model_bytes, inputs, outputs;
    uint32_t tensor_bytes[2 * NBG_ASSEMBLY_MAX_TENSORS];
    uint32_t nn_operations, descriptor_patches, state_patches, command_body_bytes;
};
/* Only privileged build-time entries are executable input. A host check may
 * substitute its own generated table through NPU_RUNTIME_CATALOGUE. */
static const struct catalogue_entry catalogue[] = {
#if defined(NPU_RUNTIME_CATALOGUE)
#include NPU_RUNTIME_CATALOGUE
#else
#include "npu_session_models.inc"
#endif
};
#define CATALOGUE_COUNT (sizeof(catalogue) / sizeof(catalogue[0]))

/* Worker phases. A LOAD copies the model into the staging area, hashing and
 * checksumming each chunk, opens and plans the copy, assembles it in steps,
 * fills END pairs up to the END mark, zeroes descriptor and coefficient
 * padding, copies coefficients, checksums the staging area again, computes
 * the interim check references and cleans what it wrote. A job
 * cleans its inputs, cleans and invalidates its output lines, then zeroes,
 * cleans and invalidates its working pool. */
enum {
    LOAD_COPY, LOAD_OPEN, LOAD_ASSEMBLE, LOAD_END, LOAD_PAD, LOAD_COEFF, LOAD_STAGED, LOAD_SUM, LOAD_CLEAN,
    PREPARE_INPUT, PREPARE_OUTPUT, PREPARE_POOL,
    CHECK_READONLY
};

static size_t pages(size_t bytes) { return (bytes + PAGE - 1) & ~(size_t)(PAGE - 1); }
static size_t whole_groups(size_t bytes)
{ return (bytes + NPU_RUNTIME_GROUP_BYTES - 1) & ~(size_t)(NPU_RUNTIME_GROUP_BYTES - 1); }
static size_t lines(size_t bytes) { return (bytes + 63) & ~(size_t)63; }
static int fault(struct npu_runtime *rt)
{ atomic_store_explicit(&rt->failed, 1, memory_order_release); return 0; }
static int good(const struct npu_runtime *rt)
{ return rt && rt->prepared && !atomic_load_explicit(&rt->failed, memory_order_acquire); }
static unsigned state(const struct npu_runtime_slot *s)
{ return atomic_load_explicit(&s->state, memory_order_acquire); }
static void set_state(struct npu_runtime_slot *s, unsigned value)
{ atomic_store_explicit(&s->state, value, memory_order_release); }

/* Catalogue identity and the plan derived from the model image. */
static int admit(const unsigned char *data, size_t bytes, unsigned *entry, struct nbg_assembly_layout *layout)
{
    unsigned char digest[32];
    struct nbg_model model;
    storage_digest_bytes(data, bytes, digest);
    unsigned e = 0;
    while (e < CATALOGUE_COUNT &&
           (catalogue[e].model_bytes != bytes || memcmp(catalogue[e].digest, digest, 32))) ++e;
    if (e == CATALOGUE_COUNT || nbg_model_open(data, bytes, &model) != NBG_DECODE_OK ||
        !nbg_assembly_layout(&model, layout)) return 0;
    const struct catalogue_entry *c = &catalogue[e];
    if (layout->inputs != c->inputs || layout->outputs != c->outputs ||
        layout->nn_operations != c->nn_operations || layout->descriptor_patches != c->descriptor_patches ||
        layout->state_patches != c->state_patches || layout->command_body_bytes != c->command_body_bytes ||
        layout->buffers[NBG_ALLOC_COMMAND].allocation_bytes != NPU_RUNTIME_COMMAND_BYTES) return 0;
    for (uint32_t i = 0; i < c->inputs + c->outputs; ++i)
        if (layout->buffers[NBG_ALLOC_INPUT + i].payload_bytes != c->tensor_bytes[i]) return 0;
    *entry = e;
    return 1;
}
struct sizes { size_t constant, read, write, staging; };
static void need(const struct nbg_assembly_layout *l, size_t model_bytes, struct sizes *s)
{
    size_t constant = l->buffers[NBG_ALLOC_NN].allocation_bytes + l->buffers[NBG_ALLOC_COEFF].allocation_bytes;
    size_t read = 0, write = l->buffers[NBG_ALLOC_POOL].allocation_bytes;
    for (unsigned i = 0; i < l->inputs; ++i) read += l->buffers[NBG_ALLOC_INPUT + i].allocation_bytes;
    for (unsigned i = 0; i < l->outputs; ++i)
        write += l->buffers[NBG_ALLOC_INPUT + l->inputs + i].allocation_bytes;
    if (constant > s->constant) s->constant = constant;
    if (read > s->read) s->read = read;
    if (write > s->write) s->write = write;
    if (pages(model_bytes) > s->staging) s->staging = pages(model_bytes);
}

/* CPU offsets and device addresses. The read-only group holds the NPU table
 * arena, the bootstrap stream, the CPU-only staging area and each slot's
 * command allocation and read-only region; then come the read windows and
 * the write windows. Each group starts and ends on a 2 MiB boundary. Device
 * addresses leave one unmapped page after every allocation. */
struct placement {
    size_t arena, arena_bytes, bootstrap, staging, bytes;
    size_t command[NPU_RUNTIME_SLOTS], constant[NPU_RUNTIME_SLOTS];
    size_t read[NPU_RUNTIME_SLOTS], write[NPU_RUNTIME_SLOTS];
    uint32_t bootstrap_device, command_device[NPU_RUNTIME_SLOTS], constant_device[NPU_RUNTIME_SLOTS];
    uint32_t read_device[NPU_RUNTIME_SLOTS], write_device[NPU_RUNTIME_SLOTS];
};
static int place(const struct sizes *z, unsigned slots, struct placement *p)
{
    memset(p, 0, sizeof(*p));
    if (!z->constant || !z->read || !z->write || !z->staging || z->constant > UINT32_MAX ||
        z->read > UINT32_MAX || z->write > UINT32_MAX) return 0;
    uint64_t device = NPU_RUNTIME_DEVICE_BASE;
#define NEXT(field, bytes) do { field = (uint32_t)device; device += (uint64_t)(bytes) + PAGE; } while (0)
    NEXT(p->bootstrap_device, NPU_RUNTIME_COMMAND_BYTES);
    for (unsigned i = 0; i < slots; ++i) {
        NEXT(p->command_device[i], NPU_RUNTIME_COMMAND_BYTES);
        NEXT(p->constant_device[i], z->constant);
    }
    for (unsigned i = 0; i < slots; ++i) NEXT(p->read_device[i], z->read);
    for (unsigned i = 0; i < slots; ++i) NEXT(p->write_device[i], z->write);
#undef NEXT
    if (device > UINT64_C(1) << 32) return 0;
    /* One 16 KiB leaf table per 16 MiB the device range touches. */
    uint64_t roots = ((device - PAGE - 1) >> 24) - (NPU_RUNTIME_DEVICE_BASE >> 24) + 1;
    p->arena_bytes = PAGE + roots * 16384u;
    size_t offset = 0;
    p->arena = offset; offset += p->arena_bytes;
    p->bootstrap = offset; offset += NPU_RUNTIME_COMMAND_BYTES;
    p->staging = offset; offset += z->staging;
    for (unsigned i = 0; i < slots; ++i) {
        p->command[i] = offset; offset += NPU_RUNTIME_COMMAND_BYTES;
        p->constant[i] = offset; offset += z->constant;
    }
    offset = whole_groups(offset);
    for (unsigned i = 0; i < slots; ++i) { p->read[i] = offset; offset += z->read; }
    offset = whole_groups(offset);
    for (unsigned i = 0; i < slots; ++i) { p->write[i] = offset; offset += z->write; }
    p->bytes = whole_groups(offset);
    return 1;
}
static int sized(const struct npu_runtime_model *models, unsigned count, unsigned slots,
                 unsigned *entries, struct nbg_assembly_layout *plans, struct sizes *z, struct placement *p)
{
    memset(z, 0, sizeof(*z));
    if (!models || !count || count > NPU_RUNTIME_MODELS || !slots || slots > NPU_RUNTIME_SLOTS) return 0;
    for (unsigned m = 0; m < count; ++m) {
        if (!a733_span_incl_nonempty(models[m].data, models[m].bytes) ||
            models[m].bytes > NBG_MODEL_MAX_BYTES ||
            !admit(models[m].data, models[m].bytes, &entries[m], &plans[m])) return 0;
        need(&plans[m], models[m].bytes, z);
    }
    return place(z, slots, p);
}
size_t npu_runtime_requirements(const struct npu_runtime_model *models, unsigned count, unsigned slots)
{
    unsigned entries[NPU_RUNTIME_MODELS];
    struct nbg_assembly_layout plans[NPU_RUNTIME_MODELS];
    struct sizes z;
    struct placement p;
    return sized(models, count, slots, entries, plans, &z, &p) ? p.bytes : 0;
}

static void fill_end(uintptr_t cpu, size_t bytes)
{
    typedef uint64_t __attribute__((may_alias)) pair;
    for (size_t i = 0; i < bytes; i += 8) *(pair *)(cpu + i) = END_PAIR;
}
static int dc_zva_block(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, dczid_el0" : "=r"(value));
    return (value & 0x1f) == 4; /* Permitted, 2^4 words = 64 bytes. */
}

int npu_runtime_prepare(struct npu_runtime *rt, const struct npu_runtime_config *config)
{
    if (!rt || !config || rt->prepared || atomic_load(&rt->failed) ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) return 0;
    const struct npu_runtime_config v = *config;
    if (!v.count || v.count > NPU_RUNTIME_MODELS || !v.slots || v.slots > NPU_RUNTIME_SLOTS ||
        v.register_physical != 0x03600000 || !v.fatal ||
        !a733_span_incl_nonempty((const void *)v.cpu, v.bytes) ||
        ((v.cpu | v.physical | v.bytes) & (NPU_RUNTIME_GROUP_BYTES - 1)) ||
        !a733_span_incl_nonempty(rt, sizeof(*rt)) ||
        a733_extents_intersect((uintptr_t)rt, sizeof(*rt), v.cpu, v.bytes) ||
        a733_extents_intersect((uintptr_t)config, sizeof(*config), v.cpu, v.bytes) ||
        a733_extents_intersect((uintptr_t)config, sizeof(*config), (uintptr_t)rt, sizeof(*rt)) ||
        a733_extents_intersect(v.register_cpu, PAGE, v.cpu, v.bytes) || !dc_zva_block()) return 0;
    for (unsigned m = 0; m < v.count; ++m)
        if (!a733_span_incl_nonempty(v.models[m].data, v.models[m].bytes) ||
            a733_extents_intersect((uintptr_t)v.models[m].data, v.models[m].bytes, v.cpu, v.bytes) ||
            a733_extents_intersect((uintptr_t)v.models[m].data, v.models[m].bytes, (uintptr_t)rt, sizeof(*rt)))
            return 0;
    atomic_init(&rt->failed, 0);
    struct sizes z;
    struct placement p;
    if (!sized(v.models, v.count, v.slots, rt->entry, rt->plans, &z, &p) || p.bytes != v.bytes) return fault(rt);
    for (unsigned m = 0; m < v.count; ++m) rt->catalogue[m] = v.models[m];
    rt->count = v.count; rt->slots = v.slots;
    rt->constant_bytes = z.constant; rt->read_bytes = z.read; rt->write_bytes = z.write;
    rt->staging_bytes = z.staging; rt->staging = v.cpu + p.staging;
    rt->parent_cpu = v.cpu; rt->parent_bytes = v.bytes;
    struct a733_npu_buffer all[1 + 4 * NPU_RUNTIME_SLOTS];
    unsigned n = 0;
#define REGION(r, offset, size, dev) ((r) = (struct npu_runtime_region){v.cpu + (offset), v.physical + (offset), (size), (dev)})
    rt->bootstrap = all[n++] = (struct a733_npu_buffer){v.cpu + p.bootstrap, v.physical + p.bootstrap,
        NPU_RUNTIME_COMMAND_BYTES, p.bootstrap_device, A733_NPU_BUFFER_BOOTSTRAP};
    for (unsigned i = 0; i < v.slots; ++i) {
        struct npu_runtime_slot *s = &rt->slot[i];
        REGION(s->command, p.command[i], NPU_RUNTIME_COMMAND_BYTES, p.command_device[i]);
        REGION(s->constant, p.constant[i], z.constant, p.constant_device[i]);
        REGION(s->read, p.read[i], z.read, p.read_device[i]);
        REGION(s->write, p.write[i], z.write, p.write_device[i]);
        all[n++] = (struct a733_npu_buffer){s->command.cpu, s->command.physical, s->command.bytes,
            s->command.device, A733_NPU_BUFFER_COMMAND};
        all[n++] = (struct a733_npu_buffer){s->constant.cpu, s->constant.physical, s->constant.bytes,
            s->constant.device, A733_NPU_BUFFER_CONSTANT};
        all[n++] = (struct a733_npu_buffer){s->read.cpu, s->read.physical, s->read.bytes,
            s->read.device, A733_NPU_BUFFER_READ};
        all[n++] = (struct a733_npu_buffer){s->write.cpu, s->write.physical, s->write.bytes,
            s->write.device, A733_NPU_BUFFER_WRITE};
        /* Every byte after an empty slot's stream is an END pair; the END
         * mark is therefore zero. The bootstrap publication cleans them. */
        fill_end(s->command.cpu, s->command.bytes);
        s->end_mark = 0; s->model = UINT32_MAX;
        atomic_init(&s->state, NPU_RUNTIME_EMPTY);
    }
#undef REGION
    fill_end(rt->bootstrap.cpu, rt->bootstrap.bytes);
    if (!a733_npu_bootstrap((void *)rt->bootstrap.cpu, rt->bootstrap.bytes) ||
        !a733_npu_native_init(&rt->native, v.register_cpu, v.register_physical,
            (void *)(v.cpu + p.arena), p.arena_bytes, v.physical + p.arena, all, n, v.fatal, v.fatal_cookie) ||
        !a733_npu_memory_command(&rt->native.memory, rt->bootstrap.device, &rt->commands[0])) return fault(rt);
    for (unsigned i = 0; i < v.slots; ++i)
        if (!a733_npu_memory_command(&rt->native.memory, rt->slot[i].command.device, &rt->commands[i + 1]))
            return fault(rt);
    rt->table_crc = npu_runtime_crc32c(UINT32_MAX, (const void *)rt->native.memory.arena,
                                       rt->native.memory.image.bytes);
    rt->bootstrap_crc = npu_runtime_crc32c(UINT32_MAX, (const void *)rt->bootstrap.cpu, rt->bootstrap.bytes);
    rt->loading = rt->checking = NPU_RUNTIME_SLOTS;
    rt->last_started = v.slots - 1;
    rt->prepared = 1;
    return 1;
}

/* Startup and stop checks: the bootstrap allocation and the NPU tables. */
static int bootstrap_intact(struct npu_runtime *rt)
{
    return npu_runtime_crc32c(UINT32_MAX, (const void *)rt->bootstrap.cpu, rt->bootstrap.bytes) ==
           rt->bootstrap_crc && npu_runtime_tables_intact(rt);
}
static int same_command(const struct a733_npu_command *a, const struct a733_npu_command *b)
{ return a->device == b->device && a->readable_bytes == b->readable_bytes && a->token == b->token; }
static struct npu_runtime_slot *slot_of(struct npu_runtime *rt, const struct a733_npu_command *c)
{
    for (unsigned i = 0; i < rt->slots; ++i)
        if (same_command(c, &rt->commands[i + 1])) return &rt->slot[i];
    return NULL;
}
static int same_image(const struct npu_runtime *rt, const struct a733_npu_mmu_image *image)
{
    const struct a733_npu_mmu_image *own = &rt->native.memory.image;
    return image && image->physical == own->physical && image->bytes == own->bytes &&
           image->pd_entry == own->pd_entry && image->tables == own->tables;
}
static int maintain(struct npu_runtime *rt, enum aarch64_cache_op op, uintptr_t cpu, size_t bytes)
{
    if (!bytes) return 1;
    rt->counters.cache_lines += bytes / 64;
    return aarch64_device_cache(op, cpu, bytes) ? 1 : fault(rt);
}

/* Backend operations installed in the controller. The bootstrap uses the
 * native memory policy once; jobs use G (the T and A checks) before the
 * trigger and invalidate only the output lines after completion. */
static int read_reg(void *cookie, unsigned offset, uint32_t *out)
{
    struct npu_runtime *rt = cookie;
    ++rt->counters.register_reads;
    return good(rt) && a733_npu_native_operations.read(&rt->native, offset, out);
}
static int write_reg(void *cookie, unsigned offset, uint32_t value)
{
    struct npu_runtime *rt = cookie;
    ++rt->counters.register_writes;
    return good(rt) && a733_npu_native_operations.write(&rt->native, offset, value);
}
static uint64_t now(void *cookie)
{ struct npu_runtime *rt = cookie; return a733_npu_native_operations.microseconds(&rt->native); }
static void delay(void *cookie, unsigned us)
{ struct npu_runtime *rt = cookie; a733_npu_native_operations.delay(&rt->native, us); }
static void barrier(void *cookie)
{ struct npu_runtime *rt = cookie; a733_npu_native_operations.barrier(&rt->native); }
static int publish(void *cookie, const struct a733_npu_mmu_image *image,
                   const struct a733_npu_command *command, int bootstrap)
{
    struct npu_runtime *rt = cookie;
    if (!good(rt) || !command || !same_image(rt, image) || (bootstrap != 0 && bootstrap != 1)) return fault(rt);
    if (bootstrap) {
        if (!same_command(command, &rt->commands[0]) || !bootstrap_intact(rt)) return fault(rt);
        return a733_npu_native_operations.publish(&rt->native, image, command, 1) ? 1 : fault(rt);
    }
    struct npu_runtime_slot *s = slot_of(rt, command);
    if (!s || state(s) != NPU_RUNTIME_PREPARED || !npu_runtime_tables_intact(rt) ||
        !npu_runtime_stream_intact(rt, s)) return fault(rt);
    return 1;
}
static int finish(void *cookie, const struct a733_npu_command *command, int canceled)
{
    struct npu_runtime *rt = cookie;
    if (!good(rt) || !command) return 0;
    if (same_command(command, &rt->commands[0]))
        return a733_npu_native_operations.finish(&rt->native, command, canceled) ? 1 : fault(rt);
    struct npu_runtime_slot *s = slot_of(rt, command);
    if (!s || canceled || state(s) != NPU_RUNTIME_RUNNING || !npu_runtime_tables_intact(rt)) return fault(rt);
    const struct nbg_assembly_layout *l = &s->layout;
    for (unsigned j = 0; j < l->outputs; ++j) {
        const struct nbg_assembly_buffer *b = &s->targets[NBG_ALLOC_INPUT + l->inputs + j];
        if (!maintain(rt, AARCH64_CACHE_INVALIDATE, (uintptr_t)b->cpu,
                      lines(l->buffers[NBG_ALLOC_INPUT + l->inputs + j].payload_bytes))) return 0;
    }
    return 1;
}
static const struct a733_npu_ops operations = {read_reg, write_reg, now, delay, barrier, publish, finish};

/* Apply one chunk of at most limit bytes of op to the slot's span cursor.
 * Returns 1 once every span is done. */
enum walk { WALK_CLEAN, WALK_FLUSH, WALK_ZERO, WALK_END };
static int walk(struct npu_runtime *rt, struct npu_runtime_slot *s, const struct npu_runtime_span *spans,
                unsigned count, size_t limit, enum walk op)
{
    while (s->span < count && s->offset >= spans[s->span].bytes) { ++s->span; s->offset = 0; }
    if (s->span == count) return 1;
    size_t n = spans[s->span].bytes - s->offset;
    if (n > limit) n = limit;
    uintptr_t at = spans[s->span].cpu + s->offset;
    switch (op) {
    case WALK_CLEAN: if (!maintain(rt, AARCH64_CACHE_CLEAN, at, n)) return 0; break;
    case WALK_FLUSH: if (!maintain(rt, AARCH64_CACHE_CLEAN_INVALIDATE, at, n)) return 0; break;
    case WALK_ZERO:
        for (size_t i = 0; i < n; i += 64) __asm__ volatile("dc zva, %0" :: "r"(at + i) : "memory");
        rt->counters.zeroed_bytes += n;
        if (!maintain(rt, AARCH64_CACHE_CLEAN_INVALIDATE, at, n)) return 0;
        break;
    case WALK_END: fill_end(at, n); break;
    }
    s->offset += n;
    while (s->span < count && s->offset >= spans[s->span].bytes) { ++s->span; s->offset = 0; }
    return s->span == count;
}
static void next_phase(struct npu_runtime_slot *s, unsigned phase)
{ s->phase = phase; s->span = 0; s->offset = 0; }

/* Assembly targets inside the slot's fixed regions for an admitted plan. */
static void targets(struct npu_runtime_slot *s, const struct nbg_assembly_layout *l)
{
    memset(s->targets, 0, sizeof(s->targets));
    uint32_t nn = l->buffers[NBG_ALLOC_NN].allocation_bytes;
    s->targets[NBG_ALLOC_NN] = (struct nbg_assembly_buffer){(void *)s->constant.cpu, nn, s->constant.device};
    s->targets[NBG_ALLOC_COEFF] = (struct nbg_assembly_buffer){(void *)(s->constant.cpu + nn),
        l->buffers[NBG_ALLOC_COEFF].allocation_bytes, s->constant.device + nn};
    s->targets[NBG_ALLOC_COMMAND] = (struct nbg_assembly_buffer){(void *)s->command.cpu,
        NPU_RUNTIME_COMMAND_BYTES, s->command.device};
    uint32_t pool = l->buffers[NBG_ALLOC_POOL].allocation_bytes, read = 0, write = pool;
    if (pool) s->targets[NBG_ALLOC_POOL] = (struct nbg_assembly_buffer){(void *)s->write.cpu, pool, s->write.device};
    for (unsigned i = 0; i < l->inputs + l->outputs; ++i) {
        uint32_t bytes = l->buffers[NBG_ALLOC_INPUT + i].allocation_bytes;
        const struct npu_runtime_region *r = i < l->inputs ? &s->read : &s->write;
        uint32_t *at = i < l->inputs ? &read : &write;
        s->targets[NBG_ALLOC_INPUT + i] = (struct nbg_assembly_buffer){(void *)(r->cpu + *at), bytes,
            r->device + *at};
        *at += bytes;
    }
}
static int same_plan(const struct nbg_assembly_layout *a, const struct nbg_assembly_layout *b)
{
    if (a->inputs != b->inputs || a->outputs != b->outputs || a->nn_operations != b->nn_operations ||
        a->command_body_bytes != b->command_body_bytes) return 0;
    for (unsigned i = 0; i < NBG_ALLOC_INPUT + a->inputs + a->outputs; ++i)
        if (memcmp(&a->buffers[i], &b->buffers[i], sizeof(a->buffers[i]))) return 0;
    return 1;
}

/* LOAD ends: READY with the END mark at the stream end, or EMPTY with the END
 * mark at the highest command offset the refused load wrote. */
static void load_end(struct npu_runtime *rt, struct npu_runtime_slot *s, int loaded)
{
    unsigned index = (unsigned)(s - rt->slot);
    if (loaded) {
        s->end_mark = s->stream_bytes;
        ++rt->counters.loads;
        set_state(s, NPU_RUNTIME_READY);
    } else {
        if (s->phase >= LOAD_ASSEMBLE && rt->cursor.offset > s->end_mark) s->end_mark = rt->cursor.offset;
        s->model = UINT32_MAX;
        ++rt->counters.refused_loads;
        set_state(s, NPU_RUNTIME_EMPTY);
    }
    rt->loading = NPU_RUNTIME_SLOTS;
    npu_service_release(&rt->service, index, loaded ? NPU_SERVICE_OK : NPU_SERVICE_INVALID, A733_NPU_READY);
}
/* A failed native adapter treats any further time read as fatal; a retained
 * runtime records no more step times. */
static uint64_t clock_us(struct npu_runtime *rt)
{ return rt->native.failed ? 0 : a733_npu_native_operations.microseconds(&rt->native); }
static void timed(struct npu_runtime *rt, enum npu_runtime_step kind, uint64_t since)
{
    if (rt->native.failed) return;
    uint64_t spent = clock_us(rt) - since;
    if (spent > rt->counters.longest[kind]) rt->counters.longest[kind] = spent;
}
static int load_work(struct npu_runtime *rt);
static int load_step(struct npu_runtime *rt)
{
    uint64_t since = clock_us(rt);
    int worked = load_work(rt);
    if (worked) timed(rt, NPU_RUNTIME_STEP_LOAD, since);
    return worked;
}
static int load_work(struct npu_runtime *rt)
{
    if (rt->loading == NPU_RUNTIME_SLOTS) {
        for (unsigned k = 1; k <= rt->slots && rt->loading == NPU_RUNTIME_SLOTS; ++k) {
            unsigned i = (rt->last_started + k) % rt->slots;
            if (state(&rt->slot[i]) == NPU_RUNTIME_LOADING) rt->loading = i;
        }
        if (rt->loading == NPU_RUNTIME_SLOTS) return 0;
        next_phase(&rt->slot[rt->loading], LOAD_COPY);
        storage_digest_init(&rt->digest);
        rt->staging_crc = UINT32_MAX;
    }
    struct npu_runtime_slot *s = &rt->slot[rt->loading];
    const struct npu_runtime_model *model = &rt->catalogue[s->model];
    ++rt->counters.steps[NPU_RUNTIME_STEP_LOAD];
    switch (s->phase) {
    case LOAD_COPY: {
        /* The source is read once; the hash and the CRC cover the staged
         * bytes while they are still in the cache. */
        size_t n = model->bytes - s->offset;
        if (n > NPU_RUNTIME_LOAD_CHUNK) n = NPU_RUNTIME_LOAD_CHUNK;
        void *to = (void *)(rt->staging + s->offset);
        memcpy(to, (const unsigned char *)model->data + s->offset, n);
        (void)storage_digest_add(&rt->digest, to, n);
        rt->staging_crc = npu_runtime_crc32c(rt->staging_crc, to, n);
        rt->counters.copied_bytes += n; rt->counters.digest_bytes += n;
        s->offset += n;
        if (s->offset == model->bytes) next_phase(s, LOAD_OPEN);
        return 1;
    }
    case LOAD_OPEN: {
        unsigned char digest[32];
        storage_digest_end(&rt->digest, digest);
        const struct nbg_assembly_layout *plan = &rt->plans[s->model];
        targets(s, plan);
        if (memcmp(digest, catalogue[rt->entry[s->model]].digest, 32) ||
            nbg_model_open((const void *)rt->staging, model->bytes, &rt->view) != NBG_DECODE_OK ||
            !nbg_assembly_begin(&rt->view, s->targets, NBG_ALLOC_INPUT + plan->inputs + plan->outputs,
                                &s->layout, &rt->cursor) || !same_plan(&s->layout, plan)) {
            load_end(rt, s, 0); return 1;
        }
        for (unsigned i = 0; i < plan->inputs + plan->outputs; ++i) {
            struct npu_runtime_tensor *t = &s->tensors[i];
            int output = i >= plan->inputs;
            if (!nbg_model_tensor(&rt->view, output ? NBG_OUTPUT : NBG_INPUT, output ? i - plan->inputs : i,
                                  &t->tensor) ||
                t->tensor.logical_bytes != plan->buffers[NBG_ALLOC_INPUT + i].payload_bytes) {
                load_end(rt, s, 0); return 1;
            }
            t->data = s->targets[NBG_ALLOC_INPUT + i].cpu; t->bytes = t->tensor.logical_bytes;
        }
        next_phase(s, LOAD_ASSEMBLE);
        return 1;
    }
    case LOAD_ASSEMBLE: {
        int r = nbg_assembly_next(&rt->view, s->targets, &s->layout, &rt->cursor, NPU_RUNTIME_ASSEMBLY_STEP);
        if (r < 0) { load_end(rt, s, 0); return 1; }
        if (r) {
            s->stream_bytes = rt->cursor.offset;
            next_phase(s, LOAD_END);
        }
        return 1;
    }
    case LOAD_END: {
        struct npu_runtime_span tail = {s->command.cpu + s->stream_bytes,
            s->end_mark > s->stream_bytes ? s->end_mark - s->stream_bytes : 0};
        if (walk(rt, s, &tail, 1, NPU_RUNTIME_CACHE_CHUNK, WALK_END)) next_phase(s, LOAD_PAD);
        return 1;
    }
    case LOAD_PAD: {
        for (unsigned i = NBG_ALLOC_NN; i <= NBG_ALLOC_COEFF; ++i) {
            uint32_t payload = s->layout.buffers[i].payload_bytes;
            memset((unsigned char *)s->targets[i].cpu + payload, 0, s->targets[i].bytes - payload);
        }
        next_phase(s, LOAD_COEFF);
        return 1;
    }
    case LOAD_COEFF: {
        struct nbg_span coeff;
        uint32_t bytes = s->layout.buffers[NBG_ALLOC_COEFF].payload_bytes;
        if (!nbg_model_section(&rt->view, NBG_COEFF, &coeff) || coeff.bytes != bytes) {
            load_end(rt, s, 0); return 1;
        }
        size_t n = bytes - s->offset;
        if (n > NPU_RUNTIME_CACHE_CHUNK) n = NPU_RUNTIME_CACHE_CHUNK;
        memcpy((unsigned char *)s->targets[NBG_ALLOC_COEFF].cpu + s->offset, coeff.data + s->offset, n);
        rt->counters.copied_bytes += n;
        s->offset += n;
        if (s->offset == bytes) { next_phase(s, LOAD_STAGED); s->crc = UINT32_MAX; }
        return 1;
    }
    case LOAD_STAGED: {
        /* Interim: every task can write the staging area, and decoding,
         * assembly and the coefficient copy read it after its chunks were
         * hashed. The slot takes only bytes that still match that pass. */
        size_t n = model->bytes - s->offset;
        if (n > NPU_RUNTIME_READ_CHUNK) n = NPU_RUNTIME_READ_CHUNK;
        s->crc = npu_runtime_crc32c(s->crc, (const void *)(rt->staging + s->offset), n);
        rt->counters.staging_check_bytes += n;
        s->offset += n;
        if (s->offset < model->bytes) return 1;
        if (s->crc != rt->staging_crc) { load_end(rt, s, 0); return 1; }
        next_phase(s, LOAD_SUM); s->crc = UINT32_MAX;
        return 1;
    }
    case LOAD_SUM:
        if (npu_runtime_reference_step(rt, s)) next_phase(s, LOAD_CLEAN);
        return 1;
    case LOAD_CLEAN: {
        uint32_t extent = s->end_mark > s->stream_bytes ? s->end_mark : s->stream_bytes;
        struct npu_runtime_span spans[3] = {
            {(uintptr_t)s->targets[NBG_ALLOC_NN].cpu, s->targets[NBG_ALLOC_NN].bytes},
            {(uintptr_t)s->targets[NBG_ALLOC_COEFF].cpu, s->targets[NBG_ALLOC_COEFF].bytes},
            {s->command.cpu, lines(extent)}};
        if (walk(rt, s, spans, 3, NPU_RUNTIME_CACHE_CHUNK, WALK_CLEAN) && good(rt)) load_end(rt, s, 1);
        return 1;
    }
    default:
        return fault(rt);
    }
}

/* P: clean the inputs; while the interim lasts, clean and invalidate the
 * output lines so a dirty client line cannot be evicted over NPU output;
 * then zero the working pool with DC ZVA and clean and invalidate it. */
static void prepare_work(struct npu_runtime *rt, struct npu_runtime_slot *s);
static void prepare_step(struct npu_runtime *rt, struct npu_runtime_slot *s)
{
    uint64_t since = clock_us(rt);
    enum npu_runtime_step kind = s->phase == PREPARE_POOL ? NPU_RUNTIME_STEP_POOL : NPU_RUNTIME_STEP_PREPARE;
    prepare_work(rt, s);
    timed(rt, kind, since);
}
static void prepare_work(struct npu_runtime *rt, struct npu_runtime_slot *s)
{
    const struct nbg_assembly_layout *l = &s->layout;
    struct npu_runtime_span spans[NBG_ASSEMBLY_MAX_TENSORS];
    unsigned count = 0, first = s->phase == PREPARE_INPUT ? 0 : l->inputs;
    unsigned tensors = s->phase == PREPARE_INPUT ? l->inputs : l->outputs;
    if (s->phase == PREPARE_POOL) {
        ++rt->counters.steps[NPU_RUNTIME_STEP_POOL];
        spans[count++] = (struct npu_runtime_span){(uintptr_t)s->targets[NBG_ALLOC_POOL].cpu,
            s->targets[NBG_ALLOC_POOL].bytes};
        if (walk(rt, s, spans, count, NPU_RUNTIME_CACHE_CHUNK, WALK_ZERO) && good(rt))
            set_state(s, NPU_RUNTIME_PREPARED);
        return;
    }
    ++rt->counters.steps[NPU_RUNTIME_STEP_PREPARE];
    for (unsigned i = 0; i < tensors; ++i)
        spans[count++] = (struct npu_runtime_span){(uintptr_t)s->targets[NBG_ALLOC_INPUT + first + i].cpu,
            lines(l->buffers[NBG_ALLOC_INPUT + first + i].payload_bytes)};
    if (walk(rt, s, spans, count, NPU_RUNTIME_CACHE_CHUNK,
             s->phase == PREPARE_INPUT ? WALK_CLEAN : WALK_FLUSH))
        next_phase(s, s->phase == PREPARE_INPUT ? PREPARE_OUTPUT : PREPARE_POOL);
}

/* H: the slot's read-only bytes after its job; the SUBMIT reply waits for it. */
static int check_step(struct npu_runtime *rt)
{
    struct npu_runtime_slot *s = &rt->slot[rt->checking];
    ++rt->counters.steps[NPU_RUNTIME_STEP_CHECK];
    uint64_t since = clock_us(rt);
    int checked = npu_runtime_check_step(rt, s);
    timed(rt, NPU_RUNTIME_STEP_CHECK, since);
    if (!checked) return 1;
    unsigned index = rt->checking;
    rt->checking = NPU_RUNTIME_SLOTS;
    if (checked < 0) {
        /* Class 3: any task can write the allocation in the interim. */
        fault(rt); npu_service_retain(&rt->service);
        return 1;
    }
    set_state(s, NPU_RUNTIME_READY);
    npu_service_release(&rt->service, index, NPU_SERVICE_OK, A733_NPU_DONE);
    return 1;
}

/* The first slot after the one started last whose state is in wanted. */
static unsigned candidate(const struct npu_runtime *rt, unsigned wanted)
{
    for (unsigned k = 1; k <= rt->slots; ++k) {
        unsigned i = (rt->last_started + k) % rt->slots;
        if (wanted & (1u << state(&rt->slot[i]))) return i;
    }
    return NPU_RUNTIME_SLOTS;
}
#define QUEUED (1u << NPU_RUNTIME_QUEUED)
#define SUBMITTED (QUEUED | 1u << NPU_RUNTIME_PREPARED)
/* G and the trigger: the publication callback runs T and A. */
static void start(struct npu_runtime *rt, unsigned i)
{
    ++rt->counters.steps[NPU_RUNTIME_STEP_START];
    uint64_t since = clock_us(rt);
    int started = npu_service_start(&rt->service, i, i + 1, rt->slot[i].timeout_us);
    timed(rt, NPU_RUNTIME_STEP_START, since);
    if (started > 0) {
        set_state(&rt->slot[i], NPU_RUNTIME_RUNNING);
        rt->last_started = i; ++rt->counters.jobs;
    } else if (started < 0) {
        fault(rt); npu_service_retain(&rt->service);
    }
}
/* One step in the design's order: a started check; with the NPU idle, the
 * next start in round-robin order (then one LOAD step) or that job's
 * preparation; the check of the job that finished first; with the NPU busy,
 * the next preparation in round-robin order; LOAD steps. */
static int step(void *cookie)
{
    struct npu_runtime *rt = cookie;
    if (!good(rt)) {
        /* A failed cache operation: retain the controller and every slot. */
        npu_service_retain(&rt->service);
        return 0;
    }
    if (rt->checking < rt->slots) return check_step(rt);
    const struct a733_npu_controller *c = &rt->service.controller;
    int idle = c->state == A733_NPU_READY && rt->service.running == NPU_SERVICE_CLIENTS;
    unsigned i;
    if (idle && (i = candidate(rt, SUBMITTED)) < rt->slots) {
        if (state(&rt->slot[i]) == NPU_RUNTIME_QUEUED) prepare_step(rt, &rt->slot[i]);
        else {
            start(rt, i);
            if (good(rt) && state(&rt->slot[i]) == NPU_RUNTIME_RUNNING) (void)load_step(rt);
        }
        return 1;
    }
    unsigned first = NPU_RUNTIME_SLOTS;
    for (i = 0; i < rt->slots; ++i)
        if (state(&rt->slot[i]) == NPU_RUNTIME_FINISHED &&
            (first == NPU_RUNTIME_SLOTS || rt->slot[i].finished < rt->slot[first].finished)) first = i;
    if (first < rt->slots) {
        struct npu_runtime_slot *s = &rt->slot[first];
        rt->checking = first;
        next_phase(s, CHECK_READONLY); s->crc = UINT32_MAX;
        set_state(s, NPU_RUNTIME_CHECKING);
        return check_step(rt);
    }
    if (!idle && (i = candidate(rt, QUEUED)) < rt->slots) {
        prepare_step(rt, &rt->slot[i]);
        return 1;
    }
    return load_step(rt);
}

static int request(void *cookie, unsigned client, const struct npu_service_request *r,
                   enum npu_service_result *result)
{
    struct npu_runtime *rt = cookie;
    *result = NPU_SERVICE_INVALID;
    if (!good(rt)) { *result = NPU_SERVICE_FAULT; return 0; }
    if (client >= rt->slots) return 0;
    struct npu_runtime_slot *s = &rt->slot[client];
    unsigned now_state = state(s);
    if (r->operation == NPU_SERVICE_LOAD) {
        if (r->timeout_us || r->command >= rt->count) return 0;
        if (now_state != NPU_RUNTIME_EMPTY && now_state != NPU_RUNTIME_READY) {
            *result = NPU_SERVICE_BUSY; return 0;
        }
        s->model = r->command;
        set_state(s, NPU_RUNTIME_LOADING);
        return 1;
    }
    if (r->operation != NPU_SERVICE_SUBMIT || r->command != client + 1) return 0;
    if (now_state != NPU_RUNTIME_READY) { *result = NPU_SERVICE_BUSY; return 0; }
    s->timeout_us = r->timeout_us;
    next_phase(s, PREPARE_INPUT);
    set_state(s, NPU_RUNTIME_QUEUED);
    return 1;
}
static void finished(void *cookie, unsigned client, int faulted)
{
    struct npu_runtime *rt = cookie;
    if (faulted || client >= rt->slots || state(&rt->slot[client]) != NPU_RUNTIME_RUNNING) {
        fault(rt); return;
    }
    rt->slot[client].finished = ++rt->finishes;
    set_state(&rt->slot[client], NPU_RUNTIME_FINISHED);
}
static void closed(void *cookie, unsigned client)
{
    struct npu_runtime *rt = cookie;
    if (client < rt->slots) set_state(&rt->slot[client], NPU_RUNTIME_CLOSED);
}

static int metadata(const struct npu_runtime *rt, const struct a733_npu_ops *ops, void *cookie,
                    const struct a733_npu_mmu_image *image, const struct a733_npu_command *commands,
                    unsigned count)
{
    if (!ops || !image || !commands || count != rt->slots + 1 || cookie != rt ||
        ops->read != read_reg || ops->write != write_reg || ops->microseconds != now ||
        ops->delay != delay || ops->barrier != barrier || ops->publish != publish ||
        ops->finish != finish || !same_image(rt, image)) return 0;
    for (unsigned i = 0; i < count; ++i) if (!same_command(&commands[i], &rt->commands[i])) return 0;
    return 1;
}
static int private_overlap(const struct npu_runtime *rt, uintptr_t p, size_t n)
{
    return !a733_span_incl_nonempty((const void *)p, n) ||
        a733_extents_intersect(p, n, (uintptr_t)rt, sizeof(*rt)) ||
        a733_extents_intersect(p, n, rt->parent_cpu, rt->parent_bytes) ||
        a733_extents_intersect(p, n, rt->native.registers, PAGE) ||
        (rt->startup && a733_extents_intersect(p, n, (uintptr_t)rt->startup, sizeof(*rt->startup)));
}
static int prepared(void *cookie, const struct a733_npu_ops *ops, void *backend,
                    const struct a733_npu_mmu_image *image, const struct a733_npu_command *commands,
                    unsigned count)
{
    struct npu_runtime *rt = cookie;
    if (!good(rt) || !rt->service.irq_bound || !metadata(rt, ops, backend, image, commands, count) ||
        !bootstrap_intact(rt)) {
        fault(rt); return -1;
    }
    int result = a733_npu_startup_prepared(rt->startup);
    if (result < 0) fault(rt);
    return result;
}
int npu_runtime_bind(struct npu_runtime *rt, struct a733_npu_startup *startup, const TaskHandle_t clients[],
                     UBaseType_t priority, UBaseType_t affinity)
{
    if (!good(rt) || rt->bound || !startup || !startup->initialized || !startup->task || !clients ||
        private_overlap(rt, (uintptr_t)startup, sizeof(*startup)) ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
        return 0;
    for (unsigned i = 0; i < rt->slots; ++i) {
        if (!clients[i] || clients[i] == startup->task) return 0;
        for (unsigned j = 0; j < i; ++j) if (clients[j] == clients[i]) return 0;
    }
    rt->startup = startup;
    const struct npu_service_activation activation = {startup->task, prepared, rt};
    static const struct npu_service_runtime hooks = {request, step, finished, closed, 1};
    if (!npu_service_init_deferred(&rt->service, &operations, rt, &rt->native.memory.image, rt->commands,
            rt->slots + 1, priority, affinity, &activation) || !npu_service_share(&rt->service) ||
        !npu_service_runtime(&rt->service, &hooks, rt))
        return fault(rt);
    for (unsigned i = 0; i < rt->slots; ++i) {
        rt->slot[i].client = clients[i];
        if (!npu_service_bind(&rt->service, i, clients[i], UINT32_C(1) << (i + 1))) return fault(rt);
    }
    rt->bound = 1;
    return 1;
}
int npu_runtime_bind_irq(struct npu_runtime *rt, const struct npu_service_irq_ops *ops, void *cookie,
                         UBaseType_t route, unsigned idle_hint)
{
    if (!good(rt) || !rt->bound || idle_hint > 1 ||
        !npu_service_bind_irq(&rt->service, ops, cookie, route)) return 0;
    rt->service.runtime->hooks.idle_hint = idle_hint;
    return 1;
}
static enum a733_npu_platform_result platform_step(struct npu_runtime *rt, int stop)
{
    if (!rt || !rt->bound || !rt->startup || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING ||
        xTaskGetCurrentTaskHandle() != rt->startup->task) return A733_NPU_PLATFORM_RETAINED;
    uint64_t level;
    __asm__ volatile("mrs %0,CurrentEL" : "=r"(level));
    if (level != 4) return A733_NPU_PLATFORM_RETAINED;
    if (stop)
        for (unsigned i = 0; i < rt->slots; ++i)
            if (state(&rt->slot[i]) != NPU_RUNTIME_CLOSED) return A733_NPU_PLATFORM_RETAINED;
    /* The service holds the only copy of the operations, image and
     * catalogue, checked once at activation. */
    if (!good(rt) || !bootstrap_intact(rt)) {
        fault(rt); return A733_NPU_PLATFORM_RETAINED;
    }
    enum a733_npu_platform_result result = stop ? a733_npu_startup_stop(rt->startup) :
        a733_npu_startup_step(rt->startup);
    if (result == A733_NPU_PLATFORM_RETAINED) fault(rt);
    return result;
}
enum a733_npu_platform_result npu_runtime_startup_poll(struct npu_runtime *rt)
{ return platform_step(rt, 0); }
enum a733_npu_platform_result npu_runtime_stop_poll(struct npu_runtime *rt)
{ return platform_step(rt, 1); }

static struct npu_runtime_slot *client(struct npu_runtime *rt, unsigned slot)
{
    if (!good(rt) || !rt->bound || slot >= rt->slots || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING ||
        xTaskGetCurrentTaskHandle() != rt->slot[slot].client) return NULL;
    return &rt->slot[slot];
}
int npu_runtime_send(struct npu_runtime *rt, unsigned slot, enum npu_service_operation op, unsigned model,
                     uint64_t timeout, TickType_t wait, uint64_t *ticket)
{
    struct npu_runtime_slot *s = client(rt, slot);
    if (!s || s->closed || s->pending || !ticket || wait == portMAX_DELAY ||
        private_overlap(rt, (uintptr_t)ticket, sizeof(*ticket))) return 0;
    struct npu_service_request r = {op, 0, 0};
    switch (op) {
    case NPU_SERVICE_OPEN:
        if (s->opened || model || !timeout || timeout > INT64_MAX) return 0;
        r.timeout_us = timeout;
        break;
    case NPU_SERVICE_STATUS: case NPU_SERVICE_CLOSE:
        if (!s->opened || model || timeout) return 0;
        break;
    case NPU_SERVICE_LOAD:
        if (!s->opened || model >= rt->count || timeout) return 0;
        r.command = model;
        break;
    case NPU_SERVICE_SUBMIT:
        if (!s->opened || model || !timeout || timeout > INT64_MAX || state(s) != NPU_RUNTIME_READY) return 0;
        r.command = slot + 1; r.timeout_us = timeout;
        break;
    default: return 0;
    }
    uint64_t t;
    if (!npu_service_send(&rt->service, &r, wait, &t)) return 0;
    s->pending = t; s->pending_operation = op;
    if (op == NPU_SERVICE_OPEN) s->opened = 1;
    *ticket = t;
    return 1;
}
int npu_runtime_collect(struct npu_runtime *rt, unsigned slot, uint64_t ticket,
                        struct npu_service_reply *out, TickType_t wait)
{
    struct npu_runtime_slot *s = client(rt, slot);
    if (!s || !ticket || s->pending != ticket || !out || wait == portMAX_DELAY ||
        private_overlap(rt, (uintptr_t)out, sizeof(*out))) return 0;
    struct npu_service_reply reply;
    if (!npu_service_collect(&rt->service, ticket, &reply, wait)) return 0;
    s->pending = 0;
    if (reply.result == NPU_SERVICE_FAULT || reply.fault != A733_NPU_NO_FAULT) fault(rt);
    else if (s->pending_operation == NPU_SERVICE_CLOSE && reply.result == NPU_SERVICE_OK) s->closed = 1;
    *out = reply;
    return 1;
}
int npu_runtime_tensor(const struct npu_runtime *rt, unsigned slot, enum nbg_section_id section, unsigned index,
                       struct npu_runtime_tensor *out)
{
    if (!good(rt) || !rt->bound || slot >= rt->slots || (section != NBG_INPUT && section != NBG_OUTPUT) ||
        !out || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING ||
        xTaskGetCurrentTaskHandle() != rt->slot[slot].client) return 0;
    const struct npu_runtime_slot *s = &rt->slot[slot];
    if (s->pending || state(s) != NPU_RUNTIME_READY ||
        index >= (section == NBG_INPUT ? s->layout.inputs : s->layout.outputs) ||
        private_overlap(rt, (uintptr_t)out, sizeof(*out))) return 0;
    *out = s->tensors[(section == NBG_INPUT ? 0 : s->layout.inputs) + index];
    return 1;
}
