/* Included after the production source with only architectural reads modeled. */
#include <assert.h>
static TaskHandle_t current;
static unsigned cache_calls, expanded_count, cr_samples, block_next, receive_timeout;
static struct pvr_cccb_command expanded[4];
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current; }
void a733_gpu_backend_timing_sample(unsigned p) { ++cr_samples;gpu_queue_timing_cr(p,100+p,10+p,20+p); }
int __real_gpu_queue_submit(struct gpu_queue_context c,const struct gpu_queue_job *j)
{ (void)c;(void)j;return GPU_QUEUE_OK; }
int __real_gpu_queue_wait(struct gpu_queue_context c,struct gpu_queue_record *r,uint32_t wait)
{ (void)c;(void)wait;*r=(struct gpu_queue_record){.sequence=7,.result=GPU_QUEUE_OK,.kick_to_done_us=123};return GPU_QUEUE_OK; }
int __real_pvr_cccb_begin(struct pvr_cccb *s,const struct pvr_cccb_command *c,unsigned n,const uint32_t *clean,unsigned nc,uint64_t end)
{ (void)s;(void)clean;(void)nc;(void)end;expanded_count=n;memcpy(expanded,c,n*sizeof(*c));return 1; }
enum pvr_cccb_result __real_pvr_cccb_poll(struct pvr_cccb *s)
{ s->phase=PVR_CCCB_IDLE;return PVR_CCCB_OK; }
enum pvr_ccb_result __real_pvr_ccb_submit(struct pvr_ccb *q,const void *c,uint64_t t,uint64_t d,struct pvr_ccb_token *token)
{ (void)q;(void)c;(void)t;(void)d;(void)token;
  gpu_queue_timing_stamp(GPU_TIMING_KICK_BEGIN);gpu_queue_timing_stamp(GPU_TIMING_KICK_END);return PVR_CCB_OK; }
