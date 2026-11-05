/* Compiled models loaded into fixed slots at runtime and run concurrently on
 * the one NPU. Design, budgets and failure classes:
 * docs/designs/npu-breadth.md. */
#include "npu_arch.h"
#include "npu_runtime.h"
#include "a733_range.h"
#include "npu_pool.h"
#include "sunstone_npu_queue_service.h"
#include <string.h>

#define PAGE 4096u
#define END_PAIR UINT64_C(0x10000000)

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

/* The plan derived from the model image. The graph is admitted by what it is, not by a table: the container and its
 * checksums are verified, every operation is checked by the assembler, and every address it patches is confined to the
 * job's buffers (docs/designs/npu-loader-admission.md). The SHA-256 taken here is the reference for LOAD, which
 * hashes the copy it stages and compares: it detects a corrupted or replaced copy, and it needs no table. The layout's
 * table (layout->buffers, capacity) is the caller's, sized by nbg_assembly_layout_entries. */
static int admit(const unsigned char *data, size_t bytes, const struct nbg_model *model, unsigned char digest[32],
                 struct nbg_assembly_layout *layout)
{
    if (digest) storage_digest_bytes(data, bytes, digest);
    /* The command allocation is the graph's own stream rounded to a page; a stream longer than one fetch continues
     * through links (nbg_assembly.c), so nothing bounds it here but the slot: a slot is sized by the largest resident
     * graph, and a graph registered later that needs a longer command region than its slot holds is refused by the
     * fit check there (npu_runtime_register, LOAD). */
    return nbg_assembly_layout(model, layout);
}
struct sizes { size_t command, constant, read, write, staging; };
static void need(const struct nbg_assembly_layout *l, size_t model_bytes, struct sizes *s)
{
    size_t constant = l->buffers[NBG_ALLOC_NN].allocation_bytes + l->buffers[NBG_ALLOC_COEFF].allocation_bytes;
    /* The dynamic-input allocation follows the coefficients in the same read-only region. */
    if (l->dynamic) constant += l->buffers[nbg_layout_dynamic_index(l)].allocation_bytes;
    size_t read = 0, write = l->buffers[NBG_ALLOC_POOL].allocation_bytes;
    for (unsigned i = 0; i < l->inputs; ++i) read += l->buffers[NBG_ALLOC_INPUT + i].allocation_bytes;
    for (unsigned i = 0; i < l->outputs; ++i)
        write += l->buffers[NBG_ALLOC_INPUT + l->inputs + i].allocation_bytes;
    size_t command = pages(l->buffers[NBG_ALLOC_COMMAND].allocation_bytes);
    if (command > s->command) s->command = command;
    if (constant > s->constant) s->constant = constant;
    if (read > s->read) s->read = read;
    if (write > s->write) s->write = write;
    if (pages(model_bytes) > s->staging) s->staging = pages(model_bytes);
}

/* Every table is taken from the pool through its allocator (include/npu_pool.h); a lay_out keeps the running total, which is
 * the size of the tables that the regions are placed behind. A refused allocation is the declaration not fitting the pool. */
struct bump { struct npu_pool *pool; struct npu_meta *meta; size_t used, meta_used; int failed; };
/* Device-visible memory (the pool): what the NPU or EL0 reads or writes by design. */
static void *take(struct bump *b, size_t bytes)
{
    void *p = b->failed ? NULL : npu_pool_allocate(b->pool, bytes);
    if (!p) { b->failed = 1; return NULL; }
    b->used += (bytes + 63) & ~(size_t)63;
    return p;
}
/* EL1-private metadata: tables that the EL1 checks trust, from memory no EL0 mapping reaches (include/npu_pool.h). A counting
 * sizing pass returns tokens, never memory. */
static void *take_meta(struct bump *b, size_t bytes)
{
    void *p = b->failed ? NULL : npu_meta_allocate(b->meta, bytes);
    if (!p) { b->failed = 1; return NULL; }
    b->meta_used += (bytes + 63) & ~(size_t)63;
    return p;
}
/* What sizing keeps of a graph when no plan table exists: its regions and its layout's entries. */
struct record { struct sizes size; unsigned entries; };

/* CPU offsets and device addresses. The read-only group holds the tables, the NPU table arena, the bootstrap stream, the
 * CPU-only staging area and each slot's command allocation and read-only region; then come the read windows and the write
 * windows. Each group starts and ends on a 2 MiB boundary. Device addresses leave one unmapped page after every
 * allocation. */
struct placement {
    size_t tables_bytes, arena, arena_bytes, bootstrap, staging, bytes;
    size_t command0, read0, write0;
    uint32_t bootstrap_device, command_device0, read_device0, write_device0;
};
/* The regions of one slot, running offsets from the three bases. */
struct slot_dims { struct sizes size; size_t command_cpu, read_cpu, write_cpu; uint64_t command_dev, read_dev, write_dev; };
static void slot_advance(struct slot_dims *d)
{
    d->command_cpu += d->size.command + d->size.constant;
    d->command_dev += d->size.command + PAGE + d->size.constant + PAGE;
    d->read_cpu += d->size.read; d->read_dev += d->size.read + PAGE;
    d->write_cpu += d->size.write; d->write_dev += d->size.write + PAGE;
}
size_t npu_runtime_tables_bytes(unsigned count, unsigned slots)
{
    const size_t per = sizeof(struct npu_runtime_model) + sizeof(struct nbg_assembly_layout) + 32;
    return ((size_t)count * per + 63) / 64 * 64 + (size_t)slots * ((sizeof(struct npu_runtime_slot) + 63) / 64 * 64);
}
static int place(struct placement *p, const struct sizes *z, size_t tables_bytes, const struct slot_dims *total)
{
    memset(p, 0, sizeof(*p));
    if (!z->command || !z->constant || !z->read || !z->write || !z->staging || z->constant > UINT32_MAX ||
        z->read > UINT32_MAX || z->write > UINT32_MAX) return 0;
    uint64_t device = NPU_RUNTIME_DEVICE_BASE;
    p->bootstrap_device = (uint32_t)device; device += (uint64_t)z->command + PAGE;
    p->command_device0 = (uint32_t)device; device += total->command_dev;
    p->read_device0 = (uint32_t)device; device += total->read_dev;
    p->write_device0 = (uint32_t)device; device += total->write_dev;
    /* The NPU's device addresses are 32 bits: the one hardware bound on the memory a declaration can map. */
    if (device > UINT64_C(1) << 32) return 0;
    /* One 16 KiB leaf table per 16 MiB the device range touches. */
    uint64_t roots = ((device - PAGE - 1) >> 24) - (NPU_RUNTIME_DEVICE_BASE >> 24) + 1;
    p->arena_bytes = PAGE + roots * 16384u;
    size_t offset = 0;
    p->tables_bytes = pages(tables_bytes); offset += p->tables_bytes;
    p->arena = offset; offset += p->arena_bytes;
    p->bootstrap = offset; offset += z->command;
    p->staging = offset; offset += z->staging;
    p->command0 = offset; offset += total->command_cpu;
    offset = whole_groups(offset);
    p->read0 = offset; offset += total->read_cpu;
    offset = whole_groups(offset);
    p->write0 = offset; offset += total->write_cpu;
    p->bytes = whole_groups(offset);
    return 1;
}
/* A graph's regions: from its plan when the runtime keeps plans, else from the record sizing kept. */
static struct sizes graph_sizes(const struct nbg_assembly_layout *plans, const struct record *records,
                                const struct npu_runtime_model *models, unsigned m)
{
    if (records) return records[m].size;
    struct sizes s = {0};
    need(&plans[m], models[m].bytes, &s);
    return s;
}
/* The work memory of a sizing call: one layout of the largest graph (entries of 12 bytes) and a record for each graph. */
static size_t scratch_need(unsigned count, unsigned max_entries)
{
    return (((size_t)max_entries * sizeof(struct nbg_allocation_requirement) + 63) & ~(size_t)63) + (size_t)count * sizeof(struct record);
}
size_t npu_runtime_scratch_bytes(const struct npu_runtime_model *models, unsigned count)
{
    unsigned max_entries = 0;
    if (!models || !count) return 0;
    for (unsigned m = 0; m < count; ++m) {
        struct nbg_model view;
        if (!a733_span_incl_nonempty(models[m].data, models[m].bytes) || models[m].bytes > NBG_MODEL_MAX_BYTES ||
            nbg_model_open(models[m].data, models[m].bytes, &view) != NBG_DECODE_OK) return 0;
        const size_t entries = nbg_assembly_layout_entries(&view);
        if (!entries || entries > UINT_MAX) return 0;
        if (entries > max_entries) max_entries = (unsigned)entries;
    }
    return scratch_need(count, max_entries);
}
/* Walk the declaration once: admit every graph, size and carve every table, place the regions. rt is NULL for a sizing call,
 * which keeps the graphs' layouts in the caller's scratch and writes nothing else; with rt, b carves the reservation and
 * the tables are those of the runtime. The result names the resource that does not fit. */
