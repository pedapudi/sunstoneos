#ifndef SUNSTONE_PVR_GPU_SERVICE_H
#define SUNSTONE_PVR_GPU_SERVICE_H
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "pvr_fw_state.h"
#include "pvr_gpu_stop.h"
#include "pvr_free_list.h"
struct pvr_compute_service;
struct pvr_gpu_service;
struct pvr_render_service;
struct a733_gpu_graphics_memory;

#define PVR_GPU_SERVICE_CLIENTS 8u
#define PVR_GPU_SERVICE_STACK_WORDS 4096u
#define PVR_GPU_SERVICE_IRQ_REARM_ATTEMPTS 32u
enum pvr_gpu_service_phase { PVR_GPU_SERVICE_COLD, PVR_GPU_SERVICE_STARTING,
    PVR_GPU_SERVICE_CHECKING, PVR_GPU_SERVICE_READY, PVR_GPU_SERVICE_STOPPING,
    PVR_GPU_SERVICE_STOPPED, PVR_GPU_SERVICE_FAILED, PVR_GPU_SERVICE_ENABLING,
    PVR_GPU_SERVICE_BUILDING, PVR_GPU_SERVICE_DRAINING, PVR_GPU_SERVICE_PUBLISHING, PVR_GPU_SERVICE_PUBLISHING_GRAPHICS };
enum pvr_gpu_service_operation { PVR_GPU_SERVICE_STATUS, PVR_GPU_SERVICE_OPEN,
    PVR_GPU_SERVICE_CLOSE, PVR_GPU_SERVICE_COMPUTE, PVR_GPU_SERVICE_RENDER, PVR_GPU_SERVICE_RENDER_OUTPUT,
    PVR_GPU_SERVICE_COMPUTE_OUTPUT, PVR_GPU_SERVICE_COMPUTE_CANCEL, PVR_GPU_SERVICE_COMPUTE_INPUT,
    PVR_GPU_SERVICE_DRAW };
enum pvr_gpu_service_result { PVR_GPU_SERVICE_OK, PVR_GPU_SERVICE_INVALID,
    PVR_GPU_SERVICE_DENIED, PVR_GPU_SERVICE_BUSY, PVR_GPU_SERVICE_FAULT };
enum pvr_gpu_service_error { PVR_GPU_SERVICE_NO_ERROR, PVR_GPU_SERVICE_CLOCK,
    PVR_GPU_SERVICE_PREPARATION, PVR_GPU_SERVICE_ENABLE, PVR_GPU_SERVICE_START,
    PVR_GPU_SERVICE_CONTROL, PVR_GPU_SERVICE_STOP, PVR_GPU_SERVICE_INTERRUPT,
    PVR_GPU_SERVICE_FREE_LISTS, PVR_GPU_SERVICE_COMPUTATION, PVR_GPU_SERVICE_GRAPHICS };

/* Optional shared peripheral interrupt delivery. Both callbacks or neither.
 * mask issues a disable command and completes CPU writes; it returns 1 on
 * success. It must not wait for distributor propagation in interrupt context.
 * rearm returns 1 when enabled, 0 while a prior disable is still propagating,
 * and -1 on failure. Failure must leave delivery disabled or require immediate
 * platform recovery. Both callbacks run under FreeRTOS critical sections and
 * must be bounded, nonblocking, and safe at EL1t/EL1h on any service/IRQ core.
 * The platform configures the interrupt disabled before init, routes it to an
 * online core at an API-safe priority, and retains exclusive ownership.
 */
struct pvr_gpu_service_irq_ops {
    int (*mask)(void *);
    int (*rearm)(void *);
};

/* Optional bounded power sequencing in the privileged worker. Supply both
 * callbacks and leave enable NULL, or use the synchronous enable callback.
 * begin returns 1 after accepting the operation with the absolute OPEN deadline;
 * any other result is a retained failure. poll returns 0 while pending, 1 when
 * stopped-device power/reset/clock ownership is established, or -1 on failure.
 * The worker invokes one poll per advance and continues processing requests.
 * Each callback must be bounded and non-reentrant, use the common context/clock,
 * and retain partial resources on failure. No firmware register operation runs
 * until completion. After service failure no further power callback runs; only
 * independently verified recovery may discard or resume the backend state.
 */
struct pvr_gpu_service_enable_ops {
    int (*begin)(void *, uint64_t deadline);
    int (*poll)(void *);
};

/* Optional native free-list service. count=0 disables it. Configuration and
 * metadata are immutable to other callers after init. The manager owns metadata
 * writes and all admitted leases. Ops use the service's common context. Initial
 * construction precedes power/start; the OPEN deadline bounds every step.
 * operation_timeout_us separately bounds construction and admitted firmware work.
 * Protected GPU table/record mappings and linked-target recovery belong to the
 * platform. Reopen after STOPPED is refused until lease disposal is implemented.
 */
struct pvr_gpu_service_free_list_config {
    const struct pvr_free_list_config *lists;
    unsigned count, minimum_pages;
    uint64_t operation_timeout_us;
    struct pvr_free_list_ops ops;
};

