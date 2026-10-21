/* Real service/native-cache/audio leases/pins; only register/DMA effects modeled. */
#define kernel_main finite_kernel_main
#define clock_now finite_clock_now
#define dma_read finite_dma_read
#define dma_write finite_dma_write
#include "i2s-stream.c"
#undef kernel_main
#undef clock_now
#undef dma_read
#undef dma_write
#define cyclic_case (*(volatile unsigned *)UINT64_C(0x47fff000))
/* Pages: RX and TX descriptors, eight RX slots, four TX slots. */
static _Alignas(4096) unsigned char cyclic_pool[14*4096];
static unsigned char cyclic_device[2][8][4096],copied[4096];
static uint32_t amounts[2],range_invalidations,copy_inject,copied_inject,link_transition,final_packet;
static unsigned rx_periods=4;
static void cycle(unsigned,unsigned);
static unsigned start_delay;
static int clock_now(void *context,uint64_t *out)
{
 (void)context;if(get(&fail_clock))return 0;
 if(cyclic_case==11 && !start_delay && (ir[0]&2) && (live&1)){
  start_delay=1;__atomic_fetch_add(&us,64000,__ATOMIC_SEQ_CST);cycle(0,1);
 }
 *out=__atomic_add_fetch(&us,1,__ATOMIC_SEQ_CST);return 1;
}
/* Destination writes for package m relative to its count, within one
 * package minus one 32-byte burst. Case 13 completes all but the first burst
 * only while the next package is written. Cases 14 and 15 have already
 * written all but the last burst of package m+2 when package m is counted. */