struct walk_out { size_t meta_bytes; struct sizes z; struct placement p; struct slot_dims total; unsigned max_slot_entries; size_t slot_tensors; unsigned later_entries; };
static enum npu_declaration_result lay_out(struct npu_runtime *rt, struct bump *b, const struct npu_runtime_model *models,
                                           unsigned count, unsigned slots, const struct npu_runtime_bounds *bounds,
                                           struct walk_out *w)
{
    memset(w, 0, sizeof(*w));
    const unsigned capacity = bounds ? bounds->capacity : 0;
    /* Graphs are given at preparation (models, count) or registered later within the declared bounds (capacity): some graph
     * must be possible. */
    if ((count && !models) || (!count && !capacity) || !slots) return NPU_DECLARATION_MODEL;
    if (bounds && bounds->per_graph && (capacity || slots > count)) return NPU_DECLARATION_MODEL;
    if (capacity > UINT32_MAX - count) return NPU_DECLARATION_MODEL;
    const size_t entries_all = (size_t)count + capacity;
    /* The graphs' tensor counts, from their headers. */
    unsigned max_entries = 0;
    for (unsigned m = 0; m < count; ++m) {
        struct nbg_model view;
        if (!a733_span_incl_nonempty(models[m].data, models[m].bytes) || models[m].bytes > NBG_MODEL_MAX_BYTES ||
            nbg_model_open(models[m].data, models[m].bytes, &view) != NBG_DECODE_OK) return NPU_DECLARATION_MODEL;
        const size_t e = nbg_assembly_layout_entries(&view);
        if (!e || e > UINT_MAX) return NPU_DECLARATION_MODEL;
        if (e > max_entries) max_entries = (unsigned)e;
    }
    struct record *records = NULL;
    struct nbg_allocation_requirement *one_table = NULL;
    if (!rt) {
        if (!bounds || !bounds->scratch || bounds->scratch_bytes < scratch_need(count, max_entries) ||
            ((uintptr_t)bounds->scratch & 7)) return NPU_DECLARATION_MEMORY;
        one_table = bounds->scratch;
        records = (struct record *)((uint8_t *)bounds->scratch + (((size_t)max_entries * sizeof(*one_table) + 63) & ~(size_t)63));
        memset(records, 0, (size_t)count * sizeof(*records));
    }
    /* The head: the catalogue, the plans, the digests and the slot records, whose sizes depend on counts alone. */
    unsigned char (*digests)[32] = NULL;
    struct nbg_assembly_layout *plans = NULL;
    if (rt) {
        rt->catalogue = take_meta(b, entries_all * sizeof(*rt->catalogue));
        plans = take_meta(b, entries_all * sizeof(*plans));
        digests = take_meta(b, entries_all * 32);
        rt->slot = take_meta(b, (size_t)slots * sizeof(*rt->slot));
        if (b->failed) return NPU_DECLARATION_MEMORY;
        rt->plans = plans; rt->model_digest = digests;
    } else {
        (void)take_meta(b, entries_all * sizeof(struct npu_runtime_model)); (void)take_meta(b, entries_all * sizeof(struct nbg_assembly_layout));
        (void)take_meta(b, entries_all * 32); (void)take_meta(b, (size_t)slots * sizeof(struct npu_runtime_slot));
    }
    struct sizes *z = &w->z;
    unsigned char digest[32];
    for (unsigned m = 0; m < count; ++m) {
        struct nbg_model view;
        (void)nbg_model_open(models[m].data, models[m].bytes, &view);
        const unsigned entries = (unsigned)nbg_assembly_layout_entries(&view);
        struct nbg_assembly_layout one = {.buffers = one_table, .capacity = entries}, *plan = &one;
        if (rt) {
            plans[m].buffers = take_meta(b, (size_t)entries * sizeof(struct nbg_allocation_requirement));
            plans[m].capacity = entries;
            if (b->failed) return NPU_DECLARATION_MEMORY;
            plan = &plans[m];
        } else (void)take_meta(b, (size_t)entries * sizeof(struct nbg_allocation_requirement)); /* counted, as prepare takes it */
        /* A model given with its digest is not hashed here: the declared digest is the reference, and LOAD hashes the copy
         * it stages and refuses a copy that differs, so the content is checked once, when it is first used (hashing
         * megabytes takes seconds before the scheduler, where the secondary cores wait only a second for the primary). */
        if (!admit(models[m].data, models[m].bytes, &view, models[m].sha256 ? NULL : (rt ? digests[m] : digest), plan))
            return NPU_DECLARATION_MODEL;
        if (models[m].sha256 && rt) memcpy(digests[m], models[m].sha256, 32);
        need(plan, models[m].bytes, z);
        if (records) { records[m].entries = entries; need(plan, models[m].bytes, &records[m].size); }
    }
    /* Graphs registered later: a model's regions are at most its bytes (the descriptors, coefficients, dynamic-input and
     * program sections are parts of it; each allocation is rounded to a page, up to five of them) and the tensors are at
     * most tensor_bytes. */
    if (capacity) {
        if (!bounds->graph_bytes || !bounds->tensor_bytes || bounds->graph_bytes > NBG_MODEL_MAX_BYTES ||
            bounds->tensor_bytes > UINT32_MAX) return NPU_DECLARATION_MODEL;
        const size_t model = pages(bounds->graph_bytes) + 5 * PAGE,
                     tensors = pages(bounds->tensor_bytes + 64); /* whole pages: the memory layer registers whole pages */
        z->command = NPU_RUNTIME_COMMAND_BYTES; /* a later graph's stream length is not known: the hardware bound */
        if (model > z->constant) z->constant = model;
        if (tensors > z->read) z->read = tensors;
        if (tensors > z->write) z->write = tensors;
        if (pages(bounds->graph_bytes) > z->staging) z->staging = pages(bounds->graph_bytes);
        /* Every input and output allocation is at least a page, so a graph that fits the regions has at most as many tensors
         * as they hold pages: its layout needs at most that many entries. */
        w->later_entries = (unsigned)(NBG_LAYOUT_ENTRIES(0, 0) + z->read / PAGE + z->write / PAGE);
    }
    const unsigned uniform_entries = w->later_entries > max_entries ? w->later_entries : max_entries;
    if (!rt) {
        for (unsigned k = 0; k < capacity; ++k) (void)take_meta(b, (size_t)w->later_entries * sizeof(struct nbg_allocation_requirement));
    }
    if (rt) {
        for (unsigned k = 0; k < capacity; ++k) {
            plans[count + k].buffers = take_meta(b, (size_t)w->later_entries * sizeof(struct nbg_allocation_requirement));
            plans[count + k].capacity = w->later_entries;
        }
        if (b->failed) return NPU_DECLARATION_MEMORY;
    }
    /* Each slot's own tables and regions. */
    struct slot_dims *total = &w->total;
    for (unsigned i = 0; i < slots; ++i) {
        unsigned m = 0, entries = uniform_entries;
        struct sizes s = *z;
        if (bounds && bounds->per_graph) {
            m = i;
            s = graph_sizes(plans, records, models, m);
            entries = records ? records[m].entries : plans[m].capacity;
        }
        if (entries > w->max_slot_entries) w->max_slot_entries = entries;
        w->slot_tensors += entries - NBG_ALLOC_INPUT;
        if (rt) {
            struct npu_runtime_slot *sl = &rt->slot[i];
            sl->layout.buffers = take_meta(b, (size_t)entries * sizeof(struct nbg_allocation_requirement));
            sl->layout.capacity = entries;
            sl->targets = take_meta(b, (size_t)entries * sizeof(*sl->targets));
            sl->tensor_capacity = entries - NBG_ALLOC_INPUT;
            sl->tensors = take_meta(b, (size_t)sl->tensor_capacity * sizeof(*sl->tensors));
            if (b->failed) return NPU_DECLARATION_MEMORY;
        } else {
            (void)take_meta(b, (size_t)entries * sizeof(struct nbg_allocation_requirement));
            (void)take_meta(b, (size_t)entries * sizeof(struct nbg_assembly_buffer));
            (void)take_meta(b, (size_t)(entries - NBG_ALLOC_INPUT) * sizeof(struct npu_runtime_tensor));
        }
        total->size = s; slot_advance(total);
    }
    /* The tables shared by the slots: the worker's spans and scratch, the commands, the registered buffers and the memory
     * layer's tables behind them, the service's tables and, for a queue image, the queue's. */
    const size_t scratch_entries = w->max_slot_entries > w->later_entries ? w->max_slot_entries : w->later_entries;
    const size_t scratch = scratch_entries * (sizeof(struct nbg_assembly_buffer) + sizeof(struct nbg_allocation_requirement));
    const size_t buffers = 1 + 4 * (size_t)slots;
    const size_t queue = bounds && bounds->queue ? ss_npu_queue_tables_bytes(slots, w->slot_tensors, bounds->queue_links) : 0;
    const size_t queue_meta = bounds && bounds->queue ? ss_npu_queue_meta_bytes(slots, bounds->queue_image_links, bounds->queue_links) : 0;
    if (rt) {
        rt->spans = take_meta(b, (size_t)w->max_slot_entries * sizeof(*rt->spans));
        rt->scratch = take_meta(b, scratch); rt->scratch_bytes = scratch;
        rt->commands = take_meta(b, ((size_t)slots + 1) * sizeof(*rt->commands));
        rt->buffers = take_meta(b, buffers * sizeof(*rt->buffers));
        rt->memory_tables = take_meta(b, a733_npu_memory_tables_bytes(buffers));
        rt->service_tables = take_meta(b, npu_service_tables_bytes(slots, slots + 1));
        rt->queue_tables = queue ? take(b, queue) : NULL; rt->queue_bytes = queue;
        rt->queue_meta = queue ? take_meta(b, queue_meta) : NULL; rt->queue_meta_bytes = queue_meta;
        rt->queue_tensors = w->slot_tensors;
        if (b->failed) return NPU_DECLARATION_MEMORY;
    } else {
        (void)take_meta(b, (size_t)w->max_slot_entries * sizeof(struct npu_runtime_span)); (void)take_meta(b, scratch);
        (void)take_meta(b, ((size_t)slots + 1) * sizeof(struct a733_npu_command)); (void)take_meta(b, buffers * sizeof(struct a733_npu_buffer));
        (void)take_meta(b, a733_npu_memory_tables_bytes(buffers)); (void)take_meta(b, npu_service_tables_bytes(slots, slots + 1));
        if (queue) { (void)take(b, queue); (void)take_meta(b, queue_meta); }
        if (b->failed) return NPU_DECLARATION_MEMORY;
    }
    w->meta_bytes = b->meta_used;
    if (!place(&w->p, z, b->used, total)) return NPU_DECLARATION_DEVICE_SPACE;
    return NPU_DECLARATION_OK;
}
enum npu_declaration_result npu_runtime_reservation_with(const struct npu_runtime_model *models, unsigned count,
                                                        unsigned slots, size_t pool_bytes, size_t *bytes,
                                                        const struct npu_runtime_bounds *bounds)
{
    struct npu_pool_window counting;
    npu_pool_window_init(&counting, NULL, SIZE_MAX / 4);
    struct npu_meta_window counting_meta;
    npu_meta_window_init(&counting_meta, NULL, SIZE_MAX / 4);
    struct bump b = {&counting.pool, &counting_meta.meta, 0, 0, 0};
    struct walk_out w;
    if (!slots) return NPU_DECLARATION_SLOTS;
    enum npu_declaration_result r = lay_out(NULL, &b, models, count, slots, bounds, &w);
    if (r != NPU_DECLARATION_OK) return r;
    *bytes = w.p.bytes;
    return w.p.bytes > pool_bytes ? NPU_DECLARATION_MEMORY : NPU_DECLARATION_OK;
}
size_t npu_runtime_meta_bytes_with(const struct npu_runtime_model *models, unsigned count, unsigned slots,
                                   const struct npu_runtime_bounds *bounds)
{
    struct npu_pool_window counting;
    npu_pool_window_init(&counting, NULL, SIZE_MAX / 4);
    struct npu_meta_window counting_meta;
    npu_meta_window_init(&counting_meta, NULL, SIZE_MAX / 4);
    struct bump b = {&counting.pool, &counting_meta.meta, 0, 0, 0};
    struct walk_out w;
    return slots && lay_out(NULL, &b, models, count, slots, bounds, &w) == NPU_DECLARATION_OK ? w.meta_bytes : 0;
}
size_t npu_runtime_requirements_with(const struct npu_runtime_model *models, unsigned count, unsigned slots,
                                     const struct npu_runtime_bounds *bounds)
{
    struct npu_pool_window counting;
    npu_pool_window_init(&counting, NULL, SIZE_MAX / 4);
    struct npu_meta_window counting_meta;
    npu_meta_window_init(&counting_meta, NULL, SIZE_MAX / 4);
    struct bump b = {&counting.pool, &counting_meta.meta, 0, 0, 0};
    struct walk_out w;
    return slots && lay_out(NULL, &b, models, count, slots, bounds, &w) == NPU_DECLARATION_OK ? w.p.bytes : 0;
}