/* Private platform configuration, copied at init; pointed-to storage stays
 * pinned and exclusively owned throughout the service lifetime. No filesystem
 * or allocator is required: the platform reserves these exact buffers once.
 * Firmware/pages must remain immutable. Buffers must not alias backend state.
 * All operations use one context and monotonic microsecond clock. The selected
 * enable interface establishes stopped-device power/reset/clock ownership, including on reopen;
 * its selected rate must match state.gpu_clock_hz. It is bounded and returns
 * 1 on success. Failure can have side effects, so all resources are retained.
 * Queue callbacks address resources 0..4; start/control/stop callbacks address
 * these same allocations. Their individual cache/order contracts still apply.
 * The platform verifies firmware identity, physical identity, CPU/PA mapping,
 * reserved RAM, recovery, and physical isolation before board use.
 */
struct pvr_gpu_service_config {
    const void *firmware;
    size_t firmware_bytes;
    struct pvr_mips_buffer firmware_buffers[3];
    struct pvr_mips_buffer resources[PVR_FW_RESOURCE_COUNT];
    struct pvr_fw_state_config state;
    struct pvr_ccb_ops queue;
    struct pvr_gpu_start_ops start;
    struct pvr_gpu_control_ops control;
    struct pvr_gpu_stop_ops stop;
    int (*enable)(void *);
    void *context;
    struct pvr_gpu_service_irq_ops irq;
    struct pvr_gpu_service_enable_ops power;
    struct pvr_gpu_service_free_list_config free_lists;
    struct pvr_compute_service *compute; /* Optional fixed native numerical job. */
    /* Mandatory with compute. Bounded worker callback, returning exactly 1
     * only for freshly checked powered MC1 identity, capabilities and retained
     * lifecycle/token. No GPU access: observation occurs during power enable.
     * Called after enable before firmware start, and before each COMPUTE. */
    int (*compute_mode)(void *);
    /* Concrete native free-list/HWRT owner; published before compute and power. */
    struct a733_gpu_graphics_memory *graphics;
    struct pvr_render_service *render;
};
struct pvr_gpu_service_request {
    enum pvr_gpu_service_operation operation;
    uint64_t timeout_us; /* OPEN/CLOSE/COMPUTE use a bound; STATUS/OUTPUT/CANCEL require zero. */
};
struct pvr_gpu_service_reply {
    uint64_t ticket, epoch;
    enum pvr_gpu_service_result result;
    enum pvr_gpu_service_phase phase;
    enum pvr_gpu_service_error error;
    uint32_t owns_session, start_error, control_error, stop_error;
    uint32_t free_list_error, free_list_pending;
    uint32_t compute_phase, compute_error, compute_job_error;
    uint32_t compute_sequence, compute_word_count, compute_words[32];
    uint32_t compute_first_word, compute_total_words;
    uint32_t graphics_phase;
    uint32_t render_phase, render_error, render_checked;
    uint32_t render_first_pixel, render_pixel_count, render_pixels[64];
    uint32_t render_sequence;
};
struct pvr_gpu_service_message {
    struct pvr_gpu_service_request request;
    uint64_t ticket;
    unsigned client;
    uint32_t render_first_pixel, compute_first_word, compute_sequence, render_sequence;
};
/* Scoped copied triangle upload. epoch must match the caller's OPEN reply.
 * The complete input is copied before enqueue; no caller pointer reaches the
 * worker. This compatibility entry submits only frame one. */
int pvr_gpu_service_draw(struct pvr_gpu_service *,uint64_t epoch,const void *vertices,
    size_t bytes,uint32_t count,uint64_t timeout_us,TickType_t wait,uint64_t *ticket);
/* Accept another frame only after acquired previous completion and copied output.
 * Acceptance acknowledges replacement of previous_frame; refusal preserves it. */
int pvr_gpu_service_draw_next(struct pvr_gpu_service *,uint64_t epoch,uint32_t previous_frame,
    const void *vertices,size_t bytes,uint32_t count,uint64_t timeout_us,TickType_t wait,uint64_t *ticket);
/* Nonzero exact frame; unlike the legacy read, zero never selects a wildcard. */
int pvr_gpu_service_render_read_sequence(struct pvr_gpu_service *,uint32_t frame,uint32_t first_pixel,
    TickType_t wait,uint64_t *ticket);
struct pvr_gpu_service_client {
    TaskHandle_t task;
    uint64_t next_ticket, pending;
    QueueHandle_t replies;
    StaticQueue_t reply_state;
    uint8_t reply_storage[sizeof(struct pvr_gpu_service_reply)];
};
/* Private static storage; visible for reservation without a heap. Only the
 * worker may access the controllers or backing after scheduler startup. */