static unsigned tail_pending,tail_slot;
static void rx_write(unsigned slot,uint32_t package,size_t from,size_t to)
{memset(cyclic_device[0][slot]+from,0x20+(package&63),to-from);}
static void cycle(unsigned ch,unsigned amount)
{
    const struct a733_dma_transfer *t=service.controller.channels[ch].transfer;
    unsigned periods=ch?4:rx_periods;
    configASSERT(t && t->count==periods);
    for(unsigned n=0;n<amount;++n){
        uint32_t count=dma[di(ch,0x30)];unsigned slot=count%periods;
        if(ch)for(unsigned i=0;i<4096;++i)configASSERT(cyclic_device[ch][slot][i]==0x60+slot);
        else if(cyclic_case==13){
            if(tail_pending)rx_write(tail_slot,count-1,32,4096);
            rx_write(slot,count,0,32);tail_pending=1;tail_slot=slot;
        }else if(cyclic_case==14 || cyclic_case==15){
            if(!count)rx_write(0,0,0,4096);
            rx_write((count+1)%periods,count+1,0,4096);rx_write((count+2)%periods,count+2,0,4096-32);
        }else rx_write(slot,count,0,4096);
        ++dma[di(ch,0x30)];ir[(ch?40:44)/4]+=1024;
    }
    unsigned current=dma[di(ch,0x30)]%periods;
    dma[di(ch,8)]=(uint32_t)(t->descriptors.bus+((current+1)%periods)*64);
    dma[di(ch,0x14)]=(uint32_t)(ch?A733_I2S0_TXFIFO:t->mappings[current].bus);
    dma[di(ch,0x18)]=4096;dma[di(ch,0x38)]|=2;
}
static void dma_advance(void)
{
    for(unsigned ch=0;ch<2;++ch)if((live&(1u<<ch)) && (get(&requested)&(1u<<ch))){
        set(&requested,get(&requested)&~(1u<<ch));
        configASSERT(ir[28/4]&(ch?128:8));cycle(ch,amounts[ch]);
    }
    for(unsigned ch=0;ch<2;++ch)if(!dma[di(ch,0)] && get(&drain_enabled))dma[0x30/4]&=~(1u<<ch);
}
static uint32_t dma_read(void *context,uintptr_t address)
{
    (void)context;privileged();configASSERT(address>=D && address<D+sizeof(dma));
    /* Reuse after the copy, before its package-count postcheck. */
    if(copied_inject && address==D+0x130){copied_inject=0;__atomic_fetch_add(&us,192000,__ATOMIC_SEQ_CST);cycle(0,3);}
    dma_advance();uint32_t value=dma[(address-D)/4];
    if(link_transition && address==D+0x108){--link_transition;return value^64;}
    return value;
}
static void dma_write(void *context,uintptr_t address,uint32_t value)
{
    (void)context;privileged();unsigned offset=address-D;
    configASSERT(offset>=0x100 && offset<sizeof(dma));unsigned ch=(offset-0x100)/64,r=(offset-0x100)%64;
    if(r==0){
        configASSERT(!(ir[28/4]&(ch?128:8)));
        if(value){
            configASSERT(dma[di(ch,0x34)]==10);dma[di(ch,0x30)]=cyclic_case==10&&!ch?1:0;
            live|=1u<<ch;dma[0x30/4]|=1u<<ch;inc(&submissions);
        }else{
            if(final_packet && ch){final_packet=0;cycle(ch,1);}
            live&=~(1u<<ch);
        }
    }
    if(r==0x38)dma[offset/4]&=~value;else dma[offset/4]=value;
}
static void cyclic_cache(void *context,enum a733_dma_cache op,void *cpu,size_t bytes)
{
    (void)context;privileged();a733_dma_native_operations.cache(&native,op,cpu,bytes);
    uintptr_t at=(uintptr_t)cpu,base=(uintptr_t)cyclic_pool;
    for(unsigned ch=0;ch<2;++ch){
        uintptr_t first=base+(2+ch*8)*4096;
        if(at<first || at>=first+(ch?4:8)*4096)continue;
        unsigned slot=(unsigned)((at-first)/4096);configASSERT(bytes==4096);
        if(op==A733_DMA_CLEAN){configASSERT(ch && !(live&(1u<<ch)));memcpy(cyclic_device[ch][slot],cpu,bytes);}
        if(op==A733_DMA_INVALIDATE){
            configASSERT(!ch);
            if(live&1){
                ++range_invalidations;
                if(copy_inject==1){copy_inject=0;__atomic_fetch_add(&us,192000,__ATOMIC_SEQ_CST);cycle(0,3);}
                if(copy_inject==2){copy_inject=0;copied_inject=1;}
            }
            memcpy(cpu,cyclic_device[ch][slot],bytes);
        }
    }
}
static struct a733_i2s_period cyclic_period(unsigned job)
{
    for(unsigned n=0;n<100;++n){struct dma_service_reply r=request(DMA_SERVICE_PERIOD_GET,job,0,0);
        if(r.result==DMA_SERVICE_OK)return r.period;
        if(r.result!=DMA_SERVICE_BUSY){platform_puts("cyclic period fault job=");platform_hex(job);
            for(unsigned j=0;j<2;++j){struct a733_i2s_stream *q=&service.streams[j];
                platform_puts(" status/stage/frames/time/since/dma=");platform_hex(q->status);platform_hex(q->stage);platform_hex(q->frames);platform_hex(q->time);platform_hex(q->cyclic_since);platform_hex(service.controller.channels[j].status);}
            platform_puts("\n");configASSERT(0);}
        vTaskDelay(1);}
    configASSERT(0);return (struct a733_i2s_period){0};
}
static void inject(unsigned mask,unsigned rx,unsigned tx)
{
    __atomic_fetch_add(&us,(uint64_t)(rx>tx?rx:tx)*64000,__ATOMIC_SEQ_CST);amounts[0]=rx;amounts[1]=tx;set(&requested,mask);vTaskDelay(3);
}
static void start_pair(void)
{
    open_stream(0);open_stream(1);
    for(unsigned i=0;i<4;++i){struct a733_i2s_period p=cyclic_period(1);configASSERT(p.kind==A733_I2S_TX_FILL && p.slot==i);
        memset(cyclic_pool+(10+i)*4096,0x60+i,4096);configASSERT(request(DMA_SERVICE_PERIOD_PUT,1,0,p.ticket).result==DMA_SERVICE_OK);}
    for(unsigned j=0;j<(cyclic_case==10?1u:2u);++j)configASSERT(request(DMA_SERVICE_SUBMIT,j,frequency,0).result==DMA_SERVICE_OK);
    vTaskDelay(3);
}
static void finish_pair(unsigned failed_mask)
{
    for(unsigned j=0;j<2;++j){struct dma_service_reply r=request(DMA_SERVICE_STATUS,j,0,0);
        if(r.phase==DMA_SERVICE_RUNNING || r.phase==DMA_SERVICE_READY)configASSERT(request(DMA_SERVICE_CANCEL,j,0,0).result==DMA_SERVICE_OK);
        r=wait_phase(j,DMA_SERVICE_RESULT);configASSERT(r.holds_buffers && r.owns_session);
        configASSERT(r.outcome==((failed_mask&(1u<<j))?DMA_SERVICE_FAILED:DMA_SERVICE_CANCELED));
        configASSERT(request(DMA_SERVICE_REAP,j,0,0).result==DMA_SERVICE_OK);close_stream(j);
        if(!j)configASSERT(bindings[0].pin_lease.id);}
    configASSERT(!bindings[0].pin_lease.id);
}
static void cyclic_client(void *unused)
{
    (void)unused;while(!get(&peer_stage))vTaskDelay(1);
    if(cyclic_case==8){
        open_stream(0);open_stream(1);struct a733_i2s_period p=cyclic_period(1);
        configASSERT(p.kind==A733_I2S_TX_FILL);configASSERT(request(DMA_SERVICE_CANCEL,1,0,0).result==DMA_SERVICE_OK);
        struct dma_service_reply r=wait_phase(1,DMA_SERVICE_RESULT);configASSERT(r.outcome==DMA_SERVICE_CANCELED && !r.stream_frames);
        configASSERT(request(DMA_SERVICE_REAP,1,0,0).result==DMA_SERVICE_BUSY);
        configASSERT(request(DMA_SERVICE_PERIOD_PUT,1,0,p.ticket).result==DMA_SERVICE_OK);
        configASSERT(request(DMA_SERVICE_REAP,1,0,0).result==DMA_SERVICE_OK);close_stream(1);
        configASSERT(request(DMA_SERVICE_CANCEL,0,0,0).result==DMA_SERVICE_OK);wait_phase(0,DMA_SERVICE_RESULT);
        configASSERT(request(DMA_SERVICE_REAP,0,0,0).result==DMA_SERVICE_OK);close_stream(0);
    }else{
        start_pair();
        if(cyclic_case==10){
            struct dma_service_reply r=request(DMA_SERVICE_STATUS,0,0,0);
            configASSERT(r.phase==DMA_SERVICE_RECOVERY && r.holds_buffers && !r.stream_frames);
            configASSERT(service.controller.channels[0].packages==0 && bindings[0].pin_lease.id);
            configASSERT(request(DMA_SERVICE_REAP,0,0,0).result==DMA_SERVICE_FAULT);
            platform_puts("PASS cyclic I2S rejected counter origin retained without progress\n");platform_exit(0);
        }
        configASSERT(get(&submissions)==2);
        for(unsigned j=0;j<2;++j){uint32_t next;memcpy(&next,cyclic_pool+j*4096+((j?4:rx_periods)-1)*64+20,4);configASSERT(next==(uintptr_t)cyclic_pool+j*4096);}
        if(cyclic_case==11){
            inject(1,1,0);struct a733_i2s_period p=cyclic_period(0);configASSERT(start_delay && p.frames==1024);
            configASSERT(request(DMA_SERVICE_PERIOD_PUT,0,0,p.ticket).result==DMA_SERVICE_OK);finish_pair(0);
        }else if(cyclic_case==12){
            __atomic_fetch_add(&us,1000000,__ATOMIC_SEQ_CST);vTaskDelay(3);
            struct dma_service_reply r=request(DMA_SERVICE_STATUS,0,0,0);
            configASSERT(r.phase==DMA_SERVICE_RECOVERY && r.holds_buffers && bindings[0].pin_lease.id);
        }else if(cyclic_case==3){inject(1,4,0);finish_pair(1);}
        else if(cyclic_case==4 || cyclic_case==5){copy_inject=cyclic_case==4?1:2;inject(1,2,0);
            struct dma_service_reply r=wait_phase(0,DMA_SERVICE_RESULT);configASSERT(r.outcome==DMA_SERVICE_FAILED);
            configASSERT(request(DMA_SERVICE_PERIOD_GET,0,0,0).result==DMA_SERVICE_BUSY);finish_pair(1);}
        else if(cyclic_case==7){inject(1,2,0);struct a733_i2s_period p=cyclic_period(0);
            configASSERT(p.kind==A733_I2S_CAPTURED_COPY);inject(1,4,0);wait_phase(0,DMA_SERVICE_RESULT);
            configASSERT(dma_service_copy_period(&service,0,p.ticket,copied,4096) && copied[0]==0x20);
            configASSERT(request(DMA_SERVICE_PERIOD_PUT,0,0,p.ticket).result==DMA_SERVICE_OK);finish_pair(1);}
        else if(cyclic_case==9){
            inject(2,0,11);struct a733_i2s_period t=cyclic_period(1);
            configASSERT(t.kind==A733_I2S_TX_REPEAT_PROGRESS && t.frames==11*1024 && !t.first_frame);
            configASSERT(request(DMA_SERVICE_PERIOD_PUT,1,0,t.ticket).result==DMA_SERVICE_INVALID);
            configASSERT(get(&submissions)==2);finish_pair(0);
        }else if(cyclic_case==15){
            /* Counted period 2 is beyond the copy window of period 0, whose
             * slot the writer has already reused: stop without copying. */
            inject(1,3,0);struct dma_service_reply r=wait_phase(0,DMA_SERVICE_RESULT);
            configASSERT(r.outcome==DMA_SERVICE_FAILED && !range_invalidations);
            configASSERT(request(DMA_SERVICE_PERIOD_GET,0,0,0).result==DMA_SERVICE_BUSY);finish_pair(1);
        }else {
            if(cyclic_case==6)link_transition=2;
            unsigned periods=3*rx_periods;
            /* Period 0 is not copied while period 1 is still in progress. */
            inject(1,1,0);
            configASSERT(request(DMA_SERVICE_PERIOD_GET,0,0,0).result==DMA_SERVICE_BUSY);
            configASSERT(service.controller.channels[0].packages==1 && !range_invalidations);
            for(unsigned n=0;n<periods;++n){inject(3,1,1);struct a733_i2s_period p=cyclic_period(0),t=cyclic_period(1);
                configASSERT(p.kind==A733_I2S_CAPTURED_COPY && p.first_frame==(uint64_t)n*1024);
                configASSERT(dma_service_copy_period(&service,0,p.ticket,copied,4096));
                for(unsigned i=0;i<4096;++i)configASSERT(copied[i]==0x20+n);
                configASSERT(!dma_service_copy_period(&service,0,p.ticket,cyclic_pool+8192,4096));
                configASSERT(t.kind==A733_I2S_TX_REPEAT_PROGRESS && t.first_frame==(uint64_t)n*1024 && t.frames==1024);
                configASSERT(request(DMA_SERVICE_PERIOD_PUT,1,0,t.ticket).result==DMA_SERVICE_INVALID);
                configASSERT(request(DMA_SERVICE_PERIOD_PUT,0,0,p.ticket).result==DMA_SERVICE_OK);
            }
            configASSERT(get(&submissions)==2 && range_invalidations==periods);
            if(cyclic_case==16){
                /* Eight periods tolerate four periods of consumer lag. */
                inject(1,4,0);
                for(unsigned n=periods;n<periods+4;++n){struct a733_i2s_period p=cyclic_period(0);
                    configASSERT(p.kind==A733_I2S_CAPTURED_COPY && p.first_frame==(uint64_t)n*1024);
                    configASSERT(dma_service_copy_period(&service,0,p.ticket,copied,4096));
                    for(unsigned i=0;i<4096;++i)configASSERT(copied[i]==0x20+n);
                    configASSERT(request(DMA_SERVICE_PERIOD_PUT,0,0,p.ticket).result==DMA_SERVICE_OK);}
                configASSERT(range_invalidations==periods+4);
            }
            final_packet=1;__atomic_fetch_add(&us,64000,__ATOMIC_SEQ_CST);finish_pair(0);
            configASSERT(service.controller.channels[1].packages==periods+1);
        }
    }
    platform_puts("PASS cyclic I2S actual providers, fixed rings, checked readonly RX staging, immutable TX progress, wrap/backpressure and retained drain\n");platform_exit(0);
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
        {24576000,16000,0,32,32,1,0},a733_i2s_platform_stream_operations(),&bindings[0],j?A733_I2S_TRANSMIT:A733_I2S_RECEIVE,cyclic_case==9?1000000:500000,1};}
    struct dma_memory_window window={(uintptr_t)cyclic_pool,(uintptr_t)cyclic_pool,sizeof(cyclic_pool),7};
    struct dma_fifo_port ports[]={{A733_I2S0_RXFIFO,3,1u<<4,1u<<8,A733_DMA_READ},{A733_I2S0_TXFIFO,3,1u<<4,1u<<8,A733_DMA_WRITE}};
    configASSERT(a733_dma_native_init(&native,0x09000000,0x09000000,&window,1,ports,2,fatal,NULL));
    rx_periods=cyclic_case==16?8:4;
    struct dma_service_job jobs[2];struct a733_dma_segment segments[2][8];struct a733_dma_mapping maps[2][8];
    for(unsigned j=0;j<2;++j){unsigned periods=j?4:rx_periods;
        for(unsigned i=0;i<periods;++i){uintptr_t at=(uintptr_t)cyclic_pool+(2+j*8+i)*4096;
        maps[j][i]=(struct a733_dma_mapping){(void *)at,at,4096,j?A733_DMA_READ:A733_DMA_WRITE};
        struct a733_dma_endpoint ram={at,0,4,8},fifo={j?A733_I2S0_TXFIFO:A733_I2S0_RXFIFO,3,4,8};
        segments[j][i]=(struct a733_dma_segment){j?ram:fifo,j?fifo:ram,4096};}
        jobs[j]=(struct dma_service_job){.kind=DMA_SERVICE_I2S_STREAM,.channel=j,.stream=&profiles[j],
            .transfer={{cyclic_pool+j*4096,(uintptr_t)cyclic_pool+j*4096,4096,A733_DMA_DESCRIPTORS},segments[j],periods,maps[j],periods}};}
    const struct a733_dma_operations ops={dma_read,dma_write,cyclic_cache,barrier,ticks,mapping,fifo};
    struct dma_service_protected protected[]={{(uintptr_t)&service,(uintptr_t)&service,sizeof(service)},
        {(uintptr_t)&native,(uintptr_t)&native,sizeof(native)},{(uintptr_t)&audio,(uintptr_t)&audio,sizeof(audio)},
        {(uintptr_t)&parent,(uintptr_t)&parent,sizeof(parent)},{(uintptr_t)&bindings,(uintptr_t)&bindings,sizeof(bindings)},
        {(uintptr_t)&pins,(uintptr_t)&pins,sizeof(pins)},{(uintptr_t)&clocks,(uintptr_t)&clocks,sizeof(clocks)},
        {(uintptr_t)&locks,(uintptr_t)&locks,sizeof(locks)}};
    creating_bootstrap=1;bootstrap_task=xTaskCreateStaticAffinitySet(bootstrap,"audio-bootstrap",4096,NULL,2,stacks[0],&tasks[0],1);creating_bootstrap=0;
    struct dma_service_activation activation={bootstrap_task,prepared,&parent};
    configASSERT(dma_service_init_deferred(&service,&ops,NULL,D,3,frequency/10,jobs,2,protected,8,3,3,&activation));
    TaskHandle_t client=xTaskCreateStaticAffinitySet(cyclic_client,"cyclic-client",4096,NULL,2,stacks[1],&tasks[1],2);
    configASSERT(client && dma_service_bind(&service,0,client,3));vTaskStartScheduler();platform_panic("scheduler returned",0);
}
