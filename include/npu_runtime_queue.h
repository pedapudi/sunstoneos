#ifndef SUNSTONE_NPU_RUNTIME_QUEUE_H
#define SUNSTONE_NPU_RUNTIME_QUEUE_H
#include "npu_runtime.h"
#include "npu_queue.h"
#include "npu_idle.h"

struct npu_runtime_queue_power_config {
    const struct npu_idle_part *part; /* register protocol and timing of the NPU */
    unsigned enabled; /* 0: matching owner/policy control without gate writes. */
    unsigned strict;  /* 1: a resume over the part's budget retains the queue. */
    UBaseType_t priority;
    /* Service IRQ cookie; no RTOS critical wrapper. 1: neither pending nor
     * active and no distributor write in progress; 0: not now, retry later;
     * -1: invalid binding. */
    int (*quiet)(void *);
};
struct npu_runtime_queue_power_stats {
    uint64_t resume_requests, last_request_to_ready_us, max_request_to_ready_us, quiet_retries;
};

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
#ifdef SUNSTONE_NPU_QUEUE_TIMING
    uint64_t irq_cpu_ticks;
#endif
    uint32_t last_events, last_idle; /* First failed completion inputs survive retention. */
    BaseType_t *woken; /* Non-null only during its ISR. */
    /* Optional shared-ring bridge. It consumes bounded work after each IRQ
     * and owns completion notification when installed. Configure before claim. */
    void (*progress_from_isr)(void *, BaseType_t *);
    void *progress_cookie;
    unsigned claimed;
    struct npu_idle power;
    struct npu_runtime_queue_power_config power_config;
    struct npu_runtime_queue_power_stats power_stats;
    TaskHandle_t power_task;
    StaticTask_t power_task_state;
    StackType_t power_stack[1024];
    uint64_t power_requested_at;
    atomic_uint power_started, power_stopped;
    unsigned power_stop, power_resume, power_resume_irq;
};
int npu_runtime_queue_claim(struct npu_runtime_queue *, struct npu_runtime *);
/* Claim and bulk registration precede this optional call. The static EL1
 * power owner runs on the IRQ core above the producer's priority. Default
 * queues create no owner and remain on. The owner remains parked after return
 * because the shared deadline registry retains its task identity. */
int npu_runtime_queue_power_start(struct npu_runtime_queue *, const struct npu_runtime_queue_power_config *);
/* Admission precedes generic submit, under the queue raw IRQ lock. Pending
 * admission changes no job, command or fence. The power owner notifies readiness
 * once; callers block instead of retrying. wait_ready takes its own lock. */
int npu_runtime_queue_admit(struct npu_runtime_queue *);
/* After pending admission and raw unlock, wake the owner in ordinary task
 * context. This call needs no privileged system-register access and is safe
 * for the EL0 doorbell wrapper after its supervisor call has returned. */
void npu_runtime_queue_power_kick(struct npu_runtime_queue *);
int npu_runtime_queue_wait_ready(struct npu_runtime_queue *);
/* Admit and submit in one raw lock span, so the owner cannot gate between them.
 * 1 submitted; 0 the device is resuming and nothing changed (call wait_ready,
 * then submit again); -1 failure or refusal. */
int npu_runtime_queue_submit(struct npu_runtime_queue *, struct npu_queue_job *,
    struct npu_queue_command *, struct npu_queue_fence after, uint64_t timeout, struct npu_queue_fence *out);
BaseType_t npu_runtime_queue_task_privileged(TaskFunction_t, void *);
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
