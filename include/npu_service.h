#ifndef SUNSTONE_NPU_SERVICE_H
#define SUNSTONE_NPU_SERVICE_H
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "a733_npu_controller.h"
#include <stdatomic.h>
#ifndef UINT_MAX
#define UINT_MAX 0xffffffffu
#endif

/* The service has no fixed count of clients or commands. A client's slot, its permission to submit each command (one bit of
 * a bitmap with a bit for every command), its member record and the request queue's storage are tables that the caller gives
 * the service before init, sized for the clients and commands it declares (npu_service_tables_bytes, npu_service_tables);
 * command 0 is the bootstrap. Memory is the only bound (docs/designs/npu-resources.md). */
#define NPU_SERVICE_STACK_WORDS 4096u
/* "No client": the owner of a service nobody has opened, the member running no job. */
#define NPU_SERVICE_NONE UINT_MAX

enum npu_service_operation {
    NPU_SERVICE_STATUS, NPU_SERVICE_OPEN, NPU_SERVICE_SUBMIT,
    NPU_SERVICE_CANCEL, NPU_SERVICE_REAP, NPU_SERVICE_CLOSE,
    NPU_SERVICE_WAIT, NPU_SERVICE_LOAD, /* Runtime mode only. */
    NPU_SERVICE_RECOVER /* Explicit active-cancellation diagnostic only. */
};
enum npu_service_result {
    NPU_SERVICE_OK, NPU_SERVICE_INVALID, NPU_SERVICE_DENIED,
    NPU_SERVICE_BUSY, NPU_SERVICE_FAULT,
    /* Runtime mode only: the job failed with an NPU MMU exception, the service has reset the NPU (or is doing so)
     * and the session stays usable. The reply carries fault A733_NPU_MMU and the status and address. */
    NPU_SERVICE_JOB_FAULT
};
/* Progress marks of one fault, kept for the failure record (docs/reference-backsolve-npu.md, "NPU MMU exceptions").
 * The service worker sets the first group, the client the second. Cleared when a SUBMIT is sent. */
