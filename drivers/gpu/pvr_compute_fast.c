/* Caller-owned compute rings and interrupt-owned fence publication. */
#include "pvr_compute_queue.h"
#include "pvr_gpu_service.h"
#include "a733_mmio.h"
#include "a733_gpu_queue.h"
#include "aarch64_device_memory.h"
#include "pvr_compute_job.h"
#include "pvr_gpu_crumb.h"
#include <string.h>

static int latch(struct pvr_compute_queue *q,unsigned error,uint32_t input,unsigned line)
{
    unsigned unclaimed=0;
    if (atomic_compare_exchange_strong_explicit(&q->fast.fault_claimed,&unclaimed,1,
            memory_order_acq_rel,memory_order_relaxed)) {
        q->fast.fault_observed=input;
        q->fast.fault_submitted=atomic_load_explicit(&q->fast.submitted,memory_order_acquire);
        q->fast.fault_line=line;
        atomic_store_explicit(&q->fast.irq_error,error,memory_order_release);
        return 1;
    }
    return 0;
}

int pvr_compute_fast_lock(struct pvr_compute_queue *q)
{
    unsigned unlocked=0;
    return atomic_compare_exchange_strong_explicit(&q->fast.locked,&unlocked,1,
        memory_order_acquire,memory_order_relaxed);
}
void pvr_compute_fast_unlock(struct pvr_compute_queue *q)
{
    atomic_store_explicit(&q->fast.locked,0,memory_order_release);
    uint32_t completed=atomic_load_explicit(&q->fast.completed,memory_order_acquire);
    for (unsigned i=0;i<GPU_QUEUE_CLIENTS;++i) {
        if (atomic_exchange_explicit(&q->fast.lock_waiter[i],0,memory_order_acq_rel))
            xTaskNotifyGiveIndexed(q->fast.task[i],PVR_COMPUTE_FAST_NOTIFY);
        unsigned target=atomic_load_explicit(&q->fast.waiter[i],memory_order_acquire);
        if (target && ((target<=completed && !atomic_load(&q->fast.irq_error)) ||
                target<=atomic_load_explicit(&q->fast.fault_through[i],memory_order_acquire)) &&
            atomic_compare_exchange_strong(&q->fast.waiter[i],&target,0))
            xTaskNotifyGiveIndexed(q->fast.task[i],PVR_COMPUTE_FAST_NOTIFY);
    }
}