int __real_aarch64_device_cache(enum aarch64_cache_op op,uintptr_t addr,size_t bytes)
{ (void)op;(void)addr;(void)bytes;++cache_calls;return 1; }
BaseType_t __real_xQueueReceive(QueueHandle_t q,void *out,TickType_t wait)
{ (void)out; if(block_next && wait)gpu_queue_timing_block_receive((void *)q);block_next=0;return receive_timeout?pdFALSE:pdTRUE; }
int main(void)
{
    static struct pvr_compute_queue q;
    static struct pvr_gpu_service service;
    static uint64_t timeline[512];
    service.worker=(TaskHandle_t)1;service.requests=(QueueHandle_t)2;
    q.service=&service;q.firmware[5].storage.cpu=timeline;q.firmware[5].storage.bytes=sizeof(timeline);
    q.firmware_address[5]=0xc0850000;gpu_queue_timing_bind(&q);
    struct gpu_queue_context ctx={.client=0};
    struct gpu_queue_job job={.sequence=7,.groups={2,3,4},.bindings=1};job.binding[0].sync_bytes=64;
    assert(__wrap_gpu_queue_submit(ctx,&job)==GPU_QUEUE_OK);
    current=service.worker;gpu_queue_timing_crumb(SUNSTONE_CRUMB_GPU_SUBMIT,7);
    assert(gpu_queue_timing_active());
    struct pvr_cccb_command cmd[2]={{PVR_CCB_MAGIC|PVR_CCCB_TASK|205u,96,1,1,&job},{PVR_CCB_MAGIC|213u,8,0,0,&job}};
    assert(__wrap_pvr_cccb_begin(&q.client,cmd,2,NULL,0,100));assert(expanded_count==4);
    assert(expanded[0].type==(PVR_CCB_MAGIC|PVR_CCCB_TASK|209u));
    assert(expanded[2].type==(PVR_CCB_MAGIC|217u));
    assert(expanded[0].bytes==8 && expanded[2].bytes==8);
    assert(((const uint32_t *)expanded[0].payload)[0]==0xc0850008);
    assert(((const uint32_t *)expanded[2].payload)[0]==0xc0850010);
    assert(((const uint32_t *)expanded[0].payload)[1]==0);
    current=(TaskHandle_t)3;__wrap_aarch64_device_cache(AARCH64_CACHE_CLEAN,0,1024);
    current=service.worker;__wrap_aarch64_device_cache(AARCH64_CACHE_CLEAN,0,64);
    __wrap_aarch64_device_cache(AARCH64_CACHE_INVALIDATE,0,128);
    struct pvr_gpu_service_message message={.client=PVR_GPU_SERVICE_CLIENTS,.ticket=PVR_GPU_SERVICE_QUEUE_WAKE};
    __wrap_xQueueReceive(service.requests,&message,1);
    message.ticket=0;__wrap_xQueueReceive((QueueHandle_t)4,&message,1);
    __wrap_xQueueReceive(service.requests,&message,0);
    assert(rows[0].public.worker_irq_wakes==1 && rows[0].public.worker_blocked_irq_wakes==0);
    block_next=1;__wrap_xQueueReceive(service.requests,&message,1);
    assert(rows[0].public.worker_blocks==1 && rows[0].public.worker_blocked_irq_wakes==1);
    assert(rows[0].public.ticks[GPU_TIMING_WORKER_BLOCK]>=rows[0].public.ticks[GPU_TIMING_WORKER_RECEIVE_BEGIN]);
    assert(rows[0].public.ticks[GPU_TIMING_WORKER_BLOCKED_IRQ_WAKE]>=rows[0].public.ticks[GPU_TIMING_WORKER_BLOCK]);
    gpu_queue_timing_irq();assert(rows[0].public.irq_count==0);
    struct pvr_ccb kernel={0};q.client.queue=&kernel;
    __wrap_pvr_ccb_submit(&kernel,q.client.kick,0,100,NULL);
    uint64_t kick=rows[0].public.ticks[GPU_TIMING_KICK_BEGIN];assert(kick);
    __wrap_pvr_ccb_submit(&kernel,&job,0,100,NULL);
    assert(rows[0].public.ticks[GPU_TIMING_KICK_BEGIN]==kick);
    gpu_queue_timing_irq();gpu_queue_timing_irq();
    /* A later backup timeout must preserve the last blocked IRQ pairing. */
    uint64_t paired_block=rows[0].public.ticks[GPU_TIMING_WORKER_BLOCK];
    uint64_t paired_wake=rows[0].public.ticks[GPU_TIMING_WORKER_BLOCKED_IRQ_WAKE];
    uint64_t paired_irq=rows[0].public.ticks[GPU_TIMING_BLOCKED_WAKE_IRQ];
    block_next=1;receive_timeout=1;__wrap_xQueueReceive(service.requests,&message,1);receive_timeout=0;
    assert(rows[0].public.worker_blocks==2);
    assert(rows[0].public.ticks[GPU_TIMING_WORKER_BLOCK]==paired_block);
    assert(rows[0].public.ticks[GPU_TIMING_WORKER_BLOCKED_IRQ_WAKE]==paired_wake);
    assert(rows[0].public.ticks[GPU_TIMING_BLOCKED_WAKE_IRQ]==paired_irq);
    q.client.phase=PVR_CCCB_WAIT;__wrap_pvr_cccb_poll(&q.client);
    timeline[1]=0x1122334455667788;timeline[2]=0x8877665544332211;
    gpu_queue_timing_crumb(SUNSTONE_CRUMB_GPU_COMPLETE,7);assert(!gpu_queue_timing_active());
    struct gpu_queue_record done;__wrap_gpu_queue_wait(ctx,&done,1);
    struct gpu_queue_timing_record r;assert(gpu_queue_timing_read(0,&r));
    assert(r.clean_bytes==64 && r.invalidate_bytes==128 && cache_calls==3);
    assert(r.worker_irq_wakes==2 && r.worker_blocked_irq_wakes==1 && r.irq_count==2 && r.kick_to_done_us==123);
    assert(r.ticks[GPU_TIMING_COMPLETE]<r.ticks[GPU_TIMING_WAIT_BEGIN]);
    assert(r.firmware_pre_raw==timeline[1] && r.firmware_post_raw==timeline[2]);
    assert(r.cr_raw[2]==102 && r.sync_bytes==64 && r.timestamp_injected);
    cmd[0].bytes=88;current=service.worker;active=&rows[0];
    __wrap_pvr_cccb_begin(&q.client,cmd,2,NULL,0,100);assert(expanded_count==2);active=NULL;
    assert(cr_samples==1);
    job.sequence=8;__wrap_gpu_queue_submit(ctx,&job);
    gpu_queue_timing_crumb(SUNSTONE_CRUMB_GPU_SUBMIT,8);
    gpu_queue_timing_crumb(SUNSTONE_CRUMB_GPU_COMPLETE,8);
    assert(cr_samples==1); /* refusal without a kick must never touch CR MMIO */
    for(unsigned i=2;i<GPU_QUEUE_TIMING_CAPACITY+3;++i){job.sequence=7+i;__wrap_gpu_queue_submit(ctx,&job);}
    assert(gpu_queue_timing_count()==GPU_QUEUE_TIMING_CAPACITY);
    assert(!gpu_queue_timing_read(GPU_QUEUE_TIMING_CAPACITY,&r));
    return 0;
}
