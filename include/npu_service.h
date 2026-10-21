#ifndef SUNSTONE_NPU_SERVICE_H
#define SUNSTONE_NPU_SERVICE_H
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "a733_npu_controller.h"
#include <stdatomic.h>

#define NPU_SERVICE_CLIENTS 8u
#define NPU_SERVICE_COMMANDS 32u
#define NPU_SERVICE_STACK_WORDS 4096u

enum npu_service_operation {
    NPU_SERVICE_STATUS, NPU_SERVICE_OPEN, NPU_SERVICE_SUBMIT,
    NPU_SERVICE_CANCEL, NPU_SERVICE_REAP, NPU_SERVICE_CLOSE
};
enum npu_service_result {
    NPU_SERVICE_OK, NPU_SERVICE_INVALID, NPU_SERVICE_DENIED,
    NPU_SERVICE_BUSY, NPU_SERVICE_FAULT
};
struct npu_service_request {
    enum npu_service_operation operation;
    unsigned command; /* Index in the private startup command table; zero is bootstrap. */
    uint64_t timeout_us; /* OPEN/SUBMIT/CANCEL only; zero for other operations. */
};
struct npu_service_reply {
    uint64_t ticket, job_ticket;
    enum npu_service_result result;
    enum a733_npu_state state;
    enum a733_npu_fault fault;
    unsigned owns_session, canceled;
};
struct npu_service_message {
    struct npu_service_request request;
    uint64_t ticket;
    unsigned client;
};
struct npu_service_client {
    TaskHandle_t task;
    uint32_t commands;
    uint64_t next_ticket, pending;
    QueueHandle_t replies;
    StaticQueue_t reply_state;
    uint8_t reply_storage[sizeof(struct npu_service_reply)];
};
/* Copied private startup authority. prepared performs bounded metadata checks
 * only: 1 ready, 0 pending, -1 retained. Authenticate the entire backend, MMU
 * image and registered command descriptors against independent startup state.
 * Neither this callback nor activation validates command-stream semantics. */
struct npu_service_activation {
    TaskHandle_t bootstrap;
    int (*prepared)(void *,const struct a733_npu_ops *,void *,
        const struct a733_npu_mmu_image *,const struct a733_npu_command *,unsigned);
    void *context;
};
/* Private static storage. Public only so the platform can reserve it without
 * a heap. Application code must not inspect or modify these fields. */
/* Optional single-owner IRQ binding. Callbacks run only in the worker and
 * return 1 complete, 0 deferred, -1 retained failure. mask proves disable has
 * propagated and no handler remains active. rearm never clears pending and is
 * invoked inside the kernel critical section after checking latched events.
 * stop follows successful controller CLOSE (events disabled/full idle), and
 * proves the owned GIC source disabled, inactive and pending clear. */
struct npu_service_irq_ops {
    int (*mask)(void *);
    int (*rearm)(void *);
    int (*stop)(void *);
};
struct npu_service {
    struct a733_npu_controller controller;
    struct a733_npu_ops ops;
    void *cookie;
    struct npu_service_irq_ops irq_ops;
    void *irq_cookie;
    atomic_uint irq_pending, irq_stopped, irq_completions;
    unsigned irq_bound, irq_masked, irq_rearming, irq_retained;
    struct npu_service_message deferred;
    unsigned has_deferred;
    struct a733_npu_mmu_image image;
    struct a733_npu_command commands[NPU_SERVICE_COMMANDS];
    unsigned command_count, owner, initialized;
    atomic_uint activation_state;
    struct npu_service_activation activation;
    uint64_t job_ticket;
    QueueHandle_t requests;
    StaticQueue_t request_state;
    uint8_t request_storage[NPU_SERVICE_CLIENTS * sizeof(struct npu_service_message)];
    struct npu_service_client clients[NPU_SERVICE_CLIENTS];
    TaskHandle_t worker;
    UBaseType_t affinity;
    StaticTask_t worker_state;
    StackType_t worker_stack[NPU_SERVICE_STACK_WORDS];
};

