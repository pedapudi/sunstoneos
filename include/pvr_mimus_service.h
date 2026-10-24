#ifndef SUNSTONE_PVR_MIMUS_SERVICE_H
#define SUNSTONE_PVR_MIMUS_SERVICE_H
/* The mimus session as a component of the GPU service
 * (docs/gpu-mimus-service.md, docs/designs/gpu-recognition.md).
 *
 * The GPU service worker owns everything here except the slot and the
 * completion queue. A client task (the recognizer) writes one job into the
 * slot and wakes the worker; the worker submits it in one step, and after the
 * job's timeline update it maintains the output region and posts one 8-byte
 * record. The session code of pvr_compute_mimus.h makes the job words, the
 * cache maintenance and the tag check; this component adds the firmware
 * objects, the client ring, the timeline and the job's life cycle. */
#include "pvr_compute_mimus.h"
#include "pvr_mimus_job.h"
#include "pvr_cccb.h"
#include "pvr_mips.h"
#include "a733_gpu_vm_memory.h"
#include "FreeRTOS.h"
#include "queue.h"
#include <stdatomic.h>

struct pvr_gpu_service;
struct pvr_gpu_service_config;

/* Firmware-only objects: compute context, register state, client-ring
 * control, client ring, memory context, timeline. Each is one uncached page. */
#define PVR_MIMUS_SERVICE_OBJECTS 6u
#define PVR_MIMUS_SERVICE_RING_BYTES 1024u
/* CPU pages cleaned once at OPEN besides the job and program pages: the
 * context-switch programs of pvr_compute_programs.c. */
#define PVR_MIMUS_SERVICE_PUBLISH 10u

enum pvr_mimus_slot_state { PVR_MIMUS_SLOT_IDLE, PVR_MIMUS_SLOT_SUBMITTED, PVR_MIMUS_SLOT_TAKEN };
/* The longest hang bound bind accepts, from the kick: one third of the
 * recognizer's 3 s service window. */
#define PVR_MIMUS_SERVICE_MAX_JOB_US UINT64_C(1000000)

struct pvr_mimus_service_config {
    /* Job and program pages, arena and output region, as pvr_mimus_session_open
     * takes them. last_sequence must be 0. */
    struct pvr_mimus_session_config session;
    /* Tables built and PREPARED over the session's mappings; published at OPEN. */
    struct a733_gpu_vm_memory *vm;
    struct pvr_mips_buffer firmware[PVR_MIMUS_SERVICE_OBJECTS];
    uint32_t firmware_address[PVR_MIMUS_SERVICE_OBJECTS];
    /* Write-back context-switch program pages, cleaned once at OPEN. */
    struct pvr_mips_buffer publish[PVR_MIMUS_SERVICE_PUBLISH];
    /* Indexed by tensor ordinal; job_rows 0 keeps a tensor on the CPU. The
     * runtime admits every entry with pvr_mimus_tensor_open before binding. */
    const struct pvr_mimus_tensor *tensors;
    uint32_t tensor_count;
    /* CDM resume program address for the compute command. */
    uint64_t resume;
    /* Hang bound from the kick, in microseconds, at most
     * PVR_MIMUS_SERVICE_MAX_JOB_US. */
    uint64_t max_job_us;
};
enum pvr_mimus_service_phase { PVR_MIMUS_SERVICE_NEW, PVR_MIMUS_SERVICE_BOUND,
    PVR_MIMUS_SERVICE_PUBLISHING, PVR_MIMUS_SERVICE_TABLES, PVR_MIMUS_SERVICE_PUBLISHED,
    PVR_MIMUS_SERVICE_FORMAT, PVR_MIMUS_SERVICE_IDLE, PVR_MIMUS_SERVICE_IN_FLIGHT,
    PVR_MIMUS_SERVICE_FAILED };
enum pvr_mimus_service_error { PVR_MIMUS_SERVICE_NO_ERROR, PVR_MIMUS_SERVICE_CLOCK,
    PVR_MIMUS_SERVICE_PUBLICATION, PVR_MIMUS_SERVICE_RING, PVR_MIMUS_SERVICE_TIMELINE,
    PVR_MIMUS_SERVICE_DEADLINE, PVR_MIMUS_SERVICE_RESET, PVR_MIMUS_SERVICE_MAINTENANCE,
    PVR_MIMUS_SERVICE_LIFECYCLE };
struct pvr_mimus_service {
    const struct pvr_mimus_service_config *config;
    struct pvr_gpu_service *service;
    struct pvr_mips_object objects[PVR_MIMUS_SERVICE_OBJECTS];
    struct pvr_mimus_session session;
    struct pvr_cccb client;
    unsigned char command[96], update[8];
    uint64_t deadline, epoch;
    uint32_t phase, error, sequence, publish_index, publish_page;
    /* A record is owed to the client: OPEN, or the job taken from the slot. */
    uint32_t owed;
    /* The worker's own copy of the job taken from the slot. */
    struct pvr_mimus_request job;
    /* Shared with the client: the slot and the lost flag. */
    atomic_uint slot_state, lost;
    struct pvr_mimus_request slot;
    QueueHandle_t completions;
    StaticQueue_t completion_state;
    uint8_t completion_storage[sizeof(struct pvr_mimus_record)];
};

/* Before the scheduler, at EL1: validate the configuration against the
 * service's firmware memory, create the completion queue and set
 * gc->mimus. No cache maintenance and no device access. */
int pvr_mimus_service_bind(struct pvr_mimus_service *, struct pvr_gpu_service *,
                           struct pvr_gpu_service_config *, const struct pvr_mimus_service_config *);

/* GPU service worker only (drivers/gpu/pvr_gpu_service.c). */
/* OPEN, before power: opens the session (writes and cleans the job and
 * program pages), writes the compute command and timeline 0. 1 accepted. */
int pvr_mimus_service_prepare(struct pvr_mimus_service *, uint64_t deadline);
/* One page per call: the context-switch programs, then the tables.
 * 1 published, 0 pending, -1 fault. */
int pvr_mimus_service_publish_poll(struct pvr_mimus_service *);
/* One READY step: formats the client ring after OPEN, takes and submits a
 * slot job up to its kick, or completes the job in flight. 0 or 1 normal,
 * -1 a fault the service must take (the job becomes LOST). */
int pvr_mimus_service_poll(struct pvr_mimus_service *);
/* 0 for a malformed event or a context reset, else 1. */
int pvr_mimus_service_event(struct pvr_mimus_service *, const unsigned char[88]);
/* The service failed: no further submit; an owed record becomes LOST. */
void pvr_mimus_service_abort(struct pvr_mimus_service *);
/* 1 while a job is in flight, with its deadline. */
int pvr_mimus_service_deadline(const struct pvr_mimus_service *, uint64_t *deadline);
/* 1 while a job waits in the slot or OPEN formats the ring. */
int pvr_mimus_service_submitting(const struct pvr_mimus_service *);
/* Progress identity for the service's burst decision. */
uint64_t pvr_mimus_service_progress(const struct pvr_mimus_service *);

/* Client task. Submit writes the slot and wakes the worker without waiting:
 * 1 submitted (a record follows), 0 refused with the slot IDLE again, -1 the
 * session is lost. Receive waits at most `wait` ticks for one record. */
int pvr_mimus_service_submit(struct pvr_mimus_service *, const struct pvr_mimus_request *);
int pvr_mimus_service_receive(struct pvr_mimus_service *, struct pvr_mimus_record *, TickType_t wait);
#endif
