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
struct ss_npu_queue_handle {
    struct ss_npu_tensor input[SS_NPU_QUEUE_MAX_TENSORS], output[SS_NPU_QUEUE_MAX_TENSORS];
    unsigned inputs, outputs;
    struct ss_npu_dirty_range dirty[2][SS_NPU_QUEUE_MAX_TENSORS];
    struct ss_npu_cache_span read, write;
    unsigned group;
    /* One-based producer handle for an aliased input; zero for own read. */
    struct { unsigned handle, output; } alias[SS_NPU_QUEUE_MAX_TENSORS];
};
/* A group is the set of handles that share tensors; its members are a bitmap with one bit for each handle. */
#define SS_NPU_QUEUE_MEMBER_WORDS ((SS_NPU_QUEUE_MAX_HANDLES + 31u) / 32u)
struct ss_npu_queue_group { uint32_t members[SS_NPU_QUEUE_MEMBER_WORDS]; unsigned device_owned; uint64_t last_use; };
static inline int ss_npu_queue_member(const struct ss_npu_queue_group *g, unsigned handle) { return g->members[handle / 32u] >> (handle % 32u) & 1u; }
struct ss_npu_queue_entry { unsigned handle; uint64_t after, timeout; };
struct ss_npu_queue {
    struct ss_npu_queue_backend backend;
    void *context;
    uintptr_t producer;
    unsigned depth, handles, edges_admitted;
    /* Edges the application asked for at open (ss_npu_queue_open_linked); the backend copies and checks them. */
    size_t requested_links;
    struct ss_npu_queue_edge requested[SS_NPU_QUEUE_MAX_HANDLES * SS_NPU_QUEUE_MAX_TENSORS];
    atomic_uint state, active;
    atomic_uint_fast64_t published, completed, released, waiter;
    uint64_t consuming, job_limit;
    struct ss_npu_queue_handle handle[SS_NPU_QUEUE_MAX_HANDLES];
    struct ss_npu_queue_group group[SS_NPU_QUEUE_MAX_HANDLES];
    struct ss_npu_queue_entry entry[SS_NPU_QUEUE_MAX_DEPTH];
};
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
