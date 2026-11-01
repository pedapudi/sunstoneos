/* Optional job diagnostics. Link wrapping keeps the disabled image unchanged. */
#include "gpu_queue_timing.h"
#include "pvr_compute_queue.h"
#include "pvr_gpu_service.h"
#include "aarch64_device_memory.h"
#include "sunstone_crumb.h"
#include <string.h>
struct timing_row {
    struct gpu_queue_timing_record public;
    struct { uint32_t address, pad; } pre, post;
    uint64_t last_state_sample;
};
static struct timing_row rows[GPU_QUEUE_TIMING_CAPACITY];
static unsigned count;
static struct pvr_compute_queue *queue;
static struct timing_row *active;
static int in_ccb_submit;
static int receive_blocked;
static uint64_t receive_block_tick;
static uint64_t ticks(void) { uint64_t t; __asm__ volatile("isb; mrs %0, cntpct_el0":"=r"(t)::"memory"); return t; }
static int worker(void) { return queue && xTaskGetCurrentTaskHandle()==queue->service->worker; }
static struct timing_row *find(unsigned client, unsigned sequence)
{ for (unsigned i=count;i;--i) if(rows[i-1].public.client==client && rows[i-1].public.sequence==sequence)return &rows[i-1]; return NULL; }
int gpu_queue_timing_active(void) { return active!=NULL; }
int gpu_queue_timing_kicking(void) { return active && in_ccb_submit; }
void gpu_queue_timing_cr(unsigned i,uint64_t cr,uint64_t begin,uint64_t end)
{ if(active && i<3){active->public.cr_raw[i]=cr;active->public.cr_cpu_before[i]=begin;active->public.cr_cpu_after[i]=end;} }
void gpu_queue_timing_bind(struct pvr_compute_queue *q) { queue=q; }
unsigned gpu_queue_timing_count(void) { return count; }
int gpu_queue_timing_read(unsigned i,struct gpu_queue_timing_record *out)
{ if(!out || i>=count)return 0; *out=rows[i].public; return 1; }
void gpu_queue_timing_stamp(enum gpu_queue_timing_stamp s)
{ if(active && (unsigned)s<GPU_TIMING_STAMPS &&
    ((s!=GPU_TIMING_KICK_BEGIN && s!=GPU_TIMING_KICK_END) || in_ccb_submit))active->public.ticks[s]=ticks(); }