static void fill_end(uintptr_t cpu, size_t bytes)
{
    typedef uint64_t __attribute__((may_alias)) pair;
    for (size_t i = 0; i < bytes; i += 8) *(pair *)(cpu + i) = END_PAIR;
}
static int dc_zva_block(void)
{
    return npu_arch_zva_block_is_64(); /* Permitted, 2^4 words = 64 bytes. */
}

int npu_runtime_prepare(struct npu_runtime *rt, const struct npu_runtime_config *config)
{
    if (!rt || !config || rt->prepared || atomic_load(&rt->failed) ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) return 0;
    const struct npu_runtime_config v = *config;
    if ((v.count && !v.models) || (!v.count && !v.bounds.capacity) || !v.slots || !v.meta ||
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
    /* Every table is carved from the start of the reservation, in the order lay_out walks the declaration: the per-graph
     * tables, each slot's tables, the service's and the memory layer's. The reservation is the whole pool; a declaration
     * whose tables and regions do not fit it is refused with the resource named in rt->refusal. */
    struct npu_pool_window window;
    npu_pool_window_init(&window, (void *)v.cpu, v.bytes);
    struct bump b = {&window.pool, v.meta, 0, 0, 0};
    struct walk_out w;
    rt->refusal = lay_out(rt, &b, v.models, v.count, v.slots, &v.bounds, &w);
    if (rt->refusal == NPU_DECLARATION_OK && w.p.bytes > v.bytes) rt->refusal = NPU_DECLARATION_MEMORY;
    if (rt->refusal != NPU_DECLARATION_OK) {
        npu_pool_release(&window.pool, (void *)v.cpu, b.used); /* the tables are returned; the reservation stays the runtime's */
        /* The metadata block is the OS's arena: released whole by its owner when the start is refused. */
        return fault(rt);
    }
    const struct sizes z = w.z;
    const struct placement p = w.p;
    const size_t entries = (size_t)v.count + v.bounds.capacity;
    for (unsigned m = 0; m < v.count; ++m) rt->catalogue[m] = (struct npu_runtime_model){v.models[m].data, v.models[m].bytes, NULL};
    atomic_init(&rt->count, v.count); rt->capacity = (unsigned)entries; rt->slots = v.slots;
    rt->later_entries = w.later_entries;
    rt->queue_links = v.bounds.queue_links; rt->queue_image_links = v.bounds.queue_image_links;
    rt->command_bytes = z.command; rt->constant_bytes = z.constant; rt->read_bytes = z.read; rt->write_bytes = z.write;
    rt->staging_bytes = z.staging; rt->staging = v.cpu + p.staging;
    rt->parent_cpu = v.cpu; rt->parent_bytes = v.bytes;
    rt->placed_bytes = p.bytes;
    if (!npu_service_tables(&rt->service, v.slots, v.slots + 1, rt->service_tables,
                            npu_service_tables_bytes(v.slots, v.slots + 1))) return fault(rt);
    struct a733_npu_buffer *const all = rt->buffers;
    unsigned n = 0;
#define REGION(r, offset, size, dev) ((r) = (struct npu_runtime_region){v.cpu + (offset), v.physical + (offset), (size), (dev)})
    rt->bootstrap = all[n++] = (struct a733_npu_buffer){v.cpu + p.bootstrap, v.physical + p.bootstrap,
        z.command, p.bootstrap_device, A733_NPU_BUFFER_BOOTSTRAP};
    struct slot_dims d = {0};
    for (unsigned i = 0; i < v.slots; ++i) {
        struct npu_runtime_slot *s = &rt->slot[i];
        d.size = v.bounds.per_graph ? graph_sizes(rt->plans, NULL, v.models, i) : z;
        REGION(s->command, p.command0 + d.command_cpu, d.size.command, p.command_device0 + (uint32_t)d.command_dev);
        REGION(s->constant, p.command0 + d.command_cpu + d.size.command, d.size.constant,
               p.command_device0 + (uint32_t)d.command_dev + (uint32_t)(d.size.command + PAGE));
        REGION(s->read, p.read0 + d.read_cpu, d.size.read, p.read_device0 + (uint32_t)d.read_dev);
        REGION(s->write, p.write0 + d.write_cpu, d.size.write, p.write_device0 + (uint32_t)d.write_dev);
        slot_advance(&d);
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
            (void *)(v.cpu + p.arena), p.arena_bytes, v.physical + p.arena, all, n, v.fatal, v.fatal_cookie,
            rt->memory_tables, a733_npu_memory_tables_bytes(n)) ||
        !a733_npu_memory_command(&rt->native.memory, rt->bootstrap.device, &rt->commands[0])) return fault(rt);
    for (unsigned i = 0; i < v.slots; ++i)
        if (!a733_npu_memory_command(&rt->native.memory, rt->slot[i].command.device, &rt->commands[i + 1]))
            return fault(rt);
    rt->table_crc = npu_runtime_crc32c(UINT32_MAX, (const void *)rt->native.memory.arena,
                                       rt->native.memory.image.bytes);
    rt->bootstrap_crc = npu_runtime_crc32c(UINT32_MAX, (const void *)rt->bootstrap.cpu, rt->bootstrap.bytes);
    rt->loading = rt->checking = NPU_RUNTIME_NONE;
    rt->last_started = v.slots - 1;
    rt->prepared = 1;
    return 1;
}

