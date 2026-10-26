/* Actual FreeRTOS service/native memory composition. Only register effects and
 * IRQ delivery are modeled; this does not establish physical GIC delivery. */
#define kernel_main unused_polled_kernel_main
#include "npu-service.c"
#undef kernel_main
/* Fixture selector is outside the ELF load/BSS interval and remains in QEMU RAM. */
#define npu_irq_case (*(volatile unsigned *)UINT64_C(0x47fff000))
static unsigned mask_calls,rearm_calls,stop_calls,masked,delivered,critical_rearm;
static unsigned runtime_closed,runtime_failed;
static TickType_t pending_start;
static int shared_case(void)
{return npu_irq_case==13 || npu_irq_case==14 || npu_irq_case==17 || npu_irq_case==18;}
static int runtime_case(void){return npu_irq_case==14 || npu_irq_case==17;}
static int stop_timeout(void)
{return npu_irq_case==12 || npu_irq_case==13 || npu_irq_case==14;}
static int timed_pending(void)
{
    if(!pending_start)pending_start=xTaskGetTickCount();
    return 0;
}
static uint64_t stationary_clock(void *unused)
{(void)unused;privileged();return 0;}
static int runtime_request(void *ctx,unsigned client,const struct npu_service_request *r,
    enum npu_service_result *result)
{(void)ctx;(void)client;(void)r;*result=NPU_SERVICE_INVALID;return 0;}
static int runtime_step(void *ctx){(void)ctx;return 0;}
static void runtime_finished(void *ctx,unsigned client,int fault)
{(void)ctx;(void)client;configASSERT(fault);++runtime_failed;}
static void runtime_close(void *ctx,unsigned client)
{(void)ctx;configASSERT(client==0);++runtime_closed;}
static const struct npu_service_runtime runtime_hooks={
    runtime_request,runtime_step,runtime_finished,runtime_close,0};
/* System counter at the latest modeled event delivery and at the owner's
 * latest DONE or READY reply: the client's share of completion latency. */
static volatile uint64_t event_ticks,reply_ticks;
static uint64_t irq_ticks(void)
{uint64_t v;__asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(v) :: "memory");return v;}
/* Case 9 pins the worker and the modeled route to core 1 instead of core 0. */
static UBaseType_t irq_route;
static void routed(void){configASSERT((UBaseType_t)1<<portGET_CORE_ID()==irq_route);}
static int irq_mask(void *cookie)
{
    configASSERT(cookie==&service);privileged();routed();++mask_calls;
    if(npu_irq_case==4)return -1;
    if(npu_irq_case==10 || npu_irq_case==16)return timed_pending();
    if(npu_irq_case==7 && mask_calls>=3)return 0;
    if(mask_calls==1)return 0;
    masked=1;return 1;
}
static int irq_rearm(void *cookie)
{
    configASSERT(cookie==&service && masked);privileged();routed();++rearm_calls;
    uint64_t pmr;__asm__ volatile("mrs %0,ICC_PMR_EL1":"=r"(pmr));
    configASSERT(pmr==portMAX_API_PRIORITY_MASK);++critical_rearm;
    if(npu_irq_case==5)return -1;
    if(npu_irq_case==11)return timed_pending();
    if(npu_irq_case==15 && (rearm_calls&1))return 0;
    masked=0;
    if(npu_irq_case==1 || npu_irq_case==8)return 1;
    /* An event becomes visible during the rearm window. Its notification must
     * survive the callback and be consumed next, without another trigger. */
    model.regs[0x10/4]|=npu_irq_case==3?UINT32_C(0x40000000):1;
    model.event_sent=1;model.regs[1]=IDLE;model.running=0;
    masked=1;++delivered;event_ticks=irq_ticks();BaseType_t woken=pdFALSE;
    npu_service_irq_notify_from_isr(&service,npu_irq_case==2?0:1,&woken);
    return npu_irq_case==19?0:1;
}
static int irq_stop(void *cookie)
{
    configASSERT(cookie==&service && masked);privileged();routed();++stop_calls;
    configASSERT(service.controller.state==A733_NPU_CLOSED && !model.regs[0x14/4] &&
        model.regs[1]==IDLE && (shared_case()?service.members==1:service.owner==0) &&
        (stop_calls==1 || service.has_deferred));
    if(npu_irq_case==6)return -1;
    if(stop_timeout())return timed_pending();
    return stop_calls<3?0:1;
}
static const struct npu_service_irq_ops irq_ops={irq_mask,irq_rearm,irq_stop};
static struct npu_service_reply irq_request(enum npu_service_operation op,unsigned command,uint64_t timeout)
{
    struct npu_service_request r={op,command,timeout};struct npu_service_reply reply;uint64_t ticket;
    configASSERT(npu_service_send(&service,&r,1,&ticket));
    while(!npu_service_collect(&service,ticket,&reply,1)){}
    return reply;
}
static struct npu_service_reply irq_wait(enum a733_npu_state wanted)
{
    TickType_t start=xTaskGetTickCount();struct npu_service_reply r;
    do {
        configASSERT(xTaskGetTickCount()-start<1000);vTaskDelay(1);
        r=irq_request(NPU_SERVICE_STATUS,0,0);
    }while(r.state!=wanted && r.state!=A733_NPU_FAULT);
    reply_ticks=irq_ticks();
    if(r.state==A733_NPU_FAULT){platform_puts("IRQ fixture fault=");platform_hex(r.fault);platform_puts(" masks=");platform_hex(mask_calls);platform_puts(" rearms=");platform_hex(rearm_calls);platform_puts(" pending=");platform_hex(atomic_load(&service.irq_pending));platform_puts("\n");}
    return r;
}
/* WAIT: exactly one held reply once the bootstrap or job leaves its active
 * state, with no STATUS polling by the owner. */
