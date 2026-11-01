#ifndef SUNSTONE_NPU_RUNTIME_QUEUE_H
#define SUNSTONE_NPU_RUNTIME_QUEUE_H
#include "npu_runtime.h"
#include "npu_queue.h"

/* Optional trusted EL1 client of the resident runtime. The owner is pinned to
 * the interrupt route core. Claim only after OPEN, LOAD and their replies;
 * the service keeps power/lifecycle ownership but performs no job work while
 * claimed. The caller serializes submit/release with local interrupt masking.
 * No other task may mutate the runtime, mappings or registered resources. */
struct npu_runtime_queue {
    struct npu_queue queue;
    struct npu_runtime *runtime;
    struct npu_service_direct direct;
    TaskHandle_t task;
    uint64_t started, timeout_ticks, frequency, observed;
    uint64_t irq_cpu_ticks; /* Updated only with SUNSTONE_NPU_QUEUE_TIMING. */
    uint32_t last_events, last_idle; /* First failed completion inputs survive retention. */
    BaseType_t *woken; /* Non-null only during its ISR. */
    unsigned claimed;
};
int npu_runtime_queue_claim(struct npu_runtime_queue *, struct npu_runtime *);
/* Device lock for submit/register/release and fence inspection. It masks
 * IRQs locally, without taking the global scheduler lock. Same-core callers
 * cannot preempt the owner inside the lock. Keep cache work outside it. */
uint64_t npu_runtime_queue_lock(void);
void npu_runtime_queue_unlock(uint64_t);
/* Block on direct ISR notification and recheck the fence with acquire.
 * Independent host-armed WDT0 bounds a lost interrupt; no tick wait runs on
 * this path. A retained fault never authorizes CLOSE or resource release. */
int npu_runtime_queue_wait(struct npu_runtime_queue *, struct npu_queue_fence);
/* The caller performs dirty input/output clean+invalidate before submission,
 * and output invalidate after a successful fence. No whole-model, table or
 * workspace maintenance runs per job. npu_runtime_tensor refuses access while
 * claimed; retain tensor windows obtained before claim. All buffers stay
 * owned by this queue until its last fence completes and records release. */
/* After all jobs are released: mask the IRQ and restore the idle worker.
 * Returns 0 if interrupt deactivation is still pending; -1 retained. */
int npu_runtime_queue_return(struct npu_runtime_queue *);
#endif
