/* Finite I2S receive with the DMA0 interrupt binding. The service, stream,
 * controller, native cache, audio providers and the image's interrupt
 * adapter (drivers/audio/dma_service_i2s_irq.c) are the real ones. DMA/I2S
 * registers, the GIC SPI and interrupt delivery are modeled; the handler
 * runs in a privileged task, so physical delivery is not established. The
 * register and FIFO model is checks/i2s-stream.c. Run by i2s-interrupt.py. */
#define kernel_main finite_kernel_main
#define dma_read finite_dma_read
#define prepared finite_prepared
#include "i2s-stream.c"
#undef kernel_main
#undef dma_read
#undef prepared
#include "dma_service_i2s_irq.h"
#define irq_case (*(volatile unsigned *)UINT64_C(0x47fff008))
static struct dma_service_i2s_irq irq;
static uint32_t spi_enabled,masks,rearms,deliveries,dma_reads,delivery_enabled,command;
static unsigned creating_device;
static StaticTask_t device_task;
static StackType_t device_stack[4096];
static void device_model(void *);
BaseType_t i2s_interrupt_task_privileged(TaskFunction_t,void *);
BaseType_t i2s_interrupt_task_privileged(TaskFunction_t entry,void *context)
{return (creating_device && entry==device_model) || i2s_stream_task_privileged(entry,context);}
static uint32_t dma_read(void *context,uintptr_t address){inc(&dma_reads);return finite_dma_read(context,address);}
static int prepared(void *context,uintptr_t base,uint16_t channels)
{configASSERT(context==&parent);return base==D && channels==1 && a733_audio_parent_ready(&parent);}
/* The DMA0 line: an enabled pending event on the receive channel. */
static int line(void){return (dma[di(0,0x38)]&dma[di(0,0x34)])!=0;}
/* Modeled distributor for the adapter's single SPI. */
int gicv3_spi_mask(const struct gicv3_spi *s)
{configASSERT(s->distributor==0x03400000 && s->intid==A733_DMA0_INTID);inc(&masks);set(&spi_enabled,0);return 1;}
int gicv3_spi_rearm(const struct gicv3_spi *s)
{
    configASSERT(s->distributor==0x03400000 && s->intid==A733_DMA0_INTID);privileged();
    /* The service rearms only after its polls acknowledged every event. */
    configASSERT(!line());inc(&rearms);set(&spi_enabled,1);return 1;
}
/* Acts as the distributor and the device: completes a requested period and
 * delivers an enabled, asserted level SPI to the handler on this core. */
static void device_model(void *unused)
{
    (void)unused;
    uint64_t el;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));configASSERT(el==4);
    for(;;){
        /* Case 0 completes the period here, while the worker waits; case 1
         * leaves it to the worker's next polled register read. */
        if(get(&command)){set(&requested,1);if(!irq_case)advance();set(&command,0);}
        if(get(&delivery_enabled) && get(&spi_enabled) && line()){
            inc(&deliveries);dma_service_i2s_irq_handler(&irq);
        }
        taskYIELD();
    }
}
static void wait_armed(void)
{
    for(unsigned n=0;n<100 && !get(&spi_enabled);++n)vTaskDelay(1);
    configASSERT(get(&spi_enabled));
}
/* The worker reads no DMA register while it waits for the interrupt. Each
 * client request also notifies the worker, so a pass can follow the last
 * request even though the SPI is already armed. Settle until no read occurs
 * for three ticks first; emulated cores can be descheduled by the host. */
