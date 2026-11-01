/* Single-context compute submission inside the privileged GPU worker. */
#include "pvr_compute_queue.h"
#include "pvr_gpu_crumb.h"
#include "pvr_gpu_service.h"
#include "a733_gpu_queue.h"
#include "a733_gpu_backend.h"
#include "pvr_gpu_events.h"
#include "pvr_compute_job.h"
#include "a733_mmio.h"
#include "a733_range.h"
#include "aarch64_device_memory.h"
#include "sunstone_crumb.h"
#include <string.h>

#define PAGE 4096u
#define LINE 64u
#define RING_BYTES 1024u
/* The image linker admits application code into this immutable section. */
extern const unsigned char __start_sunstone_gpu_program[], __stop_sunstone_gpu_program[];
enum { CONTEXT, STATE, CONTROL, RING, MEMORY, TIMELINE };
int pvr_compute_queue_client_view(struct pvr_compute_queue *q,unsigned client,
    struct pvr_compute_queue_client_view *v)
{
    if (!q || !v || client>1) return 0;
    if (!client) {
        *v=(struct pvr_compute_queue_client_view){q->completions,&q->slot_state,
            &q->client_owed,&q->application_sequence,&q->not_after,&q->slot};
    } else {
        struct pvr_compute_queue_client *p=&q->peer;
        *v=(struct pvr_compute_queue_client_view){p->completions,&p->slot_state,
            &p->client_owed,&p->sequence,&p->not_after,&p->slot};
    }
    return v->completions!=NULL;
}
int pvr_compute_queue_clients_idle(struct pvr_compute_queue *q)
{
    if (!q || !q->completions || (q->peer.task && !q->peer.completions)) return 0;
    for (unsigned i=0;i<2;++i) {
        struct pvr_compute_queue_client_view v;
        if (!pvr_compute_queue_client_view(q,i,&v)) continue;
        unsigned state=atomic_load(v.slot_state);
        if ((state!=PVR_COMPUTE_QUEUE_IDLE && state!=PVR_COMPUTE_QUEUE_CLOSED) ||
            atomic_load(v.client_owed) || uxQueueMessagesWaiting(v.completions)) return 0;
    }
    return 1;
}
static void put32(void *out,uint32_t v) { memcpy(out,&v,4); }
static void put64(void *out,uint64_t v) { memcpy(out,&v,8); }
static uint32_t timeline(const struct pvr_compute_queue *q)
{ return *(const volatile uint32_t *)q->firmware[TIMELINE].storage.cpu; }
static int ring_now(void *ctx,uint64_t *now)
{ return pvr_gpu_service_now(((struct pvr_compute_queue *)ctx)->service,now); }
static int ring_read(void *ctx,enum pvr_cccb_region r,uint32_t off,void *out,size_t n)
{
    struct pvr_compute_queue *q=ctx;
    if (r!=PVR_CCCB_CONTROL || n!=4 || (off&3) || off>=PVR_CCCB_CONTROL_BYTES) return 0;
    uint32_t v=a733_mmio_read32_dsb_sy_after((uintptr_t)q->firmware[CONTROL].storage.cpu+off);
    memcpy(out,&v,4); return 1;
}
static int ring_write(void *ctx,enum pvr_cccb_region r,uint32_t off,const void *in,size_t n)
{
    struct pvr_compute_queue *q=ctx;
    size_t limit=r==PVR_CCCB_CONTROL ? PVR_CCCB_CONTROL_BYTES : RING_BYTES;
    if ((unsigned)r>PVR_CCCB_COMMANDS || !n || off>limit || n>limit-off ||
        (r==PVR_CCCB_CONTROL && q->phase!=PVR_COMPUTE_QUEUE_FORMAT)) return 0;
    volatile unsigned char *d=(volatile unsigned char *)q->firmware[r==PVR_CCCB_CONTROL?CONTROL:RING].storage.cpu+off;
    const unsigned char *s=in;
    if (!((off|n)&7)) for (size_t i=0;i<n;i+=8) {
        uint64_t v; memcpy(&v,s+i,8); *(volatile uint64_t *)(d+i)=v;
    } else for (size_t i=0;i<n;++i) d[i]=s[i];
    return 1;
}
static int ring_barrier(void *ctx,enum pvr_ccb_barrier kind)
{ (void)ctx; if ((unsigned)kind>PVR_CCB_DEVICE_COMPLETE) return 0; a733_mmio_dsb_sy(); return 1; }
static int fault(struct pvr_compute_queue *q,unsigned detail)
{ q->error=detail; return -1; }
static int post(struct pvr_compute_queue *q,uint32_t sequence,unsigned result,unsigned detail,uint32_t duration)
{
    struct pvr_compute_queue_client_view v;
    if (!pvr_compute_queue_client_view(q,q->active_client,&v)) return fault(q,GPU_QUEUE_SERVICE);
    const struct gpu_queue_record r={sequence,(uint16_t)result,(uint16_t)detail,duration,0};
    /* Finish the claimed job before a client can wake. Publication failure
     * may retry with a fault record, but must not repeat this boundary. */
    if (q->crumb_pending) {
        q->crumb_pending=0;
        sunstone_crumb(q->active_client ? SUNSTONE_CRUMB_GPU_PEER_COMPLETE : SUNSTONE_CRUMB_GPU_COMPLETE,sequence);
    }
    /* A higher-priority client may run as soon as the queue wakes it. */
    q->owed=0;
    if (xQueueSend(v.completions,&r,0)!=pdTRUE) {
        q->owed=1; return fault(q,GPU_QUEUE_SERVICE);
    }
    return 1;
}
static int cache_range(const struct gpu_queue_lease_info *l,uint64_t off,uint64_t bytes,int output)
{
    if (!bytes || (output ? !(l->access&SS_ACCESS_GPU_WRITE) : !(l->access&SS_ACCESS_WRITE))) return 1;
    uint64_t first=off&~UINT64_C(63),end=(off+bytes+63)&~UINT64_C(63);
    return aarch64_device_cache(output?AARCH64_CACHE_CLEAN_INVALIDATE:AARCH64_CACHE_CLEAN,
        (uintptr_t)(l->address+first),(size_t)(end-first));
}
static int maintain(struct pvr_compute_queue *q,int output)
{
    for (unsigned i=0;i<q->job.bindings;++i) {
        const struct gpu_queue_binding *b=&q->job.binding[i];
        if (!cache_range(&q->lease[b->lease],b->sync_offset,b->sync_bytes,output)) return 0;
    }
    return 1;
}
static unsigned admit(const struct pvr_compute_queue *q,const struct gpu_queue_job *j)
{
    if (!j->bindings || j->bindings>GPU_QUEUE_BINDINGS || j->reserved) return GPU_QUEUE_BINDING;
    if (j->argument_bytes>GPU_QUEUE_ARGUMENT_BYTES) return GPU_QUEUE_ARGUMENTS;
    if (!j->groups[0] || !j->groups[1] || !j->groups[2]) return GPU_QUEUE_GRID;
    struct pvr_compute_queue_client_view v;
    if (!pvr_compute_queue_client_view((struct pvr_compute_queue *)q,q->active_client,&v) ||
        j->sequence<=atomic_load(v.sequence) || j->sequence==UINT32_MAX ||
        q->timeline_serial==UINT32_MAX) return GPU_QUEUE_SEQUENCE;
    for (unsigned i=0;i<j->bindings;++i) {
        const struct gpu_queue_binding *b=&j->binding[i];
        if (b->lease>=q->lease_count || b->reserved) return GPU_QUEUE_BINDING;
        const struct gpu_queue_lease_info *l=&q->lease[b->lease];
        if (b->address>=l->bytes || b->sync_offset>l->bytes || b->sync_bytes>l->bytes-b->sync_offset)
            return GPU_QUEUE_RANGE;
    }
    return GPU_QUEUE_NO_DETAIL;
}

