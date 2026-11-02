/* Actual application queue calls (drivers/gpu/gpu_queue.c) on the real FreeRTOS kernel
 * under the host shim (checks/host/). Tasks, completion queues, ticks and timeouts are the
 * kernel's. The GPU service and its worker are modeled at their boundary: the service
 * replies to requests and the worker's effects complete jobs and clear recovery. */
#include "pvr_compute_queue.h"
#include "pvr_gpu_service.h"
#include "gpu-share.h"
#include "aarch64_device_memory.h"
#include "pvr_compute_fast.h"
#include "host_rtos.h"
#include "queue.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct pvr_compute_queue q;
static struct pvr_gpu_service service;
static QueueHandle_t requests_queue;
static StaticQueue_t requests_state,owner_state;
static uint8_t requests_storage[sizeof(struct pvr_gpu_service_request)],owner_storage[sizeof(struct gpu_queue_record)];
static TaskHandle_t owner_task,peer_task,stranger_task,worker_task;
static unsigned phase=PVR_GPU_SERVICE_READY,operation,delay_reply;
static uint64_t ticket_counter;
/* The modeled service leaves job_irq_ack unset, so the queued client path
 * must never reach the caller-submission fast path or EL0 translation, nor take its
 * fast-path notification index. */
uint32_t __real_ulTaskGenericNotifyTake(UBaseType_t index,BaseType_t clear,TickType_t wait);
uint32_t __wrap_ulTaskGenericNotifyTake(UBaseType_t index,BaseType_t clear,TickType_t wait)
{assert(index!=PVR_COMPUTE_FAST_NOTIFY&&"queued path took the fast-path notification");return __real_ulTaskGenericNotifyTake(index,clear,wait);}
int aarch64_device_translate_el0(uintptr_t address,int write,struct aarch64_translation *out)
{(void)address;(void)write;(void)out;assert(!"queued path translated an EL0 address");return -1;}
int pvr_compute_fast_submit(struct pvr_compute_queue *queue,unsigned slot,const struct gpu_queue_job *job,uint32_t *serial)
{(void)queue;(void)slot;(void)job;(void)serial;assert(!"queued path used fast submission");return -1;}
int pvr_compute_fast_collect(struct pvr_compute_queue *queue,unsigned slot,uint32_t serial,uint32_t sequence,
    struct gpu_queue_record *out)
{(void)queue;(void)slot;(void)serial;(void)sequence;(void)out;assert(!"queued path collected a fast fence");return -1;}
int pvr_gpu_service_bind(struct pvr_gpu_service *s,unsigned slot,TaskHandle_t task)
{
    if (!task || s->clients[slot].task || task==s->worker) return 0;
    for (unsigned i=0;i<PVR_GPU_SERVICE_CLIENTS;++i) if(s->clients[i].task==task)return 0;
    s->clients[slot].task=task;return 1;
}
int pvr_gpu_service_send_epoch(struct pvr_gpu_service *s,uint64_t epoch,
    const struct pvr_gpu_service_request *r,TickType_t wait,uint64_t *out)
{
    (void)s;(void)epoch;(void)wait;operation=r->operation;*out=++ticket_counter;return 1;
}
int pvr_gpu_service_collect(struct pvr_gpu_service *s,uint64_t ticket,
    struct pvr_gpu_service_reply *out,TickType_t wait)
{
    (void)s;(void)ticket;
    if (delay_reply) {vTaskDelay(wait);--delay_reply;return 0;}
    if(operation==PVR_GPU_SERVICE_CLOSE) {
        phase=PVR_GPU_SERVICE_STOPPED;
        atomic_store(&q.slot_state,PVR_COMPUTE_QUEUE_CLOSED);
        atomic_store(&q.peer.slot_state,PVR_COMPUTE_QUEUE_CLOSED);
    }
    if(operation==PVR_GPU_SERVICE_OPEN) {
        phase=PVR_GPU_SERVICE_READY;
        atomic_store(&q.slot_state,PVR_COMPUTE_QUEUE_IDLE);
        atomic_store(&q.peer.slot_state,PVR_COMPUTE_QUEUE_IDLE);
    }
    memset(out,0,sizeof(*out));out->result=PVR_GPU_SERVICE_OK;out->phase=phase;return 1;
}
int pvr_compute_queue_client_view(struct pvr_compute_queue *p,unsigned client,struct pvr_compute_queue_client_view *v)
{
    if(client==0)*v=(struct pvr_compute_queue_client_view){p->completions,&p->slot_state,&p->client_owed,&p->application_sequence,&p->not_after,&p->slot};
    else if(client==1&&p->peer.task)*v=(struct pvr_compute_queue_client_view){p->peer.completions,&p->peer.slot_state,&p->peer.client_owed,&p->peer.sequence,&p->peer.not_after,&p->peer.slot};
    else return 0;
    return 1;
}
int pvr_compute_queue_clients_idle(struct pvr_compute_queue *p)
{
    for(unsigned i=0;i<2;++i){struct pvr_compute_queue_client_view v;
        if(!pvr_compute_queue_client_view(p,i,&v))continue;
        unsigned state=atomic_load(v.slot_state);
        if((state!=PVR_COMPUTE_QUEUE_IDLE&&state!=PVR_COMPUTE_QUEUE_CLOSED)||atomic_load(v.client_owed)||uxQueueMessagesWaiting(v.completions))return 0;
    }
    return 1;
}

