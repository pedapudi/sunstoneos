/* DMA0 interrupt wake path on the duplex cyclic I2S fixture. The service,
 * stream, controller, native cache and audio providers are the real ones;
 * DMA/I2S registers, the GIC SPI and its ISR are modeled. The ISR runs in a
 * privileged task, so physical interrupt delivery is not established. The
 * cyclic register model follows checks/i2s-cyclic.c without its fault hooks. */
#define kernel_main finite_kernel_main
#define clock_now finite_clock_now
#define dma_read finite_dma_read
#define dma_write finite_dma_write
#include "i2s-stream.c"
#undef kernel_main
#undef clock_now
#undef dma_read
#undef dma_write
static _Alignas(4096) unsigned char cyclic_pool[10*4096];
static unsigned char cyclic_device[2][4][4096],copied[4096];
static int clock_now(void *context,uint64_t *out)
{(void)context;*out=__atomic_add_fetch(&us,1,__ATOMIC_SEQ_CST);return 1;}
/* Complete amount RX/TX periods, leaving the cursor in the next period. */
static void cycle(unsigned ch,unsigned amount)
{
    const struct a733_dma_transfer *t=service.controller.channels[ch].transfer;
    configASSERT(t && t->count==4);
    for(unsigned n=0;n<amount;++n){
        uint32_t count=dma[di(ch,0x30)];unsigned slot=count%4;
        if(!ch)memset(cyclic_device[ch][slot],0x20+(count&63),4096);
        else for(unsigned i=0;i<4096;++i)configASSERT(cyclic_device[ch][slot][i]==0x60+slot);
        ++dma[di(ch,0x30)];ir[(ch?40:44)/4]+=1024;
    }
    unsigned current=dma[di(ch,0x30)]%4;
    dma[di(ch,8)]=(uint32_t)(t->descriptors.bus+((current+1)%4)*64);
    dma[di(ch,0x14)]=(uint32_t)(ch?A733_I2S0_TXFIFO:t->mappings[current].bus);
    dma[di(ch,0x18)]=4096;dma[di(ch,0x38)]|=2;
}
static uint32_t dma_reads;
static uint32_t dma_read(void *context,uintptr_t address)
{
    (void)context;privileged();configASSERT(address>=D && address<D+sizeof(dma));inc(&dma_reads);
    for(unsigned ch=0;ch<2;++ch)if(!dma[di(ch,0)] && get(&drain_enabled))dma[0x30/4]&=~(1u<<ch);
    return dma[(address-D)/4];
}
static void dma_write(void *context,uintptr_t address,uint32_t value)
{
    (void)context;privileged();unsigned offset=address-D;
    configASSERT(offset>=0x100 && offset<sizeof(dma));unsigned ch=(offset-0x100)/64,r=(offset-0x100)%64;
    if(r==0){
        configASSERT(!(ir[28/4]&(ch?128:8)));
        if(value){
            configASSERT(dma[di(ch,0x34)]==10);dma[di(ch,0x30)]=0;
            live|=1u<<ch;dma[0x30/4]|=1u<<ch;inc(&submissions);
        }else live&=~(1u<<ch);
    }
    if(r==0x38)dma[offset/4]&=~value;else dma[offset/4]=value;
}
static void cyclic_cache(void *context,enum a733_dma_cache op,void *cpu,size_t bytes)
{
    (void)context;privileged();a733_dma_native_operations.cache(&native,op,cpu,bytes);
    uintptr_t at=(uintptr_t)cpu,base=(uintptr_t)cyclic_pool;
    for(unsigned ch=0;ch<2;++ch){
        uintptr_t first=base+(2+ch*4)*4096;
        if(at<first || at>=first+4*4096)continue;
        unsigned slot=(unsigned)((at-first)/4096);configASSERT(bytes==4096);
        if(op==A733_DMA_CLEAN){configASSERT(ch && !(live&(1u<<ch)));memcpy(cyclic_device[ch][slot],cpu,bytes);}
        if(op==A733_DMA_INVALIDATE){configASSERT(!ch);memcpy(cpu,cyclic_device[ch][slot],bytes);}
    }
}
static struct a733_i2s_period cyclic_period(unsigned job)
{
    for(unsigned n=0;n<100;++n){struct dma_service_reply r=request(DMA_SERVICE_PERIOD_GET,job,0,0);
        if(r.result==DMA_SERVICE_OK)return r.period;
        configASSERT(r.result==DMA_SERVICE_BUSY);vTaskDelay(1);}
    configASSERT(0);return (struct a733_i2s_period){0};
}
static void start_pair(void)
{
    open_stream(0);open_stream(1);
    for(unsigned i=0;i<4;++i){struct a733_i2s_period p=cyclic_period(1);configASSERT(p.kind==A733_I2S_TX_FILL && p.slot==i);
        memset(cyclic_pool+(6+i)*4096,0x60+i,4096);configASSERT(request(DMA_SERVICE_PERIOD_PUT,1,0,p.ticket).result==DMA_SERVICE_OK);}
    for(unsigned j=0;j<2;++j)configASSERT(request(DMA_SERVICE_SUBMIT,j,frequency,0).result==DMA_SERVICE_OK);
    vTaskDelay(3);
}
static void finish_pair(unsigned failed_mask)
{
    for(unsigned j=0;j<2;++j){struct dma_service_reply r=request(DMA_SERVICE_STATUS,j,0,0);
        if(r.phase==DMA_SERVICE_RUNNING || r.phase==DMA_SERVICE_READY)configASSERT(request(DMA_SERVICE_CANCEL,j,0,0).result==DMA_SERVICE_OK);
        r=wait_phase(j,DMA_SERVICE_RESULT);configASSERT(r.holds_buffers && r.owns_session);
        configASSERT(r.outcome==((failed_mask&(1u<<j))?DMA_SERVICE_FAILED:DMA_SERVICE_CANCELED));
        configASSERT(request(DMA_SERVICE_REAP,j,0,0).result==DMA_SERVICE_OK);close_stream(j);}
    configASSERT(!bindings[0].pin_lease.id);
}
#define irq_case (*(volatile unsigned *)UINT64_C(0x47fff004))
enum { INJECT=1, DELIVER=2, MASK_FAILED=4 };
static uint32_t spi_enabled,rearms,deliveries,level_deliveries,line_at_rearm,command;
static unsigned creating_device;
static StaticTask_t device_task;
static StackType_t device_stack[4096];
static void device_model(void *);
BaseType_t dma_interrupt_task_privileged(TaskFunction_t,void *);
BaseType_t dma_interrupt_task_privileged(TaskFunction_t entry,void *context)
{return (creating_device && entry==device_model) || i2s_stream_task_privileged(entry,context);}
/* The shared DMA0 line: any channel with an enabled pending event. */
static int line(void)
{
    for(unsigned ch=0;ch<2;++ch)if(dma[di(ch,0x38)]&dma[di(ch,0x34)])return 1;
    return 0;
}
static void deliver(int mask_ok)
{
    BaseType_t woken=pdFALSE;
    set(&spi_enabled,0);inc(&deliveries);
    dma_service_irq_notify_from_isr(&service,mask_ok,&woken);
}
/* Worker only, inside its critical section. A level source still asserted
 * when the SPI is enabled is delivered at once, as the GIC would. */
