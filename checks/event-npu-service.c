/* Real service worker, FreeRTOS notifications and compare deadlines. Register
 * effects follow a733_npu_controller.c and the INTID97 event/idle contract in
 * docs/designs/npu-irq-completion.md; no physical delivery is modeled. */
#include "../drivers/npu/npu_service.c"
#include "host_rtos.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static struct npu_service service;
static struct npu_service_runtime_state runtime;
static StaticQueue_t request_state;
static uint8_t request_bytes[sizeof(struct npu_service_message)];
static unsigned device_reads,finishes,counter_reads,faults,use_runtime,mask_pending;
static unsigned unbound;
uint64_t npu_arch_current_el(void){return 4;}
uint64_t npu_arch_stack_select(void){return 0;}
int npu_arch_zva_block_is_64(void){return 1;}
void npu_arch_zero_line(uintptr_t at){memset((void *)at,0,64);}
static uint64_t clock_us(void *cookie){(void)cookie;++counter_reads;return host_time_us();}
static int read_reg_model(void *cookie,uint32_t offset,uint32_t *out)
{
    (void)cookie;++device_reads;
    assert(offset==0x10||offset==4);
    *out=offset==0x10?1:UINT32_C(0x7fffffff);
    return 1;
}
static void barrier(void *cookie){(void)cookie;}
static int finish_job(void *cookie,const struct a733_npu_command *job,int canceled)
{(void)cookie;(void)job;assert(!canceled);++finishes;return 1;}
static int mask_model(void *cookie){(void)cookie;return !mask_pending;}
static int request_model(void *cookie,unsigned client,const struct npu_service_request *request,
                         enum npu_service_result *result)
{(void)cookie;(void)client;(void)request;(void)result;assert(0);return 0;}
static int step_model(void *cookie){(void)cookie;return 0;}
static void fault_model(void *cookie,unsigned client,int failed)
{(void)cookie;assert(client==NPU_SERVICE_NONE&&failed);++faults;}
static void closed_model(void *cookie,unsigned client){(void)cookie;(void)client;assert(0);}
static void irq(void *cookie)
{
    (void)cookie;BaseType_t woken=pdFALSE;
    npu_service_irq_notify_from_isr(&service,1,&woken);
    portYIELD_FROM_ISR(woken);
}
static void pause_until(uint64_t until)
{
    assert(sunstone_deadline_wait_until(until)==SUNSTONE_DEADLINE_EXPIRED);
    assert(host_time_us()==until);
}
static void start_job(uint64_t timeout)
{
    service.controller.state=A733_NPU_RUNNING;
    service.controller.start=host_time_us();service.controller.timeout=timeout;
    service.controller.irq_seen=0;service.controller.bootstrap=0;
    service.irq_masked=0;
    xTaskNotifyGive(service.worker);
}
static void test(void *unused)
{
    (void)unused;
    assert(sunstone_deadline_register());
    service.signals=&service.own_signals;
    service.controller.state=A733_NPU_READY;
    service.controller.ops=(struct a733_npu_ops){.read=read_reg_model,.microseconds=clock_us,
        .barrier=barrier,.finish=finish_job};
    service.ops=service.controller.ops;
    service.irq_ops.mask=mask_model;
    service.initialized=1;service.irq_bound=!unbound;service.irq_masked=1;
    service.owner=service.running=NPU_SERVICE_NONE;
    atomic_init(&service.activation_state,SERVICE_ACTIVE);
    service.requests=xQueueCreateStatic(1,sizeof(struct npu_service_message),request_bytes,&request_state);
    assert(service.requests);
    if(use_runtime){
        runtime.loop=runtime_worker;
        runtime.hooks=(struct npu_service_runtime){request_model,step_model,fault_model,closed_model,0};
        service.runtime=&runtime;
    }
    instance=&service;
    service.worker=host_task_create("npu-owner",worker,&service,4);
    assert(service.worker);
    pause_until(2000137);
    assert(npu_service_wakes.waits==1&&!npu_service_wakes.events&&!npu_service_wakes.deadlines);
    assert(counter_reads==1&&!device_reads); /* Two seconds, no owner wake. */
    if(unbound){
        static struct npu_service_client client;
        service.clients=&client;service.client_count=1;
        client.task=xTaskGetCurrentTaskHandle();
        client.replies=xQueueCreateStatic(1,sizeof(struct npu_service_reply),client.reply_storage,&client.reply_state);
        start_job(200000);
        pause_until(2000138);
        assert(service.controller.state==A733_NPU_DONE&&finishes==1);
        const uint64_t wakes=npu_service_wakes.events+npu_service_wakes.deadlines;
        pause_until(4000138);
        assert(npu_service_wakes.events+npu_service_wakes.deadlines==wakes);
        const struct npu_service_request request={.operation=NPU_SERVICE_STATUS};
        struct npu_service_reply reply;uint64_t ticket;
        assert(npu_service_send(&service,&request,0,&ticket));
        assert(sunstone_deadline_wait_until(host_time_us()+100)==SUNSTONE_DEADLINE_NOTIFIED);
        assert(npu_service_collect(&service,ticket,&reply,0)&&reply.state==A733_NPU_DONE);
        puts("PASS unbound NPU: precise active completion, idle blocks, request and reply publication wake without IRQ binding");
        return;
    }
    start_job(200000);
    const uint64_t before=npu_service_wakes.events;
    host_irq_at(2100137,irq,NULL);
    pause_until(2100138);
    assert(service.controller.state==A733_NPU_DONE&&finishes==1&&device_reads==2);
    assert(npu_service_wakes.events==before+1&&!npu_service_wakes.deadlines);
    assert(npu_service_irq_completions(&service)==1);
    /* No IRQ: the one exact device deadline faults and retains the job. */
    start_job(1237);
    pause_until(2101374);
    assert(service.controller.state==A733_NPU_RUNNING);
    pause_until(2101375);
    assert(service.controller.state==A733_NPU_FAULT&&service.controller.fault==A733_NPU_TIMEOUT);
    assert(npu_service_wakes.deadlines==1&&finishes==1&&device_reads==2);
    assert(!use_runtime||faults==1);
    /* GIC propagation never completes. Its availability deadline retains the
     * source once; an expired retry must never make the faulted owner spin. */
    mask_pending=1;service.irq_masked=0;
    xTaskNotifyGive(service.worker);
    pause_until(host_time_us()+10001);
    assert(service.irq_retained&&service.irq_waiting);
    const uint64_t waits=npu_service_wakes.waits;
    pause_until(host_time_us()+2000000);
    assert(npu_service_wakes.waits==waits);
    puts("PASS NPU owner: 0 no-event idle wakes/s, one IRQ wake, exact 1237 us timeout, and retained GIC failure stays blocked");
}
int main(int argc,char **argv)
{
    use_runtime=argc>1&&atoi(argv[1])==1;
    unbound=argc>1&&atoi(argv[1])==2;
    host_timer_handler(sunstone_deadline_irq);
    host_rtos_run(test,NULL,2,3000000);
    return 0;
}