static void quiet(unsigned ticks)
{
    unsigned n=0,reads;
    do{reads=get(&dma_reads);vTaskDelay(3);}while(get(&dma_reads)!=reads && ++n<30);
    configASSERT(n<30);
    reads=get(&dma_reads);vTaskDelay(ticks);
    configASSERT(get(&dma_reads)==reads && get(&spi_enabled));
}
/* Complete one period and wait for the worker to bridge into the next. */
static TickType_t bridged(void)
{
    unsigned before=get(&submissions);TickType_t start=xTaskGetTickCount();
    set(&command,1);
    while(get(&submissions)==before){configASSERT(xTaskGetTickCount()-start<100);vTaskDelay(1);}
    configASSERT(get(&submissions)==before+1);
    return xTaskGetTickCount()-start;
}
static void receive(unsigned n)
{
    struct a733_i2s_period p=period(0);
    configASSERT(p.kind==A733_I2S_CAPTURED && p.slot==n%3 && p.first_frame==(uint64_t)n*16 &&
        p.frames==16 && p.counter==(n+1)*16 && p.observed_before_us<=p.observed_after_us);
    configASSERT(pool[4096+(n%3)*256]==0x80+n+1 && pool[4096+(n%3)*256+255]==0x80+n+1);
    configASSERT(request(DMA_SERVICE_PERIOD_PUT,0,0,p.ticket).result==DMA_SERVICE_OK);
}
static void interrupt_client(void *unused)
{
    (void)unused;while(!get(&peer_stage))vTaskDelay(1);
    open_stream(0);
    configASSERT(request(DMA_SERVICE_SUBMIT,0,frequency,0).result==DMA_SERVICE_OK);
    (void)wait_phase(0,DMA_SERVICE_RUNNING);
    if(irq_case==0){
        /* With a 200-tick fallback only the queue-end interrupt can wake the
         * worker early: each period's bridge follows its delivery, and the
         * worker reads no register between periods. */
        set(&delivery_enabled,1);wait_armed();quiet(30);configASSERT(get(&rearms)==1);
        for(unsigned n=0;n<5;++n){
            configASSERT(bridged()<20);configASSERT(get(&deliveries)==n+1);
            receive(n);wait_armed();quiet(10);
        }
        configASSERT(get(&rearms)==6 && get(&masks)==5);
    }else{
        /* The native one-tick fallback: with no delivery at all the worker
         * still polls every tick, so each bridge completes within the few
         * ticks of the polled path (stop and drain take a second pass), far
         * below a 200-tick fallback. */
        wait_armed();
        for(unsigned n=0;n<5;++n){configASSERT(bridged()<10);receive(n);}
        configASSERT(!get(&deliveries) && get(&spi_enabled) && get(&rearms)==1);
    }
    configASSERT(request(DMA_SERVICE_CANCEL,0,0,0).result==DMA_SERVICE_OK);
    struct dma_service_reply r=wait_phase(0,DMA_SERVICE_RESULT);
    configASSERT(r.outcome==DMA_SERVICE_CANCELED && r.holds_buffers);
    configASSERT(request(DMA_SERVICE_REAP,0,0,0).result==DMA_SERVICE_OK);close_stream(0);
    platform_puts("PASS I2S interrupt: finite RX queue-end wake bridges each period, ack before rearm, one-tick fallback without delivery\n");
    platform_exit(0);
}
void kernel_main(void)
{
    tx_mode=0;platform_init();__asm__ volatile("mrs %0,cntfrq_el0":"=r"(frequency));configASSERT(frequency);
    ccu[0]=0xf8000000u|(99u<<8)|(99u<<16);ccu[0x280/4]=0xf8047f00;
    ccu[0x191c/4]=0x60006;ccu[0x199c/4]=0x60000;
    dma[0x34/4]=3;dma[di(0,8)]=0x41000000;
    configASSERT(a733_platform_locks_init(&locks,clock_now,NULL));
    configASSERT(a733_clock_leases_init(&clocks,clock_now,NULL));
    struct a733_audio_operations backend={.enter=enter,.leave=leave,.now_us=clock_now,.read=read_reg,.write=write_reg};
    configASSERT(a733_audio_leases_init(&audio,&clocks,7,&backend,NULL));
    configASSERT(a733_audio_parent_init(&parent,&audio.operations,&audio,0,100000));
    struct a733_pins_operations pops={enter,leave,clock_now,read_reg,write_reg};
    configASSERT(a733_pins_init(&pins,&pops,NULL,A733_PINS_PB_MASK));
    struct a733_pin_request req[]={{5,{0,0,0},{3,0,0}},{6,{0,0,0},{3,0,0}},{8,{0,0,0},{3,0,0}}};
    configASSERT(a733_i2s_platform_stream_init(&bindings[0],&parent,&pins,&audio,7,req,3,A733_I2S_RECEIVE));
    profiles[0].stream=(struct a733_i2s_stream_profile){{24576000,16000,0,32,32,1,0},
        a733_i2s_platform_stream_operations(),&bindings[0],A733_I2S_RECEIVE,10000,0};
    struct dma_memory_window window={(uintptr_t)pool,(uintptr_t)pool,sizeof(pool),7};
    struct dma_fifo_port ports[]={{A733_I2S0_RXFIFO,3,1u<<4,1u<<8,A733_DMA_READ}};
    configASSERT(a733_dma_native_init(&native,0x09000000,0x09000000,&window,1,ports,1,fatal,NULL));
    struct dma_service_job job={0};struct a733_dma_segment segments[3];struct a733_dma_mapping maps[3];
    for(unsigned i=0;i<3;++i){
        uintptr_t data=(uintptr_t)pool+4096+i*256;
        maps[i]=(struct a733_dma_mapping){(void *)data,data,256,A733_DMA_WRITE};
        segments[i]=(struct a733_dma_segment){{A733_I2S0_RXFIFO,3,4,8},{data,0,4,8},64};
    }
    job=(struct dma_service_job){.kind=DMA_SERVICE_I2S_STREAM,.channel=0,.stream=&profiles[0],
        .transfer={{pool,(uintptr_t)pool,256,A733_DMA_DESCRIPTORS},segments,3,maps,3}};
    const struct a733_dma_operations ops={dma_read,dma_write,cache,barrier,ticks,mapping,fifo};
    struct dma_service_protected protected[]={
        {(uintptr_t)&service,(uintptr_t)&service,sizeof(service)},
        {(uintptr_t)&native,(uintptr_t)&native,sizeof(native)},
        {(uintptr_t)&audio,(uintptr_t)&audio,sizeof(audio)},
        {(uintptr_t)&parent,(uintptr_t)&parent,sizeof(parent)},
        {(uintptr_t)&bindings,(uintptr_t)&bindings,sizeof(bindings)},
        {(uintptr_t)&pins,(uintptr_t)&pins,sizeof(pins)},
        {(uintptr_t)&clocks,(uintptr_t)&clocks,sizeof(clocks)},
        {(uintptr_t)&locks,(uintptr_t)&locks,sizeof(locks)},
        {(uintptr_t)&irq,(uintptr_t)&irq,sizeof(irq)}};
    creating_bootstrap=1;
    bootstrap_task=xTaskCreateStaticAffinitySet(bootstrap,"audio-bootstrap",4096,NULL,2,stacks[0],&tasks[0],1);
    creating_bootstrap=0;configASSERT(bootstrap_task);
    struct dma_service_activation activation={bootstrap_task,prepared,&parent};
    configASSERT(dma_service_init_deferred(&service,&ops,NULL,D,1,frequency/10,&job,1,protected,9,3,3,&activation));
    configASSERT(dma_service_i2s_irq_bind(&irq,&service,frequency) && !dma_service_i2s_irq_bind(&irq,&service,frequency));
    configASSERT(service.irq.fallback==DMA_SERVICE_I2S_IRQ_FALLBACK && service.irq.fallback_ticks==frequency/configTICK_RATE_HZ);
    /* Case 0 lengthens the private fallback so that a wake before it can
     * only come from the interrupt. */
    if(irq_case==0){service.irq.fallback=200;service.irq.fallback_ticks=frequency/5;}
    creating_device=1;
    TaskHandle_t model=xTaskCreateStaticAffinitySet(device_model,"dma-irq-model",4096,NULL,2,device_stack,&device_task,1);
    creating_device=0;
    TaskHandle_t client=xTaskCreateStaticAffinitySet(interrupt_client,"irq-client",4096,NULL,2,stacks[1],&tasks[1],2);
    configASSERT(model && client && dma_service_bind(&service,0,client,1));
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