/* Activation and bootstrap-publication check: the bootstrap allocation and the
 * NPU tables. */
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
/* NPU register access only while the bound startup is READY, which requires
 * the shared SRAM routed to the NPU. A refusal makes no bus access; the
 * controller then faults with an I/O failure and retains its resources. */
static int read_reg(void *cookie, unsigned offset, uint32_t *out)
{
    struct npu_runtime *rt = cookie;
    ++rt->counters.register_reads;
    return good(rt) && a733_npu_startup_window_open(rt->startup) &&
        a733_npu_native_operations.read(&rt->native, offset, out);
}
static int write_reg(void *cookie, unsigned offset, uint32_t value)
{
    struct npu_runtime *rt = cookie;
    ++rt->counters.register_writes;
    int written = good(rt) && a733_npu_startup_window_open(rt->startup) &&
        a733_npu_native_operations.write(&rt->native, offset, value);
#ifdef SUNSTONE_NPU_QUEUE_TIMING
    if (written && offset == 0x3a4) {
        /* Match the direct adapter's timestamp after completed device writes. */
        __asm__ volatile("dsb sy\n isb\n mrs %0,cntpct_el0" : "=r"(rt->doorbell_ticks) :: "memory");
    }
#endif
#ifdef SUNSTONE_A733_OS_NPU_OBSERVE
    if(written && offset==0x3a4 && rt->service.observe) {
#ifdef __aarch64__
        __asm__ volatile("mrs %0,cntpct_el0":"=r"(rt->service.trigger_ticks)::"memory");
#else
        rt->service.trigger_ticks=a733_npu_native_operations.microseconds(&rt->native)*24;
#endif
    }
#endif
    return written;
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
        for (size_t i = 0; i < n; i += 64) npu_arch_zero_line(at + i);
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
    memset(s->targets, 0, (size_t)s->layout.capacity * sizeof(*s->targets));
    uint32_t nn = l->buffers[NBG_ALLOC_NN].allocation_bytes;
    s->targets[NBG_ALLOC_NN] = (struct nbg_assembly_buffer){(void *)s->constant.cpu, nn, s->constant.device};
    /* A graph without coefficients has an empty slot, which the assembler requires to be NULL, 0, 0. */
    if (l->buffers[NBG_ALLOC_COEFF].allocation_bytes)
        s->targets[NBG_ALLOC_COEFF] = (struct nbg_assembly_buffer){(void *)(s->constant.cpu + nn),
            l->buffers[NBG_ALLOC_COEFF].allocation_bytes, s->constant.device + nn};
    if (l->dynamic) {
        const uint32_t at = nn + l->buffers[NBG_ALLOC_COEFF].allocation_bytes;
        s->targets[nbg_layout_dynamic_index(l)] = (struct nbg_assembly_buffer){(void *)(s->constant.cpu + at),
            l->buffers[nbg_layout_dynamic_index(l)].allocation_bytes, s->constant.device + at};
    }
    s->targets[NBG_ALLOC_COMMAND] = (struct nbg_assembly_buffer){(void *)s->command.cpu,
        l->buffers[NBG_ALLOC_COMMAND].allocation_bytes, s->command.device};
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
        a->command_body_bytes != b->command_body_bytes || a->dynamic != b->dynamic ||
        a->program_offset != b->program_offset || a->program_bytes != b->program_bytes) return 0;
    for (unsigned i = 0; i < nbg_layout_buffers(a); ++i)
        if (memcmp(&a->buffers[i], &b->buffers[i], sizeof(a->buffers[i]))) return 0;
    return 1;
}