enum npu_service_mark {
    NPU_MARK_CAPTURED = 1u << 0,     /* the MMU status and address were read */
    NPU_MARK_PROGRESS_DONE = 1u << 1,/* the interrupt progress pass returned after the fault */
    NPU_MARK_SETTLE = 1u << 2,       /* the runtime settle step took the fault */
    NPU_MARK_FINISHED = 1u << 3,     /* the runtime finished hook returned */
    NPU_MARK_REPLY_POSTED = 1u << 4, /* the reply was sent to the client queue */
    NPU_MARK_CANCEL_DONE = 1u << 5,  /* the cancel handshake saw its completion bit */
    NPU_MARK_REINIT = 1u << 6,       /* reset and the initial command were started */
    NPU_MARK_READY = 1u << 7,        /* the initial command completed: the NPU is ready again */
    NPU_MARK_COLLECT = 1u << 8,      /* the client entered its wait for the reply */
    NPU_MARK_WOKE = 1u << 9,         /* the client received the reply */
    NPU_MARK_READ = 1u << 10,        /* the client read the fault out of the reply */
    NPU_MARK_RECOVER_FAILED = 1u << 11
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
    /* Valid when fault is A733_NPU_MMU: the controller's MMU status code and faulting device address. */
    uint32_t mmu_status, mmu_address;
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
    uint32_t *commands;   /* bit c: the client may submit command c; (commands + 31) / 32 words in the service's tables */
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
 * proves the owned GIC source disabled, inactive and pending clear. Deferred
 * callbacks get 10 ms plus one tick from their first pending result, then
 * retain on the next worker pass. This bounds waiting, not hardware drain. */
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
 * controller fault, client being NPU_SERVICE_NONE when no job ran.
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
struct npu_service;
/* Runtime mode state, installed by npu_service_runtime. It lives outside
 * struct npu_service, and the worker reaches the runtime loop only through
 * the loop pointer, so images that never call npu_service_runtime keep
 * neither. */
struct npu_service_runtime_state {
    struct npu_service_runtime hooks;
    void *context;
    void (*loop)(struct npu_service *);
    unsigned failed, idle_hints;
    volatile uint32_t marks;   /* npu_service_mark bits of the last fault */
    unsigned recoveries;       /* MMU exceptions recovered from */
    uint64_t open_timeout_us;  /* the bootstrap timeout of OPEN, reused by recovery */
    uint32_t fault_status, fault_address; /* the MMU capture of the job whose reply is held */
};
struct npu_service_direct {
    void (*irq)(void *, BaseType_t *);
    void *context;
};
struct npu_service_activity_sample {
    uint64_t time_us, job_ticket;
    uint32_t idle, frontend[2], fetch[2];
    enum a733_npu_state state;
};
/* Private diagnostic state, reserved for the complete service lifetime. The
 * worker owns every non-atomic field. check validates retained resources and
 * the original session deadline, at recovery entry (0), recovery completion
 * (1), and during activity/cancel/recovery progress (2). trigger returns the
 * timestamp captured after the native doorbell's completed MMIO write.
 * irq restarts the existing stopped-to-masked binding without local shutdown. */
struct npu_service_cancel_diagnostic {
    unsigned command, started, proven, canceled, reaped, recovery, recovered;
    struct npu_service_activity_sample samples[3];
    uint64_t job_ticket, trigger_us, cancel_asserted_us, cancel_released_us, cancel_done_us;
    uint32_t cancel_status,cancel_idle;
    uint64_t fault_operand,fault_ticket;
    uintptr_t fault_caller;
    unsigned fault_sample,fault_site,fault_recorded;
    uint64_t recovery_ticket;
    atomic_uint irq_receipts, irq_window;
    unsigned receipt_base, accounted_receipts, recovery_receipts;
    uint64_t (*trigger)(void *);
    int (*check)(void *,unsigned), (*irq)(void *);
    void *cookie;
};
/* The words that an owner's stop and retirement checks read: the interrupt's pending events and its stop, the worker's park
 * after the final CLOSE, and the runtime's failure (npu_runtime, which shares the words of its service). They are apart from
 * the rest of the service so that an owner can place them in memory its readers reach: the OS keeps them in its image, where
 * the application's task reads them at EL0, and the service and runtime in EL1-only memory (apps/os/npu.c). An owner sets
 * npu_service.signals before npu_runtime_prepare or npu_service_init; a service given none uses its own_signals. */
struct npu_service_signals { atomic_uint irq_pending, irq_stopped, runtime_parked, runtime_failed; };
/* Optional diagnostic observer. The worker calls it once after attribution
 * of a trigger (event 1), on subsequent passes before IRQ progress (0), and
 * before reaping a completed command (2). It must never block or modify the
 * service. Register before scheduling. All register reads use ops.read and
 * its native startup gate; event +0x10 is destructive and forbidden here. */
struct npu_service {
    /* Same routed core, installed/removed with local IRQs masked. A direct
     * client owns job MMIO; the worker parks until it returns the idle NPU. */
    const struct npu_service_direct *direct, *direct_request;
    TaskHandle_t direct_owner;
    atomic_uint direct_ready; /* 1 safe handoff acknowledged, 2 refused. */
    struct a733_npu_controller controller;
    struct a733_npu_ops ops;
    void *cookie;
    struct npu_service_irq_ops irq_ops;
    void *irq_cookie;
    atomic_uint irq_completions;
    /* The words a retirement check reads (struct npu_service_signals): the owner's copy when it gave one, else own_signals. */
    struct npu_service_signals *signals;
    struct npu_service_signals own_signals;
    unsigned irq_bound, irq_masked, irq_rearming, irq_retained;
    unsigned irq_waiting;
    TickType_t irq_wait_start[3];
    struct npu_service_message deferred;
    unsigned has_deferred;
    struct npu_service_message held;
    unsigned has_held;
    struct a733_npu_mmu_image image;
    struct a733_npu_command *commands;   /* command_count entries, in the tables */
    unsigned command_count, client_count, owner, initialized;
    void *tables; size_t table_bytes;    /* what npu_service_tables was given, until init */
    unsigned table_commands;
    atomic_uint activation_state;
    struct npu_service_activation activation;
    int (*restart_irq)(void *), (*restart_ready)(void *);
    unsigned restart_reaped, reopening;
    uint64_t job_ticket;
    QueueHandle_t requests;
    StaticQueue_t request_state;
    uint8_t *request_storage;            /* client_count messages */
    struct npu_service_client *clients;  /* client_count entries */
    TaskHandle_t worker;
    UBaseType_t affinity;
    StaticTask_t worker_state;
    StackType_t worker_stack[NPU_SERVICE_STACK_WORDS];
    unsigned shared, members, running, next;
    struct npu_service_member *member;   /* client_count entries */
    struct npu_service_runtime_state *runtime; /* Null outside runtime mode. */
    /* The diagnostic observer (npu_service_observe), null unless the application declares that it observes its jobs. */
    void (*observe)(void *,const struct npu_service *,unsigned);
    void *observe_cookie;
    uint64_t trigger_ticks; /* Native adapter's counter at the trigger write, kept only under an observer. */
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
    struct npu_service_cancel_diagnostic *cancel_diagnostic;
#endif
};

/* The bytes of the tables of a service of clients clients and commands commands (commands[]: the bootstrap and every
 * application command), and the macro form for a static array. They hold the commands, the clients, the members, the
 * permission bitmaps and the request queue's storage, each 8-byte aligned. */
#define NPU_SERVICE_PERMIT_WORDS(commands) (((commands) + 31u) / 32u)
#define NPU_SERVICE_TABLES_BYTES(clients, commands) \
    (((size_t)(commands) * sizeof(struct a733_npu_command) + 7u) / 8u * 8u + \
     (size_t)(clients) * ((sizeof(struct npu_service_client) + 7u) / 8u * 8u) + \
     (size_t)(clients) * ((sizeof(struct npu_service_member) + 7u) / 8u * 8u) + \
     (size_t)(clients) * ((NPU_SERVICE_PERMIT_WORDS(commands) * 4u + 7u) / 8u * 8u) + \
     (size_t)(clients) * ((sizeof(struct npu_service_message) + 7u) / 8u * 8u))
size_t npu_service_tables_bytes(unsigned clients, unsigned commands);
/* Before init, once: the service takes memory for its tables, for the clients and commands it will hold (init's command
 * count may not exceed commands). Returns 0, leaving the service unchanged, when the memory is too small (the caller names
 * the resource that did not fit), is not 8-byte aligned, or the counts are zero. The memory stays private to the service. */
int npu_service_tables(struct npu_service *, unsigned clients, unsigned commands, void *memory, size_t bytes);
int npu_service_runtime_parked(const struct npu_service *);
int npu_service_observe(struct npu_service *,
    void (*)(void *,const struct npu_service *,unsigned),void *);
/* Bind once after the sole client and IRQ, before scheduling. Normal restart,
 * shared/runtime use and additional clients are excluded. Recovery remains a
 * request from that client and preserves activation and exclusive ownership. */
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
int npu_service_cancel_diagnostic_enable(struct npu_service *,
    struct npu_service_cancel_diagnostic *,unsigned command,
    uint64_t (*trigger)(void *),int (*check)(void *,unsigned),void *cookie,int (*irq)(void *));
#endif

/* Optional normal restart for the deferred, single-client IRQ service. Bind
 * before scheduling, after client 0 and its IRQ. ready runs in the worker
 * with activation.context and checks the retained lifetime's absolute
 * deadline/health. irq runs masked in the routed-core critical section and
 * must prove stopped, disabled, inactive and pending-clear, leaving masked.
 * Existing bindings, worker, queues and ticket counters survive every run. */
int npu_service_restart_enable(struct npu_service *, int (*irq)(void *), int (*ready)(void *));
/* Bootstrap task only, after collecting a normal completed/reaped CLOSE.
 * prepare acquires the parked worker; resume authenticates prepared metadata
 * and asks that same worker to resume. BUSY requires another bounded poll.
 * retain withdraws any pending resume permanently; callers use it on their
 * original lifetime deadline or a platform restart failure. */
enum npu_service_result npu_service_restart_prepare(struct npu_service *);
enum npu_service_result npu_service_restart_resume(struct npu_service *);
void npu_service_restart_retain(struct npu_service *);

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
/* The same for a client that may submit commands first .. first + count - 1 (command 0, the bootstrap, is never allowed),
 * as many as the service's commands. */
int npu_service_bind_range(struct npu_service *, unsigned slot, TaskHandle_t, unsigned first, unsigned count);
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