void pvr_compute_fast_snapshot(struct pvr_compute_queue *q,BaseType_t *woken
#ifdef SUNSTONE_GPU_TIMING
    ,uint64_t irq_counter
#endif
    )
{
    if (!atomic_load_explicit(&q->fast.enabled,memory_order_acquire)) return;
    /* Completion condition: firmware UPDATE reaches the uncached timeline.
     * The device EVENT clear completes before this acquire, as pvr_device.c
     * processes timelines after clearing the MIPS interrupt. */
    uint32_t value=a733_mmio_read32_dsb_sy_after((uintptr_t)q->firmware[5].storage.cpu);
    uint32_t submitted=atomic_load_explicit(&q->fast.submitted,memory_order_acquire);
    uint32_t completed=atomic_load_explicit(&q->fast.completed,memory_order_relaxed);
    if (value<completed || value>submitted) {
        latch(q,GPU_QUEUE_TIMELINE,value,__LINE__);
        return;
    }
    if (value>completed) {
        uint64_t now;
        if (!q->service->config.job_now ||
            !q->service->config.job_now(q->service->config.context,&now) || value-completed>PVR_COMPUTE_FAST_DEPTH) {
            latch(q,GPU_QUEUE_SERVICE,value,__LINE__);
            return;
        }
        for (unsigned serial=completed;serial<value;++serial) {
            atomic_store_explicit(&q->fast.done_us[serial%PVR_COMPUTE_FAST_DEPTH],now,memory_order_relaxed);
#ifdef SUNSTONE_GPU_TIMING
            atomic_store_explicit(&q->fast.irq_counter[serial%PVR_COMPUTE_FAST_DEPTH],irq_counter,memory_order_relaxed);
#endif
        }
        atomic_store_explicit(&q->fast.completed,value,memory_order_release);
    }
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

static int failed(struct pvr_compute_queue *q,unsigned detail,uint32_t input,unsigned line)
{
    if (latch(q,detail,input,line)) {
        pvr_gpu_crumb_fail(PVR_GPU_CRUMB_QUEUE,detail,line,input,PVR_GPU_CRUMB_HERE());
    }
    q->error=detail;
    return GPU_QUEUE_FAULT;
}
#define FAILED(q,d,i) failed((q),(d),(i),__LINE__)
static int clock_now(struct pvr_compute_queue *q,uint64_t *now)
{
    if (!q->service->config.job_now(q->service->config.context,now) || *now<q->fast.last_now) return 0;
    q->fast.last_now=*now; return 1;
}
static int maintenance(struct pvr_compute_queue *q,const struct gpu_queue_job *job,int output)
{
    for (unsigned i=0;i<job->bindings;++i) {
        const struct gpu_queue_binding *b=&job->binding[i];
        const struct gpu_queue_lease_info *l=&q->lease[b->lease];
        if (!b->sync_bytes || !(l->access&(output?SS_ACCESS_GPU_WRITE:SS_ACCESS_WRITE))) continue;
        uint64_t first=b->sync_offset&~UINT64_C(63),end=((uint64_t)b->sync_offset+b->sync_bytes+63)&~UINT64_C(63);
        if (!aarch64_device_cache(output?AARCH64_CACHE_CLEAN_INVALIDATE:AARCH64_CACHE_CLEAN,
                (uintptr_t)(l->address+first),(size_t)(end-first))) return 0;
    }
    return 1;
}
static int conflicts(struct pvr_compute_queue *q,const struct gpu_queue_job *job)
{
    for (unsigned n=0;n<PVR_COMPUTE_FAST_DEPTH;++n) {
        const struct pvr_compute_fast_entry *e=&q->fast.entry[n];
        if (!e->serial) continue;
        for (unsigned i=0;i<job->bindings;++i) {
            const struct gpu_queue_binding *a=&job->binding[i];
            if (!(q->lease[a->lease].access&SS_ACCESS_GPU_WRITE)) continue;
            uint64_t af=a->sync_bytes?a->sync_offset:0,an=a->sync_bytes?a->sync_bytes:q->lease[a->lease].bytes;
            for (unsigned j=0;j<e->job.bindings;++j) {
                const struct gpu_queue_binding *b=&e->job.binding[j];
                if (a->lease!=b->lease) continue;
                uint64_t bf=b->sync_bytes?b->sync_offset:0,bn=b->sync_bytes?b->sync_bytes:q->lease[b->lease].bytes;
                if (af<bf+bn && bf<af+an) return 1;
            }
        }
    }
    return 0;
}
static unsigned valid(struct pvr_compute_queue *q,unsigned client,const struct gpu_queue_job *job)
{
    struct pvr_compute_queue_client_view v;
    if (!job || !pvr_compute_queue_client_view(q,client,&v) || !job->bindings ||
        job->bindings>GPU_QUEUE_BINDINGS || job->reserved || job->argument_bytes>GPU_QUEUE_ARGUMENT_BYTES ||
        !job->groups[0] || !job->groups[1] || !job->groups[2] || !job->sequence ||
        job->sequence==UINT32_MAX || job->sequence<=atomic_load(v.sequence) || q->timeline_serial==UINT32_MAX) return 0;
    for (unsigned i=0;i<job->bindings;++i) {
        const struct gpu_queue_binding *b=&job->binding[i];
        if (b->lease>=q->lease_count || b->reserved) return 0;
        const struct gpu_queue_lease_info *l=&q->lease[b->lease];
        if (b->address>=l->bytes || b->sync_offset>l->bytes || b->sync_bytes>l->bytes-b->sync_offset) return 0;
    }
    return 1;
}
int pvr_compute_fast_submit(struct pvr_compute_queue *q,unsigned client,const struct gpu_queue_job *job,uint32_t *fence)
{
    if (!pvr_compute_fast_lock(q)) return GPU_QUEUE_BUSY;
    int result=GPU_QUEUE_PARKED;
    if (atomic_load(&q->lost) || atomic_load(&q->fast.irq_error)) {result=GPU_QUEUE_FAULT;goto out;}
    if (!atomic_load_explicit(&q->fast.enabled,memory_order_acquire) || q->held || atomic_load(&q->close_pending) ||
        atomic_load(&q->recovering) || atomic_load(&q->final_close) || q->service->phase!=PVR_GPU_SERVICE_READY) goto out;
    if (!valid(q,client,job)) {result=GPU_QUEUE_REFUSED;goto out;}
    unsigned index=q->fast.head;
    if (q->fast.entry[index].serial || conflicts(q,job)) {result=GPU_QUEUE_BUSY;goto out;}
    uint64_t now;
    if (!clock_now(q,&now)) {result=FAILED(q,GPU_QUEUE_SERVICE,job->sequence);goto out;}
    if (job->not_after_us && now>=job->not_after_us) {result=GPU_QUEUE_REFUSED;goto out;}
    unsigned char *page=q->job_cpu+index*PVR_COMPUTE_FAST_STRIDE;
    uint64_t gpu=q->job_gpu+index*PVR_COMPUTE_FAST_STRIDE;
    uint64_t binding0=q->lease[job->binding[0].lease].address+job->binding[0].address;
    int pointer_changed=q->fast.payload[index].binding0!=binding0;
    int groups_changed=q->coefficients==7 && memcmp(q->fast.payload[index].groups,job->groups,sizeof(job->groups));
    if (pointer_changed) {
        *(uint64_t *)(page+96)=binding0;
        q->fast.payload[index].binding0=binding0;
    }
    if (groups_changed) {
        for (unsigned i=0;i<3;++i) *(uint32_t *)(page+48+4*i)=job->groups[i];
        memcpy(q->fast.payload[index].groups,job->groups,sizeof(job->groups));
    }
    unsigned char *args=page+PVR_COMPUTE_QUEUE_ARGUMENTS;
    for (unsigned i=1;i<job->bindings;++i) {
        uint64_t address=q->lease[job->binding[i].lease].address+job->binding[i].address;
        memcpy(args+8*(i-1),&address,8);
    }
    memcpy(args+8*(job->bindings-1),job->arguments,job->argument_bytes);
    uint32_t pds=(uint32_t)(gpu-UINT64_C(0xda00000000));
    pvr_compute_job_grid_cdm(page+PVR_COMPUTE_QUEUE_CDM,pds,pds+PVR_COMPUTE_QUEUE_FENCE,job->groups);
    unsigned bytes=8*(job->bindings-1)+job->argument_bytes;
    if (!maintenance(q,job,0) || (!q->payload_cache &&
            (((pointer_changed || groups_changed) && !aarch64_device_cache(AARCH64_CACHE_CLEAN,
                (uintptr_t)page+(groups_changed?0:64),groups_changed && pointer_changed?128:64)) ||
             !aarch64_device_cache(AARCH64_CACHE_CLEAN,(uintptr_t)page+PVR_COMPUTE_QUEUE_CDM,
                (PVR_COMPUTE_QUEUE_ARGUMENTS-PVR_COMPUTE_QUEUE_CDM+bytes+63)&~63u)))) {
        result=FAILED(q,GPU_QUEUE_MAINTENANCE,job->sequence);goto out;
    }
    if (!clock_now(q,&now) || UINT64_MAX-now<q->job_us) {result=FAILED(q,GPU_QUEUE_SERVICE,job->sequence);goto out;}
    if (job->not_after_us && now>=job->not_after_us) {result=GPU_QUEUE_REFUSED;goto out;}
    unsigned char command[96]={0},update[8];
    pvr_compute_job_command(command,0,gpu+PVR_COMPUTE_QUEUE_CDM,q->resume);
    uint32_t serial=q->timeline_serial+1;
    memcpy(update,&q->firmware_address[5],4);memcpy(update+4,&serial,4);
    const struct pvr_cccb_command commands[2]={
        {PVR_CCB_MAGIC|PVR_CCCB_TASK|205u,96,job->sequence,serial,command},
        {PVR_CCB_MAGIC|213u,8,0,0,update}};
    struct pvr_compute_fast_entry *e=&q->fast.entry[index];
    *e=(struct pvr_compute_fast_entry){.job=*job,.kicked=now,.deadline=now+q->job_us,.serial=serial,.client=client};
    /* Publish all fence ownership before the KICK: firmware may complete
     * and interrupt on another core before this call returns. */
    atomic_store_explicit(&q->fast.submitted,serial,memory_order_release);
    enum pvr_cccb_result sent=pvr_cccb_try_submit(&q->client,commands,2,now,e->deadline);
    if (sent==PVR_CCCB_AGAIN || sent==PVR_CCCB_INVALID) {
        e->serial=0;atomic_store_explicit(&q->fast.submitted,q->timeline_serial,memory_order_release);
        result=sent==PVR_CCCB_AGAIN?GPU_QUEUE_BUSY:GPU_QUEUE_REFUSED;goto out;
    }
    /* An uncertain doorbell retains this entry for isolation and recovery. */
    q->fast.head=(index+1)%PVR_COMPUTE_FAST_DEPTH;++q->fast.used;
    q->timeline_serial=serial;q->sequence=job->sequence;q->active_client=client;q->owed=1;
    q->phase=PVR_COMPUTE_QUEUE_IN_FLIGHT;q->kicked=now;q->deadline=e->deadline;
    struct pvr_compute_queue_client_view v;
    (void)pvr_compute_queue_client_view(q,client,&v);
    atomic_store(v.sequence,job->sequence);
    if (!atomic_fetch_add(v.client_owed,1)) atomic_store_explicit(&q->fast.pending[client],serial,memory_order_release);
    atomic_store(v.slot_state,PVR_COMPUTE_QUEUE_TAKEN);
    *fence=serial;
    result=sent==PVR_CCCB_OK?GPU_QUEUE_OK:FAILED(q,GPU_QUEUE_SERVICE,job->sequence);
out:
    pvr_compute_fast_unlock(q);return result;
}
int pvr_compute_fast_collect(struct pvr_compute_queue *q,unsigned client,uint32_t serial,uint32_t sequence,
    struct gpu_queue_record *out)
{
    if (!pvr_compute_fast_lock(q)) return GPU_QUEUE_BUSY;
    if (atomic_load(&q->lost)) {pvr_compute_fast_unlock(q);return GPU_QUEUE_FAULT;}
    struct pvr_compute_fast_entry *e=NULL;
    for (unsigned i=0;i<PVR_COMPUTE_FAST_DEPTH;++i)
        if (q->fast.entry[i].serial && q->fast.entry[i].client==client &&
            (!serial || (q->fast.entry[i].serial==serial && q->fast.entry[i].job.sequence==sequence)) &&
            (!e || q->fast.entry[i].serial<e->serial)) e=&q->fast.entry[i];
    int result=GPU_QUEUE_REFUSED;
    if (!e) goto done;
    if (!e->fault && (atomic_load(&q->fast.irq_error) || atomic_load(&q->recovering))) {result=PVR_COMPUTE_FAST_PENDING;goto done;}
    if (!e->fault && e->serial>atomic_load_explicit(&q->fast.completed,memory_order_acquire)) {result=PVR_COMPUTE_FAST_PENDING;goto done;}
    uint64_t completed=atomic_load_explicit(&q->fast.done_us[(e->serial-1)%PVR_COMPUTE_FAST_DEPTH],memory_order_relaxed);
    if (!e->fault && (completed<e->kicked || completed>=e->deadline)) {
        FAILED(q,GPU_QUEUE_HANG,e->serial);result=PVR_COMPUTE_FAST_PENDING;goto done;
    }
    if (!e->fault && !maintenance(q,&e->job,1)) {FAILED(q,GPU_QUEUE_MAINTENANCE,e->serial);result=PVR_COMPUTE_FAST_PENDING;goto done;}
    result=e->fault?GPU_QUEUE_FAULT:GPU_QUEUE_OK;
    *out=(struct gpu_queue_record){e->job.sequence,(uint16_t)result,(uint16_t)e->fault,
        e->fault?0:(uint32_t)(completed-e->kicked),0};
#ifdef SUNSTONE_GPU_TIMING
    atomic_store_explicit(&q->fast.last_irq[client],e->fault?0:
        atomic_load_explicit(&q->fast.irq_counter[(e->serial-1)%PVR_COMPUTE_FAST_DEPTH],memory_order_relaxed),memory_order_release);
#endif
    if (!e->fault) ++q->completed_jobs;
    e->serial=0;--q->fast.used;
    if (!q->fast.used && !atomic_load(&q->recovering) &&
        (q->phase==PVR_COMPUTE_QUEUE_IN_FLIGHT || q->phase==PVR_COMPUTE_QUEUE_READY)) {
        q->phase=PVR_COMPUTE_QUEUE_READY;q->owed=0;
    }
    uint32_t next=0;
    for (unsigned i=0;i<PVR_COMPUTE_FAST_DEPTH;++i)
        if (q->fast.entry[i].serial && q->fast.entry[i].client==client &&
            (!next || q->fast.entry[i].serial<next)) next=q->fast.entry[i].serial;
    atomic_store_explicit(&q->fast.pending[client],next,memory_order_release);
    struct pvr_compute_queue_client_view v;
    (void)pvr_compute_queue_client_view(q,client,&v);
    if (atomic_fetch_sub(v.client_owed,1)==1) atomic_store(v.slot_state,
        atomic_load(&q->fast.enabled)?PVR_COMPUTE_QUEUE_IDLE:PVR_COMPUTE_QUEUE_CLOSED);
done:
    pvr_compute_fast_unlock(q);return result;
}
int pvr_compute_fast_poll(struct pvr_compute_queue *q)
{
    unsigned error=atomic_load_explicit(&q->fast.irq_error,memory_order_acquire);
    if (error) {q->error=error;return -1;}
    uint32_t completed=atomic_load_explicit(&q->fast.completed,memory_order_acquire);
    uint64_t now;
    if (!clock_now(q,&now)) {FAILED(q,GPU_QUEUE_SERVICE,q->timeline_serial);return -1;}
    struct pvr_compute_fast_entry *first=NULL;
    for (unsigned i=0;i<PVR_COMPUTE_FAST_DEPTH;++i) {
        struct pvr_compute_fast_entry *e=&q->fast.entry[i];
        if (!e->serial || e->fault || e->serial<=completed) continue;
        if (!first || e->serial<first->serial) first=e;
    }
    if (!first) {q->phase=PVR_COMPUTE_QUEUE_READY;q->owed=0;return 0;}
    q->phase=PVR_COMPUTE_QUEUE_IN_FLIGHT;q->deadline=first->deadline;q->kicked=first->kicked;q->owed=1;
    if (now>=first->deadline) {FAILED(q,GPU_QUEUE_HANG,first->serial);return -1;}
    return 0;
}
int pvr_compute_fast_recovered(struct pvr_compute_queue *q)
{
    pvr_compute_fast_seal(q);
    for (unsigned i=0;i<PVR_COMPUTE_FAST_DEPTH;++i) {
        struct pvr_compute_fast_entry *e=&q->fast.entry[i];
        if (e->serial) e->fault=q->error?q->error:GPU_QUEUE_RESET;
    }
    for (unsigned i=0;i<GPU_QUEUE_CLIENTS;++i) {
        atomic_store_explicit(&q->fast.fault_through[i],q->timeline_serial,memory_order_release);
        atomic_store(&q->fast.waiter[i],0);
        if (q->fast.task[i]) xTaskNotifyGiveIndexed(q->fast.task[i],PVR_COMPUTE_FAST_NOTIFY);
    }
    atomic_store(&q->fast.irq_error,0);
    atomic_store(&q->fast.fault_claimed,0);
    q->reset_pending=0;q->held=0;q->owed=0;q->phase=PVR_COMPUTE_QUEUE_READY;
    return 1;
}