/* LOAD ends: READY with the END mark at the stream end, or EMPTY with the END
 * mark at the highest command offset the refused load wrote. */
static void load_end(struct npu_runtime *rt, struct npu_runtime_slot *s, int loaded)
{
    unsigned index = (unsigned)(s - rt->slot);
    const unsigned silent = s->resident_load; /* read before the state changes: the owner clears it once the slot is READY */
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
    rt->loading = NPU_RUNTIME_NONE;
    if (rt->bound && !silent)
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
    if (rt->loading == NPU_RUNTIME_NONE) {
        for (unsigned k = 1; k <= rt->slots && rt->loading == NPU_RUNTIME_NONE; ++k) {
            unsigned i = (rt->last_started + k) % rt->slots;
            if (state(&rt->slot[i]) == NPU_RUNTIME_LOADING) rt->loading = i;
        }
        if (rt->loading == NPU_RUNTIME_NONE) return 0;
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
        {   /* The graph must fit this slot's regions (they are sized by one graph when the slots are). */
            struct sizes fit = {0};
            need(plan, model->bytes, &fit);
            if (fit.command > s->command.bytes || fit.constant > s->constant.bytes || fit.read > s->read.bytes ||
                fit.write > s->write.bytes ||
                NBG_LAYOUT_ENTRIES(plan->inputs, plan->outputs) > s->layout.capacity) { load_end(rt, s, 0); return 1; }
        }
        targets(s, plan);
        if (memcmp(digest, rt->model_digest[s->model], 32) ||
            nbg_model_open((const void *)rt->staging, model->bytes, &rt->view) != NBG_DECODE_OK ||
            !nbg_assembly_begin(&rt->view, s->targets, nbg_layout_buffers(plan),
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
            if (s->targets[i].bytes) memset((unsigned char *)s->targets[i].cpu + payload, 0, s->targets[i].bytes - payload);
        }
        if (s->layout.dynamic) {
            /* The whole allocation: the gap before the program section may hold an earlier graph's bytes. */
            const unsigned d = nbg_layout_dynamic_index(&s->layout);
            memset((unsigned char *)s->targets[d].cpu, 0, s->targets[d].bytes);
        }
        next_phase(s, LOAD_COEFF);
        return 1;
    }
    case LOAD_COEFF: {
        /* The coefficients, the dynamic-input section and the program section, copied in order with one cursor. */
        struct nbg_span coeff = {0}, dyna = {0}, program = {0};
        const uint32_t coeff_bytes = s->layout.buffers[NBG_ALLOC_COEFF].payload_bytes,
            program_bytes = s->layout.program_bytes;
        struct nbg_span section = {0};
        const uint32_t dyna_section = nbg_model_section(&rt->view, NBG_DYNA_IO, &section) ? (uint32_t)section.bytes : 0;
        if ((coeff_bytes && (!nbg_model_section(&rt->view, NBG_COEFF, &coeff) || coeff.bytes != coeff_bytes)) ||
            (dyna_section && !nbg_model_section(&rt->view, NBG_DYNA_IO, &dyna)) ||
            (program_bytes && (!nbg_model_section(&rt->view, NBG_PPU_INSTR, &program) || program.bytes != program_bytes))) {
            load_end(rt, s, 0); return 1;
        }
        const uint32_t total = coeff_bytes + dyna_section + program_bytes;
        size_t n = total - s->offset;
        if (n > NPU_RUNTIME_CACHE_CHUNK) n = NPU_RUNTIME_CACHE_CHUNK;
        const unsigned d = nbg_layout_dynamic_index(&s->layout);
        if (!n) {
            /* Nothing remains to copy. */
        } else if (s->offset < coeff_bytes) {
            if (n > coeff_bytes - s->offset) n = coeff_bytes - s->offset;
            memcpy((unsigned char *)s->targets[NBG_ALLOC_COEFF].cpu + s->offset, coeff.data + s->offset, n);
        } else if (s->offset < coeff_bytes + dyna_section) {
            const uint32_t at = s->offset - coeff_bytes;
            if (n > dyna_section - at) n = dyna_section - at;
            memcpy((unsigned char *)s->targets[d].cpu + at, dyna.data + at, n);
        } else {
            const uint32_t at = s->offset - coeff_bytes - dyna_section;
            memcpy((unsigned char *)s->targets[d].cpu + s->layout.program_offset + at, program.data + at, n);
        }
        rt->counters.copied_bytes += n;
        s->offset += n;
        if (s->offset >= total) { next_phase(s, LOAD_STAGED); s->crc = UINT32_MAX; }
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
        const unsigned d = nbg_layout_dynamic_index(&s->layout);
        struct npu_runtime_span spans[4] = {
            {(uintptr_t)s->targets[NBG_ALLOC_NN].cpu, s->targets[NBG_ALLOC_NN].bytes},
            {(uintptr_t)s->targets[NBG_ALLOC_COEFF].cpu, s->targets[NBG_ALLOC_COEFF].bytes},
            {s->command.cpu, lines(extent)},
            {s->layout.dynamic ? (uintptr_t)s->targets[d].cpu : 0, s->layout.dynamic ? s->targets[d].bytes : 0}};
        if (walk(rt, s, spans, 4, NPU_RUNTIME_CACHE_CHUNK, WALK_CLEAN) && good(rt)) load_end(rt, s, 1);
        return 1;
    }
    default:
        return fault(rt);
    }
}