int pvr_compute_queue_bind(struct pvr_compute_queue *q,struct pvr_gpu_service *g,struct pvr_gpu_service_config *c)
{
    if (!q || !g || !c || !q->owner || q->phase || g->initialized || c->compute_queue || c->compute ||
        c->graphics || c->free_lists.count || xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED) return 0;
    q->service=g;
    for (unsigned i=0;i<PVR_COMPUTE_QUEUE_OBJECTS;++i)
        q->objects[i]=(struct pvr_mips_object){q->firmware[i].pages,1,q->firmware_address[i],PVR_MIPS_UNCACHED};
    q->completions=xQueueCreateStatic(1,sizeof(struct gpu_queue_record),q->completion_storage,&q->completion_state);
    if (!q->completions) return 0;
    atomic_store(&q->slot_state,PVR_COMPUTE_QUEUE_CLOSED);
    atomic_store(&q->peer.slot_state,PVR_COMPUTE_QUEUE_CLOSED);
    q->phase=PVR_COMPUTE_QUEUE_BOUND;
    c->compute_queue=q; return 1;
}
int pvr_compute_queue_create(struct pvr_compute_queue *q,const struct gpu_queue_context_config *c)
{
    if (!q || !c || q->phase!=PVR_COMPUTE_QUEUE_BOUND || q->service->phase!=PVR_GPU_SERVICE_COLD ||
        !c->program || !c->job_us || c->job_us>GPU_QUEUE_MAX_JOB_US) return 0;
#ifdef SUNSTONE_A733_OS_GPU_RECOVERY_TEST
    if (c->job_us!=200000) return 0;
#endif
    struct gpu_queue_program p=*c->program;
    const uintptr_t begin=(uintptr_t)__start_sunstone_gpu_program;
    const uintptr_t end=(uintptr_t)__stop_sunstone_gpu_program;
    const uintptr_t code=(uintptr_t)p.code;
    uint64_t local=(uint64_t)p.local_size[0]*p.local_size[1];
    if (p.abi!=GPU_QUEUE_ABI_GRID || p.reserved || !p.code || !p.code_bytes || (p.code_bytes&3) ||
        p.code_bytes>GPU_QUEUE_PROGRAM_BYTES || p.temps>252 ||
        (p.coefficients!=3 && p.coefficients!=7) || !p.local_size[0] || !p.local_size[1] ||
        !p.local_size[2] || local>32 || local*p.local_size[2]!=32 ||
        !a733_span_excl_nonempty(p.code,p.code_bytes) || begin>=end || code<begin || code>=end ||
        p.code_bytes>end-code ||
        !a733_ranges_disjoint_nonempty(p.code,p.code_bytes,(void *)q->owner->payload_extent.cpu,3*PAGE)) return 0;
    if (!a733_gpu_queue_create(q->owner,c)) return 0;
    unsigned char *page=(unsigned char *)q->owner->payload_extent.cpu;
    memset(page,0,3*PAGE);
    memcpy(page+PAGE,p.code,p.code_bytes);
    q->task_word=(A733_GPU_QUEUE_PROGRAM_GPU-UINT64_C(0xe000000000))|((uint64_t)((p.temps+3)/4)<<35);
    q->coefficients=p.coefficients; q->job_us=c->job_us;
    const uint32_t zero[3]={0,0,0};
    q->binding0=q->lease[0].address;
    pvr_compute_job_grid_pds(page,q->task_word,zero,A733_GPU_QUEUE_JOB_GPU+PVR_COMPUTE_QUEUE_ARGUMENTS,q->binding0);
    pvr_compute_job_fence_pds(page+PVR_COMPUTE_QUEUE_FENCE);
    if (q->service->config.job_irq_ack) for (unsigned i=0;i<PVR_COMPUTE_FAST_DEPTH;++i) {
        unsigned char *slot=q->job_cpu+i*PVR_COMPUTE_FAST_STRIDE;
        memset(slot,0,PVR_COMPUTE_FAST_STRIDE);
        pvr_compute_job_grid_pds(slot,q->task_word,zero,
            q->job_gpu+i*PVR_COMPUTE_FAST_STRIDE+PVR_COMPUTE_QUEUE_ARGUMENTS,q->binding0);
        pvr_compute_job_fence_pds(slot+PVR_COMPUTE_QUEUE_FENCE);
        q->fast.payload[i].binding0=q->binding0;
    }
    memset(q->command,0,sizeof(q->command));
    pvr_compute_job_command(q->command,0,A733_GPU_QUEUE_JOB_GPU+PVR_COMPUTE_QUEUE_CDM,q->resume);
    put32(q->update,q->firmware_address[TIMELINE]);
    for (unsigned i=0;i<q->lease_count;++i)
        if (!cache_range(&q->lease[i],0,q->lease[i].bytes,1)) {q->error=GPU_QUEUE_MAINTENANCE;return 0;}
    q->generation=1; q->phase=PVR_COMPUTE_QUEUE_CREATED;
    return 1;
}
unsigned pvr_compute_queue_objects(struct pvr_compute_queue *q,struct pvr_mips_object *out,unsigned n)
{
    if (!q || !out || n<PVR_COMPUTE_QUEUE_OBJECTS) return 0;
    memcpy(out,q->objects,sizeof(q->objects)); return PVR_COMPUTE_QUEUE_OBJECTS;
}
int pvr_compute_queue_prepare(struct pvr_compute_queue *q,uint64_t deadline)
{
    if (!q || q->phase!=PVR_COMPUTE_QUEUE_CREATED || !q->service->epoch) return 0;
    q->epoch=q->service->epoch; q->deadline=deadline;
    if (!q->create_done) q->owed=1;
    *(volatile uint32_t *)q->firmware[TIMELINE].storage.cpu=q->timeline_serial;
    a733_mmio_dsb_sy();
    /* Three payload pages contain the job page and the immutable USC code. */
    if (!aarch64_device_cache(AARCH64_CACHE_CLEAN,q->owner->payload_extent.cpu,3*PAGE)) {
        q->error=GPU_QUEUE_MAINTENANCE; return 0;
    }
    q->publish_index=q->publish_page=0; q->phase=PVR_COMPUTE_QUEUE_PUBLISHING;
    return 1;
}
int pvr_compute_queue_publish(struct pvr_compute_queue *q)
{
    if (!q || q->epoch!=q->service->epoch) return -1;
    struct a733_gpu_vm_memory *vm=&q->owner->vm;
    if (q->phase==PVR_COMPUTE_QUEUE_PUBLISHING) {
        const struct pvr_mips_buffer *b=&q->publish[q->publish_index];
        if (!aarch64_device_cache(AARCH64_CACHE_CLEAN,(uintptr_t)b->storage.cpu+q->publish_page*PAGE,PAGE))
            return fault(q,GPU_QUEUE_MAINTENANCE);
        if (++q->publish_page==b->page_count) {q->publish_page=0; ++q->publish_index;}
        if (q->publish_index==10) {
            if (vm->phase==A733_GPU_VM_MEMORY_PUBLISHED) {q->phase=PVR_COMPUTE_QUEUE_PUBLISHED; return 1;}
            if (a733_gpu_vm_memory_publish_begin(vm)!=A733_GPU_VM_MEMORY_AGAIN) return fault(q,GPU_QUEUE_MAINTENANCE);
            q->phase=PVR_COMPUTE_QUEUE_TABLES;
        }
        return 0;
    }
    if (q->phase!=PVR_COMPUTE_QUEUE_TABLES) return -1;
    enum a733_gpu_vm_memory_result r=a733_gpu_vm_memory_poll(vm);
    if (r==A733_GPU_VM_MEMORY_AGAIN) return 0;
    if (r!=A733_GPU_VM_MEMORY_OK) return fault(q,GPU_QUEUE_MAINTENANCE);
    q->phase=PVR_COMPUTE_QUEUE_PUBLISHED; return 1;
}
static int drive_to_kick(struct pvr_compute_queue *q)
{
    while (q->client.phase>=PVR_CCCB_SPACE && q->client.phase<=PVR_CCCB_SUBMIT) {
        if (q->job.not_after_us) {
            uint64_t now;
            if (pvr_gpu_service_now(q->service,&now)!=1) return fault(q,GPU_QUEUE_SERVICE);
            if (now>=q->job.not_after_us) {
                /* No kernel KICK exists before SUBMIT accepts it. Discard
                 * private ring bytes while preserving the published cursor. */
                q->client.phase=PVR_CCCB_IDLE; q->client.cursor=q->client.write;
                struct pvr_compute_queue_client_view v;
                if (!pvr_compute_queue_client_view(q,q->active_client,&v)) return fault(q,GPU_QUEUE_SERVICE);
                atomic_store(v.sequence,q->previous_sequence);
                q->timeline_serial=q->timeline_previous; q->phase=PVR_COMPUTE_QUEUE_READY;
                atomic_store(v.slot_state,PVR_COMPUTE_QUEUE_IDLE);
                return post(q,q->job.sequence,GPU_QUEUE_REFUSED,GPU_QUEUE_DEADLINE,0);
            }
        }
        uint32_t phase=q->client.phase,cursor=q->client.cursor,index=q->client.index;
        enum pvr_cccb_result r=pvr_cccb_poll(&q->client);
        if (r==PVR_CCCB_FAULT || r==PVR_CCCB_INVALID)
            return fault(q,q->client.error==PVR_CCCB_TIMEOUT ? GPU_QUEUE_HANG : GPU_QUEUE_SERVICE);
        if (q->client.phase==phase && q->client.cursor==cursor && q->client.index==index) break;
    }
    if (q->client.phase==PVR_CCCB_WAIT && !q->kicked) {
        if (pvr_gpu_service_now(q->service,&q->kicked)!=1 ||
            !a733_deadline_after(q->kicked,q->job_us,&q->deadline)) return fault(q,GPU_QUEUE_SERVICE);
    }
    return 1;
}
static int take(struct pvr_compute_queue *q)
{
    struct pvr_compute_queue_client_view v;
    unsigned selected=2;
    for (unsigned i=0;i<2;++i) {
        unsigned client=(q->next_client+i)%2;
        if (!pvr_compute_queue_client_view(q,client,&v)) continue;
        if (q->held) {
            uint64_t end=atomic_load(v.not_after),now;
            if (!end || atomic_load(v.slot_state)!=PVR_COMPUTE_QUEUE_SUBMITTED) continue;
            if (pvr_gpu_service_now(q->service,&now)!=1) return fault(q,GPU_QUEUE_SERVICE);
            if (now<end) continue;
        }
        unsigned state=PVR_COMPUTE_QUEUE_SUBMITTED;
        if (atomic_compare_exchange_strong(v.slot_state,&state,PVR_COMPUTE_QUEUE_TAKEN)) {
            selected=client; break;
        }
    }
    if (selected==2) return 0;
    q->active_client=selected; q->next_client=(selected+1)%2;
    q->job=*v.slot; q->owed=1; atomic_store(v.not_after,0);
    q->crumb_pending=1;
    sunstone_crumb(q->active_client ? SUNSTONE_CRUMB_GPU_PEER_SUBMIT : SUNSTONE_CRUMB_GPU_SUBMIT,q->job.sequence);
    unsigned detail=admit(q,&q->job);
    uint64_t now;
    if (pvr_gpu_service_now(q->service,&now)!=1) return fault(q,GPU_QUEUE_SERVICE);
    if (q->job.not_after_us && now>=q->job.not_after_us) detail=GPU_QUEUE_DEADLINE;
    if (detail) {
        atomic_store(v.slot_state,PVR_COMPUTE_QUEUE_IDLE);
        return post(q,q->job.sequence,GPU_QUEUE_REFUSED,detail,0);
    }
    if (timeline(q)!=q->timeline_serial || q->client.phase!=PVR_CCCB_IDLE) return fault(q,GPU_QUEUE_TIMELINE);
    unsigned char *page=(unsigned char *)q->owner->payload_extent.cpu;
    uint64_t binding0=q->lease[q->job.binding[0].lease].address+q->job.binding[0].address;
    int pointer_changed=q->binding0!=binding0;
    int groups_changed=q->coefficients==7 && memcmp(q->groups,q->job.groups,sizeof(q->groups));
    if (pointer_changed || groups_changed) {
        const uint32_t zero[3]={0,0,0};
        pvr_compute_job_grid_pds(page,q->task_word,q->coefficients==7?q->job.groups:zero,
            A733_GPU_QUEUE_JOB_GPU+PVR_COMPUTE_QUEUE_ARGUMENTS,binding0);
        q->binding0=binding0; memcpy(q->groups,q->job.groups,sizeof(q->groups));
    }
    unsigned char *args=page+PVR_COMPUTE_QUEUE_ARGUMENTS;
    for (unsigned i=1;i<q->job.bindings;++i)
        put64(args+8*(i-1),q->lease[q->job.binding[i].lease].address+q->job.binding[i].address);
    unsigned bytes=8*(q->job.bindings-1)+q->job.argument_bytes;
    memcpy(args+8*(q->job.bindings-1),q->job.arguments,q->job.argument_bytes);
    uint32_t pds=(uint32_t)(A733_GPU_QUEUE_JOB_GPU-UINT64_C(0xda00000000));
    pvr_compute_job_grid_cdm(page+PVR_COMPUTE_QUEUE_CDM,pds,pds+PVR_COMPUTE_QUEUE_FENCE,q->job.groups);
    if (!maintain(q,0) ||
        ((pointer_changed || groups_changed) && !aarch64_device_cache(AARCH64_CACHE_CLEAN,
            (uintptr_t)page+(groups_changed?0:64),groups_changed && pointer_changed?128:64)) ||
        !aarch64_device_cache(AARCH64_CACHE_CLEAN,(uintptr_t)page+PVR_COMPUTE_QUEUE_CDM,
            (PVR_COMPUTE_QUEUE_ARGUMENTS-PVR_COMPUTE_QUEUE_CDM+bytes+63)&~63u)) return fault(q,GPU_QUEUE_MAINTENANCE);
    if (pvr_gpu_service_now(q->service,&now)!=1) return fault(q,GPU_QUEUE_SERVICE);
    if (q->job.not_after_us && now>=q->job.not_after_us) {
        atomic_store(v.slot_state,PVR_COMPUTE_QUEUE_IDLE);
        return post(q,q->job.sequence,GPU_QUEUE_REFUSED,GPU_QUEUE_DEADLINE,0);
    }
    /* Ring staging has its own finite transport bound. A short kernel hang
     * bound starts only once the kernel-queue KICK has been accepted. */
    if (!a733_deadline_after(now,GPU_QUEUE_MAX_JOB_US,&q->deadline)) return fault(q,GPU_QUEUE_SERVICE);
    q->timeline_previous=q->timeline_serial;
    put32(q->update+4,++q->timeline_serial);
    const struct pvr_cccb_command commands[2]={
        {PVR_CCB_MAGIC|PVR_CCCB_TASK|205u,96,1,1,q->command}, {PVR_CCB_MAGIC|213u,8,0,0,q->update}};
    q->previous_sequence=atomic_load(v.sequence); atomic_store(v.sequence,q->job.sequence);
    q->sequence=q->job.sequence; q->kicked=0; q->phase=PVR_COMPUTE_QUEUE_IN_FLIGHT;
    if (!pvr_cccb_begin(&q->client,commands,2,NULL,0,q->deadline)) return fault(q,GPU_QUEUE_SERVICE);
    return drive_to_kick(q);
}
static int complete(struct pvr_compute_queue *q)
{
    if (q->client.phase!=PVR_CCCB_WAIT && q->client.phase!=PVR_CCCB_IDLE) return drive_to_kick(q);
    if (q->client.phase==PVR_CCCB_WAIT) {
        enum pvr_cccb_result r=pvr_cccb_poll(&q->client);
        if (r==PVR_CCCB_FAULT || r==PVR_CCCB_INVALID)
            return fault(q,q->client.error==PVR_CCCB_TIMEOUT ? GPU_QUEUE_HANG : GPU_QUEUE_SERVICE);
    }
    uint64_t now;
    if (pvr_gpu_service_now(q->service,&now)!=1) return fault(q,GPU_QUEUE_SERVICE);
    if (now>=q->deadline) return fault(q,GPU_QUEUE_HANG);
#ifdef SUNSTONE_A733_OS_GPU_RECOVERY_TEST
    /* The qualification image withholds the first owner's completion until
     * the ordinary deadline expires. The submitted shader is unchanged. */
    if (q->active_client==0 && q->timeline_serial==1) return 0;
#endif
    if (q->client.phase==PVR_CCCB_IDLE) {
        uint32_t value=timeline(q);
        if (value==q->timeline_serial) {
            uint32_t duration=(uint32_t)(now-q->kicked);
            if (!maintain(q,1)) return fault(q,GPU_QUEUE_MAINTENANCE);
            if (pvr_gpu_service_now(q->service,&now)!=1) return fault(q,GPU_QUEUE_SERVICE);
            if (now>=q->deadline) return fault(q,GPU_QUEUE_HANG);
            q->phase=PVR_COMPUTE_QUEUE_READY;
            ++q->completed_jobs;
            struct pvr_compute_queue_client_view v;
            if (!pvr_compute_queue_client_view(q,q->active_client,&v)) return fault(q,GPU_QUEUE_SERVICE);
            atomic_store(v.slot_state,PVR_COMPUTE_QUEUE_IDLE);
            return post(q,q->sequence,GPU_QUEUE_OK,GPU_QUEUE_NO_DETAIL,duration);
        }
        if (value>=q->timeline_serial || value!=q->timeline_previous) return fault(q,GPU_QUEUE_TIMELINE);
    }
    return 0;
}
int pvr_compute_queue_poll(struct pvr_compute_queue *q)
{
    if (!q || q->phase==PVR_COMPUTE_QUEUE_FAILED || q->epoch!=q->service->epoch) return -1;
    if (q->phase==PVR_COMPUTE_QUEUE_PUBLISHED) {
        static const struct pvr_cccb_ops ops={ring_now,ring_read,ring_write,ring_barrier};
        if (!pvr_cccb_init(&q->client,&ops,q,&q->service->queue,RING_BYTES,q->firmware_address[CONTEXT],q->deadline))
            return fault(q,GPU_QUEUE_SERVICE);
        q->phase=PVR_COMPUTE_QUEUE_FORMAT;
    }
    if (q->phase==PVR_COMPUTE_QUEUE_FORMAT) {
        enum pvr_cccb_result r=pvr_cccb_poll(&q->client);
        if (r==PVR_CCCB_FAULT || r==PVR_CCCB_INVALID) return fault(q,GPU_QUEUE_SERVICE);
        if (q->client.phase!=PVR_CCCB_IDLE) return 0;
        q->phase=PVR_COMPUTE_QUEUE_READY;
        if (q->service->config.job_irq_ack) pvr_compute_fast_ready(q);
        unsigned closed=PVR_COMPUTE_QUEUE_CLOSED;
        (void)atomic_compare_exchange_strong(&q->slot_state,&closed,PVR_COMPUTE_QUEUE_IDLE);
        closed=PVR_COMPUTE_QUEUE_CLOSED;
        if (q->peer.completions) (void)atomic_compare_exchange_strong(&q->peer.slot_state,&closed,PVR_COMPUTE_QUEUE_IDLE);
        if (q->create_pending && !q->create_done) {q->owed=1; return post(q,0,GPU_QUEUE_OK,0,0);}
        return 1;
    }
    if (q->service->config.job_irq_ack &&
        (q->phase==PVR_COMPUTE_QUEUE_READY || q->phase==PVR_COMPUTE_QUEUE_IN_FLIGHT)) return pvr_compute_fast_poll(q);
    if (q->phase==PVR_COMPUTE_QUEUE_READY) return take(q);
    if (q->phase==PVR_COMPUTE_QUEUE_IN_FLIGHT) return complete(q);
    return fault(q,GPU_QUEUE_SERVICE);
}
int pvr_compute_queue_event(struct pvr_compute_queue *q,const unsigned char command[88])
{
    struct pvr_gpu_event event;
    if (!q || pvr_gpu_event_decode(command,88,&event)!=PVR_GPU_EVENT_DECODE_OK) {
        if (q) q->error=GPU_QUEUE_RESET; return 0;
    }
    if (event.kind==PVR_GPU_EVENT_RESET) {
        q->error=GPU_QUEUE_RESET; q->reset_pending=1;
        return 2; /* Consumed here; do not forward to the platform handler. */
    }
    return 1;
}
void pvr_compute_queue_abort(struct pvr_compute_queue *q)
{
    if (!q || q->phase==PVR_COMPUTE_QUEUE_FAILED) return;
    if (q->service->config.job_irq_ack) pvr_compute_fast_seal(q);
    q->phase=PVR_COMPUTE_QUEUE_FAILED;
    if (q->client.phase!=PVR_CCCB_NEW) (void)pvr_cccb_abort(&q->client);
    atomic_store(&q->lost,1);
    atomic_store(&q->recovering,0);
    if (q->service->config.job_irq_ack) for (unsigned i=0;i<GPU_QUEUE_CLIENTS;++i)
        if (q->fast.task[i]) xTaskNotifyGiveIndexed(q->fast.task[i],PVR_COMPUTE_FAST_NOTIFY);
    /* Failure alone does not return DMA ownership. Preserve every owed
     * record and slot until independently established isolation. */
}
int pvr_compute_queue_recovered(struct pvr_compute_queue *q)
{
    if (!q) return 0;
    if (!atomic_load(&q->recovering) || atomic_load(&q->lost) ||
        q->service->phase!=PVR_GPU_SERVICE_RECOVERING) {
        pvr_gpu_crumb_fail(PVR_GPU_CRUMB_RECOVERY,1,__LINE__,
            (uint32_t)q->service->phase << 16 | !!atomic_load(&q->lost) << 1 |
            !!atomic_load(&q->recovering),PVR_GPU_CRUMB_HERE());
        return 0;
    }
    if (!a733_gpu_backend_stopped(q->service->config.context,q->service)) {
        pvr_gpu_crumb_fail(PVR_GPU_CRUMB_RECOVERY,2,__LINE__,
            (uint32_t)q->service->stop.phase << 16 | q->service->stop.isolation_observed,
            PVR_GPU_CRUMB_HERE());
        return 0;
    }
    if (q->service->config.job_irq_ack) return pvr_compute_fast_recovered(q);
    if (q->owed) {
        struct pvr_compute_queue_client_view v;
        if (!pvr_compute_queue_client_view(q,q->active_client,&v)) {
            pvr_gpu_crumb_fail(PVR_GPU_CRUMB_RECOVERY,3,__LINE__,q->active_client,PVR_GPU_CRUMB_HERE());
            return 0;
        }
        if (atomic_load(v.slot_state)!=PVR_COMPUTE_QUEUE_TAKEN) {
            pvr_gpu_crumb_fail(PVR_GPU_CRUMB_RECOVERY,4,__LINE__,
                atomic_load(v.slot_state),PVR_GPU_CRUMB_HERE());
            return 0;
        }
        atomic_store(v.slot_state,PVR_COMPUTE_QUEUE_CLOSED);
        if (post(q,q->job.sequence,GPU_QUEUE_FAULT,q->error,0)!=1) {
            pvr_gpu_crumb_fail(PVR_GPU_CRUMB_RECOVERY,5,__LINE__,q->job.sequence,PVR_GPU_CRUMB_HERE());
            return 0;
        }
    }
    q->reset_pending=0; q->held=0; q->phase=PVR_COMPUTE_QUEUE_READY;
    return 1;
}
int pvr_compute_queue_close(struct pvr_compute_queue *q)
{
    if (!q || q->phase!=PVR_COMPUTE_QUEUE_READY || q->owed ||
        !pvr_compute_queue_clients_idle(q) || atomic_load(&q->lost)) return 0;
    unsigned idle=PVR_COMPUTE_QUEUE_IDLE;
    if (!atomic_compare_exchange_strong(&q->slot_state,&idle,PVR_COMPUTE_QUEUE_CLOSED)) return 0;
    if (q->peer.completions) {
        idle=PVR_COMPUTE_QUEUE_IDLE;
        if (!atomic_compare_exchange_strong(&q->peer.slot_state,&idle,PVR_COMPUTE_QUEUE_CLOSED)) {
            atomic_store(&q->slot_state,PVR_COMPUTE_QUEUE_IDLE); return 0;
        }
    }
    if (q->service->config.job_irq_ack) pvr_compute_fast_seal(q);
    return 1;
}
int pvr_compute_queue_deadline(const struct pvr_compute_queue *q,uint64_t *out)
{
    if (!q || !out) return 0;
    if (q->phase==PVR_COMPUTE_QUEUE_IN_FLIGHT) {
        *out=q->deadline;
        if (!q->kicked && q->job.not_after_us && q->job.not_after_us<*out) *out=q->job.not_after_us;
        return 1;
    }
    uint64_t end=UINT64_MAX;
    for (unsigned i=0;i<2;++i) {
        struct pvr_compute_queue_client_view v;
        if (pvr_compute_queue_client_view((struct pvr_compute_queue *)q,i,&v) &&
            atomic_load(v.slot_state)==PVR_COMPUTE_QUEUE_SUBMITTED) {
            uint64_t candidate=atomic_load(v.not_after);
            if (candidate && candidate<end) end=candidate;
        }
    }
    if (end==UINT64_MAX) return 0;
    *out=end; return 1;
}
int pvr_compute_queue_submitting(const struct pvr_compute_queue *q)
{ return q && (q->phase==PVR_COMPUTE_QUEUE_PUBLISHED || q->phase==PVR_COMPUTE_QUEUE_FORMAT ||
    (q->phase==PVR_COMPUTE_QUEUE_READY && !q->held &&
     (atomic_load(&q->slot_state)==PVR_COMPUTE_QUEUE_SUBMITTED ||
      (q->peer.completions && atomic_load(&q->peer.slot_state)==PVR_COMPUTE_QUEUE_SUBMITTED)))); }
uint64_t pvr_compute_queue_progress(const struct pvr_compute_queue *q)
{ return q ? (uint64_t)q->phase|(uint64_t)q->client.phase<<8|(uint64_t)q->client.cursor<<16|(uint64_t)q->sequence<<32 : 0; }
int pvr_compute_queue_hold(struct pvr_compute_queue *q)
{ if (!q || atomic_load(&q->lost)) return 0; q->held=1; return q->phase==PVR_COMPUTE_QUEUE_READY &&
    atomic_load(&q->slot_state)!=PVR_COMPUTE_QUEUE_RESERVED &&
    (!q->peer.completions || atomic_load(&q->peer.slot_state)!=PVR_COMPUTE_QUEUE_RESERVED); }
int pvr_compute_queue_release(struct pvr_compute_queue *q)
{ if (!q || !q->held || atomic_load(&q->lost)) return 0; q->held=0; return 1; }
int pvr_compute_queue_reopen(struct pvr_compute_queue *q)
{ return q ? a733_gpu_queue_reopen(q->owner) : 0; }
int pvr_compute_queue_wake(struct pvr_compute_queue *q)
{ if (!q) return 0; atomic_store(&q->wake,0); return 1; }
