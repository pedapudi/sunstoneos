/* Application calls use shared slots and the service's copied ticket path. */
#include "pvr_compute_queue.h"
#include "pvr_gpu_service.h"
#include "a733_range.h"
#include "sunstone_latency.h"
#include <string.h>

static struct pvr_compute_queue *instance;
__attribute__((weak)) int pvr_compute_queue_runtime_sync(void) { return 1; }
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
        if (!c->program || !pvr_compute_queue_runtime_program(c->program) || !c->lease_count || c->lease_count>GPU_QUEUE_LEASES || c->reserved) return GPU_QUEUE_REFUSED;
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
int gpu_queue_submit(struct gpu_queue_context c,const struct gpu_queue_job *job)
{
    struct pvr_compute_queue *q=instance;
    int r=context(q,c); if (r!=GPU_QUEUE_OK) return r;
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
int gpu_queue_wait(struct gpu_queue_context c,struct gpu_queue_record *out,uint32_t ticks)
{
    struct pvr_compute_queue *q=instance;
    struct pvr_compute_queue_client_view v;
    /* Recovery never consumes a caller's owed fault record. */
    if (!caller(q,c.client) || !out || c.generation!=q->generation ||
        !pvr_compute_queue_client_view(q,c.client,&v)) return GPU_QUEUE_REFUSED;
    if (!atomic_load(v.client_owed)) return atomic_load(&q->lost)?GPU_QUEUE_FAULT:GPU_QUEUE_REFUSED;
    struct gpu_queue_record record;
    if (xQueueReceive(v.completions,&record,(TickType_t)ticks)!=pdTRUE) return GPU_QUEUE_TIMEOUT;
    SS_LATENCY_GPU_RECEIVED(c.client);
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
        if (atomic_load(&q->client_owed)) {
            struct gpu_queue_record record;
            int r=gpu_queue_wait(c,&record,ticks);
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
