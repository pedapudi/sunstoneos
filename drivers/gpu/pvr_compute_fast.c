/* Caller-owned compute rings and interrupt-owned fence publication. */
#include "pvr_compute_queue.h"
#include "pvr_gpu_service.h"
#include "a733_mmio.h"

int pvr_compute_fast_lock(struct pvr_compute_queue *q)
{
    unsigned unlocked=0;
    return atomic_compare_exchange_strong_explicit(&q->fast.locked,&unlocked,1,
        memory_order_acquire,memory_order_relaxed);
}
void pvr_compute_fast_unlock(struct pvr_compute_queue *q)
{ atomic_store_explicit(&q->fast.locked,0,memory_order_release); }

void pvr_compute_fast_snapshot(struct pvr_compute_queue *q,BaseType_t *woken)
{
    if (!atomic_load_explicit(&q->fast.enabled,memory_order_acquire)) return;
    /* Completion condition: firmware UPDATE reaches the uncached timeline.
     * The device EVENT clear completes before this acquire, as pvr_device.c
     * processes timelines after clearing the MIPS interrupt. */
    uint32_t value=a733_mmio_read32_dsb_sy_after((uintptr_t)q->firmware[5].storage.cpu);
    uint32_t submitted=atomic_load_explicit(&q->fast.submitted,memory_order_acquire);
    uint32_t completed=atomic_load_explicit(&q->fast.completed,memory_order_relaxed);
    if (value<completed || value>submitted) {
        if (!atomic_load_explicit(&q->fast.irq_error,memory_order_relaxed)) {
            q->fast.fault_observed=value; q->fast.fault_submitted=submitted;
            atomic_store_explicit(&q->fast.irq_error,GPU_QUEUE_TIMELINE,memory_order_release);
        }
        return;
    }
    atomic_store_explicit(&q->fast.completed,value,memory_order_release);
    for (unsigned i=0;i<GPU_QUEUE_CLIENTS;++i) {
        unsigned target=atomic_load_explicit(&q->fast.waiter[i],memory_order_acquire);
        if (target && target<=value && atomic_compare_exchange_strong_explicit(
                &q->fast.waiter[i],&target,0,memory_order_acq_rel,memory_order_relaxed)) {
            if (woken) vTaskNotifyGiveIndexedFromISR(q->fast.task[i],PVR_COMPUTE_FAST_NOTIFY,woken);
            else xTaskNotifyGiveIndexed(q->fast.task[i],PVR_COMPUTE_FAST_NOTIFY);
        }
    }
}
void pvr_compute_fast_ready(struct pvr_compute_queue *q)
{
    q->fast.task[0]=q->service->clients[0].task;
    q->fast.task[1]=q->peer.task;
    atomic_store_explicit(&q->fast.submitted,q->timeline_serial,memory_order_release);
    atomic_store_explicit(&q->fast.completed,q->timeline_serial,memory_order_release);
    taskENTER_CRITICAL();
    atomic_store_explicit(&q->fast.enabled,1,memory_order_release);
    taskEXIT_CRITICAL();
}
void pvr_compute_fast_seal(struct pvr_compute_queue *q)
{
    /* Same SMP kernel critical section as the interrupt dispatcher: no
     * device ACK/timeline read can cross power retirement after this seal. */
    taskENTER_CRITICAL();
    atomic_store_explicit(&q->fast.enabled,0,memory_order_release);
    taskEXIT_CRITICAL();
}
