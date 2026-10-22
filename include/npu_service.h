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
    NPU_SERVICE_CANCEL, NPU_SERVICE_REAP, NPU_SERVICE_CLOSE,
    NPU_SERVICE_WAIT, NPU_SERVICE_LOAD /* Runtime mode only. */
};
enum npu_service_result {
    NPU_SERVICE_OK, NPU_SERVICE_INVALID, NPU_SERVICE_DENIED,
    NPU_SERVICE_BUSY, NPU_SERVICE_FAULT
};
struct npu_service_request {
    enum npu_service_operation operation;
    /* SUBMIT: index in the private startup command table; zero is bootstrap.
     * LOAD: the runtime's catalogue index. Zero for other operations. */
    unsigned command;
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
/* Private per-client record in shared mode; one outstanding job each. */
struct npu_service_member {
    struct npu_service_message wait;
    uint64_t job_ticket, timeout_us;
    unsigned state, command, has_wait, refused;
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
/* Runtime mode callbacks, all called by the worker only. request receives a
 * member's LOAD or SUBMIT: return 1 to hold the reply until
 * npu_service_release, or 0 with *result for an immediate reply. step runs at
 * most one bounded step of the runtime's bulk work and returns 1 when it did
 * work, 0 when none remains. finished reports a completed job of client,
 * already reaped, whose reply stays held; with faulted set it reports a
 * controller fault, client being NPU_SERVICE_CLIENTS when no job ran.
 * closed reports a member's successful CLOSE. idle_hint enables the
 * idle-register completion hint while interrupt delivery is unqualified. */
struct npu_service_runtime {
    int (*request)(void *, unsigned client, const struct npu_service_request *,
                   enum npu_service_result *);
    int (*step)(void *);
    void (*finished)(void *, unsigned client, int faulted);
    void (*closed)(void *, unsigned client);
    unsigned idle_hint;
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
    struct npu_service_message held;
    unsigned has_held;
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
    unsigned shared, members, running, next;
    struct npu_service_member member[NPU_SERVICE_CLIENTS];
    struct npu_service_runtime runtime;
    void *runtime_context;
    unsigned runtime_mode, runtime_failed, idle_hints;
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
/* Optional shared mode, after init and before scheduling. Every bound client
 * then opens its own session on the one controller instead of claiming it
 * exclusively. The first OPEN boots the controller; later OPENs join it. Each
 * member has at most one job. SUBMIT queues it and replies at once; the worker
 * starts queued jobs one at a time in round-robin client order, beginning
 * after the client it served last, so every member with queued work runs once
 * before any member runs twice. On completion the worker finishes the job's
 * cache maintenance and frees the controller for the next member; the result
 * stays with its member until that member's REAP, so an unreaped result never
 * blocks other members. Replies report the member's own view: READY when it
 * has no job, RUNNING while its job is queued or executing, DONE or CANCELED
 * until REAP, BOOTING until the bootstrap completes. WAIT is held per member.
 * CANCEL removes only a job that has not started; a started job is bounded by
 * its deadline, and a controller fault retains the shared session for every
 * member. CLOSE leaves the session; the last member's CLOSE closes the
 * controller as in exclusive mode. Commands must lie outside internal SRAM. */
int npu_service_share(struct npu_service *);
/* Optional runtime mode, after npu_service_share and before scheduling; the
 * interrupt must also be bound before activation. Members still OPEN and
 * CLOSE as in shared mode. LOAD and SUBMIT go to the runtime's request
 * callback, which normally holds the reply; WAIT, CANCEL and REAP are
 * refused. The worker starts no job by itself: the runtime starts one with
 * npu_service_start once its preparation is done and releases each held
 * reply with npu_service_release. Each worker pass handles interrupt
 * progress, at most one request with a zero wait and one runtime step. While
 * steps remain the worker does not block; with none left it blocks on its
 * notification, for at most one tick while the controller is active. With
 * idle_hint set, it reads the idle register (offset 0x4, which acknowledges
 * nothing) after each step and after each such tick while the controller is
 * active; full idle then runs the masked interrupt progress sequence as for a
 * notification. A controller fault releases every held reply with FAULT. */
int npu_service_runtime(struct npu_service *, const struct npu_service_runtime *, void *context);
/* Worker only, from a runtime callback. start masks the interrupt and
 * submits command for client, whose request is held, while the controller is
 * READY with no job: 1 started, 0 mask deferred (retry later), -1 refused or
 * faulted. release sends client's held reply with result and state.
 * retain faults the controller after a runtime integrity failure, which then
 * releases every held reply with FAULT and refuses all later requests. */
int npu_service_start(struct npu_service *, unsigned client, unsigned command, uint64_t timeout_us);
void npu_service_release(struct npu_service *, unsigned client, enum npu_service_result,
                         enum a733_npu_state);
void npu_service_retain(struct npu_service *);

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
 * OPEN claims the session; only its owner may submit, cancel, reap, close or
 * wait. STATUS is diagnostic for any registered task. WAIT (owner only, zero
 * timeout) replies like STATUS, but the worker holds the reply while the
 * bootstrap, job or cancellation is still active and sends it at the first
 * poll or interrupt pass after the controller leaves that state: READY, DONE,
 * CANCELED or FAULT, including a controller deadline. A held WAIT never delays
 * other clients' requests. Collect it with finite waits like any reply. Active polling waits at most
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
