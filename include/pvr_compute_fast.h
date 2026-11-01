#ifndef SUNSTONE_PVR_COMPUTE_FAST_H
#define SUNSTONE_PVR_COMPUTE_FAST_H
#include <stdatomic.h>
#include "gpu_queue.h"
#include "FreeRTOS.h"
#include "task.h"

#define PVR_COMPUTE_FAST_DEPTH 4u
#define PVR_COMPUTE_FAST_NOTIFY 1u
#define PVR_COMPUTE_FAST_STRIDE 512u
#define PVR_COMPUTE_FAST_PENDING 7
struct pvr_compute_queue;
struct pvr_compute_fast_entry {
    struct gpu_queue_job job;
    uint64_t kicked, deadline;
    uint32_t serial, client, fault;
};
struct pvr_compute_fast {
    /* Task-only transport lock. ISR never acquires it. */
    atomic_uint locked, enabled, submitted, completed, irq_error, fault_claimed;
    atomic_uint waiter[GPU_QUEUE_CLIENTS];
    atomic_uint lock_waiter[GPU_QUEUE_CLIENTS];
    atomic_uint pending[GPU_QUEUE_CLIENTS];
    atomic_uint fault_through[GPU_QUEUE_CLIENTS];
    atomic_uint_fast64_t done_us[PVR_COMPUTE_FAST_DEPTH];
#ifdef SUNSTONE_GPU_FAST_TIMING
    atomic_uint_fast64_t irq_counter[PVR_COMPUTE_FAST_DEPTH],last_irq[GPU_QUEUE_CLIENTS],last_wake[GPU_QUEUE_CLIENTS];
#endif
    uint32_t fault_observed, fault_submitted, fault_line;
    TaskHandle_t task[GPU_QUEUE_CLIENTS]; /* Immutable after CREATE. */
    struct pvr_compute_fast_entry entry[PVR_COMPUTE_FAST_DEPTH];
    struct {uint64_t binding0;uint32_t groups[3];} payload[PVR_COMPUTE_FAST_DEPTH];
    unsigned head, used;
    uint64_t last_now;
};
#ifdef SUNSTONE_GPU_FAST_TIMING
#define PVR_COMPUTE_SNAPSHOT(q,w,t) pvr_compute_fast_snapshot((q),(w),(t))
#else
#define PVR_COMPUTE_SNAPSHOT(q,w,t) pvr_compute_fast_snapshot((q),(w))
#endif
int pvr_compute_fast_lock(struct pvr_compute_queue *);
void pvr_compute_fast_unlock(struct pvr_compute_queue *);
/* Called only after device IRQ acknowledgement completed. The ISR variant
 * uses FromISR notification APIs; neither operation takes the transport lock. */
void pvr_compute_fast_snapshot(struct pvr_compute_queue *, BaseType_t *woken
#ifdef SUNSTONE_GPU_FAST_TIMING
    ,uint64_t irq_counter
#endif
    );
void pvr_compute_fast_ready(struct pvr_compute_queue *);
void pvr_compute_fast_seal(struct pvr_compute_queue *);
int pvr_compute_fast_submit(struct pvr_compute_queue *, unsigned, const struct gpu_queue_job *, uint32_t *);
int pvr_compute_fast_collect(struct pvr_compute_queue *,unsigned,uint32_t serial,uint32_t sequence,
    struct gpu_queue_record *);
int pvr_compute_fast_poll(struct pvr_compute_queue *);
int pvr_compute_fast_recovered(struct pvr_compute_queue *);
#endif