static struct npu_service_reply irq_hold(void)
{
    struct npu_service_reply r=irq_request(NPU_SERVICE_WAIT,0,0);
    reply_ticks=irq_ticks();vTaskDelay(2);configASSERT(!uxQueueMessagesWaiting(service.clients[0].replies));
    return r;
}
static void irq_owner(void *unused)
{
    (void)unused;configASSERT(npu_irq_case<=19);
    struct npu_service_reply r=irq_request(NPU_SERVICE_OPEN,0,100000);
    if(npu_irq_case==4 || npu_irq_case==10 || npu_irq_case==16)configASSERT(r.result==NPU_SERVICE_FAULT);
    else configASSERT(r.result==NPU_SERVICE_OK);
    if(shared_case()){
        r=irq_wait(A733_NPU_READY);configASSERT(r.state==A733_NPU_READY);
    }else if(npu_irq_case==8){
        r=irq_request(NPU_SERVICE_CANCEL,0,100000);configASSERT(r.result==NPU_SERVICE_OK);
        r=irq_wait(A733_NPU_CANCELED);configASSERT(r.state==A733_NPU_CANCELED);
        r=irq_request(NPU_SERVICE_REAP,0,0);configASSERT(r.result==NPU_SERVICE_OK && r.canceled);
    }else if(npu_irq_case==0 || npu_irq_case==6 || npu_irq_case==9 || npu_irq_case==12 || npu_irq_case==15 || npu_irq_case==19){
        r=npu_irq_case==9?irq_hold():irq_wait(A733_NPU_READY);configASSERT(r.state==A733_NPU_READY);
        for(unsigned j=0;j<2;++j){
            if(npu_irq_case==15 || npu_irq_case==19)vTaskDelay(pdMS_TO_TICKS(20));
            r=irq_request(NPU_SERVICE_SUBMIT,1,100000);configASSERT(r.result==NPU_SERVICE_OK);
            uint64_t job=r.job_ticket;
            r=npu_irq_case==9?irq_hold():irq_wait(A733_NPU_DONE);
            configASSERT(r.state==A733_NPU_DONE && r.result==NPU_SERVICE_OK && r.job_ticket==job);
            if(npu_irq_case!=6) {
                uint64_t hz;__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));
                platform_puts(npu_irq_case==9?"MEASURE WAIT":"MEASURE STATUS polls");
                platform_puts(" job event to DONE reply us=");
                platform_hex((reply_ticks-event_ticks)*1000000/hz);platform_puts("\n");
            }
            configASSERT(native.memory.state==A733_NPU_MEMORY_IDLE && data[4096]==(uint8_t)(0x10080000>>19));
            r=irq_request(NPU_SERVICE_REAP,0,0);configASSERT(r.result==NPU_SERVICE_OK);
        }
        configASSERT(npu_service_irq_completions(&service)==3 && delivered==3 && critical_rearm==(npu_irq_case==15?6u:3u));
    }else {
        r=npu_irq_case==1?irq_hold():irq_wait(A733_NPU_FAULT);configASSERT(r.state==A733_NPU_FAULT);
        if(npu_irq_case==1)configASSERT(r.result==NPU_SERVICE_FAULT && r.fault==A733_NPU_TIMEOUT);
        configASSERT(!npu_service_irq_completions(&service) && !npu_service_irq_stopped(&service));
        unsigned accesses=model.accesses,mc=mask_calls,rc=rearm_calls;
        vTaskDelay(4);r=irq_request(NPU_SERVICE_STATUS,0,0);
        configASSERT(model.accesses==accesses && (npu_irq_case==7 || mask_calls==mc) && rearm_calls==rc && !stop_calls);
        if(npu_irq_case==10 || npu_irq_case==11 || npu_irq_case==16) {
            configASSERT(r.fault==A733_NPU_IRQ && service.irq_retained);
            configASSERT((TickType_t)(xTaskGetTickCount()-pending_start)>=pdMS_TO_TICKS(10));
            configASSERT(r.owns_session==(npu_irq_case==11) && !model.finished);
            configASSERT(mask_calls==mc && rearm_calls==rc);
        } else if(npu_irq_case!=4 && npu_irq_case!=7)configASSERT(r.owns_session);
        platform_puts("PASS NPU IRQ retained failure\n");platform_exit(0);
    }
    r=irq_request(NPU_SERVICE_CLOSE,0,0);
    if(stop_timeout()) {
        configASSERT(r.result==NPU_SERVICE_FAULT && r.fault==A733_NPU_IRQ && r.owns_session);
        configASSERT(service.irq_retained && !npu_service_irq_stopped(&service));
        configASSERT((TickType_t)(xTaskGetTickCount()-pending_start)>=pdMS_TO_TICKS(10));
        unsigned accesses=model.accesses,mc=mask_calls,rc=rearm_calls,sc=stop_calls;
        vTaskDelay(4);r=irq_request(NPU_SERVICE_STATUS,0,0);
        configASSERT(r.result==NPU_SERVICE_FAULT && r.owns_session && model.accesses==accesses);
        configASSERT(mask_calls==mc && rearm_calls==rc && stop_calls==sc);
        if(shared_case())configASSERT(service.members==1);
        if(runtime_case())configASSERT(runtime_failed==1 && !runtime_closed);
    } else if(npu_irq_case==6){configASSERT(r.result==NPU_SERVICE_FAULT && r.owns_session && stop_calls==1);}
    else {
        configASSERT(r.result==NPU_SERVICE_OK && !r.owns_session && stop_calls==3);
        configASSERT(npu_service_irq_stopped(&service));
        if(runtime_case())configASSERT(runtime_closed==1 && !runtime_failed);
        r=irq_request(NPU_SERVICE_OPEN,0,100000);configASSERT(r.result==NPU_SERVICE_FAULT);
    }
    platform_puts("PASS NPU IRQ native ownership, rearm window, repeated jobs, deferred CLOSE and retained failure\n");platform_exit(0);
}
void kernel_main(void)
{
    platform_init();memset(data,0x69,sizeof(data));set(&model.validated,1);
    struct a733_npu_buffer buffers[4];
    for(unsigned i=0;i<3;++i)buffers[i]=(struct a733_npu_buffer){(uintptr_t)command_memory[i],
        (uintptr_t)command_memory[i],sizeof(command_memory[i]),0x10000000+i*524288,
        i?A733_NPU_BUFFER_COMMAND:A733_NPU_BUFFER_BOOTSTRAP};
    buffers[3]=(struct a733_npu_buffer){(uintptr_t)data+4096,(uintptr_t)data+4096,4096,0x10180000,A733_NPU_BUFFER_WRITE};
    configASSERT(a733_npu_bootstrap(command_memory[0],sizeof(command_memory[0])));
    configASSERT(a733_npu_native_init(&native,0x09000000,0x09000000,arena,sizeof(arena),(uintptr_t)arena,buffers,4,fatal,NULL));
    struct a733_npu_command commands[3];
    for(unsigned i=0;i<3;++i)configASSERT(a733_npu_memory_command(&native.memory,buffers[i].device,&commands[i]));
    model.regs[0x20/4]=0x9000;model.regs[0x24/4]=0x9202;model.regs[0x30/4]=0x1000003b;
    model.regs[0x28/4]=0x20240101;model.regs[1]=IDLE;
    irq_route=npu_irq_case==9 ? 2 : 1;
    struct a733_npu_ops selected_ops=model_ops;
    if(npu_irq_case==16)selected_ops.microseconds=stationary_clock;
    configASSERT(npu_service_init(&service,&selected_ops,NULL,&native.memory.image,commands,3,4,irq_route));
    if(shared_case())configASSERT(npu_service_share(&service));
    if(runtime_case())configASSERT(npu_service_runtime(&service,&runtime_hooks,&service));
    /* The route must name exactly the one CPU the worker is pinned to. */
    configASSERT(!npu_service_bind_irq(&service,&irq_ops,&service,0));
    configASSERT(!npu_service_bind_irq(&service,&irq_ops,&service,3));
    configASSERT(!npu_service_bind_irq(&service,&irq_ops,&service,irq_route^3));
    configASSERT(!service.irq_bound);
    configASSERT(npu_service_bind_irq(&service,&irq_ops,&service,irq_route));
    TaskHandle_t client=xTaskCreateStaticAffinitySet(irq_owner,"irq-owner",4096,NULL,5,stacks[0],&tasks[0],1);
    configASSERT(client && npu_service_bind(&service,0,client,2));
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
