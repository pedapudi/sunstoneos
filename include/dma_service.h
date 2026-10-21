#ifndef SUNSTONE_DMA_SERVICE_H
#define SUNSTONE_DMA_SERVICE_H
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "dma_service_i2s_stream.h"
#include <stdatomic.h>

#define DMA_SERVICE_CLIENTS 8u
#define DMA_SERVICE_JOBS 16u
#define DMA_SERVICE_SEGMENTS 8u
#define DMA_SERVICE_MAPPINGS 16u
#define DMA_SERVICE_STACK_WORDS 4096u
#define DMA_SERVICE_NO_JOB DMA_SERVICE_JOBS
/* I2S0 stream jobs use one channel, or two for a duplex pair. */
#define DMA_SERVICE_STREAMS 2u

enum dma_service_kind { DMA_SERVICE_RAM, DMA_SERVICE_I2S_STREAM, DMA_SERVICE_FIFO };
enum dma_service_operation { DMA_SERVICE_STATUS, DMA_SERVICE_OPEN, DMA_SERVICE_SUBMIT,
    DMA_SERVICE_CANCEL, DMA_SERVICE_REAP, DMA_SERVICE_CLOSE,
    DMA_SERVICE_PERIOD_GET, DMA_SERVICE_PERIOD_PUT,
    DMA_SERVICE_PERIOD_END }; /*8: finite TX only; timeout is valid tail frames. */
enum dma_service_result { DMA_SERVICE_OK, DMA_SERVICE_INVALID, DMA_SERVICE_DENIED,
    DMA_SERVICE_BUSY, DMA_SERVICE_FAULT };
enum dma_service_phase { DMA_SERVICE_CLOSED, DMA_SERVICE_OPENING, DMA_SERVICE_READY,
    DMA_SERVICE_RUNNING, DMA_SERVICE_DRAINING, DMA_SERVICE_RESULT, DMA_SERVICE_CLOSING,
    DMA_SERVICE_RECOVERY };
enum dma_service_outcome { DMA_SERVICE_NO_RESULT, DMA_SERVICE_COMPLETE,
    DMA_SERVICE_CANCELED, DMA_SERVICE_FAILED };
struct dma_service_request {
    enum dma_service_operation operation;
    unsigned job; /* Index in private startup table; selects its fixed channel. */
    uint64_t timeout; /* SUBMIT: per-period DMA ticks. I2S OPEN/CLOSE: microseconds. END: valid final frames (nonzero). Otherwise zero. */
    uint64_t period_ticket; /* PERIOD_PUT/END: exact ticket returned by GET. */
};
struct dma_service_reply {
    uint64_t ticket, job_ticket;
    enum dma_service_result result;
    enum dma_service_phase phase;
    enum dma_service_outcome outcome;
    unsigned owns_session, holds_buffers;
    struct a733_i2s_period period; /* Kind distinguishes raw, copied and metadata-only progress. */
    uint64_t stream_frames; /* Cumulative completed cyclic frames, including final drain. */
};
/* Metadata is copied at initialization; pointed-to RAM allocations remain
 * reserved/pinned for service lifetime. RAM jobs prohibit FIFO requests.
 * FIFO jobs require one fixed FIFO endpoint and direction throughout a finite
 * transfer; native port policy authenticates its request, width and burst.
 * FIFO producer request gating and final wire drain belong to its sole client.
 * Single-direction I2S stream profiles must use one fixed DMA channel; a
 * duplex pair uses two distinct channels. */
struct dma_service_protected {
    uintptr_t cpu;
    uint64_t physical;
    size_t bytes;
};
/* Protected ranges are trusted platform reservations. Include a range covering
 * all service storage and every adapter/callback context. CPU-to-physical facts
 * and all other reserved firmware/page-table memory are platform obligations. */
struct dma_service_job {
    enum dma_service_kind kind;
    unsigned channel;
    struct a733_dma_transfer transfer;
    const struct dma_service_i2s_stream_profile *stream;
};
struct dma_service_private_job {
    enum dma_service_kind kind;
    unsigned channel;
    struct a733_dma_transfer transfer;
    struct a733_dma_segment segments[DMA_SERVICE_SEGMENTS];
    struct a733_dma_mapping mappings[DMA_SERVICE_MAPPINGS];
    struct dma_service_i2s_stream_profile stream;
};
struct dma_service_message { struct dma_service_request request; uint64_t ticket; unsigned client; };
struct dma_service_client {
    TaskHandle_t task;
    uint32_t jobs;
    uint64_t next_ticket,pending;
    QueueHandle_t replies;
    StaticQueue_t queue;
    uint8_t storage[sizeof(struct dma_service_reply)];
};
struct dma_service_channel {
    unsigned owner,profile,job,holds_buffers;
    unsigned stream; /* Index in streams, or DMA_SERVICE_STREAMS. */
    uint64_t job_ticket;
    enum dma_service_phase phase;
    enum dma_service_outcome outcome;
};
/* Trusted private bootstrap configuration, copied before scheduling starts.
 * prepared is bounded and performs no device operations. It authenticates the
 * prepared platform controller and exact channel mask: 1 ready, 0 pending,
 * -1 retained failure. Its context remains private and protected for lifetime. */