#ifndef GPU_CLIENT_BASELINE
/* Calls made as the peer and as a task that is registered for nothing. */
static struct gpu_queue_context peer_ctx,owner_ctx;
static struct gpu_queue_lease_info lease_info[GPU_QUEUE_LEASES];
static struct gpu_queue_job job;
static struct gpu_queue_record record;
static int rc;
static void peer_join(void *unused){(void)unused;rc=gpu_queue_context_join(&peer_ctx,lease_info);}
static void stranger_join(void *unused){(void)unused;struct gpu_queue_context c;rc=gpu_queue_context_join(&c,lease_info);}
static void peer_close(void *unused){(void)unused;rc=gpu_queue_close(peer_ctx,10);}
static void peer_submit_owner(void *unused){(void)unused;rc=gpu_queue_submit(owner_ctx,&job);}
static void peer_submit_stale(void *unused){(void)unused;rc=gpu_queue_submit((struct gpu_queue_context){8,1},&job);}
static void peer_submit(void *unused){(void)unused;rc=gpu_queue_submit(peer_ctx,&job);}
static void peer_wait(void *unused){(void)unused;rc=gpu_queue_wait(peer_ctx,&record,0);}
static void recovery_done(void *unused){(void)unused;atomic_store(&q.recovering,0);phase=PVR_GPU_SERVICE_READY;}
#endif
static void initialize(void)
{
    q.service=&service;q.generation=7;q.create_done=1;q.lease_count=1;
    q.lease[0]=(struct gpu_queue_lease_info){.address=0x44000000,.bytes=4096};
    q.completions=xQueueCreateStatic(1,sizeof(struct gpu_queue_record),owner_storage,&owner_state);
    requests_queue=xQueueCreateStatic(1,sizeof(struct pvr_gpu_service_request),requests_storage,&requests_state);
    q.slot_state=PVR_COMPUTE_QUEUE_IDLE;q.peer.slot_state=PVR_COMPUTE_QUEUE_IDLE;
    service.clients[0].task=owner_task;service.worker=worker_task;service.requests=requests_queue;
    assert(xTaskGetSchedulerState()==taskSCHEDULER_NOT_STARTED);
    assert(pvr_compute_queue_client_bind(&q));
}
#ifndef GPU_CLIENT_BASELINE
/* The worker's effect: it publishes the completion record, then returns the slot. */
static void complete(unsigned client,unsigned sequence,unsigned result)
{
    struct pvr_compute_queue_client_view v;assert(pvr_compute_queue_client_view(&q,client,&v));
    struct gpu_queue_record r={.sequence=sequence,.result=result};
    assert(atomic_load(v.client_owed)&&!uxQueueMessagesWaiting(v.completions));
    assert(xQueueSend(v.completions,&r,0)==pdTRUE);
    atomic_store(v.sequence,sequence);atomic_store(v.slot_state,PVR_COMPUTE_QUEUE_IDLE);
}
static void peer_entry(const struct ss_grant *g){(void)g;}
static void admission(void)
{
    typedef SS_MANIFEST_STORAGE(1) manifest_storage_t;
    manifest_storage_t owner_storage={.manifest={.magic=SS_MANIFEST_MAGIC,.version=SS_MANIFEST_VERSION,
        .bytes=SS_MANIFEST_HEAD_BYTES+sizeof(struct ss_region),
        .name="owner",.devices_required=SS_DEVICE_GPU,.priority=2,.stack_bytes=4096,.region_count=1}};
    struct ss_manifest *owner_ptr=&owner_storage.manifest,*owner=owner_ptr;(void)owner;
    owner_ptr->regions[0]=(struct ss_region){.address=0x44000000,.bytes=4096,.kind=SS_REGION_WORK,.access=SS_ACCESS_READ|SS_ACCESS_GPU_READ};
    manifest_storage_t peer_storage=owner_storage;
    struct ss_manifest *peer=&peer_storage.manifest;memcpy(peer->name,"peer\0",5);
    struct gpu_queue_program program={0};
    struct ss_gpu_share share={owner,peer,peer_entry,&program,1,0};
    static struct ss_span spans[1]={{(void *)0x44000000,4096}};
    struct ss_grant grant={.devices=SS_DEVICE_GPU,.main_core=2,.worker_cores=8,.region_count=1,.regions=spans},out;
    assert(os_gpu_share_admit(&share,owner,&grant,&out));assert(out.main_core==3&&out.worker_cores==0&&out.devices==SS_DEVICE_GPU);
    for(unsigned i=0;i<12;++i){struct ss_gpu_share a=share;manifest_storage_t m_storage=peer_storage;struct ss_manifest *m_ptr=&m_storage.manifest;struct ss_grant g=grant;a.manifest=m_ptr;
        switch(i){case 0:g.worker_cores=0;break;case 1:g.worker_cores=1u<<31;break;case 2:g.worker_cores=1u<<2;break;
        case 3:m_ptr->devices_optional=SS_DEVICE_USB_STORAGE;break;case 4:m_ptr->regions[0].access|=SS_ACCESS_WRITE;break;
        case 5:a.shared_regions=0;break;case 6:m_ptr->stack_bytes=16392;break;case 7:m_ptr->workers=1;break;case 8:m_ptr->stack_bytes=8;break;case 9:m_ptr->stack_bytes=24;break;case 10:m_ptr->name[0]='A';break;case 11:m_ptr->name[6]='a';break;}
        assert(!os_gpu_share_admit(&a,owner,&g,&out));
    }
}
#endif

