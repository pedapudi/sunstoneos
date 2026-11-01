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
    uint64_t last_use;
    unsigned device_owned;
};
struct ss_npu_queue_entry { unsigned handle; uint64_t after, timeout; };
struct ss_npu_queue {
    struct ss_npu_queue_backend backend;
    void *context;
    uintptr_t producer;
    unsigned depth, handles;
    atomic_uint state, active;
    atomic_uint_fast64_t published, completed, released, waiter;
    uint64_t consuming, job_limit;
    struct ss_npu_queue_handle handle[SS_NPU_QUEUE_MAX_HANDLES];
    struct ss_npu_queue_entry entry[SS_NPU_QUEUE_MAX_DEPTH];
};
/* Bind once before application startup. Zero queue storage once. */
int ss_npu_queue_service_bind(struct ss_npu_queue *, const struct ss_npu_queue_backend *, void *);
/* Called only by backend.open before ready is published. Spans cover the
 * complete private DMA allocation sets (including device scratch/padding).
 * Tensor windows must lie inside them and be EL0 read/write WB mappings.
 * Admission proves cache stride, mapping and exclusive 64-byte ownership. */
int ss_npu_queue_service_handle(struct ss_npu_queue *,
    const struct ss_npu_tensor *, unsigned, const struct ss_npu_tensor *, unsigned,
    struct ss_npu_cache_span, struct ss_npu_cache_span);
int ss_npu_queue_service_step(struct ss_npu_queue *);
/* Task SVC entry or IRQ hook, serialized with local IRQ masking. completed
 * means the direct driver has already processed this one hardware completion.
 * At most one finish and one new start occur per call. */
int ss_npu_queue_service_progress(struct ss_npu_queue *, int completed, int in_isr);
void ss_npu_queue_service_fail(struct ss_npu_queue *, int in_isr);
#endif
