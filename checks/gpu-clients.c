/* Actual application queue calls with modeled scheduler and worker effects. */
#include "pvr_compute_queue.h"
#include "pvr_gpu_service.h"
#include "gpu-share.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct host_queue { unsigned count; struct gpu_queue_record record; } owner_records,peer_records,requests;
static struct pvr_compute_queue q;
static struct pvr_gpu_service service;
static TaskHandle_t current=(void *)1;
static unsigned scheduler,ticks,phase=PVR_GPU_SERVICE_READY,operation,delay_reply,recover_at;
static uint64_t ticket_counter;
int xTaskGetSchedulerState(void){return scheduler;}
TaskHandle_t xTaskGetCurrentTaskHandle(void){return current;}
TickType_t xTaskGetTickCount(void){return ticks;}
void vTaskDelay(TickType_t n)
{
    ticks+=n;
    if (recover_at && ticks>=recover_at) {atomic_store(&q.recovering,0);phase=PVR_GPU_SERVICE_READY;}
}
BaseType_t xQueueSend(QueueHandle_t queue,const void *item,TickType_t wait)
{(void)queue;(void)item;(void)wait;return pdTRUE;}
BaseType_t xQueueReceive(QueueHandle_t queue,void *out,TickType_t wait)
{
    struct host_queue *h=queue;
    if (!h->count) {ticks+=wait;return pdFALSE;}
    memcpy(out,&h->record,sizeof(h->record));h->count=0;return pdTRUE;
}
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
    if (delay_reply) {ticks+=wait;--delay_reply;return 0;}
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
        if((state!=PVR_COMPUTE_QUEUE_IDLE&&state!=PVR_COMPUTE_QUEUE_CLOSED)||atomic_load(v.client_owed)||((struct host_queue *)v.completions)->count)return 0;
    }
    return 1;
}
static void initialize(void)
{
    q.service=&service;q.generation=7;q.create_done=1;q.lease_count=1;
    q.lease[0]=(struct gpu_queue_lease_info){.address=0x44000000,.bytes=4096};
    q.completions=&owner_records;q.peer.completions=&peer_records;
    q.slot_state=PVR_COMPUTE_QUEUE_IDLE;q.peer.slot_state=PVR_COMPUTE_QUEUE_IDLE;
    service.clients[0].task=(void *)1;service.worker=(void *)4;service.requests=&requests;
    scheduler=taskSCHEDULER_NOT_STARTED;
    assert(pvr_compute_queue_client_bind(&q));
}
#ifndef GPU_CLIENT_BASELINE
static void complete(unsigned client,unsigned sequence,unsigned result)
{
    struct pvr_compute_queue_client_view v;assert(pvr_compute_queue_client_view(&q,client,&v));
    struct host_queue *h=v.completions;
    assert(atomic_load(v.client_owed)&&!h->count);
    h->record=(struct gpu_queue_record){.sequence=sequence,.result=result};h->count=1;
    atomic_store(v.sequence,sequence);atomic_store(v.slot_state,PVR_COMPUTE_QUEUE_IDLE);
}
static void peer_entry(const struct ss_grant *g){(void)g;}
static void admission(void)
{
    struct ss_manifest owner={.magic=SS_MANIFEST_MAGIC,.version=1,.bytes=sizeof(owner),
        .name="owner",.devices_required=SS_DEVICE_GPU,.priority=2,.stack_bytes=4096,.region_count=1,
        .regions={{.address=0x44000000,.bytes=4096,.kind=SS_REGION_WORK,.access=SS_ACCESS_READ|SS_ACCESS_GPU_READ}}};
    struct ss_manifest peer=owner;memcpy(peer.name,"peer\0",5);
    struct gpu_queue_program program={0};
    struct ss_gpu_share share={&owner,&peer,peer_entry,&program,1,0};
    struct ss_grant grant={.devices=SS_DEVICE_GPU,.main_core=2,.worker_cores=8,.region_count=1},out;
    assert(os_gpu_share_admit(&share,&owner,&grant,&out));assert(out.main_core==3&&out.worker_cores==0&&out.devices==SS_DEVICE_GPU);
    for(unsigned i=0;i<12;++i){struct ss_gpu_share a=share;struct ss_manifest m=peer;struct ss_grant g=grant;a.manifest=&m;
        switch(i){case 0:g.worker_cores=0;break;case 1:g.worker_cores=1u<<31;break;case 2:g.worker_cores=1u<<2;break;
        case 3:m.devices_optional=SS_DEVICE_USB_STORAGE;break;case 4:m.regions[0].access|=SS_ACCESS_WRITE;break;
        case 5:a.shared_regions=0;break;case 6:m.stack_bytes=16392;break;case 7:m.workers=1;break;case 8:m.stack_bytes=8;break;case 9:m.stack_bytes=24;break;case 10:m.name[0]='A';break;case 11:m.name[6]='a';break;}
        assert(!os_gpu_share_admit(&a,&owner,&g,&out));
    }
}
#endif
int main(void)
{
    initialize();
#ifdef GPU_CLIENT_BASELINE
    scheduler=taskSCHEDULER_RUNNING;
    assert(gpu_queue_close((struct gpu_queue_context){.generation=7},10)==GPU_QUEUE_OK);
    assert(!atomic_load(&q.final_close));
    puts("PASS reversible close regression");
#else
    assert(!pvr_compute_queue_client_register(&q,(void *)1));
    assert(pvr_compute_queue_client_register(&q,(void *)2));
    assert(!pvr_compute_queue_client_register(&q,(void *)3));
    scheduler=taskSCHEDULER_RUNNING;
    struct gpu_queue_context owner={7,0},peer;
    struct gpu_queue_lease_info info[GPU_QUEUE_LEASES];
    current=(void *)3;assert(gpu_queue_context_join(&peer,info)==GPU_QUEUE_REFUSED);
    current=(void *)2;assert(gpu_queue_context_join(&peer,info)==GPU_QUEUE_OK&&peer.client==1);
    assert(gpu_queue_close(peer,10)==GPU_QUEUE_REFUSED);
    struct gpu_queue_job job={.sequence=1,.bindings=1,.groups={1,1,1}};
    assert(gpu_queue_submit(owner,&job)==GPU_QUEUE_REFUSED);
    assert(gpu_queue_submit((struct gpu_queue_context){8,1},&job)==GPU_QUEUE_REFUSED);
    assert(gpu_queue_submit(peer,&job)==GPU_QUEUE_OK);
    current=(void *)1;assert(gpu_queue_submit(peer,&job)==GPU_QUEUE_REFUSED);
    assert(gpu_queue_submit(owner,&job)==GPU_QUEUE_OK);
    assert(q.slot.sequence==1&&q.peer.slot.sequence==1);
    complete(0,1,GPU_QUEUE_OK);
    struct gpu_queue_record record;
    assert(gpu_queue_wait(owner,&record,0)==GPU_QUEUE_OK&&record.sequence==1);
    assert(gpu_queue_close(owner,10)==GPU_QUEUE_BUSY);
    complete(1,1,GPU_QUEUE_OK);
    current=(void *)2;assert(gpu_queue_wait(peer,&record,0)==GPU_QUEUE_OK&&record.sequence==1);
    current=(void *)1;
    job.sequence=2;
    for(unsigned cycle=0;cycle<10;++cycle){
        assert(gpu_queue_close(owner,10)==GPU_QUEUE_OK&&!atomic_load(&q.final_close));
        assert(gpu_queue_submit(owner,&job)==GPU_QUEUE_PARKED);
        assert(gpu_queue_open(owner,10)==GPU_QUEUE_OK);
    }
    job.sequence=2;assert(gpu_queue_submit(owner,&job)==GPU_QUEUE_OK);
    complete(0,2,GPU_QUEUE_FAULT);q.recovering=1;recover_at=ticks+3;
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
    return 0;
}
