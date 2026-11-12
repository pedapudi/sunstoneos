#ifndef SUNSTONE_APP_NPU_QUEUE_SERVICE_H
#define SUNSTONE_APP_NPU_QUEUE_SERVICE_H
#include "sunstone_npu_queue.h"
#include <stdatomic.h>
/* Fixed trusted backend; descriptors contain handle IDs, never callbacks or
 * addresses. open/close run in the privileged owner task. start/finish run
 * masked on the route core, in either the doorbell or its bounded IRQ hook. */
struct ss_npu_queue_backend {
    uintptr_t (*identity)(void *);
    int (*on_core)(void *);
    void (*request)(void *);
    int (*open)(void *, unsigned);
    int (*close)(void *);
    int (*start)(void *, unsigned, uint64_t, uint64_t, uint64_t);
    int (*finish)(void *, uint64_t);
    void (*cache)(void *, uintptr_t, size_t);
    int (*doorbell)(void *);
    void (*notify)(void *, uintptr_t, int);
    void (*wait)(void *);
};
struct ss_npu_cache_span { uintptr_t cpu; size_t bytes; };
struct ss_npu_dirty_range { size_t first, end; };
/* The tables of one queue, in memory the OS gives it (ss_npu_queue_service_tables): sized by the handles, the tensors of
 * all of them and the links the application may ask for. A handle's tensors, dirty ranges and aliases are carved from the
 * tables as ss_npu_queue_service_handle adds it. */
struct ss_npu_queue_alias { unsigned handle, output; };
struct ss_npu_queue_handle {
    struct ss_npu_tensor *input, *output;
    unsigned inputs, outputs;
    struct ss_npu_dirty_range *dirty[2];
    struct ss_npu_cache_span read, write;
    unsigned group;
    /* One-based producer handle for an aliased input; zero for own read. */
    struct ss_npu_queue_alias *alias;
};
/* A group is the set of handles that share tensors; its members are a bitmap with one bit for each handle. */
struct ss_npu_queue_group { uint32_t *members; unsigned device_owned; uint64_t last_use; };
static inline unsigned ss_npu_queue_member_words(unsigned handles) { return (handles + 31u) / 32u; }
static inline int ss_npu_queue_member(const struct ss_npu_queue *q, const struct ss_npu_queue_group *g, unsigned handle)
{ (void)q; return g->members[handle / 32u] >> (handle % 32u) & 1u; }
struct ss_npu_queue_entry { unsigned handle; uint64_t after, timeout; };
struct ss_npu_queue {
    struct ss_npu_queue_backend backend;
    void *context;
    uintptr_t producer;
    unsigned depth, handles, edges_admitted;
    /* Edges the application asked for at open (ss_npu_queue_open_linked); the backend copies and checks them. */
    size_t requested_links;
    struct ss_npu_queue_edge *requested;
    size_t requested_capacity;               /* the links the declaration allows */
    atomic_uint state, active;
    atomic_uint_fast64_t published, completed, released, waiter;
    uint64_t consuming, job_limit;
    struct ss_npu_queue_handle *handle;      /* handle_capacity entries, in the tables */
    struct ss_npu_queue_group *group;
    unsigned handle_capacity, tensor_capacity, tensors_used, member_words;
    uint8_t *tables, *at; size_t table_bytes; /* the tables and the cursor of the next carve */
    struct ss_npu_queue_entry *entry;        /* entry_capacity entries, in the tables: one for each job declared in flight */
    unsigned entry_capacity;
    /* Completions whose event came before all NPU units were idle: how many, the counter ticks waited in all and at most, and
     * the idle-word bits seen busy. EL1 only adds to these; the application may clear them while no job is in flight. */
    struct ss_npu_queue_idle_wait { uint64_t waits, ticks, max_ticks; uint32_t busy_bits, reserved; } idle_wait;
};
/* The bytes of the tables for handles handles holding tensors tensors in all (inputs and outputs), links requested edges and a
 * ring of depth entries. */
static inline size_t ss_npu_queue_tables_bytes(unsigned handles, size_t tensors, size_t links, unsigned depth)
{
    const size_t words = ss_npu_queue_member_words(handles);
    return 64u + (size_t)handles * (sizeof(struct ss_npu_queue_handle) + sizeof(struct ss_npu_queue_group) + words * 4u + 32u) +
           tensors * (sizeof(struct ss_npu_tensor) + 2u * sizeof(struct ss_npu_dirty_range) + sizeof(struct ss_npu_queue_alias) + 24u) +
           links * sizeof(struct ss_npu_queue_edge) + (size_t)depth * sizeof(struct ss_npu_queue_entry) + 64u;
}
/* What the runtime reserves for the queue of slots slots holding tensors tensors in all, with image_links edges in the image's
 * constant table and links more the application may ask for. The ring's tables, which EL0 writes by design, come from the
 * pool (ss_npu_queue_tables_bytes). The adapter's edge table (image + requested), the private copy of the requested edges and
 * one hardware command record for each handle (at most 64 bytes: apps/os/npu-queue.c checks it) are trusted by EL1 and come
 * from the metadata allocator (ss_npu_queue_meta_bytes). */
static inline size_t ss_npu_queue_meta_bytes(unsigned slots, size_t image_links, size_t links)
{
    return (((image_links + links) * sizeof(struct ss_npu_queue_edge) + 63u) & ~(size_t)63) +
           ((links * sizeof(struct ss_npu_queue_edge) + 63u) & ~(size_t)63) + (size_t)slots * 64u;
}
/* After bind, before the ring opens: take memory for handles handles of up to tensors tensors in all, links requested edges
 * and a ring of depth entries. 0, leaving the queue unchanged, when the memory is too small (the open is refused and says so)
 * or the queue is not waiting to open. */
int ss_npu_queue_service_tables(struct ss_npu_queue *, unsigned handles, size_t tensors, size_t links, unsigned depth,
                                void *memory, size_t bytes);
/* Bind once before application startup. Zero queue storage once. */
int ss_npu_queue_service_bind(struct ss_npu_queue *, const struct ss_npu_queue_backend *, void *);
/* Called only by backend.open before ready is published. Spans cover the
 * complete private DMA allocation sets (including device scratch/padding).
 * Output windows must lie inside write. Inputs lie inside read or exactly
 * alias an output admitted by service_edges. All windows are EL0 RW WB.
 * Admission proves cache stride, mapping and exclusive 64-byte ownership. */
int ss_npu_queue_service_handle(struct ss_npu_queue *,
    const struct ss_npu_tensor *, unsigned, const struct ss_npu_tensor *, unsigned,
    struct ss_npu_cache_span, struct ss_npu_cache_span);
/* Final slow admission after every handle. Undeclared/partial aliases and
 * overlapping private allocation sets are refused. Connected handles share
 * one DMA lease and last-use fence; no graph traversal occurs in the IRQ. */
int ss_npu_queue_service_edges(struct ss_npu_queue *, const struct ss_npu_queue_edge *, size_t);
int ss_npu_queue_service_step(struct ss_npu_queue *);
/* Task SVC entry or IRQ hook, serialized with local IRQ masking. completed
 * means the direct driver has already processed this one hardware completion.
 * At most one finish and one new start occur per call. */
int ss_npu_queue_service_progress(struct ss_npu_queue *, int completed, int in_isr);
void ss_npu_queue_service_fail(struct ss_npu_queue *, int in_isr);
#endif
