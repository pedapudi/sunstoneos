/* Producer of the optional GPU job diagnostics (apps/os/gpu-timing.c), linked as it is on the
 * real FreeRTOS kernel under the host shim (checks/host/). The wrapped functions are the
 * production link wraps. The device-facing callees (submission, command ring, cache
 * maintenance, backend register reads) are modeled at their boundary; the service
 * request queue, the worker task, blocking receives, the interrupt that wakes a blocked
 * worker and the counter are the kernel's and the shim's. */
#include <assert.h>
#include <string.h>
#include "gpu_queue_timing.h"
#include "pvr_compute_queue.h"
#include "pvr_gpu_service.h"
#include "aarch64_device_memory.h"
#include "sunstone_crumb.h"
#include "host_rtos.h"
#include "queue.h"
struct pvr_compute_queue timing_queue;
#define q timing_queue
static struct pvr_gpu_service service;
static uint64_t timeline[512];
extern unsigned cache_calls, expanded_count, cr_samples, state_samples, complete_during_sample;
extern struct pvr_cccb_command expanded[4];
#ifdef SUNSTONE_GPU_NSI_PMU
extern unsigned nsi_calls[3];
#endif
static QueueHandle_t other_queue;
static StaticQueue_t requests_state,other_state;
static uint8_t requests_storage[sizeof(struct pvr_gpu_service_message)],other_storage[sizeof(struct pvr_gpu_service_message)];
static struct gpu_queue_timing_record snap(void){struct gpu_queue_timing_record r;assert(gpu_queue_timing_read(0,&r));return r;}
static void wake_irq(void *unused)
{
    (void)unused;
    struct pvr_gpu_service_message m={.client=PVR_GPU_SERVICE_CLIENTS,.ticket=0};
    BaseType_t woken=pdFALSE;
    assert(xQueueSendFromISR(service.requests,&m,&woken)==pdTRUE);
    portYIELD_FROM_ISR(woken);
}
static void foreign_cache(void *unused){(void)unused;aarch64_device_cache(AARCH64_CACHE_CLEAN,0,1024);}
static void send_message(QueueHandle_t h,unsigned ticket)
{
    struct pvr_gpu_service_message m={.client=PVR_GPU_SERVICE_CLIENTS,.ticket=ticket};
    assert(xQueueSend(h,&m,0)==pdTRUE);
}
static TaskHandle_t foreign_task;
static void ms(unsigned n){host_advance_us(1000u*n);}

