/* One two-core composition: real service/controller/native memory/cache and
 * clock/pin/platform-lock ownership; register/FIFO progress is modeled. */
#include "dma_service.h"
#include "a733_dma_native.h"
#include "a733_audio_leases.h"
#include "a733_platform_locks.h"
#include "platform.h"
#include <string.h>
#ifndef I2S_CHECK_DUPLEX
#define I2S_CHECK_DUPLEX 0
#endif
#define C UINT32_C(0x02002000)
#define P UINT32_C(0x02000000)
#define D UINT32_C(0x04601000)
static struct dma_service service;
static struct a733_dma_native native;
static struct a733_platform_locks locks;
static struct a733_clock_leases clocks;
static struct a733_audio_leases audio;
static struct a733_audio_parent parent;
static struct a733_i2s_platform bindings[2];
static struct a733_pins pins;
static struct dma_service_i2s_stream_profile profiles[2];
static _Alignas(4096) unsigned char pool[8*4096];
static unsigned char device[6][256];
static uint32_t ccu[0x2000/4],pio[0x200/4],ir[0x80/4],dma[0x300/4];
static uint32_t requested,drain_enabled=1,invalidations,submissions,io_count,peer_stage;
static uint32_t late_drain,fail_clock,pause_open,pause_close,paused,gen_writes;
static uint64_t us=1,frequency;
static unsigned creating_bootstrap,tx_mode,live;
static TaskHandle_t bootstrap_task;
static StaticTask_t tasks[2];
static StackType_t stacks[2][4096];
static unsigned get(const uint32_t *p){return __atomic_load_n(p,__ATOMIC_ACQUIRE);}
static void set(uint32_t *p,uint32_t v){__atomic_store_n(p,v,__ATOMIC_RELEASE);}
static void inc(uint32_t *p){__atomic_fetch_add(p,1,__ATOMIC_RELEASE);}
BaseType_t i2s_stream_task_privileged(TaskFunction_t entry,void *context);
static void bootstrap(void *);
BaseType_t i2s_stream_task_privileged(TaskFunction_t entry,void *context)
{return (creating_bootstrap && entry==bootstrap) || dma_service_task_privileged(entry,context);}
static void privileged(void)
{
    uint64_t el;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));configASSERT(el==4);
    TaskHandle_t task=xTaskGetCurrentTaskHandle();
    configASSERT(task==service.worker || task==bootstrap_task);
}
static int clock_now(void *context,uint64_t *out)
{
    (void)context;
    if(get(&fail_clock))return 0;
    *out=++us;return 1;
}
static int enter(void *context,uint64_t deadline)
{(void)context;privileged();return a733_platform_lock_enter(&locks,A733_PLATFORM_REGISTER_LOCK,deadline);}
static void leave(void *context)
{
    (void)context;privileged();a733_platform_lock_leave(&locks,A733_PLATFORM_REGISTER_LOCK);
    if(get(&pause_open) && audio.tokens[A733_AUDIO_LOCAL-1].id &&
        !bindings[0].pin_lease.id && !ccu[0x120c/4]) {
        atomic_store(&clocks.busy,1);set(&pause_open,0);set(&paused,1);
    }
    if(get(&pause_close) && bindings[0].pin_lease.id && !ccu[0x120c/4] &&
        !(ccu[0x1200/4]&(1u<<31))) {
        atomic_store(&clocks.busy,1);set(&pause_close,0);set(&paused,2);
    }
}
static void locked(void)
{
    privileged();configASSERT(xSemaphoreGetMutexHolder(locks.mutex[0])==xTaskGetCurrentTaskHandle());
}
static uint32_t *word(uintptr_t a)
{
    if(a>=C && a<C+sizeof(ccu))return &ccu[(a-C)/4];
    if(a>=P && a<P+sizeof(pio))return &pio[(a-P)/4];
    if(a>=A733_I2S0_BASE && a<A733_I2S0_BASE+sizeof(ir))return &ir[(a-A733_I2S0_BASE)/4];
    configASSERT(a==0x07090160);static uint32_t osc=0x10000002;return &osc;
}
static int read_reg(void *context,uintptr_t a,uint32_t *out)
{
    (void)context;locked();inc(&io_count);
    if(a==A733_I2S0_BASE+24)*out=tx_mode==2?((64u<<16)|8):tx_mode?(64u<<16):8;
    else *out=*word(a);
    return 1;
}
static int write_reg(void *context,uintptr_t a,uint32_t v)
{
    (void)context;locked();inc(&io_count);
    if(a==A733_I2S0_BASE+12){ir[3]&=~v;return 1;}
    if(a==A733_I2S0_BASE && (v&1))inc(&gen_writes);
    if(a==A733_I2S0_BASE+20)v&=~(3u<<24);
    if(a==A733_I2S0_BASE+28 && v){
        configASSERT(ir[0]&1);
        if(tx_mode==2){
            if(v&8)configASSERT(dma[0x100/4]);
            if(v&128)configASSERT(dma[0x140/4]);
        }else configASSERT(dma[0x100/4]);
    }
    if(a==C+0x120c && (v&0x10001)!=0x10001) {
        configASSERT(!live && !dma[0x30/4]);memset(ir,0,sizeof(ir));
    }
    *word(a)=v;return 1;
}
static unsigned di(unsigned channel,unsigned offset){return (0x100+channel*0x40+offset)/4;}
static void advance(void)
{
    for(unsigned ch=0;ch<(tx_mode==2?2u:1u);++ch)if((live&(1u<<ch)) && (get(&requested)&(1u<<ch))) {
        set(&requested,get(&requested)&~(1u<<ch));
        unsigned transmit=tx_mode==2?ch:tx_mode;
        const struct a733_dma_transfer *t=service.controller.channels[ch].transfer;
        configASSERT(t && t->count==1 && (ir[28/4]&(transmit?128u:8u)));
        configASSERT(ir[0]&(transmit?4u:2u));
        uintptr_t base=(uintptr_t)pool+(tx_mode==2&&ch?8192:4096);
        unsigned slot=(unsigned)(((uintptr_t)t->mappings[0].cpu-base)/256);
        configASSERT(slot<3);unsigned index=slot+(tx_mode==2&&ch?3:0);
        if(!transmit) {
            memset(device[index],0x80+get(&submissions),256);
            ir[44/4]+=t->segments[0].bytes/4;
        } else {
            for(unsigned k=0;k<t->segments[0].bytes;++k)configASSERT(device[index][k]==0x30+slot);
            ir[40/4]+=t->segments[0].bytes/4;
        }
        dma[di(ch,0x38)]|=4;live&=~(1u<<ch);
    }
    for(unsigned ch=0;ch<2;++ch)
        if(!dma[di(ch,0)] && get(&drain_enabled))dma[0x30/4]&=~(1u<<ch);

}
static uint32_t dma_read(void *context,uintptr_t a)
{
    (void)context;privileged();inc(&io_count);configASSERT(a>=D && a<D+sizeof(dma));
    advance();return dma[(a-D)/4];
}
static void dma_write(void *context,uintptr_t a,uint32_t v)
{
    (void)context;privileged();inc(&io_count);
    unsigned offset=a-D;configASSERT(offset>=0x100 && offset<sizeof(dma));
    unsigned channel=(offset-0x100)/0x40,r=(offset-0x100)%0x40;
    if((channel==0 || tx_mode==2) && r==0) {
        unsigned bit=(tx_mode==2?channel:tx_mode)?128:8;
        configASSERT(!(ir[28/4]&bit)); /* Own DRQ off before submit and stop. */
        if(v){live|=1u<<channel;dma[0x30/4]|=1u<<channel;inc(&submissions);}
        else live&=~(1u<<channel);
    }
    if(r==0x38)dma[offset/4]&=~v;else dma[offset/4]=v;
}
static void cache(void *context,enum a733_dma_cache op,void *cpu,size_t bytes)
{
    (void)context;privileged();
    a733_dma_native_operations.cache(&native,op,cpu,bytes);
    unsigned ch=tx_mode==2 && (uintptr_t)cpu>=(uintptr_t)pool+8192;
    uintptr_t base=(uintptr_t)pool+(ch?8192:4096);
    if((uintptr_t)cpu>=base && (uintptr_t)cpu<base+768) {
        unsigned slot=(unsigned)(((uintptr_t)cpu-base)/256)+ch*3;
        configASSERT(bytes==256);
        if(op==A733_DMA_CLEAN)memcpy(device[slot],cpu,bytes);
        if(op==A733_DMA_INVALIDATE) {
            configASSERT(!dma[di(ch,0)] && !(dma[0x30/4]&(1u<<ch)) && !(ir[28/4]&(ch?128:8)));
            memcpy(cpu,device[slot],bytes);inc(&invalidations);
            if(get(&late_drain)){set(&late_drain,0);us+=20000;}
        }
    }

}
static void barrier(void *context){(void)context;privileged();a733_dma_native_operations.barrier(&native);}
static uint64_t ticks(void *context){(void)context;privileged();return a733_dma_native_operations.ticks(&native);}
static int mapping(void *context,const struct a733_dma_mapping *m)
{(void)context;privileged();return a733_dma_native_operations.validate_mapping(&native,m);}
static int fifo(void *context,const struct a733_dma_endpoint *e,enum a733_dma_access access)
{(void)context;privileged();return a733_dma_native_operations.validate_fifo(&native,e,access);}
static void fatal(void *context){(void)context;platform_panic("native DMA policy",0);}
static int prepared(void *context,uintptr_t base,uint16_t channels)
{configASSERT(context==&parent);return base==D && channels==3 && a733_audio_parent_ready(&parent);}
static struct dma_service_reply request(enum dma_service_operation op,unsigned job,uint64_t timeout,uint64_t period)
{
    struct dma_service_request r={op,job,timeout,period};uint64_t ticket;
    struct dma_service_reply reply;
    configASSERT(dma_service_send(&service,&r,100,&ticket));
    r.job=999;r.period_ticket=UINT64_MAX; /* Request queue owns a copy. */
    configASSERT(dma_service_collect(&service,ticket,&reply,100));return reply;
}
static struct dma_service_reply wait_phase(unsigned job,enum dma_service_phase phase)
{
    struct dma_service_reply r={0};
    for(unsigned n=0;n<100;++n) {
        r=request(DMA_SERVICE_STATUS,job,0,0);
        if(r.phase==phase)return r;
        configASSERT(r.phase!=DMA_SERVICE_RECOVERY);vTaskDelay(1);
    }
    configASSERT(0);return r;
}
static struct a733_i2s_period period(unsigned job)
{
    for(unsigned n=0;n<100;++n) {
        struct dma_service_reply r=request(DMA_SERVICE_PERIOD_GET,job,0,0);
        if(r.result==DMA_SERVICE_OK)return r.period;
        configASSERT(r.result==DMA_SERVICE_BUSY);vTaskDelay(1);
    }
    configASSERT(0);return (struct a733_i2s_period){0};
}
static void complete_period(void)
{
    unsigned before=get(&submissions);set(&requested,1);
    for(unsigned n=0;n<100 && get(&submissions)==before;++n)vTaskDelay(1);
    configASSERT(get(&submissions)==before+1);
}
static void open_stream(unsigned job)
{
    unsigned pausing=get(&pause_open);
    configASSERT(request(DMA_SERVICE_OPEN,job,100000,0).result==DMA_SERVICE_OK);
    if(pausing) {
        for(unsigned n=0;n<100 && get(&paused)!=1;++n)vTaskDelay(1);
        configASSERT(get(&paused)==1);unsigned io=get(&io_count);
        struct dma_service_reply r=request(DMA_SERVICE_STATUS,job,0,0);
        configASSERT(r.phase==DMA_SERVICE_OPENING && r.holds_buffers && get(&io_count)==io);
        atomic_store(&clocks.busy,0);set(&paused,0);
    }
    (void)wait_phase(job,DMA_SERVICE_READY);
}
static void close_stream(unsigned job)
{
    unsigned pausing=get(&pause_close);
    configASSERT(request(DMA_SERVICE_CLOSE,job,100000,0).result==DMA_SERVICE_OK);
    if(pausing) {
        configASSERT(get(&paused)==2);unsigned io=get(&io_count);
        struct dma_service_reply r=request(DMA_SERVICE_STATUS,job,0,0);
        configASSERT(r.phase==DMA_SERVICE_CLOSING && r.holds_buffers && get(&io_count)==io);
        atomic_store(&clocks.busy,0);set(&paused,0);
    }
    (void)wait_phase(job,DMA_SERVICE_CLOSED);
}
static void duplex_case(void)
{
    open_stream(0);
    configASSERT(request(DMA_SERVICE_SUBMIT,0,frequency,0).result==DMA_SERVICE_BUSY);
    open_stream(1);
    configASSERT(bindings[0].pin_lease.pb_mask==((1u<<5)|(1u<<6)|(1u<<7)|(1u<<8)));
    for(unsigned i=0;i<3;++i){
        struct a733_i2s_period p=period(1);configASSERT(p.kind==A733_I2S_TX_FILL && p.slot==i);
        memset(pool+8192+i*256,0x30+i,256);
        configASSERT(request(DMA_SERVICE_PERIOD_PUT,1,0,p.ticket).result==DMA_SERVICE_OK);
    }
    for(unsigned j=0;j<2;++j)configASSERT(request(DMA_SERVICE_SUBMIT,j,frequency,0).result==DMA_SERVICE_OK);
    vTaskDelay(3);
    for(unsigned n=0;n<5;++n){
        unsigned before=get(&submissions);set(&requested,3);
        for(unsigned k=0;k<100 && get(&submissions)<before+2;++k)vTaskDelay(1);
        configASSERT(get(&submissions)==before+2);
        for(unsigned j=0;j<2;++j){
            struct a733_i2s_period p=period(j);
            configASSERT(p.kind==(j?A733_I2S_TX_MEMORY_CONSUMED:A733_I2S_CAPTURED) &&
                p.first_frame==n*16 && p.counter==(n+1)*16 && p.slot==n%3);
            configASSERT(request(DMA_SERVICE_PERIOD_PUT,j,0,p.ticket).result==DMA_SERVICE_OK);
        }
    }
    /* Drain and close RX while TX remains active; shared pin/clock ownership
     * and TX requests must survive the entire RX close. */
    set(&drain_enabled,0);
    configASSERT(request(DMA_SERVICE_CANCEL,0,0,0).result==DMA_SERVICE_OK);
    configASSERT((ir[0]&7)==5 && ir[28/4]==128);
    configASSERT(request(DMA_SERVICE_REAP,0,0,0).result==DMA_SERVICE_BUSY);
    set(&drain_enabled,1);(void)wait_phase(0,DMA_SERVICE_RESULT);
    configASSERT(request(DMA_SERVICE_REAP,0,0,0).result==DMA_SERVICE_OK);close_stream(0);
    configASSERT(bindings[0].pin_lease.id && ccu[0x120c/4] && (ir[0]&7)==5 && ir[28/4]==128);
    configASSERT(request(DMA_SERVICE_OPEN,0,100000,0).result==DMA_SERVICE_BUSY);
    configASSERT(request(DMA_SERVICE_CANCEL,1,0,0).result==DMA_SERVICE_OK);
    (void)wait_phase(1,DMA_SERVICE_RESULT);
    configASSERT(request(DMA_SERVICE_REAP,1,0,0).result==DMA_SERVICE_OK);close_stream(1);
    configASSERT(!bindings[0].pin_lease.id && !ccu[0x120c/4]);
    /* Reverse close order: TX closes while RX holds the binding. */
    open_stream(1);open_stream(0);close_stream(1);
    configASSERT(bindings[0].pin_lease.id);close_stream(0);
    configASSERT(!bindings[0].pin_lease.id);
    /* Fault one direction while its peer is READY: neither may issue another
     * device access, and both STATUS replies must expose retained recovery. */
    open_stream(0);open_stream(1);
    if(I2S_CHECK_DUPLEX==2){
        configASSERT(request(DMA_SERVICE_SUBMIT,0,frequency,0).result==DMA_SERVICE_OK);
        vTaskDelay(3);configASSERT(ir[0]&2);
    }
    set(&fail_clock,1);unsigned io=get(&io_count);
    configASSERT(request(DMA_SERVICE_PERIOD_GET,I2S_CHECK_DUPLEX==2?1:0,0,0).result==DMA_SERVICE_FAULT);
    for(unsigned j=0;j<2;++j){
        struct dma_service_reply r=request(DMA_SERVICE_STATUS,j,0,0);
        configASSERT(r.result==DMA_SERVICE_FAULT && r.phase==DMA_SERVICE_RECOVERY && r.holds_buffers);
        configASSERT(request(DMA_SERVICE_CLOSE,j,100000,0).result==DMA_SERVICE_FAULT);
    }
    configASSERT(get(&io_count)==io && bindings[0].pin_lease.id);
    platform_puts("PASS I2S duplex: simultaneous RX/TX accounting, disjoint catalogues, shared CTL/DRQ, both close orders, delayed drain and paired recovery retention\n");
    platform_exit(0);
}
static void client(void *unused)
{
    (void)unused;
    while(!get(&peer_stage))vTaskDelay(1);
    if(I2S_CHECK_DUPLEX)duplex_case();
    set(&pause_open,1);open_stream(0);
    configASSERT(request(DMA_SERVICE_SUBMIT,0,frequency,0).result==DMA_SERVICE_OK);
    (void)wait_phase(0,DMA_SERVICE_RUNNING);vTaskDelay(2);
    complete_period();struct a733_i2s_period a=period(0);
    configASSERT(a.kind==A733_I2S_CAPTURED && a.slot==0 && !a.first_frame && a.frames==16 &&
        a.observed_before_us<=a.observed_after_us && a.counter==16 && get(&invalidations)==1);
    configASSERT(pool[4096]==0x81 && pool[4096+255]==0x81);
    complete_period();struct a733_i2s_period b=period(0);
    configASSERT(b.slot==1 && b.first_frame==16 && b.counter==32);
    /* Keep slot 0 client-held: the third completion must stop instead of wrap. */
    set(&requested,1);struct dma_service_reply r=wait_phase(0,DMA_SERVICE_RESULT);
    configASSERT(r.outcome==DMA_SERVICE_FAILED && r.holds_buffers);
    configASSERT(request(DMA_SERVICE_REAP,0,0,0).result==DMA_SERVICE_BUSY);
    configASSERT(request(DMA_SERVICE_PERIOD_PUT,0,0,a.ticket).result==DMA_SERVICE_OK);
    configASSERT(request(DMA_SERVICE_PERIOD_PUT,0,0,a.ticket).result==DMA_SERVICE_INVALID);
    configASSERT(request(DMA_SERVICE_PERIOD_PUT,0,0,b.ticket).result==DMA_SERVICE_OK);
    configASSERT(request(DMA_SERVICE_REAP,0,0,0).result==DMA_SERVICE_OK);
    set(&pause_close,1);close_stream(0);
    /* Canceling a never-started stream turns DRQ off but never sets GEN, so
     * no bit clock or frame clock reaches the external device. */
    open_stream(0);unsigned gen=get(&gen_writes);
    configASSERT(request(DMA_SERVICE_CANCEL,0,0,0).result==DMA_SERVICE_OK);
    r=wait_phase(0,DMA_SERVICE_RESULT);configASSERT(r.outcome==DMA_SERVICE_CANCELED);
    configASSERT(request(DMA_SERVICE_REAP,0,0,0).result==DMA_SERVICE_OK);
    configASSERT(get(&gen_writes)==gen && !(ir[0]&7) && !ir[28/4]);close_stream(0);

    tx_mode=1;
    /* RAM job retains the TX source: stream OPEN must reject READ/READ alias. */
    configASSERT(request(DMA_SERVICE_OPEN,2,0,0).result==DMA_SERVICE_OK);
    configASSERT(request(DMA_SERVICE_SUBMIT,2,frequency,0).result==DMA_SERVICE_OK);
    configASSERT(request(DMA_SERVICE_OPEN,1,100000,0).result==DMA_SERVICE_BUSY);
    configASSERT(request(DMA_SERVICE_CANCEL,2,0,0).result==DMA_SERVICE_OK);
    (void)wait_phase(2,DMA_SERVICE_RESULT);
    configASSERT(request(DMA_SERVICE_REAP,2,0,0).result==DMA_SERVICE_OK);
    open_stream(1);
    configASSERT(request(DMA_SERVICE_SUBMIT,2,frequency,0).result==DMA_SERVICE_BUSY);
    configASSERT(request(DMA_SERVICE_CLOSE,2,0,0).result==DMA_SERVICE_OK);
    for(unsigned i=0;i<3;++i) {
        /* START refuses a partly committed playback catalogue. */
        if(i==2)configASSERT(request(DMA_SERVICE_SUBMIT,1,frequency,0).result==DMA_SERVICE_BUSY);
        struct a733_i2s_period p=period(1);configASSERT(p.slot==i && p.kind==A733_I2S_TX_FILL);
        memset(pool+4096+i*256,0x30+i,256);
        configASSERT(request(DMA_SERVICE_PERIOD_PUT,1,0,p.ticket).result==DMA_SERVICE_OK);
    }
    configASSERT(request(DMA_SERVICE_SUBMIT,1,frequency,0).result==DMA_SERVICE_OK);
    vTaskDelay(3);complete_period();a=period(1);
    configASSERT(a.kind==A733_I2S_TX_MEMORY_CONSUMED && a.slot==0 && a.counter==16);
    memset(pool+4096,0x30,256);
    configASSERT(request(DMA_SERVICE_PERIOD_PUT,1,0,a.ticket).result==DMA_SERVICE_OK);
    complete_period();b=period(1);configASSERT(b.slot==1 && b.first_frame==16);
    configASSERT(request(DMA_SERVICE_PERIOD_PUT,1,0,b.ticket).result==DMA_SERVICE_OK);
    set(&drain_enabled,0);
    configASSERT(request(DMA_SERVICE_CANCEL,1,0,0).result==DMA_SERVICE_OK);
    r=request(DMA_SERVICE_STATUS,1,0,0);
    configASSERT(r.phase==DMA_SERVICE_DRAINING && r.holds_buffers && !ir[28/4] && !(ir[0]&6));
    configASSERT(request(DMA_SERVICE_REAP,1,0,0).result==DMA_SERVICE_BUSY);
    set(&drain_enabled,1);
    (void)wait_phase(1,DMA_SERVICE_RESULT);
    configASSERT(request(DMA_SERVICE_REAP,1,0,0).result==DMA_SERVICE_OK);close_stream(1);

    tx_mode=0;open_stream(0);
    /* A busy registry leaves GET output empty and makes no device access. */
    atomic_store(&clocks.busy,1);unsigned io=get(&io_count);
    r=request(DMA_SERVICE_PERIOD_GET,0,0,0);configASSERT(r.result==DMA_SERVICE_BUSY && !r.period.ticket);
    configASSERT(get(&io_count)==io);atomic_store(&clocks.busy,0);
    configASSERT(request(DMA_SERVICE_SUBMIT,0,frequency,0).result==DMA_SERVICE_OK);
    vTaskDelay(3);set(&late_drain,1);set(&requested,1);
    r=wait_phase(0,DMA_SERVICE_RECOVERY);
    configASSERT(r.holds_buffers && r.owns_session && get(&invalidations)==4);
    io=get(&io_count);vTaskDelay(3);
    configASSERT(request(DMA_SERVICE_PERIOD_GET,0,0,0).result==DMA_SERVICE_FAULT);
    configASSERT(request(DMA_SERVICE_REAP,0,0,0).result==DMA_SERVICE_FAULT && get(&io_count)==io);
    platform_puts("PASS I2S stream: real leases/locks, copied queues, RX/TX periods, full cache ownership, held-slot overrun, strict alias exclusion, stop/drain and late-publication retention\n");
    platform_exit(0);
}
static int no_time(void *cookie,uint64_t *out){(void)cookie;(void)out;return 0;}
static void bootstrap(void *unused)
{
    (void)unused;privileged();
    struct a733_i2s_stream peer={.status=A733_I2S_READY};
    struct a733_i2s_stream failed={.peer=&peer,.status=A733_I2S_READY,
        .profile={.operations={.io={.now_us=no_time}}}};
    configASSERT(dma_service_i2s_stream_close(&failed,100)==A733_I2S_RECOVERY && failed.status==A733_I2S_RECOVERY && peer.status==A733_I2S_RECOVERY);
    for(unsigned i=0;i<30 && !a733_audio_parent_ready(&parent);++i) {
        configASSERT(a733_audio_parent_poll(&parent)!=A733_I2S_RESOURCE_RETAINED);vTaskDelay(1);
    }
    configASSERT(a733_audio_parent_ready(&parent) && dma_service_activate(&service)==DMA_SERVICE_OK);
    set(&peer_stage,1);vTaskSuspend(NULL);configASSERT(0);
}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name){(void)task;platform_panic(name,0);}
void kernel_main(void)
{
    tx_mode=I2S_CHECK_DUPLEX?2:0;
    platform_init();__asm__ volatile("mrs %0,cntfrq_el0":"=r"(frequency));configASSERT(frequency);
    ccu[0]=0xf8000000u|(99u<<8)|(99u<<16);ccu[0x280/4]=0xf8047f00;
    ccu[0x191c/4]=0x60006;ccu[0x199c/4]=0x60000;
    dma[0x34/4]=3;dma[di(0,8)]=0x41000000;dma[di(1,8)]=0x41000040;
    configASSERT(a733_platform_locks_init(&locks,clock_now,NULL));
    configASSERT(a733_clock_leases_init(&clocks,clock_now,NULL));
    struct a733_audio_operations backend={.enter=enter,.leave=leave,.now_us=clock_now,.read=read_reg,.write=write_reg};
    configASSERT(a733_audio_leases_init(&audio,&clocks,7,&backend,NULL));
    configASSERT(a733_audio_parent_init(&parent,&audio.operations,&audio,0,100000));
    struct a733_pins_operations pops={enter,leave,clock_now,read_reg,write_reg};
    configASSERT(a733_pins_init(&pins,&pops,NULL,A733_PINS_PB_MASK));
    for(unsigned j=0;j<2;++j) {
        struct a733_pin_request req[]={{5,{0,0,0},{3,0,0}},{6,{0,0,0},{3,0,0}},
            {I2S_CHECK_DUPLEX?8u:j?7u:8u,{0,0,0},{3,0,0}},{7,{0,0,0},{3,0,0}}};
        if(I2S_CHECK_DUPLEX){
            if(!j)configASSERT(a733_i2s_platform_duplex_init(&bindings[0],&parent,&pins,&audio,7,req,4));
            profiles[j].duplex=1;
        }else configASSERT(a733_i2s_platform_stream_init(&bindings[j],&parent,&pins,&audio,7,req,3,
            j?A733_I2S_TRANSMIT:A733_I2S_RECEIVE));
        profiles[j].stream=(struct a733_i2s_stream_profile){
            {24576000,16000,0,32,32,1,0},a733_i2s_platform_stream_operations(),&bindings[I2S_CHECK_DUPLEX?0:j],
            j?A733_I2S_TRANSMIT:A733_I2S_RECEIVE,10000,0};
    }
    struct dma_memory_window window={(uintptr_t)pool,(uintptr_t)pool,sizeof(pool),7};
    struct dma_fifo_port ports[]={{A733_I2S0_RXFIFO,3,1u<<4,1u<<8,A733_DMA_READ},
        {A733_I2S0_TXFIFO,3,1u<<4,1u<<8,A733_DMA_WRITE}};
    configASSERT(a733_dma_native_init(&native,0x09000000,0x09000000,&window,1,ports,2,fatal,NULL));
    struct dma_service_job jobs[3]={0};struct a733_dma_segment segments[2][3],ram;
    struct a733_dma_mapping maps[2][3],rammaps[2];
    for(unsigned j=0;j<2;++j) {
        for(unsigned i=0;i<3;++i) {
            uintptr_t data=(uintptr_t)pool+(I2S_CHECK_DUPLEX&&j?8192:4096)+i*256;
            maps[j][i]=(struct a733_dma_mapping){(void *)data,data,256,j?A733_DMA_READ:A733_DMA_WRITE};
            struct a733_dma_endpoint memory={data,0,4,8},peripheral={j?A733_I2S0_TXFIFO:A733_I2S0_RXFIFO,3,4,8};
            segments[j][i]=(struct a733_dma_segment){j?memory:peripheral,j?peripheral:memory,64};
        }
        jobs[j]=(struct dma_service_job){.kind=DMA_SERVICE_I2S_STREAM,.channel=I2S_CHECK_DUPLEX?j:0,.stream=&profiles[j],
            .transfer={{pool+(I2S_CHECK_DUPLEX&&j?512:0),(uintptr_t)pool+(I2S_CHECK_DUPLEX&&j?512:0),256,A733_DMA_DESCRIPTORS},segments[j],3,maps[j],3}};
    }
    rammaps[0]=maps[1][0];rammaps[1]=(struct a733_dma_mapping){pool+8192,(uintptr_t)pool+8192,256,A733_DMA_WRITE};
    ram=(struct a733_dma_segment){{rammaps[0].bus,0,4,8},{rammaps[1].bus,0,4,8},64};
    jobs[2]=(struct dma_service_job){.kind=DMA_SERVICE_RAM,.channel=1,
        .transfer={{pool+512,(uintptr_t)pool+512,64,A733_DMA_DESCRIPTORS},&ram,1,rammaps,2}};
    const struct a733_dma_operations ops={dma_read,dma_write,cache,barrier,ticks,mapping,fifo};
    struct dma_service_protected protected[]={
        {(uintptr_t)&service,(uintptr_t)&service,sizeof(service)},
        {(uintptr_t)&native,(uintptr_t)&native,sizeof(native)},
        {(uintptr_t)&audio,(uintptr_t)&audio,sizeof(audio)},
        {(uintptr_t)&parent,(uintptr_t)&parent,sizeof(parent)},
        {(uintptr_t)&bindings,(uintptr_t)&bindings,sizeof(bindings)},
        {(uintptr_t)&pins,(uintptr_t)&pins,sizeof(pins)},
        {(uintptr_t)&clocks,(uintptr_t)&clocks,sizeof(clocks)},
        {(uintptr_t)&locks,(uintptr_t)&locks,sizeof(locks)}};
    creating_bootstrap=1;
    bootstrap_task=xTaskCreateStaticAffinitySet(bootstrap,"audio-bootstrap",4096,NULL,2,stacks[0],&tasks[0],1);
    creating_bootstrap=0;configASSERT(bootstrap_task);
    struct dma_service_activation activation={bootstrap_task,prepared,&parent};
    const struct dma_service_i2s_stream_profile *saved=jobs[0].stream;
    memcpy(&service.controller,saved,sizeof(*saved));
    jobs[0].stream=(const struct dma_service_i2s_stream_profile *)&service.controller;
    configASSERT(!dma_service_init_deferred(&service,&ops,NULL,D,3,frequency/10,jobs,I2S_CHECK_DUPLEX?2:3,
        protected,8,3,3,&activation));
    jobs[0].stream=saved;memset(&service.controller,0,sizeof(service.controller));
    if(I2S_CHECK_DUPLEX){
        jobs[1].channel=0;
        configASSERT(!dma_service_init_deferred(&service,&ops,NULL,D,3,frequency/10,jobs,2,protected,8,3,3,&activation));
        jobs[1].channel=1;
        profiles[1].stream.format.rate_hz=8000;
        configASSERT(!dma_service_init_deferred(&service,&ops,NULL,D,3,frequency/10,jobs,2,protected,8,3,3,&activation));
        profiles[1].stream.format.rate_hz=16000;
        struct a733_dma_mapping saved_map=maps[1][0];
        maps[1][0].cpu=maps[0][0].cpu;maps[1][0].bus=maps[0][0].bus;
        segments[1][0].source.address=maps[0][0].bus;
        configASSERT(!dma_service_init_deferred(&service,&ops,NULL,D,3,frequency/10,jobs,2,protected,8,3,3,&activation));
        maps[1][0]=saved_map;segments[1][0].source.address=saved_map.bus;
    }
    configASSERT(dma_service_init_deferred(&service,&ops,NULL,D,3,frequency/10,jobs,I2S_CHECK_DUPLEX?2:3,
        protected,8,3,3,&activation));
    memset(jobs,0,sizeof(jobs));memset(segments,0,sizeof(segments));memset(maps,0,sizeof(maps));
    TaskHandle_t task=xTaskCreateStaticAffinitySet(client,"audio-client",4096,NULL,2,stacks[1],&tasks[1],2);
    configASSERT(task && dma_service_bind(&service,0,task,I2S_CHECK_DUPLEX?3:7));
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
