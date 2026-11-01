/* Application calls use shared slots and the service's copied ticket path. */
#include "pvr_compute_queue.h"
#include "pvr_gpu_service.h"
#include "a733_range.h"
#include <string.h>

static struct pvr_compute_queue *instance;
__attribute__((weak)) int pvr_compute_queue_runtime_sync(void) { return 1; }
__attribute__((weak)) void pvr_compute_queue_runtime_opened(uint64_t epoch) { (void)epoch; }
__attribute__((weak)) void pvr_compute_queue_runtime_closed(void) { }
int pvr_compute_queue_client_bind(struct pvr_compute_queue *q)
{
    if (!q || instance || !q->service || xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED) return 0;
    instance=q; return 1;
}
static int caller(struct pvr_compute_queue *q)
{ return q && q->service->clients[0].task==xTaskGetCurrentTaskHandle(); }
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
    if (!caller(q) || !c || !out || !info) return GPU_QUEUE_REFUSED;
    if (atomic_load(&q->lost)) return GPU_QUEUE_FAULT;
    if (q->final_close || q->create_done) return GPU_QUEUE_REFUSED;
    if (!q->create_pending) {
        if (!c->program || !c->lease_count || c->lease_count>GPU_QUEUE_LEASES || c->reserved) return GPU_QUEUE_REFUSED;
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
    *out=(struct gpu_queue_context){q->generation};
    memcpy(info,q->lease,q->lease_count*sizeof(*info));
    return GPU_QUEUE_OK;
}
static int context(struct pvr_compute_queue *q,struct gpu_queue_context c)
{
    if (!caller(q)) return GPU_QUEUE_REFUSED;
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
    if (atomic_load(&q->client_owed)) return GPU_QUEUE_BUSY;
    if (q->final_close) return GPU_QUEUE_PARKED;
    int live=pvr_compute_queue_runtime_sync();
    if (live<0) return GPU_QUEUE_FAULT;
    if (!live) return GPU_QUEUE_PARKED;
    if (!job || !job->bindings || job->bindings>GPU_QUEUE_BINDINGS || job->reserved ||
        job->argument_bytes>GPU_QUEUE_ARGUMENT_BYTES || !job->groups[0] || !job->groups[1] || !job->groups[2])
        return GPU_QUEUE_REFUSED;
    if (job->sequence<=q->sequence || job->sequence==UINT32_MAX) return GPU_QUEUE_REFUSED;
    for (unsigned i=0;i<job->bindings;++i) {
        const struct gpu_queue_binding *b=&job->binding[i];
        if (b->lease>=q->lease_count || b->reserved) return GPU_QUEUE_REFUSED;
        const struct gpu_queue_lease_info *l=&q->lease[b->lease];
        if (b->address>=l->bytes || b->sync_offset>l->bytes || b->sync_bytes>l->bytes-b->sync_offset)
            return GPU_QUEUE_REFUSED;
    }
    unsigned idle=PVR_COMPUTE_QUEUE_IDLE;
    if (!atomic_compare_exchange_strong(&q->slot_state,&idle,PVR_COMPUTE_QUEUE_RESERVED))
        return idle==PVR_COMPUTE_QUEUE_CLOSED?GPU_QUEUE_PARKED:GPU_QUEUE_BUSY;
    q->slot=*job;
    q->not_after=job->not_after_us;
    atomic_store(&q->client_owed,1);
    atomic_store(&q->slot_state,PVR_COMPUTE_QUEUE_SUBMITTED);
    if (atomic_load(&q->lost)) {
        unsigned submitted=PVR_COMPUTE_QUEUE_SUBMITTED;
        if (atomic_compare_exchange_strong(&q->slot_state,&submitted,PVR_COMPUTE_QUEUE_CLOSED)) {
            atomic_store(&q->client_owed,0); return GPU_QUEUE_FAULT;
        }
    }
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
    /* An owed fault record must remain collectible after the lost flag. */
    if (!caller(q) || !out || c.generation!=q->generation) return GPU_QUEUE_REFUSED;
    if (!atomic_load(&q->client_owed)) return atomic_load(&q->lost)?GPU_QUEUE_FAULT:GPU_QUEUE_REFUSED;
    struct gpu_queue_record record;
    if (xQueueReceive(q->completions,&record,(TickType_t)ticks)!=pdTRUE) return GPU_QUEUE_TIMEOUT;
    atomic_store(&q->client_owed,0); *out=record;
    return record.result;
}
int gpu_queue_close(struct gpu_queue_context c,uint32_t ticks)
{
    struct pvr_compute_queue *q=instance;
    int r=context(q,c); if (r!=GPU_QUEUE_OK) return r;
    q->final_close=1;
    TickType_t began=xTaskGetTickCount(),budget=(TickType_t)ticks;
    if (atomic_load(&q->client_owed)) {
        struct gpu_queue_record record;
        r=gpu_queue_wait(c,&record,ticks);
        if (r==GPU_QUEUE_TIMEOUT || r==GPU_QUEUE_FAULT) return r;
    }
    for (;;) {
        struct pvr_gpu_service_reply reply;
        if (!q->close_ticket && !send(q,(q->close_pending || atomic_load(&q->slot_state)==PVR_COMPUTE_QUEUE_CLOSED)
                ? PVR_GPU_SERVICE_STATUS : PVR_GPU_SERVICE_CLOSE,&q->close_ticket))
            return GPU_QUEUE_BUSY;
        if (!collect(q,&q->close_ticket,remaining(began,budget),&reply)) return GPU_QUEUE_TIMEOUT;
        if (reply.result==PVR_GPU_SERVICE_FAULT) return GPU_QUEUE_FAULT;
        if (reply.result==PVR_GPU_SERVICE_BUSY) return GPU_QUEUE_BUSY;
        if (reply.result!=PVR_GPU_SERVICE_OK) return GPU_QUEUE_REFUSED;
        if (reply.phase==PVR_GPU_SERVICE_STOPPED) {pvr_compute_queue_runtime_closed();return GPU_QUEUE_OK;}
        q->close_pending=1;
        if (!remaining(began,budget)) return GPU_QUEUE_TIMEOUT;
        vTaskDelay(1);
    }
}
