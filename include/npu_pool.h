#ifndef SUNSTONE_NPU_POOL_H
#define SUNSTONE_NPU_POOL_H
#include <stddef.h>
#include <stdint.h>

/* The one way the NPU service takes memory for the tables of a registration (its slot, client, permission, buffer, queue and
 * layout tables): an allocate and a release over the pool. Every table of the runtime, the service and the queue is carved
 * through these two calls, and nothing else in the NPU code addresses the pool's window, so a pool that is backed by another
 * allocator (a ledger shared with the CPU and the GPU) changes only the implementation behind this pair.
 *
 * allocate returns bytes of zeroed memory, 64-byte aligned, or NULL when the pool cannot hold it; the reason is then the
 * caller's to name (NPU_DECLARATION_MEMORY). release returns a block that allocate gave. The window implementation below
 * is a bump over a fixed range: it releases only the most recent blocks (in reverse order) and otherwise keeps the memory
 * until the pool is retired. A counting window (base NULL) answers non-NULL without memory, for a sizing call. */
struct npu_pool {
    void *(*allocate)(struct npu_pool *, size_t bytes);
    void (*release)(struct npu_pool *, void *block, size_t bytes);
};
static inline void *npu_pool_allocate(struct npu_pool *p, size_t bytes) { return p->allocate(p, bytes); }
static inline void npu_pool_release(struct npu_pool *p, void *block, size_t bytes) { p->release(p, block, bytes); }

struct npu_pool_window { struct npu_pool pool; uint8_t *base; size_t used, limit; };
/* A window over [base, base + bytes); base NULL counts without memory (bytes then bounds the total). */
void npu_pool_window_init(struct npu_pool_window *, void *base, size_t bytes);

/* The second pair: memory for the metadata that the EL1 code trusts (the service's clients, permission bitmaps, commands and
 * request storage, the runtime's slot, layout, target, tensor and buffer tables, the memory layer's tables, the adapter's
 * edge tables and command records). It must be memory that no EL0 mapping reaches and that the NPU's page tables do not map:
 * a task that can write it can grant itself another client's commands. The pool (above) holds what the device reads and
 * writes and what EL0 writes by design; nothing the EL1 checks trust may come from it. The OS supplies the implementation
 * (apps/os/npu.c: EL1 image memory sized from the declaration); the NPU code only calls this pair. */
struct npu_meta {
    void *(*allocate)(struct npu_meta *, size_t bytes);
    void (*release)(struct npu_meta *, void *block, size_t bytes);
};
static inline void *npu_meta_allocate(struct npu_meta *m, size_t bytes) { return m->allocate(m, bytes); }
static inline void npu_meta_release(struct npu_meta *m, void *block, size_t bytes) { m->release(m, block, bytes); }
/* A bump over a range the caller owns (base NULL counts without memory, as the pool's window does). */
struct npu_meta_window { struct npu_meta meta; uint8_t *base; size_t used, limit; };
void npu_meta_window_init(struct npu_meta_window *, void *base, size_t bytes);
#endif
