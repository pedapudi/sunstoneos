#ifndef SUNSTONE_PVR_COMPUTE_QUEUE_H
#define SUNSTONE_PVR_COMPUTE_QUEUE_H
/* The GPU compute queue inside the GPU service worker
 * (drivers/gpu/pvr_gpu_service.c, docs/designs/gpu-compute-queue.md).
 *
 * The worker calls these bounded operations at EL1, one at a time and
 * without a tick wait. */
#include <stdatomic.h>
#include <stdint.h>
#include "gpu_queue.h"
#include "pvr_cccb.h"
#include "pvr_mips.h"
#include "a733_gpu_vm_memory.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "pvr_compute_fast.h"

struct a733_gpu_queue;
struct pvr_gpu_service;
struct pvr_gpu_service_config;

/* Firmware-only objects, one uncached page each:
 * compute context, register state, client-ring control, client ring,
 * memory context, timeline. */
#define PVR_COMPUTE_QUEUE_OBJECTS 6u

/* The job page (GPU-read-only, in the PDS heap):
 * - 0-127: PDS data, written at creation; the binding-0 address and the
 *   group counts are rewritten only when they differ from the previous job,
 *   and the counts only for a program that reads them;
 * - 128-191: PDS dispatch program, written at creation;
 * - 192-255: fence PDS program, written at creation;
 * - 256-315: CDM stream, per job;
 * - 320-407: argument block, per job (at most 24 bytes of binding addresses,
 *   then at most 64 argument bytes).
 * One clean from byte 256 to the end of the argument block covers each job:
 * 2 lines for a 32-byte block, at most 3. Changed PDS lines add maintenance. */
#define PVR_COMPUTE_QUEUE_PDS_DATA 0u
#define PVR_COMPUTE_QUEUE_FENCE 192u
#define PVR_COMPUTE_QUEUE_CDM 256u
#define PVR_COMPUTE_QUEUE_ARGUMENTS 320u

/* Slot shared with the client task. Every access to slot_state, lost and
 * wake is sequentially consistent on both sides. */
enum pvr_compute_queue_slot { PVR_COMPUTE_QUEUE_CLOSED, PVR_COMPUTE_QUEUE_IDLE,
    PVR_COMPUTE_QUEUE_RESERVED, PVR_COMPUTE_QUEUE_SUBMITTED, PVR_COMPUTE_QUEUE_TAKEN };

enum pvr_compute_queue_phase { PVR_COMPUTE_QUEUE_NEW, PVR_COMPUTE_QUEUE_BOUND,
    PVR_COMPUTE_QUEUE_CREATED, PVR_COMPUTE_QUEUE_PUBLISHING, PVR_COMPUTE_QUEUE_TABLES,
    PVR_COMPUTE_QUEUE_PUBLISHED, PVR_COMPUTE_QUEUE_FORMAT, PVR_COMPUTE_QUEUE_READY,
    PVR_COMPUTE_QUEUE_IN_FLIGHT, PVR_COMPUTE_QUEUE_FAILED };

/* The second admitted task shares the owner's trusted program and mappings.
 * Each caller owns one submission and completion slot. */
struct pvr_compute_queue_client {
    QueueHandle_t completions;
    StaticQueue_t completion_state;
    uint8_t completion_storage[sizeof(struct gpu_queue_record)];
    TaskHandle_t task;
    atomic_uint slot_state, client_owed;
    atomic_uint_fast64_t not_after;
    struct gpu_queue_job slot;
    atomic_uint sequence;
};
struct pvr_compute_queue_client_view {
    QueueHandle_t completions;
    atomic_uint *slot_state, *client_owed, *sequence;
    atomic_uint_fast64_t *not_after;
    struct gpu_queue_job *slot;
};

struct pvr_compute_queue {
    struct pvr_compute_fast fast;
    struct a733_gpu_queue *owner;
    struct pvr_mips_buffer firmware[PVR_COMPUTE_QUEUE_OBJECTS], publish[10];
    uint32_t firmware_address[PVR_COMPUTE_QUEUE_OBJECTS];
    uint32_t previous_sequence;
    uint32_t publish_index, publish_page, generation;
    atomic_uint final_close;
    atomic_uint client_owed;
    unsigned char *job_cpu;
    uint64_t epoch, resume, task_word, job_gpu;
    struct gpu_queue_context_config create_config;
    struct gpu_queue_program create_program;
    uint64_t create_ticket, close_ticket;
    atomic_uint create_pending, create_done;
    atomic_uint close_pending;
    QueueHandle_t completions;
    StaticQueue_t completion_state;
    uint8_t completion_storage[sizeof(struct gpu_queue_record)];
    struct pvr_gpu_service *service;
    struct pvr_mips_object objects[PVR_COMPUTE_QUEUE_OBJECTS];
    struct pvr_cccb client;
    struct gpu_queue_lease_info lease[GPU_QUEUE_LEASES];
    uint32_t lease_count, coefficients, phase, error, sequence, owed, held, priority, payload_cache;
    uint32_t common_words; /* immutable admitted system values plus local words */
    /* Worker-owned: one terminal breadcrumb before publishing a job record. */
    unsigned crumb_pending;
    uint64_t job_us, deadline, kicked;
    atomic_uint_fast64_t not_after;
    unsigned char command[96], update[8];
    uint64_t binding0;            /* last PDS binding-0 address */
    uint32_t groups[3];           /* last PDS group counts */
    struct gpu_queue_job job;     /* the worker's copy */
    /* Shared with the client. */
    atomic_uint slot_state, lost, wake;
    struct gpu_queue_job slot;
    struct pvr_compute_queue_client peer;
    atomic_uint application_sequence, recovering;
    unsigned active_client, next_client;
    uint32_t timeline_serial, timeline_previous;
    atomic_uint completed_jobs;
    unsigned reset_pending;
};

