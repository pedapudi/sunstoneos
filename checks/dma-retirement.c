/* Real service/controller and native cache adapter, with modeled DMA registers.
 * Each process starts fresh. No physical interrupt or DMA authority is claimed. */
#define kernel_main original_kernel_main
#include "dma-service.c"
#undef kernel_main
#define retirement_case (*(volatile unsigned *)UINT64_C(0x47fff004))
static uint32_t mask_calls,late_irqs,rearm_calls,peer_command,peer_sent,peer_collected;
static uint64_t other_ticket;
static uint32_t admin_ready,sender_resume;
BaseType_t __real_xQueueGenericSend(QueueHandle_t,const void *,TickType_t,BaseType_t);
BaseType_t __wrap_xQueueGenericSend(QueueHandle_t queue,const void *item,TickType_t ticks,BaseType_t position)
{
    if(retirement_case==9&&queue==service.requests&&!get(&sender_resume)){
        configASSERT(service.retirement_senders==1);
        set(&admin_ready,1);wait_flag(&sender_resume,1);
    }
    return __real_xQueueGenericSend(queue,item,ticks,position);
}
static void retirement_admin(void *unused);
BaseType_t __real_dma_service_task_privileged(TaskFunction_t,void *);
BaseType_t __wrap_dma_service_task_privileged(TaskFunction_t entry,void *argument)
{return (entry==retirement_admin&&!argument)||__real_dma_service_task_privileged(entry,argument);}
static void retirement_admin(void *unused)
{
    (void)unused;wait_flag(&admin_ready,1);
    int result=dma_service_quiesce(&service);
    if(retirement_case==9){
        configASSERT(!result);vTaskDelay(3);
        configASSERT(service.retirement_senders==1&&!get(&mask_calls)&&!dma_service_retired(&service));
        set(&sender_resume,1);
    }
    TickType_t began=xTaskGetTickCount();
    while(!result){configASSERT(xTaskGetTickCount()-began<100);vTaskDelay(1);result=dma_service_quiesce(&service);}
    unsigned reads=get(&accesses[0]),rearms=get(&rearm_calls);
    if(retirement_case==5){
        configASSERT(result==1&&dma_service_retired(&service)&&get(&mask_calls)==1);
    }else{
        configASSERT(result==-1&&!dma_service_retired(&service));
        if(retirement_case==6)configASSERT(service.channels[0].owner==0&&service.controller.channels[0].claimed);
        if(retirement_case==7)configASSERT(service.clients[0].pending&&uxQueueMessagesWaiting(service.clients[0].replies));
        configASSERT(get(&mask_calls)==(retirement_case==8?1:0));
        configASSERT(atomic_load(&service.quiesce_fault)==(retirement_case==6?DMA_QUIESCE_CHANNEL_HELD:
            retirement_case==7?DMA_QUIESCE_REPLY_HELD:
            retirement_case==9?DMA_QUIESCE_REQUEST_HELD:DMA_QUIESCE_MASK_FAILED));
    }
    BaseType_t woken=pdFALSE;dma_service_irq_notify_from_isr(&service,1,&woken);
    configASSERT(!woken);
    vTaskDelay(3);configASSERT(get(&accesses[0])==reads&&get(&rearm_calls)==rearms);
    platform_puts("PASS DMA retirement: owner request retires idle service and preserves abandoned channel or reply\n");
    platform_exit(0);
}
static int retirement_mask(void *cookie)
{
    configASSERT(cookie==&mask_calls);privileged();
    uint64_t pmr;__asm__ volatile("mrs %0,ICC_PMR_EL1":"=r"(pmr));
    configASSERT(pmr>portMAX_API_PRIORITY_MASK);
    /* Model delayed disable completion without retaining scheduler locks. */
    vTaskDelay(2);
    __atomic_fetch_add(&mask_calls,1,__ATOMIC_RELEASE);
    /* A late handler must not republish a notification during retirement. */
    BaseType_t woken=pdFALSE;
    dma_service_irq_notify_from_isr(&service,1,&woken);
    __atomic_fetch_add(&late_irqs,1,__ATOMIC_RELEASE);
    configASSERT(!woken && !atomic_load(&service.irq_pending));
    return retirement_case==1||retirement_case==8?0:1;
}
static int retirement_rearm(void *cookie)
{configASSERT(cookie==&mask_calls);privileged();__atomic_fetch_add(&rearm_calls,1,__ATOMIC_RELEASE);return 1;}
static void retirement_peer(void *unused)
{
    (void)unused;wait_flag(&peer_command,1);
    struct dma_service_request r={.operation=DMA_SERVICE_STATUS};
    configASSERT(dma_service_send(&service,&r,0,&other_ticket));set(&peer_sent,1);
    wait_flag(&peer_command,2);
    struct dma_service_reply reply;
    configASSERT(dma_service_collect(&service,other_ticket,&reply,100) && reply.result==DMA_SERVICE_OK);
    set(&peer_collected,1);vTaskSuspend(NULL);configASSERT(0);
}
static void expect_busy(void)
{configASSERT(request(DMA_SERVICE_RETIRE,0,0).result==DMA_SERVICE_BUSY && !get(&mask_calls) && !dma_service_retired(&service));}
static void retirement_client(void *unused)
{
    (void)unused;
    configASSERT(!dma_service_bind_retirement(&service,retirement_mask,&mask_calls));
    if(retirement_case>=5){
        if(retirement_case==6)configASSERT(request(DMA_SERVICE_OPEN,0,0).result==DMA_SERVICE_OK);
        if(retirement_case==7||retirement_case==9){
            struct dma_service_request r={.operation=DMA_SERVICE_STATUS};uint64_t ticket;
            configASSERT(dma_service_send(&service,&r,0,&ticket));
            if(retirement_case==9)for(;;)vTaskSuspend(NULL);
            while(!uxQueueMessagesWaiting(service.clients[0].replies))vTaskDelay(1);
        }
        set(&admin_ready,1);for(;;)vTaskSuspend(NULL);
    }
    configASSERT(request(DMA_SERVICE_RETIRE,0,1).result==DMA_SERVICE_INVALID);
    if(retirement_case==0 || retirement_case==4){
        configASSERT(request(DMA_SERVICE_OPEN,0,0).result==DMA_SERVICE_OK);expect_busy();
        set(&drain_allowed[0],0);set(&complete[0],retirement_case==4);
        configASSERT(request(DMA_SERVICE_SUBMIT,0,frequency).result==DMA_SERVICE_OK);expect_busy();
        if(retirement_case==0)configASSERT(request(DMA_SERVICE_CANCEL,0,0).result==DMA_SERVICE_OK);
        struct dma_service_reply reply=wait_phase(0,DMA_SERVICE_DRAINING);
        configASSERT(reply.holds_buffers);expect_busy();
        configASSERT(request(DMA_SERVICE_REAP,0,0).result==DMA_SERVICE_BUSY);
        if(retirement_case==4){
            reply=wait_phase(0,DMA_SERVICE_RECOVERY);
            configASSERT(reply.holds_buffers && service.controller.channels[0].transfer);
            expect_busy();vTaskDelay(3);configASSERT(!get(&mask_calls));
            platform_puts("PASS DMA retirement: stuck drain retains transfer and refuses retirement\n");platform_exit(0);
        }
        vTaskDelay(3);configASSERT(!get(&finished[0]));
        set(&drain_allowed[0],1);reply=wait_phase(0,DMA_SERVICE_RESULT);
        configASSERT(reply.outcome==DMA_SERVICE_CANCELED && reply.holds_buffers);expect_busy();
        configASSERT(request(DMA_SERVICE_REAP,0,0).result==DMA_SERVICE_OK);expect_busy();
        configASSERT(request(DMA_SERVICE_CLOSE,0,0).phase==DMA_SERVICE_CLOSED);
        /* Exercise successful output consumption as well as cancellation. */
        set(&complete[0],1);
        configASSERT(request(DMA_SERVICE_OPEN,0,0).result==DMA_SERVICE_OK);
        configASSERT(request(DMA_SERVICE_SUBMIT,0,frequency).result==DMA_SERVICE_OK);
        reply=wait_phase(0,DMA_SERVICE_RESULT);
        configASSERT(reply.outcome==DMA_SERVICE_COMPLETE && !memcmp(pool+4096,pool+8192,64));
        configASSERT(request(DMA_SERVICE_REAP,0,0).outcome==DMA_SERVICE_COMPLETE);
        configASSERT(request(DMA_SERVICE_CLOSE,0,0).phase==DMA_SERVICE_CLOSED);
    }
    if(retirement_case==2){
        set(&peer_command,1);wait_flag(&peer_sent,1);
        while(!uxQueueMessagesWaiting(service.clients[1].replies))vTaskDelay(1);
        expect_busy();set(&peer_command,2);wait_flag(&peer_collected,1);
    }
    if(retirement_case==3){
        vTaskSuspend(service.worker);
        struct dma_service_request r={.operation=DMA_SERVICE_RETIRE};uint64_t ticket;
        configASSERT(dma_service_send(&service,&r,0,&ticket));
        set(&peer_command,1);wait_flag(&peer_sent,1);
        vTaskResume(service.worker);
        struct dma_service_reply reply;
        configASSERT(dma_service_collect(&service,ticket,&reply,100) && reply.result==DMA_SERVICE_BUSY);
        configASSERT(!get(&mask_calls));set(&peer_command,2);wait_flag(&peer_collected,1);
    }
    /* Until the reply is collected, there is no retirement acknowledgment. */
    struct dma_service_request r={.operation=DMA_SERVICE_RETIRE};uint64_t ticket;
    configASSERT(dma_service_send(&service,&r,0,&ticket));
    while(!uxQueueMessagesWaiting(service.clients[0].replies))vTaskDelay(1);
    configASSERT(!dma_service_retired(&service));
    struct dma_service_reply reply;
    configASSERT(dma_service_collect(&service,ticket,&reply,100));
    if(retirement_case==1){
        configASSERT(reply.result==DMA_SERVICE_FAULT);vTaskDelay(3);configASSERT(!dma_service_retired(&service));
    }else{
        configASSERT(reply.result==DMA_SERVICE_OK);
        TickType_t began=xTaskGetTickCount();
        while(!dma_service_retired(&service)){configASSERT(xTaskGetTickCount()-began<100);vTaskDelay(1);}
    }
    unsigned reads=get(&accesses[0]),rearms=get(&rearm_calls);
    BaseType_t woken=pdFALSE;dma_service_irq_notify_from_isr(&service,1,&woken);
    configASSERT(!woken && !atomic_load(&service.irq_pending));
    uint64_t unchanged=123;
    configASSERT(!dma_service_send(&service,&r,0,&unchanged) && unchanged==123);
    vTaskDelay(3);
    configASSERT(get(&mask_calls)==1 && get(&late_irqs)==1 && get(&accesses[0])==reads && get(&rearm_calls)==rearms);
    platform_puts("PASS DMA retirement: closed channels, collected reply, worker acknowledgment, retained mask failure and late IRQ exclusion\n");
    platform_exit(0);
}
void kernel_main(void)
{
    platform_init();__asm__ volatile("mrs %0,cntfrq_el0":"=r"(frequency));configASSERT(frequency);
    regs[0x34/4]=1;regs[index_of(0,8)]=0x41000000;drain_allowed[0]=1;
    for(unsigned i=0;i<64;++i)pool[4096+i]=(uint8_t)(i^0xa5);
    struct dma_memory_window window={(uintptr_t)pool,(uintptr_t)pool,sizeof(pool),7};
    configASSERT(a733_dma_native_init(&native,0x09000000,0x09000000,&window,1,NULL,0,fatal,NULL));
    mappings[0][0]=(struct a733_dma_mapping){pool+4096,(uintptr_t)pool+4096,64,A733_DMA_READ};
    mappings[0][1]=(struct a733_dma_mapping){pool+8192,(uintptr_t)pool+8192,64,A733_DMA_WRITE};
    segments[0]=(struct a733_dma_segment){{(uintptr_t)pool+4096,0,4,8},{(uintptr_t)pool+8192,0,4,8},64};
    jobs[0]=(struct dma_service_job){.kind=DMA_SERVICE_RAM,.channel=0,
        .transfer={{pool,(uintptr_t)pool,64,A733_DMA_DESCRIPTORS},segments,1,mappings[0],2}};
    const struct a733_dma_operations ops={read32,write32,cache,barrier,ticks,mapping,fifo};
    struct dma_service_protected protected[]={{(uintptr_t)&service,(uintptr_t)&service,sizeof(service)},
        {(uintptr_t)&native,(uintptr_t)&native,sizeof(native)}};
    configASSERT(dma_service_init(&service,&ops,NULL,0x04601000,1,frequency/20,jobs,1,protected,2,3,3));
    configASSERT(!dma_service_bind_retirement(&service,retirement_mask,&mask_calls));
    const struct dma_service_irq irq={retirement_rearm,&mask_calls,1,frequency/configTICK_RATE_HZ};
    configASSERT(dma_service_bind_irq(&service,&irq));
    configASSERT(!dma_service_bind_retirement(&service,NULL,&mask_calls));
    configASSERT(dma_service_bind_retirement(&service,retirement_mask,&mask_calls));
    configASSERT(!dma_service_bind_retirement(&service,retirement_mask,&mask_calls));
    TaskHandle_t a=xTaskCreateStaticAffinitySet(retirement_client,"retire-client",2048,NULL,2,stacks[0],&tasks[0],1);
    TaskHandle_t b=xTaskCreateStaticAffinitySet(retirement_peer,"retire-peer",2048,NULL,2,stacks[1],&tasks[1],2);
    configASSERT(a && b && dma_service_bind(&service,0,a,1) && dma_service_bind(&service,1,b,1));
    if(retirement_case>=5)configASSERT(xTaskCreateStaticAffinitySet(retirement_admin,"return-owner",2048,NULL,2,stacks[2],&tasks[2],2));
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
