/* Actual FreeRTOS worker, controller and native memory; MMIO effects and ISR
 * notification delivery are modeled. The native adapter has a separate
 * instruction fixture for its MMIO accesses and completing barriers. */
#define kernel_main unused_polled_kernel_main
#include "npu-service.c"
#undef kernel_main
#define cancel_case (*(volatile unsigned *)UINT64_C(0x47fff000))
static struct npu_service_cancel_diagnostic diagnostic;
static uint64_t modeled_us=1,triggered_us,asserted_us,released_us;
static unsigned submitted,bootstrap_count,masked=1,stopped,stop_count,restarts,checkpoints;
static TaskHandle_t bootstrap_task;
static uint64_t diagnostic_now(void *unused)
{(void)unused;privileged();modeled_us+=100;return modeled_us;}
static void diagnostic_delay(void *unused,unsigned us)
{(void)unused;privileged();configASSERT(us<=1000);modeled_us+=us;}
static int diagnostic_read(void *unused,unsigned offset,uint32_t *value)
{
    (void)unused;privileged();configASSERT(offset<sizeof(model.regs));
    if(model.canceled && cancel_case!=3 && modeled_us-released_us>=300) {
        model.regs[0x3a8/4]|=16;
        if(cancel_case!=2){model.regs[1]=IDLE;model.running=0;}
    }
    if(offset==0x660)model.regs[offset/4]=5;
    if(offset==0x664)model.regs[offset/4]=cancel_case==9 ? 0xe018a000 : 0xe010a008;
    *value=model.regs[offset/4];
    if(offset==0x10)model.regs[offset/4]=0;
    return 1;
}
static int diagnostic_write(void *unused,unsigned offset,uint32_t value)
{
    (void)unused;privileged();configASSERT(offset<sizeof(model.regs));
    uint32_t previous=model.regs[offset/4];model.regs[offset/4]=value;
    if(offset==0x3a8) {
        if(value&1){model.regs[0x388/4]=0;model.regs[1]=IDLE;model.running=0;}
        if(!(previous&8) && (value&8))asserted_us=modeled_us;
        if((previous&8) && !(value&8)) {
            configASSERT(modeled_us-asserted_us>100);
            model.canceled=1;released_us=modeled_us;model.regs[offset/4]&=~16u;
        }
    }
    if(offset==0x3a4) {
        configASSERT(value==0x1ffff && native.memory.state==A733_NPU_MEMORY_DEVICE);
        model.running=1;model.canceled=model.event_sent=0;model.regs[1]=0;
        model.bootstrap=model.regs[0x654/4]==0xe0089000;
        if(model.bootstrap)++bootstrap_count;else ++submitted;
        triggered_us=modeled_us;
        if(model.regs[0x654/4]==0xe010a000) {
            if(cancel_case==1)model.regs[1]=IDLE;
            if(cancel_case==8)model.regs[0x3a8/4]|=16;
        }
    }
    return 1;
}
static int mask_irq(void *cookie)
{configASSERT(cookie==&service);privileged();masked=1;return 1;}
static void notify(void)
{
    BaseType_t woken=pdFALSE;
    npu_service_irq_notify_from_isr(&service,1,&woken);
}
static int rearm_irq(void *cookie)
{
    configASSERT(cookie==&service && masked && !stopped);privileged();masked=0;
    if(model.regs[0x654/4]==0xe010a000)return 1;
    model.regs[0x10/4]=1;model.regs[1]=IDLE;model.running=0;model.event_sent=1;
    if(cancel_case==6 && bootstrap_count==2)return 1;
    masked=1;notify();return 1;
}
static int stop_irq(void *cookie)
{
    configASSERT(cookie==&service && masked && service.owner==0);privileged();
    configASSERT(service.controller.state==A733_NPU_CLOSED && !model.regs[0x14/4] &&
        !model.regs[0x388/4] && model.regs[1]==IDLE && native.memory.state==A733_NPU_MEMORY_IDLE);
    stopped=1;++stop_count;return 1;
}
static int restart_irq(void *cookie)
{
    configASSERT(cookie==&service && stopped && masked && service.owner==0);privileged();
    configASSERT(service.controller.state==A733_NPU_CLOSED && npu_service_irq_stopped(&service));
    stopped=0;++restarts;return 1;
}
static uint64_t trigger_time(void *cookie)
{configASSERT(cookie==&service);privileged();return cancel_case==10 ? triggered_us-3000 : triggered_us;}
static int check_diagnostic(void *cookie,unsigned phase)
{
    configASSERT(cookie==&service);privileged();
    if(phase==2)return 1;
    configASSERT(phase==checkpoints && native.memory.state==A733_NPU_MEMORY_IDLE &&
        service.owner==0 && diagnostic.reaped);
    if(!phase && cancel_case==4) {
        taskENTER_CRITICAL();notify();taskEXIT_CRITICAL();
    }
    ++checkpoints;return cancel_case!=5;
}
static int prepared_diagnostic(void *cookie,const struct a733_npu_ops *ops,void *backend,
    const struct a733_npu_mmu_image *image,const struct a733_npu_command *commands,unsigned count)
{
    (void)ops;(void)backend;(void)image;(void)commands;
    configASSERT(cookie==&service && count==3 && xTaskGetCurrentTaskHandle()==bootstrap_task);return 1;
}
static struct npu_service_reply diagnostic_request(enum npu_service_operation op,unsigned command,uint64_t timeout)
{
    struct npu_service_request request={op,command,timeout};struct npu_service_reply reply;uint64_t ticket;
    configASSERT(npu_service_send(&service,&request,1,&ticket));
    while(!npu_service_collect(&service,ticket,&reply,1)){}
    configASSERT(reply.ticket==ticket);return reply;
}
static void retained_result(struct npu_service_reply reply)
{
    configASSERT(reply.result==NPU_SERVICE_FAULT && reply.state==A733_NPU_FAULT && reply.owns_session &&
        service.owner==0 && service.initialized && atomic_load(&service.activation_state)==2 &&
        native.initialized && native.memory.count==4);
    configASSERT(diagnostic_request(NPU_SERVICE_CLOSE,0,0).result==NPU_SERVICE_FAULT);
    platform_puts("PASS active NPU cancel retained failure\n");platform_exit(0);
}
static void diagnostic_owner(void *unused)
{
    (void)unused;
    while(atomic_load(&service.activation_state)!=2)vTaskDelay(1);
    struct npu_service_reply r=diagnostic_request(NPU_SERVICE_OPEN,0,100000);
    configASSERT(r.result==NPU_SERVICE_OK);
    r=diagnostic_request(NPU_SERVICE_WAIT,0,0);
    configASSERT(r.state==A733_NPU_READY && npu_service_irq_completions(&service)==1);
    configASSERT(diagnostic_request(NPU_SERVICE_RECOVER,0,100000).result==NPU_SERVICE_DENIED);
    r=diagnostic_request(NPU_SERVICE_SUBMIT,2,100000);
    if(cancel_case==10)retained_result(r);
    configASSERT(r.state==A733_NPU_RUNNING && r.result==NPU_SERVICE_OK);
    uint64_t ticket=r.job_ticket;
    r=diagnostic_request(NPU_SERVICE_CANCEL,0,20000);
    if(cancel_case==1 || cancel_case==8 || cancel_case==9)retained_result(r);
    configASSERT(r.result==NPU_SERVICE_OK && diagnostic.proven && diagnostic.job_ticket==ticket);
    r=diagnostic_request(NPU_SERVICE_WAIT,0,0);
    if(cancel_case==2 || cancel_case==3) {
        configASSERT(native.memory.state==A733_NPU_MEMORY_DEVICE && model.finished==1);
        retained_result(r);
    }
    configASSERT(r.state==A733_NPU_CANCELED && r.job_ticket==ticket && native.memory.state==A733_NPU_MEMORY_IDLE);
    for(unsigned i=0;i<4096;++i)configASSERT(data[4096+i]==0x69);
    r=diagnostic_request(NPU_SERVICE_REAP,0,0);
    configASSERT(r.canceled && r.state==A733_NPU_DRAINED && r.job_ticket==ticket && diagnostic.reaped);
    r=diagnostic_request(NPU_SERVICE_RECOVER,0,100000);
    if(cancel_case==4 || cancel_case==5)retained_result(r);
    configASSERT(r.result==NPU_SERVICE_OK && r.state==A733_NPU_BOOTING && r.owns_session);
    r=diagnostic_request(NPU_SERVICE_WAIT,0,0);
    if(cancel_case==6)retained_result(r);
    configASSERT(r.state==A733_NPU_READY && diagnostic.recovered && checkpoints==2 &&
        npu_service_irq_completions(&service)==2 && restarts==1 && stop_count==1);
    r=diagnostic_request(NPU_SERVICE_SUBMIT,1,100000);configASSERT(r.result==NPU_SERVICE_OK);
    r=diagnostic_request(NPU_SERVICE_WAIT,0,0);configASSERT(r.state==A733_NPU_DONE);
    for(unsigned i=0;i<4096;++i)configASSERT(data[4096+i]==(uint8_t)(0xe0004000u>>19));
    if(cancel_case==7) {
        /* A client that rejects numerical output retains its unreaped result. */
        configASSERT(service.owner==0 && service.controller.state==A733_NPU_DONE &&
            diagnostic_request(NPU_SERVICE_CLOSE,0,0).result==NPU_SERVICE_BUSY);
        platform_puts("PASS active NPU cancel retained output mismatch\n");platform_exit(0);
    }
    r=diagnostic_request(NPU_SERVICE_REAP,0,0);configASSERT(r.state==A733_NPU_READY && !r.canceled);
    r=diagnostic_request(NPU_SERVICE_CLOSE,0,0);
    configASSERT(r.state==A733_NPU_CLOSED && !r.owns_session && !r.job_ticket && npu_service_irq_stopped(&service) &&
        npu_service_irq_completions(&service)==3 && atomic_load(&diagnostic.irq_receipts)==3 &&
        submitted==2 && bootstrap_count==2 && restarts==1 && stop_count==2 && model.finished==4);
    platform_puts("PASS active NPU cancel complete controller recovery\n");platform_exit(0);
}
static void diagnostic_bootstrap(void *unused)
{(void)unused;configASSERT(npu_service_activate(&service)==NPU_SERVICE_OK);vTaskSuspend(NULL);}
BaseType_t npu_active_cancel_task_privileged(TaskFunction_t entry,void *argument)
{
    if(entry==diagnostic_bootstrap && !argument)return pdTRUE;
    return npu_service_task_privileged(entry,argument);
}
void kernel_main(void)
{
    platform_init();memset(data,0x69,sizeof(data));set(&model.validated,1);
    struct a733_npu_buffer buffers[4];
    static const uint32_t addresses[3]={0xe0089000,0xe0004000,0xe010a000};
    for(unsigned i=0;i<3;++i)buffers[i]=(struct a733_npu_buffer){(uintptr_t)command_memory[i],
        (uintptr_t)command_memory[i],sizeof(command_memory[i]),addresses[i],i?A733_NPU_BUFFER_COMMAND:A733_NPU_BUFFER_BOOTSTRAP};
    buffers[3]=(struct a733_npu_buffer){(uintptr_t)data+4096,(uintptr_t)data+4096,4096,0xe0087000,A733_NPU_BUFFER_WRITE};
    configASSERT(a733_npu_bootstrap(command_memory[0],sizeof(command_memory[0])));
    configASSERT(a733_npu_native_init(&native,0x09000000,0x09000000,arena,sizeof(arena),(uintptr_t)arena,buffers,4,fatal,NULL));
    struct a733_npu_command commands[3];
    for(unsigned i=0;i<3;++i)configASSERT(a733_npu_memory_command(&native.memory,addresses[i],&commands[i]));
    model.regs[0x20/4]=0x9000;model.regs[0x24/4]=0x9202;model.regs[0x30/4]=0x1000003b;model.regs[1]=IDLE;
    bootstrap_task=xTaskCreateStaticAffinitySet(diagnostic_bootstrap,"cancel-boot",4096,NULL,5,stacks[1],&tasks[1],1);
    const struct npu_service_activation activation={bootstrap_task,prepared_diagnostic,&service};
    struct a733_npu_ops ops={diagnostic_read,diagnostic_write,diagnostic_now,diagnostic_delay,barrier,publish,finish};
    configASSERT(npu_service_init_deferred(&service,&ops,NULL,&native.memory.image,commands,3,4,1,&activation));
    TaskHandle_t client=xTaskCreateStaticAffinitySet(diagnostic_owner,"cancel-owner",4096,NULL,5,stacks[0],&tasks[0],1);
    configASSERT(client && npu_service_bind(&service,0,client,6));
    const struct npu_service_irq_ops irq={mask_irq,rearm_irq,stop_irq};
    configASSERT(npu_service_bind_irq(&service,&irq,&service,1));
    configASSERT(npu_service_cancel_diagnostic_enable(&service,&diagnostic,2,trigger_time,check_diagnostic,&service,restart_irq));
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