/* One statically configured service for the single NPU. Call init/bind before
 * starting the scheduler, at EL1. Zero storage once. This service exclusively
 * owns its controller/backend; callbacks are never called by application tasks.
 * Backend setup must have established reserved memory, trusted command streams,
 * platform resources, masked NPU IRQ, and independent recovery before OPEN.
 * commands[0] is bootstrap; entries 1..count-1 are trusted application commands.
 * Tables/tokens and backend state remain pinned for the service's lifetime.
 *
 * The worker runs at EL1 using its task stack and remains preemptible. On the
 * SMP port this requires patches/freertos-smp-privileged.patch. Ordinary tasks
 * stay at EL0. The current shared mappings provide no malicious-task isolation.
 * Client task handles must remain live and cannot be deleted/reused. */
int npu_service_init(struct npu_service *, const struct a733_npu_ops *, void *cookie,
                      const struct a733_npu_mmu_image *, const struct a733_npu_command *,
                      unsigned count, UBaseType_t priority, UBaseType_t affinity);
/* Optional deferred initialization. Init/bind remain pre-scheduler; hardware
 * readiness is deferred to one designated privileged bootstrap task. No worker
 * callback, including the time provider, or client send occurs before activation.
 * Memory reservations and trusted command admission remain init prerequisites.
 * The existing init API starts active and preserves its previous behavior. */
int npu_service_init_deferred(struct npu_service *,const struct a733_npu_ops *,void *,
    const struct a733_npu_mmu_image *,const struct a733_npu_command *,unsigned,
    UBaseType_t priority,UBaseType_t affinity,const struct npu_service_activation *);
/* Task identity precedes EL1 validation. One atomic claim serializes prepared
 * callbacks; successful release publication precedes worker notification.
 * BUSY means pending/already active; FAULT permanently retains inactive state.
 * Unauthorized and duplicate calls cannot invoke prepared or change readiness.
 * Bootstrap task, callback context and prepared resources remain live/pinned. */
enum npu_service_result npu_service_activate(struct npu_service *);
int npu_service_bind(struct npu_service *, unsigned slot, TaskHandle_t, uint32_t command_mask);

/* Optional private binding after init, before scheduling. The route/cookie and
 * worker remain pinned. ISR entry follows root-owned mask of INTID97; it never
 * reads NPU registers. mask_ok failure is sticky. Pre-binding IRQ dispatch must
 * be retained by the platform. route is the affinity mask of the one CPU the
 * interrupt is routed to; binding is refused unless the worker was created
 * pinned to exactly that CPU, because the callbacks run in the worker and the
 * route owner must mask, rearm and stop the source on its own CPU. The worker
 * affinity must not change after binding. */
int npu_service_bind_irq(struct npu_service *,const struct npu_service_irq_ops *,void *,
    UBaseType_t route);
void npu_service_irq_notify_from_isr(struct npu_service *,int mask_ok,BaseType_t *woken);
int npu_service_irq_stopped(const struct npu_service *);
unsigned npu_service_irq_completions(const struct npu_service *);

/* Task-only calls. Caller identity comes from FreeRTOS, never from the request.
 * send copies its request; success means queued, not accepted by the controller.
 * A client has one pending reply. collect timeout preserves that pending ticket;
 * collect it before sending again. Waits must be finite (portMAX_DELAY refused).
 * Output arguments are unchanged on refusal/timeout. Queues own all message data,
 * so a timed-out caller's stack is never retained by the worker.
 *
 * OPEN claims the session; only its owner may submit, cancel, reap, or close.
 * STATUS is diagnostic for any registered task. Active polling waits at most
 * one tick for requests; scheduling/callback latency can extend that interval.
 * Idle sessions sample the counter at least once per one-second queue wait.
 * No client request is needed to advance completion/deadlines. Fault retains
 * session ownership/resources. REAP consumes one result; only successful CLOSE
 * releases the session. Task abandonment does not release pinned resources. */
int npu_service_send(struct npu_service *, const struct npu_service_request *,
                      TickType_t wait, uint64_t *ticket);
int npu_service_collect(struct npu_service *, uint64_t ticket,
                         struct npu_service_reply *, TickType_t wait);

/* Startup-only port hook. Grants EL1 only to this service's internal worker
 * while init is constructing that task. No application-selectable privilege bit. */
BaseType_t npu_service_task_privileged(TaskFunction_t, void *);
#endif