struct pvr_gpu_service {
    struct pvr_gpu_service_config config;
    struct pvr_ccb queue;
    struct pvr_gpu_start start;
    struct pvr_gpu_control control;
    struct pvr_gpu_stop stop;
    struct pvr_free_lists free_lists;
    struct pvr_gpu_events free_list_events;
    struct pvr_mips_boot boot;
    struct pvr_mips_object mappings[PVR_MIPS_MAX_OBJECTS];
    uint64_t epoch, deadline, last_now;
    enum pvr_gpu_service_phase phase;
    enum pvr_gpu_service_error error;
    unsigned owner, initialized;
    unsigned irq_masked, irq_fault, irq_wake_pending, irq_deferred;
    QueueHandle_t requests;
    StaticQueue_t request_state;
    uint8_t request_storage[PVR_GPU_SERVICE_CLIENTS * sizeof(struct pvr_gpu_service_message)];
    struct pvr_gpu_service_client clients[PVR_GPU_SERVICE_CLIENTS];
    TaskHandle_t worker;
    StaticTask_t worker_state;
    StackType_t worker_stack[PVR_GPU_SERVICE_STACK_WORDS];
};

/* One instance. Zero storage once. Init/bind run at EL1 before the scheduler.
 * The SMP port must grant EL1t only through the internal worker hook below.
 * Client handles must remain live, never deleted/reused. Current shared
 * mappings enforce cooperative API ownership, not malicious-task isolation.
 * Init validates allocation spans without changing buffers or invoking ops.
 */
int pvr_gpu_service_init(struct pvr_gpu_service *, const struct pvr_gpu_service_config *,
                         UBaseType_t priority, UBaseType_t affinity);
int pvr_gpu_service_bind(struct pvr_gpu_service *, unsigned slot, TaskHandle_t);

/* Task-only copied requests/replies, one pending ticket per registered caller.
 * Finite waits only. Refusal/timeout preserves outputs; collect timeout retains
 * the pending ticket. A queued OPEN/CLOSE reply reports acceptance, not device
 * readiness or shutdown. STATUS reports progress without driving it.
 * OPEN claims an unowned COLD/STOPPED device. Its deadline covers preparation,
 * startup and health. Only the owner may CLOSE a READY device. Shutdown closes
 * admission immediately; STOPPED releases ownership only after isolation.
 * Failure retains ownership, diagnostics and all resources; no further backend
 * callback or automatic restart occurs. Reopening after successful stop uses a
 * fresh epoch and prepares all backing again when free lists are disabled.
 * With free lists enabled, CLOSE drains retained firmware actions before stop
 * and preserves all leases after verified isolation. Reopen then returns BUSY
 * until explicit resource disposal exists. Optional COMPUTE repeatedly admits the fixed
 * numerical workload after acquired completion for the READY owner; its reply reports acceptance. STATUS
 * includes compute progress. COMPUTE_OUTPUT copies acquired results with sequence
 * identity after completion. COMPUTE_CANCEL closes admission and faults/retains
 * the session; its acknowledgment does not establish hardware cancellation. CLOSE refuses pending compute and retains backing
 * after completed-job isolation; reopen remains BUSY until context disposal.
 * Configured graphics and compute publication are part of OPEN and precede
 * power enable. Graphics preparation is not a rendering/completion claim.
 */
int pvr_gpu_service_send(struct pvr_gpu_service *, const struct pvr_gpu_service_request *,
                         TickType_t wait, uint64_t *ticket);
int pvr_gpu_service_collect(struct pvr_gpu_service *, uint64_t ticket,
                            struct pvr_gpu_service_reply *, TickType_t wait);
/* Only the dispatcher for the configured interrupt may call this at an
 * API-safe priority. Masking and notification precede the dispatcher's EOI.
 * Return 0 requires platform recovery if delivery cannot remain disabled.
 * The caller supplies a pdFALSE-initialized wake flag, performs the port's
 * yield-from-ISR operation, and owns GIC acknowledge/end-of-interrupt.
 * Queue saturation is safe: an existing request already wakes the worker.
 */
int pvr_gpu_service_interrupt(struct pvr_gpu_service *, BaseType_t *woken);
BaseType_t pvr_gpu_service_task_privileged(TaskFunction_t, void *);
/* Queue one copied 64-pixel output slice for the current session owner, using
 * the same send/collect ticket lifetime. Requires completed verified render.
 * first_pixel is a multiple of 64 below 4096. Reply owns its copied pixels;
 * device backing remains retained. No application pointer reaches the worker. */
int pvr_gpu_service_render_read(struct pvr_gpu_service *, uint32_t first_pixel,
    TickType_t wait, uint64_t *ticket);
/* Copied owner-only output slice; at most 32 words, after acquired completion. */
int pvr_gpu_service_compute_read(struct pvr_gpu_service *,uint32_t first_word,
    TickType_t wait,uint64_t *ticket);
/* Copy a complete same-shape input before queue acceptance. Only one staged
 * request exists per compute component. Caller may reuse input after return.
 * A successful enqueue returns a normal ticket; reply reports worker admission.
 * sequence names the acquired result being replaced; no wrap is supported. */
int pvr_gpu_service_compute_input(struct pvr_gpu_service *, uint32_t sequence,
    const void *input, size_t bytes, uint64_t timeout_us, TickType_t wait, uint64_t *ticket);
/* Exact sequence read; stale requests never copy a newer result. */
int pvr_gpu_service_compute_read_sequence(struct pvr_gpu_service *, uint32_t sequence,
    uint32_t first_word, TickType_t wait, uint64_t *ticket);
#endif