void gpu_queue_timing_state_sample(unsigned index)
{
    if(!active || index>3 || !worker())return;
    struct timing_row *r=active;
    struct gpu_queue_timing_state state={0};
    int middle=index==1;
    uint64_t now=ticks();
    /* Dense early observations, then 10 ms spacing through the one-second
     * maximum job deadline, with a fixed upper bound on diagnostic reads. */
    uint64_t interval=r->public.mid_samples<16 ? 24000 : 240000;
    if(middle && (r->public.mid_samples>=128 || now-r->last_state_sample<interval))return;
    if(!r->public.timestamp_injected || !queue->firmware[5].storage.cpu || queue->firmware[5].storage.bytes<24)return;
    const volatile uint64_t *slots=(const volatile uint64_t *)((const unsigned char *)queue->firmware[5].storage.cpu+8);
    state.firmware_pre_raw=slots[0];
    __asm__ volatile("dmb oshld":::"memory");
    if(middle && (!state.firmware_pre_raw || slots[1]))return;
    state.cpu_before=ticks();
    if(!a733_gpu_backend_timing_state(&state))return;
    state.cpu_after=ticks();
    __asm__ volatile("dmb oshld":::"memory");
    state.firmware_post_raw=slots[1];
    if(middle && state.firmware_post_raw)return;
    if(middle) {
        if(!r->public.mid_samples)r->public.state[1]=state;
        r->public.state[2]=state;++r->public.mid_samples;r->last_state_sample=now;
    } else r->public.state[index]=state;
}
void gpu_queue_timing_crumb(uint32_t stage,uint32_t sequence)
{
    if(!worker())return;
    unsigned client=(stage==SUNSTONE_CRUMB_GPU_PEER_SUBMIT || stage==SUNSTONE_CRUMB_GPU_PEER_COMPLETE);
    struct timing_row *r=find(client,sequence);
    if(!r)return;
    if(stage==SUNSTONE_CRUMB_GPU_SUBMIT || stage==SUNSTONE_CRUMB_GPU_PEER_SUBMIT) {
        active=r; r->public.ticks[GPU_TIMING_TAKE]=ticks();
    } else {
        r->public.ticks[GPU_TIMING_COMPLETE]=ticks();
        if(r->public.ticks[GPU_TIMING_KICK_BEGIN]) {
            a733_gpu_backend_timing_sample(2);gpu_queue_timing_state_sample(3);
        }
        if(r->public.timestamp_injected) {
            volatile uint64_t *p=(volatile uint64_t *)((unsigned char *)queue->firmware[5].storage.cpu+8);
            __asm__ volatile("dmb oshld":::"memory");
            r->public.firmware_pre_raw=p[0];r->public.firmware_post_raw=p[1];
        }
        active=NULL;
    }
}
int __real_gpu_queue_submit(struct gpu_queue_context,const struct gpu_queue_job *);
int __wrap_gpu_queue_submit(struct gpu_queue_context c,const struct gpu_queue_job *j)
{
    struct timing_row *r=NULL;
    if(j && count<GPU_QUEUE_TIMING_CAPACITY) {
        r=&rows[count++];r->public.sequence=j->sequence;r->public.client=c.client;
        memcpy(r->public.groups,j->groups,sizeof(j->groups));
        for(unsigned i=0;i<j->bindings && i<GPU_QUEUE_BINDINGS;++i)r->public.sync_bytes+=j->binding[i].sync_bytes;
        r->public.ticks[GPU_TIMING_SUBMIT_BEGIN]=ticks();
    }
    int result=__real_gpu_queue_submit(c,j);
    if(r){r->public.ticks[GPU_TIMING_SUBMIT_END]=ticks();r->public.result=result;}
    return result;
}
int __real_gpu_queue_wait(struct gpu_queue_context,struct gpu_queue_record *,uint32_t);
int __wrap_gpu_queue_wait(struct gpu_queue_context c,struct gpu_queue_record *done,uint32_t wait)
{
    uint64_t begin=ticks();
    int result=__real_gpu_queue_wait(c,done,wait); uint64_t end=ticks();
    if(result==GPU_QUEUE_OK && done) {
        struct timing_row *r=find(c.client,done->sequence);
        if(r){if(!r->public.ticks[GPU_TIMING_WAIT_BEGIN])r->public.ticks[GPU_TIMING_WAIT_BEGIN]=begin;
            r->public.ticks[GPU_TIMING_WAIT_END]=end;r->public.kick_to_done_us=done->kick_to_done_us;r->public.result=done->result;}
    }
    return result;
}
int __real_pvr_cccb_begin(struct pvr_cccb *,const struct pvr_cccb_command *,unsigned,const uint32_t *,unsigned,uint64_t);
int __wrap_pvr_cccb_begin(struct pvr_cccb *s,const struct pvr_cccb_command *cmd,unsigned n,const uint32_t *clean,unsigned nc,uint64_t deadline)
{
    if(worker() && active && s==&queue->client) {
        gpu_queue_timing_stamp(GPU_TIMING_CCCB_BEGIN);
        if(n==2 && cmd[0].type==(PVR_CCB_MAGIC|PVR_CCCB_TASK|205u) && cmd[0].bytes==96 &&
           cmd[1].type==(PVR_CCB_MAGIC|213u) && cmd[1].bytes==8 && queue->firmware[5].storage.bytes>=32) {
            volatile uint64_t *slots=(volatile uint64_t *)((unsigned char *)queue->firmware[5].storage.cpu+8);
            slots[0]=0;slots[1]=0;__asm__ volatile("dmb oshst":::"memory");
            active->pre.address=queue->firmware_address[5]+8;active->post.address=queue->firmware_address[5]+16;
            struct pvr_cccb_command expanded[4]={
                {PVR_CCB_MAGIC|PVR_CCCB_TASK|209u,8,0,0,&active->pre},cmd[0],
                {PVR_CCB_MAGIC|217u,8,0,0,&active->post},cmd[1]};
            int result=__real_pvr_cccb_begin(s,expanded,4,clean,nc,deadline);
            active->public.timestamp_injected=result!=0;return result;
        }
    }
    return __real_pvr_cccb_begin(s,cmd,n,clean,nc,deadline);
}
enum pvr_cccb_result __real_pvr_cccb_poll(struct pvr_cccb *);
enum pvr_cccb_result __wrap_pvr_cccb_poll(struct pvr_cccb *s)
{
    unsigned before=s->phase; enum pvr_cccb_result result=__real_pvr_cccb_poll(s);
    if(worker() && active && s==&queue->client && before==PVR_CCCB_WAIT && s->phase==PVR_CCCB_IDLE)
        gpu_queue_timing_stamp(GPU_TIMING_ACCEPT);
    return result;
}
enum pvr_ccb_result __real_pvr_ccb_submit(struct pvr_ccb *,const void *,uint64_t,uint64_t,struct pvr_ccb_token *);
enum pvr_ccb_result __wrap_pvr_ccb_submit(struct pvr_ccb *q,const void *cmd,uint64_t now,uint64_t deadline,struct pvr_ccb_token *token)
{
    int track=worker() && active && q==queue->client.queue && cmd==queue->client.kick;
    if(track){in_ccb_submit=1;gpu_queue_timing_stamp(GPU_TIMING_CCB_BEGIN);}
    enum pvr_ccb_result result=__real_pvr_ccb_submit(q,cmd,now,deadline,token);
    if(track){gpu_queue_timing_stamp(GPU_TIMING_CCB_END);in_ccb_submit=0;}return result;
}
int __real_aarch64_device_cache(enum aarch64_cache_op,uintptr_t,size_t);
int __wrap_aarch64_device_cache(enum aarch64_cache_op op,uintptr_t address,size_t bytes)
{
    struct timing_row *r=worker()?active:NULL;uint64_t begin=r?ticks():0;
    if(r && op!=AARCH64_CACHE_CLEAN && !r->public.ticks[GPU_TIMING_ACQUIRE_BEGIN])r->public.ticks[GPU_TIMING_ACQUIRE_BEGIN]=begin;
    int result=__real_aarch64_device_cache(op,address,bytes);
    if(r){uint64_t end=ticks();if(op==AARCH64_CACHE_CLEAN){r->public.clean_ticks+=end-begin;r->public.clean_bytes+=bytes;}
        else {r->public.invalidate_ticks+=end-begin;r->public.invalidate_bytes+=bytes;r->public.ticks[GPU_TIMING_ACQUIRE_END]=end;}}
    return result;
}
void gpu_queue_timing_block_receive(void *handle)
{
    if(worker() && active && handle==(void *)queue->service->requests) {
        receive_blocked=1;
        ++active->public.worker_blocks;
        receive_block_tick=ticks();
    }
}
BaseType_t __real_xQueueReceive(QueueHandle_t,void *,TickType_t);
BaseType_t __wrap_xQueueReceive(QueueHandle_t q,void *out,TickType_t wait)
{
    int track=worker() && active && q==queue->service->requests;
    uint64_t begin=track?ticks():0;
    if(track){receive_blocked=0;receive_block_tick=0;}
    BaseType_t result=__real_xQueueReceive(q,out,wait);
    if(result==pdTRUE && track && active) {
        const struct pvr_gpu_service_message *m=out;
        if(m->client==PVR_GPU_SERVICE_CLIENTS && !m->ticket) {
            ++active->public.worker_irq_wakes;
            active->public.ticks[GPU_TIMING_WORKER_RECEIVE_BEGIN]=begin;
            gpu_queue_timing_stamp(GPU_TIMING_WORKER_WAKE);
            if(receive_blocked) {
                ++active->public.worker_blocked_irq_wakes;
                active->public.ticks[GPU_TIMING_WORKER_BLOCK]=receive_block_tick;
                active->public.ticks[GPU_TIMING_BLOCKED_WAKE_IRQ]=active->public.ticks[GPU_TIMING_IRQ_LAST];
                active->public.ticks[GPU_TIMING_WORKER_BLOCKED_IRQ_WAKE]=active->public.ticks[GPU_TIMING_WORKER_WAKE];
            }
        }
    }
    if(track)gpu_queue_timing_state_sample(1);
    return result;
}
void gpu_queue_timing_irq(void)
{
    if(active && active->public.ticks[GPU_TIMING_KICK_BEGIN]){uint64_t t=ticks();if(!active->public.irq_count++)active->public.ticks[GPU_TIMING_IRQ_FIRST]=t;
        active->public.ticks[GPU_TIMING_IRQ_LAST]=t;}
}
