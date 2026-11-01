#ifndef SUNSTONE_NPU_QUEUE_H
#define SUNSTONE_NPU_QUEUE_H
#include "a733_npu_controller.h"
#include <stdatomic.h>

/* Trusted, resident command queue for the single NPU. No model identities or
 * network partition policy enter this interface. The platform opens the
 * controller and validates/maps commands and buffers before queue_init.
 *
 * Submit and release serialize with irq through the platform's device lock.
 * Submission runs in the caller; irq runs in INTID97 and completes at most
 * one record. Callbacks are bounded, allocation-free and never block. */
struct npu_queue;
struct npu_queue_command {
    struct a733_npu_command hardware;
    struct npu_queue *owner;
    size_t users; /* Includes completed records until explicit release. */
};
struct npu_queue_fence { const struct npu_queue *queue; uint64_t value; };
struct npu_queue_job {
    struct npu_queue_job *next;
    struct npu_queue_command *command;
    struct npu_queue_fence fence;
    uint64_t timeout_us;
    unsigned held;
};
struct npu_queue_ops {
    /* validate runs once at registration, never from submit/irq. */
    int (*validate)(void *, const struct a733_npu_command *);
    /* start publishes only a resident command address and fetch size. */
    int (*start)(void *, const struct a733_npu_command *, uint64_t timeout_us);
    /* complete acknowledges one IRQ and proves event AND full idle. 1 is
     * success; anything else retains the queue and every resource. */
    int (*complete)(void *);
    /* Direct notification of the waiting task from the ISR. No service hop.
     * Failure wakes the same task; it observes failed in fence_status. */
    void (*notify_from_isr)(void *);
};
struct npu_queue {
    struct npu_queue_ops ops;
    void *context;
    struct npu_queue_job *head, *tail;
    uint64_t submitted;
    atomic_uint_fast64_t completed;
    atomic_uint failed;
    size_t held;
    unsigned initialized, failure_site;
    uint64_t failure_fence; /* First start/IRQ failure, retained with all resources. */
};
/* Caller owns the records; keep queue, commands, jobs and backing resources
 * pinned until release. Zero each object before its first use. Job records
 * may be reused only after release, command records after queue retirement.
 * There is no driver-imposed job count. Caller storage bounds admission. */
int npu_queue_init(struct npu_queue *, const struct npu_queue_ops *, void *);
/* Slow registration precedes the first submit, with IRQs enabled. An IRQ
 * during validation can only retain the empty queue; success rechecks health
 * before publishing a command. Only the owning task registers commands. */
int npu_queue_register(struct npu_queue *, struct npu_queue_command *,
                       const struct a733_npu_command *);
/* after is either {0,0} or an already-issued fence from this queue. FIFO
 * execution satisfies every such dependency, including a released job's
 * fence; future and foreign fences are refused before touching hardware.
 * Returns 1 accepted, 0 refused, -1 retained. A failed start is accepted and
 * retained: the job and buffers must remain pinned. Output is unchanged on
 * refusal. Several jobs may share an immutable command and input buffers;
 * CPU access to any shared device-write buffer waits for its last user. */
int npu_queue_submit(struct npu_queue *, struct npu_queue_job *,
                     struct npu_queue_command *, struct npu_queue_fence after,
                     uint64_t timeout_us, struct npu_queue_fence *out);
void npu_queue_irq(struct npu_queue *);
/* Acquire loads order device completion before output cache invalidation.
 * 0 pending, 1 event/full-idle complete, -1 invalid/retained. A timeout or
 * failure never proves drain and never releases resources. */
int npu_queue_fence_status(const struct npu_queue *, struct npu_queue_fence);
/* After successful fence_status AND output acquire/cache invalidation.
 * release changes no hardware and permits reuse of this job record. */
int npu_queue_release(struct npu_queue *, struct npu_queue_job *);
int npu_queue_idle(const struct npu_queue *);
#endif