static void test(void *unused)
{
    (void)unused;
    assert(xTaskGetSchedulerState()==taskSCHEDULER_RUNNING);
#ifdef GPU_CLIENT_BASELINE
    assert(gpu_queue_close((struct gpu_queue_context){.generation=7},10)==GPU_QUEUE_OK);
    assert(!atomic_load(&q.final_close));
    puts("PASS reversible close regression");
#else
    struct gpu_queue_context owner=owner_ctx=(struct gpu_queue_context){7,0};
    host_call(stranger_task,stranger_join,NULL);assert(rc==GPU_QUEUE_REFUSED);
    host_call(peer_task,peer_join,NULL);assert(rc==GPU_QUEUE_OK&&peer_ctx.client==1);
    host_call(peer_task,peer_close,NULL);assert(rc==GPU_QUEUE_REFUSED);
    job=(struct gpu_queue_job){.sequence=1,.bindings=1,.groups={1,1,1}};
    host_call(peer_task,peer_submit_owner,NULL);assert(rc==GPU_QUEUE_REFUSED);
    host_call(peer_task,peer_submit_stale,NULL);assert(rc==GPU_QUEUE_REFUSED);
    host_call(peer_task,peer_submit,NULL);assert(rc==GPU_QUEUE_OK);
    assert(gpu_queue_submit(peer_ctx,&job)==GPU_QUEUE_REFUSED); /* The owner may not use the peer context. */
    assert(gpu_queue_submit(owner,&job)==GPU_QUEUE_OK);
    assert(q.slot.sequence==1&&q.peer.slot.sequence==1);
    complete(0,1,GPU_QUEUE_OK);
    assert(gpu_queue_wait(owner,&record,0)==GPU_QUEUE_OK&&record.sequence==1);
    assert(gpu_queue_close(owner,10)==GPU_QUEUE_BUSY);
    complete(1,1,GPU_QUEUE_OK);
    host_call(peer_task,peer_wait,NULL);assert(rc==GPU_QUEUE_OK&&record.sequence==1);
    job.sequence=2;
    for(unsigned cycle=0;cycle<10;++cycle){
        assert(gpu_queue_close(owner,10)==GPU_QUEUE_OK&&!atomic_load(&q.final_close));
        assert(gpu_queue_submit(owner,&job)==GPU_QUEUE_PARKED);
        assert(gpu_queue_open(owner,10)==GPU_QUEUE_OK);
    }
    job.sequence=2;assert(gpu_queue_submit(owner,&job)==GPU_QUEUE_OK);
    complete(0,2,GPU_QUEUE_FAULT);q.recovering=1;
    host_run_at(host_time_us()+3000,recovery_done,NULL);
    assert(gpu_queue_recover(owner,1)==GPU_QUEUE_TIMEOUT);
    assert(atomic_load(&q.client_owed));
    assert(gpu_queue_recover(owner,10)==GPU_QUEUE_OK);
    assert(gpu_queue_wait(owner,&record,0)==GPU_QUEUE_FAULT&&record.sequence==2);
    assert(!atomic_load(&q.client_owed));
    delay_reply=1;assert(gpu_queue_close(owner,1)==GPU_QUEUE_TIMEOUT);
    assert(gpu_queue_open(owner,10)==GPU_QUEUE_BUSY);
    assert(gpu_queue_close(owner,10)==GPU_QUEUE_OK);
    assert(gpu_queue_open(owner,10)==GPU_QUEUE_OK);
    q.lost=1;q.client_owed=1;
    assert(gpu_queue_wait(owner,&record,0)==GPU_QUEUE_TIMEOUT&&atomic_load(&q.client_owed));
    assert(gpu_queue_recover(owner,10)==GPU_QUEUE_FAULT);
    admission();
    puts("PASS GPU clients: ten close/open cycles, task ownership, independent pending jobs, retained recovery record, timeout tickets, permanent isolation loss and explicit two-application admission");
#endif
}
int main(void)
{
    owner_task=host_rtos_prepare(test,NULL,2,1000000);
    peer_task=host_server_create("peer",2);
    stranger_task=host_server_create("stranger",2);
    worker_task=host_server_create("worker",2);
    initialize();
#ifndef GPU_CLIENT_BASELINE
    /* Registration happens before the scheduler starts. The owner cannot register as the
     * peer, one peer registers, and a second is refused; the peer queue is the
     * module's own. */
    assert(!pvr_compute_queue_client_register(&q,owner_task));
    assert(pvr_compute_queue_client_register(&q,peer_task));
    assert(q.peer.completions&&uxQueueGetQueueLength(q.peer.completions)==1);
    assert(!pvr_compute_queue_client_register(&q,stranger_task));
#endif
    host_rtos_start();
    return 0;
}