/* P: clean the inputs; while the interim lasts, clean and invalidate the
 * output lines so a dirty client line cannot be evicted over NPU output;
 * then zero the working pool with DC ZVA and clean and invalidate it.
 * Output preparation must stay clean and invalidate: the OS NPU client
 * (apps/os/npu-client.c) writes a fill pattern to the outputs before SUBMIT
 * and detects a job that writes nothing by the bytes that keep it. An
 * invalidate alone would discard that pattern and expose stale memory.
 * checks/npu-session-runtime.c requires the whole output span flushed. */
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
    struct npu_runtime_span *const spans = rt->spans;
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
    rt->checking = NPU_RUNTIME_NONE;
    if (checked < 0) {
        /* Class 3: any task can write the allocation in the interim. */
        fault(rt); npu_service_retain(&rt->service);
        return 1;
    }
    set_state(s, NPU_RUNTIME_READY);
    const unsigned job_fault = s->faulted;
    s->faulted = 0;
    npu_service_release(&rt->service, index, job_fault ? NPU_SERVICE_JOB_FAULT : NPU_SERVICE_OK,
                        job_fault ? A733_NPU_FAULT : A733_NPU_DONE);
    return 1;
}

/* The first slot after the one started last whose state is in wanted. */
static unsigned candidate(const struct npu_runtime *rt, unsigned wanted)
{
    for (unsigned k = 1; k <= rt->slots; ++k) {
        unsigned i = (rt->last_started + k) % rt->slots;
        if (wanted & (1u << state(&rt->slot[i]))) return i;
    }
    return NPU_RUNTIME_NONE;
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
    int idle = c->state == A733_NPU_READY && rt->service.running == NPU_SERVICE_NONE;
    unsigned i;
    if (idle && (i = candidate(rt, SUBMITTED)) < rt->slots) {
        if (state(&rt->slot[i]) == NPU_RUNTIME_QUEUED) prepare_step(rt, &rt->slot[i]);
        else {
            start(rt, i);
            if (good(rt) && state(&rt->slot[i]) == NPU_RUNTIME_RUNNING) (void)load_step(rt);
        }
        return 1;
    }
    unsigned first = NPU_RUNTIME_NONE;
    for (i = 0; i < rt->slots; ++i)
        if (state(&rt->slot[i]) == NPU_RUNTIME_FINISHED &&
            (first == NPU_RUNTIME_NONE || rt->slot[i].finished < rt->slot[first].finished)) first = i;
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
    /* faulted 2: the job failed with an NPU MMU exception and the service recovers the NPU. The slot goes through the
     * same check of its read-only region as after a finished job, and its output is not used. */
    if ((faulted && faulted != 2) || client >= rt->slots || state(&rt->slot[client]) != NPU_RUNTIME_RUNNING) {
        fault(rt); return;
    }
    if (faulted == 2) { ++rt->counters.faulted_jobs; rt->slot[client].faulted = 1; }
    rt->slot[client].finished = ++rt->finishes;
    set_state(&rt->slot[client], NPU_RUNTIME_FINISHED);
}
static void closed(void *cookie, unsigned client)
{
    struct npu_runtime *rt = cookie;
    if (rt->resident && !client) {
        for (unsigned i = 0; i < rt->slots; ++i) set_state(&rt->slot[i], NPU_RUNTIME_CLOSED);
    } else if (client < rt->slots) set_state(&rt->slot[client], NPU_RUNTIME_CLOSED);
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
/* clients[i] is slot i's task; a resident runtime gives every slot the one task, passed as owner with clients NULL. */
static int bind(struct npu_runtime *rt, struct a733_npu_startup *startup, const TaskHandle_t clients[], TaskHandle_t owner,
                UBaseType_t priority, UBaseType_t affinity, unsigned resident)
{
    if (!good(rt) || rt->bound || (rt->tensor_links && !resident) ||
        !startup || !startup->initialized || !startup->task || (resident ? !owner : !clients) ||
        private_overlap(rt, (uintptr_t)startup, sizeof(*startup)) ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
        return 0;
    for (unsigned i = 0; i < rt->slots; ++i) {
        const TaskHandle_t task = resident ? owner : clients[i];
        if (!task || task == startup->task) return 0;
        for (unsigned j = 0; !resident && j < i; ++j) if (clients[j] == clients[i]) return 0;
    }
    rt->startup = startup;
    const struct npu_service_activation activation = {startup->task, prepared, rt};
    static const struct npu_service_runtime hooks = {request, step, finished, closed, 1};
    if (!npu_service_init_deferred(&rt->service, &operations, rt, &rt->native.memory.image, rt->commands,
            rt->slots + 1, priority, affinity, &activation) || !npu_service_share(&rt->service) ||
        !npu_service_runtime(&rt->service, &hooks, rt))
        return fault(rt);
    for (unsigned i = 0; i < rt->slots; ++i) {
        const TaskHandle_t task = resident ? owner : clients[i];
        rt->slot[i].client = task;
        /* The resident owner may submit every slot's command; any other client only its own (command i + 1). */
        if ((!resident || !i) &&
            !(resident ? npu_service_bind_range(&rt->service, i, task, 1, rt->slots) :
                         npu_service_bind_range(&rt->service, i, task, i + 1, 1))) return fault(rt);
    }
    rt->resident = resident; rt->bound = 1;
    return 1;
}
int npu_runtime_bind(struct npu_runtime *rt, struct a733_npu_startup *startup, const TaskHandle_t clients[],
                     UBaseType_t priority, UBaseType_t affinity)
{ return bind(rt, startup, clients, NULL, priority, affinity, 0); }
int npu_runtime_bind_resident(struct npu_runtime *rt, struct a733_npu_startup *startup, TaskHandle_t task,
                              UBaseType_t priority, UBaseType_t affinity)
{
    if (!rt || !rt->slots) return 0;
    return bind(rt, startup, NULL, task, priority, affinity, 1);
}
int npu_runtime_preload(struct npu_runtime *rt, unsigned slot, unsigned model)
{
    if (!good(rt) || rt->bound || slot >= rt->slots || model >= rt->count ||
        state(&rt->slot[slot]) != NPU_RUNTIME_EMPTY ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) return 0;
    struct npu_runtime_slot *s = &rt->slot[slot];
    s->model = model; set_state(s, NPU_RUNTIME_LOADING);
    while (state(s) == NPU_RUNTIME_LOADING && good(rt)) (void)load_work(rt);
    if (state(s) != NPU_RUNTIME_READY || !good(rt)) return 0;
    /* Workspace and padding initialize once, before any device can use them. */
    memset((void *)s->read.cpu, 0, s->read.bytes);
    memset((void *)s->write.cpu, 0, s->write.bytes);
    return maintain(rt, AARCH64_CACHE_CLEAN_INVALIDATE, s->read.cpu, s->read.bytes) &&
        maintain(rt, AARCH64_CACHE_CLEAN_INVALIDATE, s->write.cpu, s->write.bytes);
}
/* Load the graph of a slot after the scheduler has started, for a resident client (a queue image keeps one graph in each
 * slot). The owner marks the slot LOADING like a LOAD request and wakes the service worker, which stages, hashes and
 * assembles it in its steps, as for any LOAD; the owner waits and then initializes the slot's windows as preload does. A
 * graph whose copy does not hash to its declared digest leaves the slot EMPTY and returns 0. This keeps the megabytes of
 * hashing and copying out of the start-up that the secondary cores wait for (one second). */
int npu_runtime_load_resident(struct npu_runtime *rt, unsigned slot, unsigned model)
{
    if (!good(rt) || !rt->bound || !rt->resident || slot >= rt->slots || model >= atomic_load_explicit(&rt->count, memory_order_acquire) ||
        xTaskGetSchedulerState() != taskSCHEDULER_RUNNING || xTaskGetCurrentTaskHandle() != rt->slot[0].client) return -1;
    /* The NPU start-up is not finished, or a direct queue owns the runtime: not yet. */
    if (rt->service.direct || rt->service.direct_request || rt->service.has_deferred ||
        rt->service.controller.state != A733_NPU_READY) return 0;
    struct npu_runtime_slot *s = &rt->slot[slot];
    if (state(s) == NPU_RUNTIME_READY) return s->model == model ? 1 : -1;
    if (state(s) != NPU_RUNTIME_EMPTY) return -1;
    s->model = model; s->resident_load = 1; set_state(s, NPU_RUNTIME_LOADING);
    for (unsigned waited = 0; state(s) == NPU_RUNTIME_LOADING && good(rt); ++waited) {
        if (waited > 20000) return -1;          /* a graph takes about 15 ms; 20 s is a stuck worker */
        xTaskNotifyGive(rt->service.worker);
        vTaskDelay(1);
    }
    s->resident_load = 0;
    if (state(s) != NPU_RUNTIME_READY || !good(rt)) return -1;
    /* Workspace and padding initialize once, before any device can use them. */
    memset((void *)s->read.cpu, 0, s->read.bytes);
    memset((void *)s->write.cpu, 0, s->write.bytes);
    return maintain(rt, AARCH64_CACHE_CLEAN_INVALIDATE, s->read.cpu, s->read.bytes) &&
        maintain(rt, AARCH64_CACHE_CLEAN_INVALIDATE, s->write.cpu, s->write.bytes) ? 1 : -1;
}
/* Copy a layout into a table of its own: the destination keeps its buffers and capacity. */
static void copy_layout(struct nbg_assembly_layout *to, const struct nbg_assembly_layout *from)
{
    struct nbg_allocation_requirement *const table = to->buffers;
    const uint32_t capacity = to->capacity;
    memmove(table, from->buffers, (size_t)nbg_layout_buffers(from) * sizeof(*table));
    *to = *from;
    to->buffers = table; to->capacity = capacity;
}
int npu_runtime_register(struct npu_runtime *rt, const void *model, size_t bytes, unsigned *index)
{
    if (!good(rt) || !rt->prepared || !index || !model || !bytes || bytes > NBG_MODEL_MAX_BYTES ||
        !a733_span_incl_nonempty(model, bytes) || private_overlap(rt, (uintptr_t)model, bytes)) return 0;
    const unsigned n = atomic_load_explicit(&rt->count, memory_order_acquire);
    if (n >= rt->capacity || pages(bytes) > rt->staging_bytes) return 0;
    struct nbg_model view;
    if (nbg_model_open(model, bytes, &view) != NBG_DECODE_OK) return 0;
    /* The graph's layout needs as many entries as its tensors; the reservation holds the declared bound (rt->later_entries). */
    const size_t entries = nbg_assembly_layout_entries(&view);
    if (!entries || entries > rt->later_entries) return 0;
    struct nbg_assembly_layout plan = {.buffers = rt->scratch, .capacity = rt->later_entries};
    unsigned char digest[32];
    struct sizes z = {0};
    if (!admit(model, bytes, &view, digest, &plan)) return 0;
    need(&plan, bytes, &z);
    if (z.command > rt->command_bytes || z.constant > rt->constant_bytes || z.read > rt->read_bytes || z.write > rt->write_bytes) return 0;
    rt->catalogue[n] = (struct npu_runtime_model){model, bytes};
    copy_layout(&rt->plans[n], &plan);
    memcpy(rt->model_digest[n], digest, 32);
    atomic_store_explicit(&rt->count, n + 1, memory_order_release);
    *index = n;
    return 1;
}
static int link_checked(struct npu_runtime *rt, unsigned producer, unsigned output, unsigned consumer, unsigned input)
{
    if (producer >= rt->slots || consumer >= rt->slots || producer == consumer) return 0;
    struct npu_runtime_slot *p = &rt->slot[producer], *s = &rt->slot[consumer];
    if (state(p) != NPU_RUNTIME_READY || state(s) != NPU_RUNTIME_READY ||
        output >= p->layout.outputs || input >= s->layout.inputs) return 0;
    const unsigned pi = p->layout.inputs + output;
    const struct nbg_tensor *from = &p->tensors[pi].tensor, *to = &s->tensors[input].tensor;
    /* Names are local metadata. The complete tensor representation preceding
     * them, including dimensions and quantization, must agree at this edge. */
    if (memcmp(from, to, offsetof(struct nbg_tensor, name)) ||
        p->targets[NBG_ALLOC_INPUT + pi].bytes != s->targets[NBG_ALLOC_INPUT + input].bytes) return 0;
    /* The consumer's targets and layout are re-assembled in the runtime's scratch first (one slot's entries of each). */
    const size_t entries = s->layout.capacity;
    struct nbg_assembly_buffer *const targets_copy = rt->scratch;
    struct nbg_assembly_layout layout = {.buffers = (struct nbg_allocation_requirement *)(targets_copy + entries), .capacity = (uint32_t)entries};
    if (entries * (sizeof(*targets_copy) + sizeof(*layout.buffers)) > rt->scratch_bytes) return 0;
    memcpy(targets_copy, s->targets, entries * sizeof(*targets_copy));
    targets_copy[NBG_ALLOC_INPUT + input] = p->targets[NBG_ALLOC_INPUT + pi];
    const struct npu_runtime_model *model = &rt->catalogue[s->model];
    struct nbg_model view;
    struct nbg_assembly_cursor cursor;
    /* begin verifies every target placement before any descriptor is changed.
     * Both slots' backing allocations already belong to this runtime's MMU. */
    if (nbg_model_open(model->data, model->bytes, &view) != NBG_DECODE_OK ||
        !nbg_assembly_begin(&view, targets_copy, nbg_layout_buffers(&s->layout),
                            &layout, &cursor) || !same_plan(&layout, &s->layout)) return 0;
    int result;
    do {
        result = nbg_assembly_next(&view, targets_copy, &layout, &cursor, NPU_RUNTIME_ASSEMBLY_STEP);
    } while (!result);
    if (result < 0) return fault(rt); /* Partial assembly can never be submitted. */
    memcpy(s->targets, targets_copy, entries * sizeof(*targets_copy));
    rt->tensor_links = 1;
    s->tensors[input].data = targets_copy[NBG_ALLOC_INPUT + input].cpu;
    copy_layout(&s->layout, &layout); s->stream_bytes = cursor.offset; s->end_mark = cursor.offset;
    fill_end(s->command.cpu + cursor.offset, s->command.bytes - cursor.offset);
    s->span = 0; s->offset = 0; s->crc = UINT32_MAX;
    while (!npu_runtime_reference_step(rt, s)) { }
    return maintain(rt, AARCH64_CACHE_CLEAN, s->constant.cpu, s->constant.bytes) &&
        maintain(rt, AARCH64_CACHE_CLEAN, s->command.cpu, s->command.bytes);
}
int npu_runtime_link_tensor(struct npu_runtime *rt, unsigned producer, unsigned output,
                            unsigned consumer, unsigned input)
{
    if (!good(rt) || rt->bound || xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) return 0;
    return link_checked(rt, producer, output, consumer, input);
}
/* Linking after the scheduler has started. The same re-assembly as before the start, allowed only while nothing can use
 * the slots: the runtime is bound for a resident client, the caller is its client task, the service is open and has no
 * request, no direct queue owner and no deferred work, the controller is idle, and every slot is loaded and idle. Those are
 * the conditions npu_runtime_queue_claim needs next, so a queue open links first and claims after. */
int npu_runtime_link_tensor_running(struct npu_runtime *rt, unsigned producer, unsigned output,
                                    unsigned consumer, unsigned input)
{
    if (!good(rt) || !rt->bound || !rt->resident || !rt->service.runtime || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING ||
        xTaskGetCurrentTaskHandle() != rt->slot[0].client || !rt->slot[0].opened || rt->slot[0].closed ||
        rt->slot[0].pending || rt->service.direct || rt->service.direct_request || rt->service.has_deferred ||
        rt->service.controller.state != A733_NPU_READY) return 0;
    for (unsigned i = 0; i < rt->slots; ++i)
        if (rt->slot[i].pending || state(&rt->slot[i]) != NPU_RUNTIME_READY) return 0;
    return link_checked(rt, producer, output, consumer, input);
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
    uint64_t level = npu_arch_current_el();
    if (level != 4) return A733_NPU_PLATFORM_RETAINED;
    if (stop)
        for (unsigned i = 0; i < rt->slots; ++i)
            if (state(&rt->slot[i]) != NPU_RUNTIME_CLOSED) return A733_NPU_PLATFORM_RETAINED;
    /* The bootstrap allocation and the NPU tables are checked once, at
     * activation (prepared); the startup and stop tables write only clock and
     * reset words. */
    if (!good(rt)) {
        fault(rt); return A733_NPU_PLATFORM_RETAINED;
    }
    /* A collected CLOSE reply can precede the worker's last runtime step.
     * Preserve the live startup window until its explicit parked release. */
    if(stop&&!npu_service_runtime_parked(&rt->service))return A733_NPU_PLATFORM_PENDING;
    if(stop&&!npu_service_irq_stopped(&rt->service)){
        fault(rt);return A733_NPU_PLATFORM_RETAINED;
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
    if (!s || (rt->resident && slot) || rt->service.direct || s->closed || s->pending || !ticket || wait == portMAX_DELAY ||
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
        if (!s->opened || rt->tensor_links || model >= rt->count || timeout) return 0;
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
    if (reply.result == NPU_SERVICE_JOB_FAULT && reply.fault == A733_NPU_MMU) { /* the job only */ }
    else if (reply.result == NPU_SERVICE_FAULT || reply.fault != A733_NPU_NO_FAULT) fault(rt);
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
    if (rt->service.direct || s->pending || state(s) != NPU_RUNTIME_READY ||
        index >= (section == NBG_INPUT ? s->layout.inputs : s->layout.outputs) ||
        private_overlap(rt, (uintptr_t)out, sizeof(*out))) return 0;
    *out = s->tensors[(section == NBG_INPUT ? 0 : s->layout.inputs) + index];
    return 1;
}

size_t npu_runtime_trim_parent(struct npu_runtime *rt)
{
    if (!rt || !rt->prepared || !rt->placed_bytes || rt->placed_bytes > rt->parent_bytes) return 0;
    rt->parent_bytes = rt->placed_bytes;
    return rt->parent_bytes;
}
