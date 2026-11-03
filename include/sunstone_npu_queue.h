#ifndef SUNSTONE_APP_NPU_QUEUE_H
#define SUNSTONE_APP_NPU_QUEUE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The ring holds as many jobs as the application declares in flight (manifest npu.in_flight, docs/designs/npu-resources.md):
 * an image is built with SS_NPU_QUEUE_MAX_DEPTH set to it (cmake -DSUNSTONE_OS_NPU_QUEUE_DEPTH=N), and a session that
 * declares more is refused as image tables. 64 is the default of an image built without a declaration. */
#ifndef SS_NPU_QUEUE_MAX_DEPTH
#define SS_NPU_QUEUE_MAX_DEPTH 64u
#endif
/* The queue has a handle for every resident graph the application declares and as many tensors in a handle as its graph has:
 * their tables, the group bitmaps and the tables of edges are allocated when the queue opens, from the NPU pool and sized by
 * the declaration (docs/designs/npu-resources.md). Only the ring's depth is fixed (above): a full ring is backpressure. */
/* Optional immutable image composition. Prepared-entry and tensor indices
 * are zero based. Every declared output/input pair has identical shape and
 * representation. Connected handles share one buffer lease. The OS copies and admits this table before scheduling. */
struct ss_npu_queue_edge { uint32_t producer, output, consumer, input; };
struct ss_npu_queue_edges { const struct ss_npu_queue_edge *edges; size_t count; };
extern const struct ss_npu_queue_edges ss_npu_queue_edges;
struct ss_npu_queue;
typedef uint32_t ss_npu_handle;
struct ss_npu_fence { struct ss_npu_queue *queue; uint64_t value; };
struct ss_npu_tensor { void *data; size_t bytes; };
enum ss_npu_tensor_kind { SS_NPU_INPUT, SS_NPU_OUTPUT };
/* One trusted EL0 producer, pinned to the NPU IRQ core, owns one queue for
 * its entire lifetime. Open is a slow asynchronous request: repeat until 1
 * (ready), 0 (pending), or -1 (refused/retained). depth is fixed at open.
 * Prepared entries and their DMA allocations are admitted by the image. */
int ss_npu_queue_open(unsigned depth, struct ss_npu_queue **);
/* The same open, with tensor links chosen at run time instead of the image's constant table: after the scheduler has
 * started, the application passes up to its declared npu.links edges (manifest, docs/designs/npu-resources.md). The OS
 * copies the table once, checks every edge against the loaded graphs (same representation and size, one producer for each
 * consumer input) and links them before the ring becomes ready. A refused table refuses the open (-1). The table is read
 * only on the first call; later calls poll. */
int ss_npu_queue_open_linked(unsigned depth, const struct ss_npu_queue_edge *, size_t count, struct ss_npu_queue **);
/* Slow lookup of an image-prepared resident entry; handles remain immutable.
 * 1 success, 0 unknown index. No command stream or device address is exposed. */
int ss_npu_queue_register(struct ss_npu_queue *, unsigned prepared_entry, ss_npu_handle *);
int ss_npu_queue_tensor(struct ss_npu_queue *, ss_npu_handle,
                       enum ss_npu_tensor_kind, unsigned, struct ss_npu_tensor *);
/* Record CPU writes after acquisition. Initial tensor contents are all dirty;
 * later leases clean only marked ranges. Mark every written input/output byte
 * before submit. Calls merge ranges per tensor in constant time. A returned
 * tensor pointer alone does not mark it dirty. 1 marked, 0 invalid/owned. */
int ss_npu_queue_dirty(struct ss_npu_queue *, ss_npu_handle,
    enum ss_npu_tensor_kind, unsigned index, size_t offset, size_t bytes);
/* FIFO dependencies are zero or an already-published fence of this queue.
 * 1 accepted, 0 backpressure/invalid, -1 retained. No allocation or syscall
 * occurs while the consumer is active; idle-to-active publication rings one
 * SVC doorbell. A failed doorbell retains the published entry and buffers.
 * First submission transfers the connected handles' tensor allocation sets to
 * the device after cache handoff. Repeats use unchanged inputs. No CPU may
 * touch that allocation set until acquire succeeds for its last queued use. */
int ss_npu_queue_submit(struct ss_npu_queue *, ss_npu_handle, struct ss_npu_fence after,
                        uint64_t timeout_us, struct ss_npu_fence *);
/* 0 pending, 1 complete, -1 invalid/retained. Completion alone gives no CPU
 * buffer ownership. A fence remains queryable after its receipt is released. */
int ss_npu_queue_status(struct ss_npu_queue *, struct ss_npu_fence);
/* Acquire the handle's outputs after all its published uses complete. The
 * live receipt fence identifies a use of that handle; a later pending repeat refuses CPU
 * access. Completion is acquired before cache maintenance. 1 acquired,
 * 0 pending, -1 invalid/retained. Read outputs before submitting that handle
 * again. Input and output pointers are unavailable while device-owned. */
int ss_npu_queue_acquire(struct ss_npu_queue *, struct ss_npu_fence);
/* Acquire by resident handle after releasing or wrapping its receipt. This
 * waits for the current last use of every handle connected by image edges.
 * It never reads a recycled receipt. 1 acquired, 0 pending, -1 invalid/fault. */
int ss_npu_queue_acquire_handle(struct ss_npu_queue *, ss_npu_handle);
/* Release completed receipts in FIFO order. This returns ring capacity only;
 * it never gives the CPU access to buffers. A handle lease remains pinned
 * even when every receipt has been released. 1 released, 0 pending/invalid. */
int ss_npu_queue_release(struct ss_npu_queue *, struct ss_npu_fence);
/* Optional final-fence wait: a persistent direct IRQ notification, no service
 * hop or tick polling. Independent session watchdog bounds lost interrupts. */
int ss_npu_queue_wait(struct ss_npu_queue *, struct ss_npu_fence);
/* Release every receipt and acquire every used handle or connected group. Repeat until
 * 1 closed, 0 pending, -1 retained. A fault never authorizes resource release. */
int ss_npu_queue_close(struct ss_npu_queue *);
#ifdef __cplusplus
}
#endif
#endif