static void test(void *unused)
{
    (void)unused;
    struct gpu_queue_context ctx={.client=0};
    struct gpu_queue_job job={.sequence=7,.groups={2,3,4},.bindings=1};job.binding[0].sync_bytes=64;
    assert(gpu_queue_submit(ctx,&job)==GPU_QUEUE_OK);
    gpu_queue_timing_crumb(SUNSTONE_CRUMB_GPU_SUBMIT,7);
    assert(gpu_queue_timing_active());
    struct pvr_cccb_command cmd[2]={{PVR_CCB_MAGIC|PVR_CCCB_TASK|205u,96,1,1,&job},{PVR_CCB_MAGIC|213u,8,0,0,&job}};
    assert(pvr_cccb_begin(&q.client,cmd,2,NULL,0,100));assert(expanded_count==4);
    assert(expanded[0].type==(PVR_CCB_MAGIC|PVR_CCCB_TASK|209u));
    assert(expanded[2].type==(PVR_CCB_MAGIC|217u));
    assert(expanded[0].bytes==8 && expanded[2].bytes==8);
    assert(((const uint32_t *)expanded[0].payload)[0]==0xc0850008);
    assert(((const uint32_t *)expanded[2].payload)[0]==0xc0850010);
    assert(((const uint32_t *)expanded[0].payload)[1]==0);
    host_call(foreign_task,foreign_cache,NULL);
    aarch64_device_cache(AARCH64_CACHE_CLEAN,0,64);
    aarch64_device_cache(AARCH64_CACHE_INVALIDATE,0,128);
    struct pvr_gpu_service_message message;
    /* A queue wake is no interrupt wake, and neither is a message on another queue. */
    send_message(service.requests,PVR_GPU_SERVICE_QUEUE_WAKE);
    assert(xQueueReceive(service.requests,&message,1)==pdTRUE);
    send_message(other_queue,0);
    assert(xQueueReceive(other_queue,&message,1)==pdTRUE);
    send_message(service.requests,0);
    assert(xQueueReceive(service.requests,&message,0)==pdTRUE);
    struct gpu_queue_timing_record r=snap();
    assert(r.worker_irq_wakes==1 && r.worker_blocked_irq_wakes==0);
    /* The worker blocks on the empty request queue; an interrupt delivers the message. */
    host_irq_at(host_time_us()+400,wake_irq,NULL);
    assert(xQueueReceive(service.requests,&message,1)==pdTRUE);
    r=snap();
    assert(r.worker_blocks==1 && r.worker_blocked_irq_wakes==1);
    assert(r.ticks[GPU_TIMING_WORKER_BLOCK]>=r.ticks[GPU_TIMING_WORKER_RECEIVE_BEGIN]);
    assert(r.ticks[GPU_TIMING_WORKER_BLOCKED_IRQ_WAKE]>r.ticks[GPU_TIMING_WORKER_BLOCK]);
    gpu_queue_timing_irq();assert(snap().irq_count==0);
    struct pvr_ccb kernel={0};q.client.queue=&kernel;
    pvr_ccb_submit(&kernel,q.client.kick,0,100,NULL);
    uint64_t kick=snap().ticks[GPU_TIMING_KICK_BEGIN];assert(kick);
    ms(1);
    pvr_ccb_submit(&kernel,&job,0,100,NULL);
    assert(snap().ticks[GPU_TIMING_KICK_BEGIN]==kick);
    gpu_queue_timing_irq();gpu_queue_timing_irq();
    /* A later backup timeout must preserve the last blocked IRQ pairing. */
    r=snap();
    uint64_t paired_block=r.ticks[GPU_TIMING_WORKER_BLOCK];
    uint64_t paired_wake=r.ticks[GPU_TIMING_WORKER_BLOCKED_IRQ_WAKE];
    uint64_t paired_irq=r.ticks[GPU_TIMING_BLOCKED_WAKE_IRQ];
    assert(xQueueReceive(service.requests,&message,1)==pdFALSE);
    r=snap();
    assert(r.worker_blocks==2);
    assert(r.ticks[GPU_TIMING_WORKER_BLOCK]==paired_block);
    assert(r.ticks[GPU_TIMING_WORKER_BLOCKED_IRQ_WAKE]==paired_wake);
    assert(r.ticks[GPU_TIMING_BLOCKED_WAKE_IRQ]==paired_irq);
    q.client.phase=PVR_CCCB_WAIT;pvr_cccb_poll(&q.client);
    gpu_queue_timing_state_sample(0);assert(state_samples==1);
    gpu_queue_timing_state_sample(1);assert(state_samples==1); /* PRE not executed */
    timeline[1]=0x1122334455667788;
    ms(1);
    gpu_queue_timing_state_sample(1);assert(state_samples==2 && snap().mid_samples==1);
    gpu_queue_timing_state_sample(1);assert(state_samples==2); /* cadence bound */
    ms(1);
    gpu_queue_timing_state_sample(1);assert(state_samples==3 && snap().mid_samples==2);
    uint64_t first_sample=snap().state[1].cpu_before;
    assert(snap().state[2].cpu_before>first_sample);
    ms(1);
    complete_during_sample=1;gpu_queue_timing_state_sample(1);complete_during_sample=0;
    assert(state_samples==4 && snap().mid_samples==2); /* POST raced state reads */
    timeline[2]=0x8877665544332211;
    ms(1);
    gpu_queue_timing_state_sample(1);assert(state_samples==4); /* POST excludes mid */
    ms(1);
    gpu_queue_timing_crumb(SUNSTONE_CRUMB_GPU_COMPLETE,7);assert(!gpu_queue_timing_active());
    ms(1);
    struct gpu_queue_record done;gpu_queue_wait(ctx,&done,1);
    assert(gpu_queue_timing_read(0,&r));
    assert(r.clean_bytes==64 && r.invalidate_bytes==128 && cache_calls==3);
    assert(r.worker_irq_wakes==2 && r.worker_blocked_irq_wakes==1 && r.irq_count==2 && r.kick_to_done_us==123);
    assert(r.ticks[GPU_TIMING_COMPLETE]<r.ticks[GPU_TIMING_WAIT_BEGIN]);
    assert(r.firmware_pre_raw==timeline[1] && r.firmware_post_raw==timeline[2]);
    assert(r.state[1].firmware_pre_raw && !r.state[1].firmware_post_raw);
    assert(r.state[2].pll_raw==0xe8105300 && r.state[2].clock_raw==0x80000000);
    assert(r.state[3].pmic_retained && r.state[3].pmic_selector_raw==0x2e);
    assert(r.mid_samples==2 && state_samples==5);
    assert(r.cr_raw[2]==102 && r.sync_bytes==64 && r.timestamp_injected);
#ifdef SUNSTONE_GPU_NSI_PMU
    assert(nsi_calls[0]==1 && nsi_calls[1]==1 && nsi_calls[2]==1 && r.nsi.samples==7);
#endif
    /* A job whose command is not the instrumented pair is passed through unexpanded. */
    job.sequence=9;gpu_queue_submit(ctx,&job);
    gpu_queue_timing_crumb(SUNSTONE_CRUMB_GPU_SUBMIT,9);
    cmd[0].bytes=88;
    pvr_cccb_begin(&q.client,cmd,2,NULL,0,100);assert(expanded_count==2);
    gpu_queue_timing_crumb(SUNSTONE_CRUMB_GPU_COMPLETE,9);
    assert(cr_samples==1);
    job.sequence=8;gpu_queue_submit(ctx,&job);
    gpu_queue_timing_crumb(SUNSTONE_CRUMB_GPU_SUBMIT,8);
    gpu_queue_timing_crumb(SUNSTONE_CRUMB_GPU_COMPLETE,8);
    assert(cr_samples==1 && state_samples==5); /* refusal without a kick must never touch CR MMIO */
#ifdef SUNSTONE_GPU_NSI_PMU
    assert(nsi_calls[0]==1 && nsi_calls[1]==1 && nsi_calls[2]==1);
#endif
    for(unsigned i=3;i<GPU_QUEUE_TIMING_CAPACITY+3;++i){job.sequence=7+i;gpu_queue_submit(ctx,&job);}
    assert(gpu_queue_timing_count()==GPU_QUEUE_TIMING_CAPACITY);
    assert(!gpu_queue_timing_read(GPU_QUEUE_TIMING_CAPACITY,&r));
}
int main(void)
{
    host_set_time_us(2000);
    TaskHandle_t worker=host_rtos_prepare(test,NULL,2,1000000);
    foreign_task=host_server_create("foreign",2);
    service.worker=worker;
    service.requests=xQueueCreateStatic(1,sizeof(struct pvr_gpu_service_message),requests_storage,&requests_state);
    other_queue=xQueueCreateStatic(1,sizeof(struct pvr_gpu_service_message),other_storage,&other_state);
    q.service=&service;q.firmware[5].storage.cpu=timeline;q.firmware[5].storage.bytes=sizeof(timeline);
    q.firmware_address[5]=0xc0850000;gpu_queue_timing_bind(&q);
    host_rtos_start();
    return 0;
}
