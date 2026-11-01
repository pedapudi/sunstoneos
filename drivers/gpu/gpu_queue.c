/* Application calls use shared slots and the service's copied ticket path. */
#include "pvr_compute_queue.h"
#include "pvr_gpu_service.h"
#include "a733_range.h"
#include "aarch64_device_memory.h"
#include "pvr_gpu_call.h"
#include <string.h>

static struct pvr_compute_queue *instance;
__attribute__((weak)) int pvr_compute_queue_runtime_sync(void) { return 1; }
__attribute__((weak)) int pvr_compute_queue_runtime_admit(void) { return 1; }
__attribute__((weak)) int pvr_compute_queue_runtime_program(const struct gpu_queue_program *p) { (void)p;return 1; }
__attribute__((weak)) void pvr_compute_queue_runtime_opened(uint64_t epoch) { (void)epoch; }
__attribute__((weak)) void pvr_compute_queue_runtime_closed(void) { }
int pvr_compute_queue_client_bind(struct pvr_compute_queue *q)
{
    if (!q || instance || !q->service || xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED) return 0;
    instance=q; return 1;
}
int pvr_compute_queue_client_register(struct pvr_compute_queue *q,TaskHandle_t task)
{
    if (!q || q!=instance || !task || q->peer.task ||
        xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED) return 0;
    if (!q->peer.completions) {
        q->peer.completions=xQueueCreateStatic(1,sizeof(struct gpu_queue_record),
            q->peer.completion_storage,&q->peer.completion_state);
        if (!q->peer.completions) return 0;
    }
    if (!pvr_gpu_service_bind(q->service,2,task)) return 0;
    q->peer.task=task; return 1;
}
static int caller(struct pvr_compute_queue *q,unsigned client)
{
    if (!q || client>=GPU_QUEUE_CLIENTS || xTaskGetSchedulerState()!=taskSCHEDULER_RUNNING) return 0;
    TaskHandle_t expected=client?q->peer.task:q->service->clients[0].task;
    return expected && expected==xTaskGetCurrentTaskHandle();
}
static TickType_t remaining(TickType_t began,TickType_t budget)
{ TickType_t elapsed=xTaskGetTickCount()-began; return elapsed>=budget ? 0 : budget-elapsed; }
static int collect(struct pvr_compute_queue *q,uint64_t *ticket,TickType_t wait,struct pvr_gpu_service_reply *reply)
{
    if (!*ticket) return 0;
    if (!pvr_gpu_service_collect(q->service,*ticket,reply,wait)) return 0;
    *ticket=0; return 1;
}
static int send(struct pvr_compute_queue *q,unsigned operation,uint64_t *ticket)
{
    const struct pvr_gpu_service_request r={(enum pvr_gpu_service_operation)operation,
        operation==PVR_GPU_SERVICE_STATUS?0:UINT64_C(3000000)};
    return pvr_gpu_service_send_epoch(q->service,atomic_load_explicit(&q->service->published_epoch,memory_order_acquire),
        &r,0,ticket);
}
static int creation(struct pvr_compute_queue *q,TickType_t wait)
{
    TickType_t began=xTaskGetTickCount();
    if (q->create_ticket) {
        struct pvr_gpu_service_reply r;
        if (!collect(q,&q->create_ticket,wait,&r)) return GPU_QUEUE_TIMEOUT;
        if (r.result!=PVR_GPU_SERVICE_OK) {
            q->create_done=2;
            return r.result==PVR_GPU_SERVICE_FAULT ? GPU_QUEUE_FAULT : GPU_QUEUE_REFUSED;
        }
        q->create_pending=2;
    }
    if (q->create_done) return q->create_done==1 ? GPU_QUEUE_OK : GPU_QUEUE_REFUSED;
    if (q->create_pending!=2) return GPU_QUEUE_REFUSED;
    struct gpu_queue_record record;
    if (xQueueReceive(q->completions,&record,remaining(began,wait))!=pdTRUE)
        return atomic_load(&q->lost)?GPU_QUEUE_FAULT:GPU_QUEUE_TIMEOUT;
    if (record.result!=GPU_QUEUE_OK || record.sequence) return GPU_QUEUE_FAULT;
    q->create_done=1;
    pvr_compute_queue_runtime_opened(atomic_load_explicit(&q->service->published_epoch,memory_order_acquire));
    return GPU_QUEUE_OK;
}
int gpu_queue_context_create(const struct gpu_queue_context_config *c,uint32_t ticks,
    struct gpu_queue_context *out,struct gpu_queue_lease_info *info)
{
    struct pvr_compute_queue *q=instance;
    if (!caller(q,0) || !c || !out || !info) return GPU_QUEUE_REFUSED;
    if (atomic_load(&q->lost)) return GPU_QUEUE_FAULT;
    if (q->final_close || q->create_done) return GPU_QUEUE_REFUSED;
    if (!q->create_pending) {
        if (!c->program || !pvr_compute_queue_runtime_program(c->program) || !c->lease_count || c->lease_count>GPU_QUEUE_LEASES || c->reserved || c->priority>GPU_QUEUE_PRIORITY_HIGH || c->payload_cache>GPU_QUEUE_PAYLOAD_UNCACHED) return GPU_QUEUE_REFUSED;
        q->create_program=*c->program;
        q->create_config=*c;
        q->create_config.program=&q->create_program;
        q->create_pending=1;
        if (!send(q,PVR_GPU_SERVICE_QUEUE_CREATE,&q->create_ticket)) {
            q->create_pending=0; return GPU_QUEUE_BUSY;
        }
    }
    int result=creation(q,(TickType_t)ticks);
    if (result!=GPU_QUEUE_OK) return result;
    *out=(struct gpu_queue_context){q->generation,0};
    memcpy(info,q->lease,q->lease_count*sizeof(*info));
    return GPU_QUEUE_OK;
}
static int context(struct pvr_compute_queue *q,struct gpu_queue_context c)
{
    if (!caller(q,c.client)) return GPU_QUEUE_REFUSED;
    if (c.generation!=q->generation) return GPU_QUEUE_REFUSED;
    if (atomic_load(&q->lost)) return GPU_QUEUE_FAULT;
    if (!q->create_done && q->create_pending) {
        int r=creation(q,0); if (r!=GPU_QUEUE_OK) return r;
    }
    return q->create_done==1 && c.generation==q->generation ? GPU_QUEUE_OK : GPU_QUEUE_REFUSED;
}
/* Fixed same-task kernel entry. The argument stays on the caller's stack;
 * no function pointer or private GPU-memory pointer crosses the interface. */