int pvr_compute_queue_client_view(struct pvr_compute_queue *, unsigned,
    struct pvr_compute_queue_client_view *);
int pvr_compute_queue_clients_idle(struct pvr_compute_queue *);
int pvr_compute_queue_client_register(struct pvr_compute_queue *, TaskHandle_t);
int pvr_compute_queue_runtime_program(const struct gpu_queue_program *);
/* After native reset/readback, complete OFF and memory reclaim only. Publish
 * the failed active record, preserving the other caller's submitted slot. */
int pvr_compute_queue_recovered(struct pvr_compute_queue *);

/* Before the scheduler, at EL1, after the platform reserves payload and
 * firmware pages: bind the component and set config->compute_queue.
 * No device access. */
int pvr_compute_queue_bind(struct pvr_compute_queue *, struct pvr_gpu_service *,
                           struct pvr_gpu_service_config *);
/* CREATE request, device COLD: copy the program; plan the leases and the
 * internal pages with pvr_gpu_vm_require; acquire the planned table pages,
 * at most 32, from the GPU write-back pool;
 * build the GPU address space once with pvr_gpu_vm_build, which needs that
 * exact size; build the firmware context. The worker then runs OPEN.
 * 1 accepted, 0 refused without device access. */
int pvr_compute_queue_create(struct pvr_compute_queue *, const struct gpu_queue_context_config *);

/* Serialized worker operations. */
unsigned pvr_compute_queue_objects(struct pvr_compute_queue *, struct pvr_mips_object *, unsigned);
int pvr_compute_queue_prepare(struct pvr_compute_queue *, uint64_t deadline);
int pvr_compute_queue_publish(struct pvr_compute_queue *);   /* 1 done, 0 pending, -1 fault */
int pvr_compute_queue_poll(struct pvr_compute_queue *);      /* -1 fault */
int pvr_compute_queue_event(struct pvr_compute_queue *, const unsigned char command[88]);
void pvr_compute_queue_abort(struct pvr_compute_queue *);
int pvr_compute_queue_close(struct pvr_compute_queue *);     /* 1 sealed, 0 busy */
/* 1 while a job is in flight (deadline: its hang bound) or waits in the slot
 * with a kick deadline (deadline: not_after). */
int pvr_compute_queue_deadline(const struct pvr_compute_queue *, uint64_t *deadline);
int pvr_compute_queue_submitting(const struct pvr_compute_queue *);
uint64_t pvr_compute_queue_progress(const struct pvr_compute_queue *);
/* Performance hold: stop kicking; 1 once nothing is in flight. A job
 * submitted during the hold waits in the slot. release kicks it. */
int pvr_compute_queue_hold(struct pvr_compute_queue *);
int pvr_compute_queue_release(struct pvr_compute_queue *);
/* After healthy STOPPED: restore the memory-context record and the firmware
 * context, and restore the timeline to the last
 * sequence. The GPU tables stay published and unchanged. */
int pvr_compute_queue_reopen(struct pvr_compute_queue *);

/* Client side of gpu_queue_submit after the slot holds the job:
 *   atomic_store(&slot_state, SUBMITTED);
 *   if (atomic_exchange(&wake, 1) == 0) send one wake message;
 * Worker, on that wake message, before taking the slot:
 *   atomic_store(&wake, 0);
 *   if (atomic_load(&slot_state) == SUBMITTED) take the job;
 * In the single order of these sequentially consistent operations, either
 * the worker's clear precedes the client's exchange, which then sends, or the
 * client's store precedes the worker's load, which then sees the job. The
 * worker can therefore wait with portMAX_DELAY when idle without losing a
 * job. The request queue holds one message per client, one interrupt wake
 * and this one wake, so the send cannot find it full. */
int pvr_compute_queue_wake(struct pvr_compute_queue *);
/* Bind the singleton application interface before the scheduler starts. */
int pvr_compute_queue_client_bind(struct pvr_compute_queue *);
/* Runtime hook: 1 live, 0 thermally parked, -1 failed. */
int pvr_compute_queue_runtime_sync(void);
/* Hot-path admission reads published lifecycle/thermal state only. */
int pvr_compute_queue_runtime_admit(void);
void pvr_compute_queue_runtime_opened(uint64_t epoch);
void pvr_compute_queue_runtime_closed(void);
#endif