static int rearm(void *cookie)
{
    configASSERT(cookie==&spi_enabled);privileged();
    inc(&rearms);
    if(line()){inc(&line_at_rearm);inc(&level_deliveries);deliver(1);return 1;}
    set(&spi_enabled,1);return 1;
}
static void device_model(void *unused)
{
    (void)unused;
    uint64_t el;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));configASSERT(el==4);
    for(;;){
        unsigned c=get(&command);
        if(!c){vTaskDelay(1);continue;}
        UBaseType_t saved=taskENTER_CRITICAL_FROM_ISR();
        if(c&INJECT){__atomic_fetch_add(&us,64000,__ATOMIC_SEQ_CST);cycle(0,1);cycle(1,1);}
        if((c&DELIVER) && (get(&spi_enabled) || (c&MASK_FAILED)))deliver(!(c&MASK_FAILED));
        taskEXIT_CRITICAL_FROM_ISR(saved);
        set(&command,0);taskYIELD();
    }
}
static void hardware(unsigned c)
{
    set(&command,c);
    for(unsigned n=0;n<100 && get(&command);++n)vTaskDelay(1);
    configASSERT(!get(&command));
}
static void wait_armed(void)
{
    for(unsigned n=0;n<100 && !get(&spi_enabled);++n)vTaskDelay(1);
    configASSERT(get(&spi_enabled));
}
/* The worker reads no DMA register while it waits for the interrupt. */
static void quiet(unsigned ticks)
{
    unsigned reads=get(&dma_reads);vTaskDelay(ticks);
    configASSERT(get(&dma_reads)==reads && get(&spi_enabled));
}
/* The worker publishes the RX copy without any client request. */
static TickType_t copied_by(uint64_t raw)
{
    TickType_t start=xTaskGetTickCount();
    while(__atomic_load_n(&service.streams[0].raw_next,__ATOMIC_ACQUIRE)<raw){
        configASSERT(xTaskGetTickCount()-start<100);vTaskDelay(1);
    }
    return xTaskGetTickCount()-start;
}
static void until(TickType_t start,TickType_t offset)
{
    TickType_t elapsed=xTaskGetTickCount()-start;
    configASSERT(elapsed<offset);vTaskDelay(offset-elapsed);
}
static void consume(unsigned n)
{
    struct a733_i2s_period p=cyclic_period(0),t=cyclic_period(1);
    configASSERT(p.kind==A733_I2S_CAPTURED_COPY && p.first_frame==(uint64_t)n*1024);
    configASSERT(dma_service_copy_period(&service,0,p.ticket,copied,4096));
    for(unsigned i=0;i<4096;++i)configASSERT(copied[i]==0x20+n);
    configASSERT(t.kind==A733_I2S_TX_REPEAT_PROGRESS && t.first_frame==(uint64_t)n*1024);
    configASSERT(request(DMA_SERVICE_PERIOD_PUT,1,0,t.ticket).result==DMA_SERVICE_INVALID);
    configASSERT(request(DMA_SERVICE_PERIOD_PUT,0,0,p.ticket).result==DMA_SERVICE_OK);
}
static void interrupt_client(void *unused)
{
    (void)unused;while(!get(&peer_stage))vTaskDelay(1);
    start_pair();
    if(irq_case==0){
        /* Each package end wakes the worker well before the 200-tick fallback,
         * and the rearm follows acknowledgement of every enabled event. */
        wait_armed();quiet(30);configASSERT(get(&rearms)==1);
        for(unsigned n=0;n<4;++n){
            hardware(INJECT|DELIVER);configASSERT(copied_by(n+1)<20);
            wait_armed();quiet(10);consume(n);
        }
        configASSERT(get(&deliveries)==4 && get(&rearms)==5 && !get(&line_at_rearm));
        finish_pair(0);
    }else if(irq_case==1){
        /* An interrupt without an acknowledged event returns to tick polling
         * until a polled event; only then is the SPI enabled again. */
        wait_armed();hardware(DELIVER);
        unsigned reads=get(&dma_reads);vTaskDelay(10);
        configASSERT(service.irq_spurious && get(&rearms)==1 && get(&dma_reads)>reads+5);
        hardware(INJECT);(void)copied_by(1);wait_armed();quiet(10);
        /* A rearm that raced the polled event delivers it at once. */
        unsigned level=get(&level_deliveries);
        configASSERT(get(&rearms)==2+level && get(&deliveries)==1+level && !service.irq_spurious);
        consume(0);finish_pair(0);
    }else if(irq_case==2){
        /* A failed ISR mask permanently returns the service to tick polling;
         * periods still complete and the SPI is never enabled again. */
        wait_armed();hardware(INJECT|DELIVER|MASK_FAILED);(void)copied_by(1);consume(0);
        for(unsigned n=1;n<3;++n){hardware(INJECT);(void)copied_by(n+1);consume(n);}
        unsigned reads=get(&dma_reads);vTaskDelay(10);
        configASSERT(service.irq_retained && get(&rearms)==1 && get(&dma_reads)>reads+5);
        finish_pair(0);
    }else{
        /* Without packages the worker wakes only at the fallback until the
         * period deadline is within twice the fallback (600 of 1000 ticks),
         * then polls every tick so expiry is not delayed by a fallback wait. */
        TickType_t start=xTaskGetTickCount();wait_armed();quiet(30);
        until(start,250);quiet(100);until(start,650);
        unsigned reads=get(&dma_reads);vTaskDelay(10);
        configASSERT(get(&dma_reads)>reads+5 && get(&rearms)==1 && !get(&deliveries));
        finish_pair(0);
    }
    platform_puts("PASS DMA interrupt: package-end wake without tick polling, acknowledge before rearm, spurious and failed-mask tick fallback, deadline-margin tick polling\n");
    platform_exit(0);
}
void kernel_main(void)
{
    tx_mode=2;platform_init();__asm__ volatile("mrs %0,cntfrq_el0":"=r"(frequency));
    ccu[0]=0xf8000000u|(99u<<8)|(99u<<16);ccu[0x280/4]=0xf8047f00;
    ccu[0x191c/4]=0x60006;ccu[0x199c/4]=0x60000;
    dma[0x34/4]=3;dma[di(0,8)]=0x41000000;dma[di(1,8)]=0x41000040;
    configASSERT(a733_clock_leases_init(&clocks,clock_now,NULL));configASSERT(a733_platform_locks_init(&locks,clock_now,NULL));
    const struct a733_audio_operations backend={.enter=enter,.leave=leave,.now_us=clock_now,.read=read_reg,.write=write_reg};
    configASSERT(a733_audio_leases_init(&audio,&clocks,7,&backend,NULL));
    configASSERT(a733_audio_parent_init(&parent,&audio.operations,&audio,0,2000000));
    struct a733_pins_operations pops={enter,leave,clock_now,read_reg,write_reg};configASSERT(a733_pins_init(&pins,&pops,NULL,A733_PINS_PB_MASK));
    struct a733_pin_request req[]={{5,{0,0,0},{3,0,0}},{6,{0,0,0},{3,0,0}},{8,{0,0,0},{3,0,0}},{7,{0,0,0},{3,0,0}}};
    configASSERT(a733_i2s_platform_duplex_init(&bindings[0],&parent,&pins,&audio,7,req,4));
    for(unsigned j=0;j<2;++j){profiles[j].duplex=1;profiles[j].stream=(struct a733_i2s_stream_profile){
        {24576000,16000,0,32,32,1,0},a733_i2s_platform_stream_operations(),&bindings[0],j?A733_I2S_TRANSMIT:A733_I2S_RECEIVE,500000,1};}
    struct dma_memory_window window={(uintptr_t)cyclic_pool,(uintptr_t)cyclic_pool,sizeof(cyclic_pool),7};
    struct dma_fifo_port ports[]={{A733_I2S0_RXFIFO,3,1u<<4,1u<<8,A733_DMA_READ},{A733_I2S0_TXFIFO,3,1u<<4,1u<<8,A733_DMA_WRITE}};
    configASSERT(a733_dma_native_init(&native,0x09000000,0x09000000,&window,1,ports,2,fatal,NULL));
    struct dma_service_job jobs[2];struct a733_dma_segment segments[2][4];struct a733_dma_mapping maps[2][4];
    for(unsigned j=0;j<2;++j){for(unsigned i=0;i<4;++i){uintptr_t at=(uintptr_t)cyclic_pool+(2+j*4+i)*4096;
        maps[j][i]=(struct a733_dma_mapping){(void *)at,at,4096,j?A733_DMA_READ:A733_DMA_WRITE};
        struct a733_dma_endpoint ram={at,0,4,8},fifo={j?A733_I2S0_TXFIFO:A733_I2S0_RXFIFO,3,4,8};
        segments[j][i]=(struct a733_dma_segment){j?ram:fifo,j?fifo:ram,4096};}
        jobs[j]=(struct dma_service_job){.kind=DMA_SERVICE_I2S_STREAM,.channel=j,.stream=&profiles[j],
            .transfer={{cyclic_pool+j*4096,(uintptr_t)cyclic_pool+j*4096,4096,A733_DMA_DESCRIPTORS},segments[j],4,maps[j],4}};}
    const struct a733_dma_operations ops={dma_read,dma_write,cyclic_cache,barrier,ticks,mapping,fifo};
    struct dma_service_protected protected[]={{(uintptr_t)&service,(uintptr_t)&service,sizeof(service)},
        {(uintptr_t)&native,(uintptr_t)&native,sizeof(native)},{(uintptr_t)&audio,(uintptr_t)&audio,sizeof(audio)},
        {(uintptr_t)&parent,(uintptr_t)&parent,sizeof(parent)},{(uintptr_t)&bindings,(uintptr_t)&bindings,sizeof(bindings)},
        {(uintptr_t)&pins,(uintptr_t)&pins,sizeof(pins)},{(uintptr_t)&clocks,(uintptr_t)&clocks,sizeof(clocks)},
        {(uintptr_t)&locks,(uintptr_t)&locks,sizeof(locks)}};
    creating_bootstrap=1;bootstrap_task=xTaskCreateStaticAffinitySet(bootstrap,"audio-bootstrap",4096,NULL,2,stacks[0],&tasks[0],1);creating_bootstrap=0;
    struct dma_service_activation activation={bootstrap_task,prepared,&parent};
    configASSERT(dma_service_init_deferred(&service,&ops,NULL,D,3,frequency/10,jobs,2,protected,8,3,3,&activation));
    /* 200 kernel ticks, so a wake that a tick poll or fallback caused is
     * distinguishable from an interrupt wake. */
    struct dma_service_irq irq={rearm,&spi_enabled,200,frequency/5};
    struct dma_service_irq bad=irq;bad.fallback=configTICK_RATE_HZ/4+1;
    configASSERT(!dma_service_bind_irq(&service,&bad));
    bad=irq;bad.rearm=NULL;configASSERT(!dma_service_bind_irq(&service,&bad));
    configASSERT(dma_service_bind_irq(&service,&irq) && !dma_service_bind_irq(&service,&irq));
    creating_device=1;
    TaskHandle_t model=xTaskCreateStaticAffinitySet(device_model,"dma-irq-model",4096,NULL,2,device_stack,&device_task,1);
    creating_device=0;
    TaskHandle_t client=xTaskCreateStaticAffinitySet(interrupt_client,"irq-client",4096,NULL,2,stacks[1],&tasks[1],2);
    configASSERT(model && client && dma_service_bind(&service,0,client,3));vTaskStartScheduler();platform_panic("scheduler returned",0);
}
