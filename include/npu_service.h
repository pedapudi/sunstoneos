#ifndef SUNSTONE_NPU_SERVICE_H
#define SUNSTONE_NPU_SERVICE_H
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "a733_npu_controller.h"

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
/* Private static storage. Public only so the platform can reserve it without
 * a heap. Application code must not inspect or modify these fields. */
struct npu_service {
    struct a733_npu_controller controller;
    struct a733_npu_ops ops;
    void *cookie;
    struct a733_npu_mmu_image image;
    struct a733_npu_command commands[NPU_SERVICE_COMMANDS];
    unsigned command_count, owner, initialized;
    uint64_t job_ticket;
    QueueHandle_t requests;
    StaticQueue_t request_state;
    uint8_t request_storage[NPU_SERVICE_CLIENTS * sizeof(struct npu_service_message)];
    struct npu_service_client clients[NPU_SERVICE_CLIENTS];
    TaskHandle_t worker;
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
int npu_service_bind(struct npu_service *, unsigned slot, TaskHandle_t, uint32_t command_mask);

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