extern int pvr_gpu_call(unsigned,void *) __attribute__((weak));
enum { FAST_SUBMIT=1, FAST_COLLECT=2, FAST_BATCH=3 };
struct fast_call {
    struct gpu_queue_context context;
    struct gpu_queue_job job;
    struct gpu_queue_record record;
    uint32_t fence,sequence;
};
struct fast_batch_call {
    struct gpu_queue_context context;
    struct gpu_queue_job job[GPU_QUEUE_DEPTH];
    uint32_t fence[GPU_QUEUE_DEPTH],count,accepted;
};
int pvr_gpu_call_dispatch(unsigned operation,void *argument)
{
    struct pvr_compute_queue *q=instance;
    if (operation!=FAST_SUBMIT && operation!=FAST_COLLECT && operation!=FAST_BATCH) return GPU_QUEUE_REFUSED;
    size_t bytes=operation==FAST_BATCH?sizeof(struct fast_batch_call):sizeof(struct fast_call);
    if (!argument || ((uintptr_t)argument&7) || !a733_span_excl_nonempty(argument,bytes)) return GPU_QUEUE_REFUSED;
    struct aarch64_translation translation;
    uintptr_t first=(uintptr_t)argument,last=first+bytes-1;
    /* EL0 write permission also admits reads. Validate both pages of this
     * fixed-size kernel argument; the transport's retained maps need no walk. */
    if (!aarch64_device_translate_el0(first,1,&translation) ||
        ((first>>12)!=(last>>12) && !aarch64_device_translate_el0(last,1,&translation))) return GPU_QUEUE_REFUSED;
    struct fast_call *a=argument;
    struct gpu_queue_context identity;
    memcpy(&identity,argument,sizeof(identity));
    int r=context(q,identity);
    if (r!=GPU_QUEUE_OK || !q->service->config.job_irq_ack) return r==GPU_QUEUE_OK?GPU_QUEUE_REFUSED:r;
    if (operation==FAST_COLLECT) return pvr_compute_fast_collect(q,a->context.client,a->fence,a->sequence,&a->record);
    int live=pvr_compute_queue_runtime_admit();
    if (live!=1) return live<0?GPU_QUEUE_FAULT:GPU_QUEUE_PARKED;
    if (operation==FAST_BATCH) {
        struct fast_batch_call *batch=argument;
        unsigned count=batch->count;
        if (!count || count>GPU_QUEUE_DEPTH) return GPU_QUEUE_REFUSED;
        batch->accepted=0;
        for (unsigned i=0;i<count;++i) {
            int result=pvr_compute_fast_submit(q,batch->context.client,&batch->job[i],&batch->fence[i]);
            if (result!=GPU_QUEUE_OK) {
                if (result==GPU_QUEUE_FAULT && batch->fence[i]) ++batch->accepted;
                return result;
            }
            ++batch->accepted;
        }
        return GPU_QUEUE_OK;
    }
    return pvr_compute_fast_submit(q,a->context.client,&a->job,&a->fence);
}
static int direct_submit(struct gpu_queue_context c,const struct gpu_queue_job *job,uint32_t *fence)
{
    if (!job) return GPU_QUEUE_REFUSED;
    if (!pvr_gpu_call) return GPU_QUEUE_REFUSED;
    struct fast_call call={.context=c,.job=*job};
    int r=pvr_gpu_call(FAST_SUBMIT,&call);
    if (fence && (r==GPU_QUEUE_OK || (r==GPU_QUEUE_FAULT && call.fence))) *fence=call.fence;
    return r;
}
static int direct_wait(struct pvr_compute_queue *q,struct gpu_queue_context c,
    struct gpu_queue_record *out,uint32_t ticks,uint32_t requested,uint32_t sequence)
{
    TickType_t begin=xTaskGetTickCount(),budget=ticks==GPU_QUEUE_WAIT_FOREVER?portMAX_DELAY:(TickType_t)ticks;
    int queried=0;
#ifdef SUNSTONE_GPU_FAST_TIMING
    atomic_store_explicit(&q->fast.last_wake[c.client],0,memory_order_relaxed);
#endif
    for (;;) {
        if (atomic_load(&q->lost)) return GPU_QUEUE_FAULT;
        uint32_t target=requested?requested:atomic_load_explicit(&q->fast.pending[c.client],memory_order_acquire);
        if (!target) return atomic_load(&q->lost)?GPU_QUEUE_FAULT:GPU_QUEUE_REFUSED;
        if (target<=atomic_load_explicit(&q->fast.completed,memory_order_acquire) ||
            !atomic_load_explicit(&q->fast.enabled,memory_order_acquire) || (requested && !queried)) {
            struct fast_call call={.context=c,.fence=requested,.sequence=sequence};
            int r=pvr_gpu_call(FAST_COLLECT,&call);
            if (r==PVR_COMPUTE_FAST_PENDING) queried=1;
            else if (r!=GPU_QUEUE_BUSY) {
                if (r==GPU_QUEUE_OK || (r==GPU_QUEUE_FAULT && call.record.sequence)) *out=call.record;
                return r;
            }
            if (r==GPU_QUEUE_BUSY) {
                if (!budget || (budget!=portMAX_DELAY && !remaining(begin,budget))) return GPU_QUEUE_TIMEOUT;
                (void)ulTaskNotifyTakeIndexed(PVR_COMPUTE_FAST_NOTIFY,pdTRUE,0);
                atomic_store_explicit(&q->fast.lock_waiter[c.client],1,memory_order_release);
                if (!atomic_load_explicit(&q->fast.locked,memory_order_acquire) || atomic_load(&q->lost)) {
                    atomic_store(&q->fast.lock_waiter[c.client],0);continue;
                }
                TickType_t wait=budget==portMAX_DELAY?portMAX_DELAY:remaining(begin,budget);
                if (!wait || !ulTaskNotifyTakeIndexed(PVR_COMPUTE_FAST_NOTIFY,pdTRUE,wait)) {
                    atomic_store(&q->fast.lock_waiter[c.client],0);return GPU_QUEUE_TIMEOUT;
                }
                atomic_store(&q->fast.lock_waiter[c.client],0);continue;
            }
        }
        if (!budget || (budget!=portMAX_DELAY && !remaining(begin,budget))) return GPU_QUEUE_TIMEOUT;
        /* Clear stale notifications BEFORE publishing the waiter, then
         * recheck. An IRQ in either gap cannot lose the wakeup. */
        (void)ulTaskNotifyTakeIndexed(PVR_COMPUTE_FAST_NOTIFY,pdTRUE,0);
        atomic_store_explicit(&q->fast.waiter[c.client],target,memory_order_release);
        if (atomic_load(&q->lost)) {atomic_store(&q->fast.waiter[c.client],0);return GPU_QUEUE_FAULT;}
        if (((!atomic_load_explicit(&q->fast.irq_error,memory_order_acquire) &&
              target<=atomic_load_explicit(&q->fast.completed,memory_order_acquire)) ||
             target<=atomic_load_explicit(&q->fast.fault_through[c.client],memory_order_acquire)) &&
            !atomic_load_explicit(&q->fast.locked,memory_order_acquire)) {
            atomic_store(&q->fast.waiter[c.client],0);continue;
        }
        TickType_t wait=budget==portMAX_DELAY?portMAX_DELAY:remaining(begin,budget);
        if (!wait || !ulTaskNotifyTakeIndexed(PVR_COMPUTE_FAST_NOTIFY,pdTRUE,wait)) {
            atomic_store(&q->fast.waiter[c.client],0);return GPU_QUEUE_TIMEOUT;
        }
#ifdef SUNSTONE_GPU_FAST_TIMING
        uint64_t resumed;
        __asm__ volatile("isb\n mrs %0,cntvct_el0" : "=r"(resumed) :: "memory");
        if (target<=atomic_load_explicit(&q->fast.completed,memory_order_acquire))
            atomic_store_explicit(&q->fast.last_wake[c.client],resumed,memory_order_release);
#endif
        atomic_store(&q->fast.waiter[c.client],0);
    }
}
int gpu_queue_submit(struct gpu_queue_context c,const struct gpu_queue_job *job)
{
    struct pvr_compute_queue *q=instance;
    int r=context(q,c); if (r!=GPU_QUEUE_OK) return r;
    if (q->service->config.job_irq_ack) return direct_submit(c,job,NULL);
    struct pvr_compute_queue_client_view v;
    if (!pvr_compute_queue_client_view(q,c.client,&v)) return GPU_QUEUE_REFUSED;
    if (atomic_load(v.client_owed)) return GPU_QUEUE_BUSY;
    if (atomic_load(&q->close_pending) || atomic_load(&q->recovering)) return GPU_QUEUE_PARKED;
    if (q->final_close) return GPU_QUEUE_PARKED;
    int live=pvr_compute_queue_runtime_sync();
    if (live<0) return GPU_QUEUE_FAULT;
    if (!live) return GPU_QUEUE_PARKED;
    if (!job || !job->bindings || job->bindings>GPU_QUEUE_BINDINGS || job->reserved ||
        job->argument_bytes>GPU_QUEUE_ARGUMENT_BYTES || !job->groups[0] || !job->groups[1] || !job->groups[2])
        return GPU_QUEUE_REFUSED;
    if (job->sequence<=atomic_load(v.sequence) || job->sequence==UINT32_MAX) return GPU_QUEUE_REFUSED;
    for (unsigned i=0;i<job->bindings;++i) {
        const struct gpu_queue_binding *b=&job->binding[i];
        if (b->lease>=q->lease_count || b->reserved) return GPU_QUEUE_REFUSED;
        const struct gpu_queue_lease_info *l=&q->lease[b->lease];
        if (b->address>=l->bytes || b->sync_offset>l->bytes || b->sync_bytes>l->bytes-b->sync_offset)
            return GPU_QUEUE_REFUSED;
    }
    unsigned idle=PVR_COMPUTE_QUEUE_IDLE;
    if (!atomic_compare_exchange_strong(v.slot_state,&idle,PVR_COMPUTE_QUEUE_RESERVED))
        return idle==PVR_COMPUTE_QUEUE_CLOSED?GPU_QUEUE_PARKED:GPU_QUEUE_BUSY;
    if (atomic_load(&q->close_pending) || atomic_load(&q->recovering) ||
        atomic_load(&q->final_close) || atomic_load(&q->lost)) {
        unsigned reserved=PVR_COMPUTE_QUEUE_RESERVED;
        (void)atomic_compare_exchange_strong(v.slot_state,&reserved,PVR_COMPUTE_QUEUE_IDLE);
        return atomic_load(&q->lost)?GPU_QUEUE_FAULT:GPU_QUEUE_PARKED;
    }
    *v.slot=*job;
    atomic_store(v.not_after,job->not_after_us);
    atomic_store(v.client_owed,1);
    atomic_store(v.slot_state,PVR_COMPUTE_QUEUE_SUBMITTED);
    if (!atomic_exchange(&q->wake,1)) {
        const struct pvr_gpu_service_message wake={.client=PVR_GPU_SERVICE_CLIENTS,.ticket=PVR_GPU_SERVICE_QUEUE_WAKE};
        /* One pending ticket per client, one IRQ wake and one queue wake fit. */
        configASSERT(xQueueSend(q->service->requests,&wake,0)==pdTRUE);
    }
    return GPU_QUEUE_OK;
}
int gpu_queue_submit_fence(struct gpu_queue_context c,const struct gpu_queue_job *job,struct gpu_queue_fence *out)
{
    if (!out) return GPU_QUEUE_REFUSED;
    *out=(struct gpu_queue_fence){0};
    struct pvr_compute_queue *q=instance;
    int r=context(q,c);
    if (r!=GPU_QUEUE_OK || !out) return r==GPU_QUEUE_OK?GPU_QUEUE_REFUSED:r;
    if (!q->service->config.job_irq_ack) return GPU_QUEUE_REFUSED;
    uint32_t serial=0;
    r=direct_submit(c,job,&serial);
    if (serial) *out=(struct gpu_queue_fence){c,serial,job->sequence};
    return r;
}
int gpu_queue_submit_batch(struct gpu_queue_context c,const struct gpu_queue_job *jobs,uint32_t count,
    struct gpu_queue_fence *out,uint32_t *accepted)
{
    if (!accepted) return GPU_QUEUE_REFUSED;
    *accepted=0;
    struct pvr_compute_queue *q=instance;
    int r=context(q,c);
    if (r!=GPU_QUEUE_OK) return r;
    if (!q->service->config.job_irq_ack || !pvr_gpu_call || !jobs || !out || !count || count>GPU_QUEUE_DEPTH) return GPU_QUEUE_REFUSED;
    struct fast_batch_call call={.context=c,.count=count};
    memcpy(call.job,jobs,count*sizeof(*jobs));
    r=pvr_gpu_call(FAST_BATCH,&call);
    for (unsigned i=0;i<call.accepted;++i) out[i]=(struct gpu_queue_fence){c,call.fence[i],jobs[i].sequence};
    *accepted=call.accepted;return r;
}
int gpu_queue_wait_fence(struct gpu_queue_fence fence,struct gpu_queue_record *out,uint32_t ticks)
{
    struct pvr_compute_queue *q=instance;
    if (!out || !fence.serial || !fence.sequence || !caller(q,fence.context.client) ||
        fence.context.generation!=q->generation || !q->service->config.job_irq_ack) return GPU_QUEUE_REFUSED;
    return direct_wait(q,fence.context,out,ticks,fence.serial,fence.sequence);
}
uint64_t gpu_queue_last_irq_counter(struct gpu_queue_context c)
{
#ifdef SUNSTONE_GPU_FAST_TIMING
    struct pvr_compute_queue *q=instance;
    if (!caller(q,c.client) || c.generation!=q->generation) return 0;
    return atomic_load_explicit(&q->fast.last_irq[c.client],memory_order_acquire);
#else
    (void)c;return 0;
#endif
}
uint64_t gpu_queue_last_wake_counter(struct gpu_queue_context c)
{
#ifdef SUNSTONE_GPU_FAST_TIMING
    struct pvr_compute_queue *q=instance;
    if (!caller(q,c.client) || c.generation!=q->generation) return 0;
    return atomic_load_explicit(&q->fast.last_wake[c.client],memory_order_acquire);
#else
    (void)c;return 0;
#endif
}
int gpu_queue_wait(struct gpu_queue_context c,struct gpu_queue_record *out,uint32_t ticks)
{
    struct pvr_compute_queue *q=instance;
    struct pvr_compute_queue_client_view v;
    /* Recovery never consumes a caller's owed fault record. */
    if (!caller(q,c.client) || !out || c.generation!=q->generation ||
        !pvr_compute_queue_client_view(q,c.client,&v)) return GPU_QUEUE_REFUSED;
    if (!atomic_load(v.client_owed)) return atomic_load(&q->lost)?GPU_QUEUE_FAULT:GPU_QUEUE_REFUSED;
    if (q->service->config.job_irq_ack) return direct_wait(q,c,out,ticks,0,0);
    struct gpu_queue_record record;
    if (xQueueReceive(v.completions,&record,(TickType_t)ticks)!=pdTRUE) return GPU_QUEUE_TIMEOUT;
    atomic_store(v.client_owed,0); *out=record;
    return record.result;
}
int gpu_queue_context_join(struct gpu_queue_context *out,struct gpu_queue_lease_info *info)
{
    struct pvr_compute_queue *q=instance;
    if (!caller(q,1) || !out || !info) return GPU_QUEUE_REFUSED;
    if (atomic_load(&q->lost)) return GPU_QUEUE_FAULT;
    if (atomic_load(&q->create_done)!=1) return GPU_QUEUE_BUSY;
    *out=(struct gpu_queue_context){q->generation,1};
    memcpy(info,q->lease,q->lease_count*sizeof(*info)); return GPU_QUEUE_OK;
}
static int lifetime(struct gpu_queue_context c,uint32_t ticks,unsigned operation)
{
    struct pvr_compute_queue *q=instance;
    if (!caller(q,0) || c.client || c.generation!=q->generation ||
        atomic_load(&q->create_done)!=1) return GPU_QUEUE_REFUSED;
    if (atomic_load(&q->lost)) return GPU_QUEUE_FAULT;
    if (atomic_load(&q->final_close)) return GPU_QUEUE_PARKED;
    unsigned transition=atomic_load(&q->close_pending);
    if (transition && (transition&255u)!=operation+1) return GPU_QUEUE_BUSY;
    atomic_store(&q->close_pending,transition?transition:operation+1);
    TickType_t began=xTaskGetTickCount(),budget=(TickType_t)ticks;
    if (operation==PVR_GPU_SERVICE_CLOSE) {
        while (atomic_load(&q->client_owed)) {
            struct gpu_queue_record record;
            int r=gpu_queue_wait(c,&record,(uint32_t)remaining(began,budget));
            if (r==GPU_QUEUE_TIMEOUT) return r;
            if (r==GPU_QUEUE_FAULT) {atomic_store(&q->close_pending,0);return r;}
        }
        if (!pvr_compute_queue_clients_idle(q)) return GPU_QUEUE_BUSY;
    }
    /* A pending ticket owns this lifecycle operation across caller timeouts.
     * Once accepted, STATUS observes its final phase without reissuing it. */
    for (;;) {
        struct pvr_gpu_service_reply reply;
        if (!q->close_ticket) {
            unsigned request=(transition&256u)?PVR_GPU_SERVICE_STATUS:
                (transition&512u)?operation:PVR_GPU_SERVICE_STATUS;
            if (!send(q,request,&q->close_ticket)) return GPU_QUEUE_BUSY;
            transition=operation+1+(request==operation?256u:(transition&256u)?256u:512u);
            atomic_store(&q->close_pending,transition);
        }
        if (!collect(q,&q->close_ticket,remaining(began,budget),&reply)) return GPU_QUEUE_TIMEOUT;
        if (reply.result==PVR_GPU_SERVICE_FAULT) {atomic_store(&q->close_pending,0);return GPU_QUEUE_FAULT;}
        if (reply.result==PVR_GPU_SERVICE_BUSY) {atomic_store(&q->close_pending,0);return GPU_QUEUE_BUSY;}
        if (reply.result!=PVR_GPU_SERVICE_OK) {atomic_store(&q->close_pending,0);return GPU_QUEUE_REFUSED;}
        if (reply.phase==(operation==PVR_GPU_SERVICE_CLOSE?PVR_GPU_SERVICE_STOPPED:PVR_GPU_SERVICE_READY)) {
            atomic_store(&q->close_pending,0);
            if (operation==PVR_GPU_SERVICE_CLOSE) pvr_compute_queue_runtime_closed();
            else pvr_compute_queue_runtime_opened(atomic_load_explicit(&q->service->published_epoch,memory_order_acquire));
            return GPU_QUEUE_OK;
        }
        transition=atomic_load(&q->close_pending);
        if (!remaining(began,budget)) return GPU_QUEUE_TIMEOUT;
        vTaskDelay(1);
    }
}
int gpu_queue_close(struct gpu_queue_context c,uint32_t ticks)
{return lifetime(c,ticks,PVR_GPU_SERVICE_CLOSE);}
int gpu_queue_open(struct gpu_queue_context c,uint32_t ticks)
{return lifetime(c,ticks,PVR_GPU_SERVICE_OPEN);}
int gpu_queue_recover(struct gpu_queue_context c,uint32_t ticks)
{
    struct pvr_compute_queue *q=instance;
    if (!caller(q,0) || c.client || c.generation!=q->generation ||
        atomic_load(&q->create_done)!=1) return GPU_QUEUE_REFUSED;
    TickType_t began=xTaskGetTickCount(),budget=(TickType_t)ticks;
    if (q->close_ticket) {
        struct pvr_gpu_service_reply reply;
        if (!collect(q,&q->close_ticket,budget,&reply)) return GPU_QUEUE_TIMEOUT;
    }
    atomic_store(&q->close_pending,0);
    for (;;) {
        if (!atomic_load(&q->recovering)) {
            if (atomic_load(&q->lost)) return GPU_QUEUE_FAULT;
            return lifetime(c,remaining(began,budget),PVR_GPU_SERVICE_OPEN);
        }
        if (!remaining(began,budget)) return GPU_QUEUE_TIMEOUT;
        vTaskDelay(1);
    }
}