struct dma_service_activation {
    TaskHandle_t bootstrap;
    int (*prepared)(void *, uintptr_t controller, uint16_t channels);
    void *context;
};
/* Private static storage. Public layout permits reservation without a heap.
 * Applications must not modify/read these fields. Only this service's worker
 * calls the controller, native adapter or I2S callbacks after initialization. */
struct dma_service {
    struct a733_dma_controller controller;
    struct dma_service_private_job jobs[DMA_SERVICE_JOBS];
    unsigned job_count,initialized;
    atomic_uint activation_state;
    struct dma_service_activation activation;
    struct dma_service_channel channels[A733_DMA_CHANNELS];
    /* Bound to channels at initialization; never reassigned. */
    struct a733_i2s_stream streams[DMA_SERVICE_STREAMS];
    struct dma_service_client clients[DMA_SERVICE_CLIENTS];
    QueueHandle_t requests;
    StaticQueue_t request_queue;
    uint8_t request_storage[DMA_SERVICE_CLIENTS*sizeof(struct dma_service_message)];
    TaskHandle_t worker;
    StaticTask_t worker_state;
    StackType_t worker_stack[DMA_SERVICE_STACK_WORDS];
};
/* Initialize/bind once at EL1 before scheduler start. Controller initialization
 * performs no MMIO. One service instance owns DMA0 and authorized channels.
 * Prerequisites: pinned reserved RAM and native memory/FIFO policy, prepared
 * stable DMA clocks/reset/CPU routing, masked DMA IRQ and watchdog recovery.
 * Every admitted mapping must exclude ALL privileged state, including adapter,
 * page tables and callback state. Init additionally rejects the complete service
 * allocation and copied metadata overlapping registered DMA allocations.
 * Native adapter and callback state outlive the service. I2S parents/pins need
 * their separate leases. Current shared mappings do not isolate malicious EL0
 * tasks. Bound task handles must remain live; deletion/reuse is forbidden. */
int dma_service_init(struct dma_service *, const struct a733_dma_operations *, void *,
    uintptr_t base, uint16_t channels, uint64_t drain_timeout_ticks,
    const struct dma_service_job *, unsigned count,
    const struct dma_service_protected *, unsigned protected_count,
    UBaseType_t priority, UBaseType_t affinity);
/* Deferred variant: initialize and bind before scheduling, then prepare hardware
 * from the designated EL1 bootstrap task using shared task mutexes. No worker
 * device callbacks or client sends occur before successful activation. Hardware
 * readiness is deferred; memory reservations and all other init prerequisites
 * remain mandatory. Existing init creates an immediately active service. */
int dma_service_init_deferred(struct dma_service *, const struct a733_dma_operations *, void *,
    uintptr_t base, uint16_t channels, uint64_t drain_timeout_ticks,
    const struct dma_service_job *, unsigned count,
    const struct dma_service_protected *, unsigned protected_count,
    UBaseType_t priority, UBaseType_t affinity, const struct dma_service_activation *);
/* Designated bootstrap task only, EL1, scheduler running. No supplied token or
 * callback can replace copied authority. Pending returns BUSY without activation;
 * a failed prepared callback permanently retains the inactive service. Success
 * publishes readiness across cores and wakes its worker. Activation is one-way;
 * unauthorized or duplicate calls do not invoke prepared or change readiness. */
enum dma_service_result dma_service_activate(struct dma_service *);
int dma_service_bind(struct dma_service *, unsigned slot, TaskHandle_t, uint32_t job_mask);
/* Task-only. Identity comes from FreeRTOS. send copies the request, never
 * retains a caller stack pointer. A queued SUBMIT freezes its registered
 * buffers until refusal or terminal completion. Consume successful payloads
 * while STATUS reports RESULT with a held reservation; finish all access before
 * queueing REAP, which relinquishes that reservation. Acceptance retains
 * allocations after DMA drain until REAP; CLOSE cannot discard unreaped results.
 * One pending reply per task. A collection timeout preserves its ticket.
 * All client waits are finite; outputs unchanged on refusal/timeout.
 * Task abandonment retains session, results and buffers indefinitely.
 * Stream OPEN reserves the complete ring until CLOSE; GET grants one period
 * and PUT returns RX storage or commits TX data. REAP requires all client-held
 * periods returned after a terminal stop. TX completion means DMA consumed
 * memory, not wire playback. No automatic reclamation after task abandonment.
 * A finite stream permits one START per open; after REAP, CLOSE and OPEN
 * before reuse. The worker polls all active channels between requests, and
 * polls the request's channel before answering STATUS, PERIOD_GET or REAP.
 * Each I2S channel dispatches only through stream APIs, never through an
 * independent generic DMA poll. */
int dma_service_send(struct dma_service *, const struct dma_service_request *, TickType_t wait, uint64_t *ticket);
int dma_service_collect(struct dma_service *, uint64_t ticket, struct dma_service_reply *, TickType_t wait);
/* Copy only a collected CAPTURED_COPY staging lease for this task/job/ticket.
 * No pointer is queued; CPU destination must not alias service or DMA storage. */
int dma_service_copy_period(struct dma_service *,unsigned,uint64_t,void *,size_t);
/* Only init's private worker receives EL1t through the startup port hook. */
BaseType_t dma_service_task_privileged(TaskFunction_t, void *);
#endif
