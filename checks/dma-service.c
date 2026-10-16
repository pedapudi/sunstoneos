/* Two-core FreeRTOS service integration. Device registers/FIFO are modeled;
 * CPU translation/cache operations use the native adapter. */
#include "dma_service.h"
#include "a733_dma_native.h"
#include "platform.h"
#include <string.h>

static _Alignas(64) struct dma_service service;
static struct a733_dma_native native;
static _Alignas(4096) uint8_t pool[6*4096];
static uint32_t regs[0x2000/4],ir[0xa0/4];
static uint32_t complete[4],drain_allowed[4],finished[4],accesses[4];
static uint64_t launched[4],frequency;
static unsigned live[4],i2s_acquired,i2s_pending;
static uint32_t stage,peer_done,observer_done;
static StaticTask_t tasks[3];
static StackType_t stacks[3][2048];
static struct dma_service_job jobs[6];
static struct a733_dma_segment segments[6];
static struct a733_dma_mapping mappings[6][2];
static struct dma_service_i2s_profile profile;
static const struct a733_dma_operations *native_ops=&a733_dma_native_operations;
static uint32_t get(const uint32_t *p) { return __atomic_load_n(p,__ATOMIC_ACQUIRE); }
static void set(uint32_t *p,uint32_t v) { __atomic_store_n(p,v,__ATOMIC_RELEASE); }
static unsigned task_level(void)
{
    register uint64_t r __asm__("x0");
    __asm__ volatile("svc #120" : "=r"(r) :: "memory");
    return r;
}
static void privileged(void)
{
    uint64_t el,sp;
    __asm__ volatile("mrs %0,CurrentEL\n mrs %1,spsel" : "=r"(el),"=r"(sp));
    configASSERT(el == 4 && sp == 0 && xTaskGetCurrentTaskHandle() == service.worker);
}
static uint64_t counter(void)
{ uint64_t v; __asm__ volatile("isb\n mrs %0,cntpct_el0" : "=r"(v)); return v; }
static uint64_t ticks(void *cookie) { (void)cookie; privileged(); return native_ops->ticks(&native); }
static uint64_t microseconds(void) { return counter()*UINT64_C(1000000)/frequency; }
static void fatal(void *cookie) { (void)cookie; platform_panic("DMA native callback",0); }
static unsigned index_of(unsigned channel,unsigned offset) { return (0x100+channel*0x40+offset)/4; }
static void advance(void)
{
    for (unsigned ch=0;ch<4;++ch) {
        if (live[ch] && get(&complete[ch]) && counter()-launched[ch] >= frequency/1000) {
            const struct a733_dma_transfer *t=service.controller.channels[ch].transfer;
            configASSERT(t && t->count == 1);
            const struct a733_dma_segment *s=t->segments;
            if (ch == 2) {
                configASSERT(i2s_acquired && (ir[0]&2) && ir[28/4] == 8);
                for (unsigned k=0;k<s->bytes;k+=4) *(uint32_t *)(uintptr_t)(s->destination.address+k)=0x12345600u+k;
            } else memcpy((void *)(uintptr_t)s->destination.address,(void *)(uintptr_t)s->source.address,s->bytes);
            regs[index_of(ch,0x38)] |= 4; live[ch]=0;
        }
        if (!regs[index_of(ch,0)] && get(&drain_allowed[ch])) regs[0x30/4] &= ~(1u<<ch);
    }
}
static uint32_t read32(void *cookie,uintptr_t address)
{
    (void)cookie; privileged();
    configASSERT(address >= 0x04601000 && address < 0x04603000);
    unsigned offset=address-0x04601000;
    if (offset >= 0x100 && offset < 0x200) __atomic_fetch_add(&accesses[(offset-0x100)/0x40],1,__ATOMIC_RELEASE);
    advance(); return regs[offset/4];
}
static void write32(void *cookie,uintptr_t address,uint32_t value)
{
    (void)cookie; privileged();
    unsigned offset=address-0x04601000;
    configASSERT(offset >= 0x100 && offset < 0x200);
    unsigned ch=(offset-0x100)/0x40,r=(offset-0x100)%0x40;
    __atomic_fetch_add(&accesses[ch],1,__ATOMIC_RELEASE);
    if (r == 0 && value) {
        if (ch == 2) configASSERT(!(ir[0]&2) && !ir[28/4]);
        regs[0x30/4] |= 1u<<ch; live[ch]=1; launched[ch]=counter();
    }
    if (r == 0 && !value) {
        if (ch == 2) configASSERT(!(ir[0]&2) && !ir[28/4]);
        live[ch]=0;
    }
    if (r == 0x38) regs[offset/4] &= ~value;
    else regs[offset/4]=value;
}
static void cache(void *cookie,enum a733_dma_cache op,void *cpu,size_t bytes)
{
    (void)cookie; privileged();
    if (op == A733_DMA_INVALIDATE) {
        for (unsigned ch=0;ch<4;++ch) {
            const struct a733_dma_transfer *t=service.controller.channels[ch].transfer;
            if (!t) continue;
            for (size_t n=0;n<t->mapping_count;++n) if (t->mappings[n].cpu == cpu) {
                configASSERT(!(regs[0x30/4]&(1u<<ch)) && !regs[index_of(ch,0)]);
                if (ch == 2) configASSERT(!(ir[0]&2) && !ir[28/4]);
                __atomic_fetch_add(&finished[ch],1,__ATOMIC_RELEASE);
            }
        }
    }
    native_ops->cache(&native,op,cpu,bytes);
}
static void barrier(void *cookie) { (void)cookie; privileged(); native_ops->barrier(&native); }
static int mapping(void *cookie,const struct a733_dma_mapping *m)
{ (void)cookie; privileged(); return native_ops->validate_mapping(&native,m); }
static int fifo(void *cookie,const struct a733_dma_endpoint *e,enum a733_dma_access access)
{ (void)cookie; privileged(); return native_ops->validate_fifo(&native,e,access); }
static enum a733_i2s_resource_result acquire(void *cookie,const struct a733_i2s_plan *p,uint64_t deadline)
{
    configASSERT(cookie == &profile && p->format.rate_hz == 16000 && microseconds() < deadline);
    privileged(); configASSERT(!i2s_acquired); i2s_acquired=1; i2s_pending=1;
    return A733_I2S_RESOURCE_PENDING;
}
static enum a733_i2s_resource_result acquire_poll(void *cookie,uint64_t deadline)
{
    configASSERT(cookie == &profile && i2s_acquired && microseconds() < deadline);
    privileged(); i2s_pending=0; return A733_I2S_RESOURCE_READY;
}
static enum a733_i2s_resource_result release(void *cookie,uint64_t deadline)
{
    configASSERT(cookie == &profile && microseconds() < deadline); privileged();
    configASSERT(i2s_acquired && !(ir[0]&7) && !ir[28/4] && !(regs[0x30/4]&4));
    i2s_acquired=0; return A733_I2S_RESOURCE_READY;
}
static int now_us(void *cookie,uint64_t *out)
{ configASSERT(cookie == &profile); privileged(); *out=microseconds(); return 1; }
static int i2s_read(void *cookie,uintptr_t address,uint32_t *out)
{
    configASSERT(cookie == &profile && i2s_acquired && address >= A733_I2S0_BASE && address < A733_I2S0_BASE+sizeof(ir));
    privileged(); *out=ir[(address-A733_I2S0_BASE)/4]; return 1;
}
static int i2s_write(void *cookie,uintptr_t address,uint32_t value)
{
    configASSERT(cookie == &profile && i2s_acquired && address >= A733_I2S0_BASE && address < A733_I2S0_BASE+sizeof(ir));
    privileged(); unsigned offset=address-A733_I2S0_BASE;
    if (offset == 28 && value == 8) configASSERT(regs[index_of(2,0)] && (ir[0]&2));
    if (offset == 20) value &= ~(1u<<24);
    ir[offset/4]=value; return 1;
}
static struct dma_service_reply request(enum dma_service_operation op,unsigned job,uint64_t timeout)
{
    configASSERT(task_level() == 0);
    struct dma_service_request r={op,job,timeout}; struct dma_service_reply reply; uint64_t ticket;
    configASSERT(dma_service_send(&service,&r,100,&ticket));
    configASSERT(dma_service_collect(&service,ticket,&reply,100));
    configASSERT(reply.ticket == ticket && task_level() == 0); return reply;
}
static void wait_flag(const uint32_t *p,uint32_t value)
{
    TickType_t start=xTaskGetTickCount();
    while (get(p) != value) { configASSERT(xTaskGetTickCount()-start < 500); vTaskDelay(1); }
}
static void wait_finished(unsigned ch,uint32_t value)
{ wait_flag(&finished[ch],value); }
static void peer(void *unused)
{
    (void)unused; wait_flag(&stage,1);
    configASSERT(request(DMA_SERVICE_CANCEL,0,0).result == DMA_SERVICE_DENIED);
    configASSERT(request(DMA_SERVICE_OPEN,0,0).result == DMA_SERVICE_BUSY);
    configASSERT(request(DMA_SERVICE_OPEN,1,0).result == DMA_SERVICE_OK);
    configASSERT(request(DMA_SERVICE_SUBMIT,1,frequency/2).result == DMA_SERVICE_BUSY);
    configASSERT(request(DMA_SERVICE_SUBMIT,2,frequency/2).result == DMA_SERVICE_OK);
    set(&complete[1],1); set(&stage,2);
    wait_finished(1,1);
    struct dma_service_reply r=request(DMA_SERVICE_STATUS,2,0);
    configASSERT(r.phase == DMA_SERVICE_RESULT && r.holds_buffers);
    set(&peer_done,1);
    /* A registered task abandons its completed result and channel deliberately. */
    vTaskSuspend(NULL); configASSERT(0);
}
static void observer(void *unused)
{
    (void)unused; struct dma_service_request r={DMA_SERVICE_STATUS,0,0}; uint64_t t=77;
    configASSERT(task_level() == 0 && !dma_service_send(&service,&r,0,&t) && t == 77);
    set(&observer_done,1); vTaskSuspend(NULL); configASSERT(0);
}
static void client(void *unused)
{
    (void)unused; wait_flag(&observer_done,1);
    struct dma_service_request r={DMA_SERVICE_STATUS,0,0}; uint64_t ticket=0;
    struct dma_service_reply reply={.ticket=123};
    configASSERT(!dma_service_send(&service,&r,portMAX_DELAY,&ticket));
    vTaskSuspend(service.worker); /* Fixture forces a late reply while worker is idle. */
    configASSERT(dma_service_send(&service,&r,0,&ticket));
    r.operation=DMA_SERVICE_CLOSE; r.job=99; /* Queue must contain a copy. */
    configASSERT(!dma_service_collect(&service,ticket,&reply,0) && reply.ticket == 123);
    configASSERT(!dma_service_send(&service,&r,0,&ticket));
    vTaskResume(service.worker);
    configASSERT(dma_service_collect(&service,ticket,&reply,100) && reply.result == DMA_SERVICE_OK);
    configASSERT(request(DMA_SERVICE_OPEN,0,0).result == DMA_SERVICE_OK);
    configASSERT(request(DMA_SERVICE_SUBMIT,0,frequency/2).result == DMA_SERVICE_OK);
    configASSERT(request(DMA_SERVICE_REAP,0,0).result == DMA_SERVICE_BUSY);
    set(&stage,1); wait_flag(&stage,2); wait_flag(&peer_done,1);
    set(&complete[0],1); wait_finished(0,1); /* No request advances completion. */
    reply=request(DMA_SERVICE_STATUS,0,0);
    configASSERT(reply.phase == DMA_SERVICE_RESULT && reply.holds_buffers);
    configASSERT(request(DMA_SERVICE_CLOSE,0,0).result == DMA_SERVICE_BUSY);
    reply=request(DMA_SERVICE_REAP,0,0);
    configASSERT(reply.result == DMA_SERVICE_OK && reply.outcome == DMA_SERVICE_COMPLETE && !reply.holds_buffers);
    configASSERT(!memcmp(pool+4096,pool+8192,64));
    /* Peer result remains reserved after its controller released the transfer. */
    configASSERT(request(DMA_SERVICE_SUBMIT,5,frequency/2).result == DMA_SERVICE_BUSY);
    configASSERT(request(DMA_SERVICE_CLOSE,2,0).result == DMA_SERVICE_DENIED);

    set(&complete[0],0);
    configASSERT(request(DMA_SERVICE_SUBMIT,0,frequency/1000).result == DMA_SERVICE_OK);
    wait_finished(0,2);
    reply=request(DMA_SERVICE_REAP,0,0); configASSERT(reply.outcome == DMA_SERVICE_FAILED && !reply.holds_buffers);

    set(&drain_allowed[0],0);
    configASSERT(request(DMA_SERVICE_SUBMIT,0,frequency/2).result == DMA_SERVICE_OK);
    configASSERT(request(DMA_SERVICE_CANCEL,0,0).result == DMA_SERVICE_OK);
    reply=request(DMA_SERVICE_STATUS,0,0); configASSERT(reply.phase == DMA_SERVICE_DRAINING && reply.holds_buffers);
    configASSERT(request(DMA_SERVICE_REAP,0,0).result == DMA_SERVICE_BUSY);
    set(&drain_allowed[0],1); wait_finished(0,3);
    reply=request(DMA_SERVICE_REAP,0,0); configASSERT(reply.outcome == DMA_SERVICE_CANCELED && !reply.holds_buffers);
    configASSERT(request(DMA_SERVICE_CLOSE,0,0).result == DMA_SERVICE_OK);

    configASSERT(request(DMA_SERVICE_OPEN,3,100000).result == DMA_SERVICE_OK);
    vTaskDelay(2);
    reply=request(DMA_SERVICE_STATUS,3,0); configASSERT(reply.phase == DMA_SERVICE_READY && !i2s_pending);
    configASSERT(request(DMA_SERVICE_SUBMIT,3,frequency/2).result == DMA_SERVICE_OK);
    set(&complete[2],1); wait_finished(2,1);
    reply=request(DMA_SERVICE_REAP,3,0); configASSERT(reply.outcome == DMA_SERVICE_COMPLETE);
    configASSERT(*(uint32_t *)(pool+8192+128) == 0x12345600);
    configASSERT(request(DMA_SERVICE_SUBMIT,3,frequency/2).result == DMA_SERVICE_BUSY);
    configASSERT(request(DMA_SERVICE_CLOSE,3,100000).result == DMA_SERVICE_OK && !i2s_acquired);

    configASSERT(request(DMA_SERVICE_OPEN,4,0).result == DMA_SERVICE_OK);
    set(&drain_allowed[3],0); set(&complete[3],1);
    configASSERT(request(DMA_SERVICE_SUBMIT,4,frequency/2).result == DMA_SERVICE_OK);
    vTaskDelay(25);
    reply=request(DMA_SERVICE_STATUS,4,0);
    configASSERT(reply.phase == DMA_SERVICE_RECOVERY && reply.holds_buffers && reply.owns_session);
    uint32_t before=get(&accesses[3]); vTaskDelay(3);
    configASSERT(get(&accesses[3]) == before);
    configASSERT(request(DMA_SERVICE_REAP,4,0).result == DMA_SERVICE_FAULT);
    configASSERT(request(DMA_SERVICE_CLOSE,4,0).result == DMA_SERVICE_FAULT && get(&accesses[3]) == before);
    platform_puts("PASS DMA service: EL0 clients, private EL1 worker, copied metadata, late replies, retained reservations, autonomous drain/deadlines and I2S producer ordering\n");
    platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{ (void)task; platform_panic(name,0); }
void kernel_main(void)
{
    platform_init(); __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(frequency)); configASSERT(frequency);
    for (unsigned i=0;i<64;++i) pool[4096+i]=(uint8_t)(i^0xa5);
    for (unsigned ch=0;ch<4;++ch) { drain_allowed[ch]=1; regs[index_of(ch,8)]=0x41000000+ch*64; }
    regs[0x34/4]=15;
    struct dma_memory_window window={(uintptr_t)pool,(uintptr_t)pool,sizeof(pool),7};
    struct dma_fifo_port port={A733_I2S0_RXFIFO,3,1u<<4,1u<<8,A733_DMA_READ};
    configASSERT(a733_dma_native_init(&native,0x09000000,0x09000000,&window,1,&port,1,fatal,NULL));
    profile=(struct dma_service_i2s_profile){{24576000,16000,0,32,32,1,0},
        {acquire,acquire_poll,release,now_us,i2s_read,i2s_write},&profile};
    const unsigned channels[]={0,1,1,2,3,0},destinations[]={0,0,64,128,192,64};
    for (unsigned j=0;j<6;++j) {
        unsigned ch=channels[j];
        mappings[j][0]=(struct a733_dma_mapping){pool+4096,(uintptr_t)pool+4096,64,A733_DMA_READ};
        mappings[j][1]=(struct a733_dma_mapping){pool+8192+destinations[j],(uintptr_t)pool+8192+destinations[j],64,A733_DMA_WRITE};
        segments[j]=(struct a733_dma_segment){{(uintptr_t)pool+4096,0,4,8},
            {(uintptr_t)pool+8192+destinations[j],0,4,8},64};
        jobs[j]=(struct dma_service_job){DMA_SERVICE_RAM,ch,
            {{pool+ch*64,(uintptr_t)pool+ch*64,64,A733_DMA_DESCRIPTORS},&segments[j],1,mappings[j],2},NULL};
    }
    jobs[3].kind=DMA_SERVICE_I2S; jobs[3].i2s=&profile;
    jobs[3].transfer.mappings=&mappings[3][1]; jobs[3].transfer.mapping_count=1;
    segments[3].source=(struct a733_dma_endpoint){A733_I2S0_RXFIFO,3,4,8};
    const struct a733_dma_operations ops={read32,write32,cache,barrier,ticks,mapping,fifo};
    struct dma_service_protected protected[]={
        {(uintptr_t)&service,(uintptr_t)&service,sizeof(service)},
        {(uintptr_t)&native,(uintptr_t)&native,sizeof(native)},
        {(uintptr_t)&profile,(uintptr_t)&profile,sizeof(profile)}};
    struct a733_dma_mapping saved=mappings[0][1];
    mappings[0][1].bus=(uintptr_t)&service;
    configASSERT(!dma_service_init(&service,&ops,NULL,0x04601000,15,frequency/100,jobs,6,protected,3,3,3));
    mappings[0][1]=saved;
    segments[0].source.request=3;
    configASSERT(!dma_service_init(&service,&ops,NULL,0x04601000,15,frequency/100,jobs,6,protected,3,3,3));
    segments[0].source.request=0;
    configASSERT(dma_service_init(&service,&ops,NULL,0x04601000,15,frequency/100,jobs,6,protected,3,3,3));
    /* Private copies must survive destruction of every caller-owned table. */
    memset(jobs,0,sizeof(jobs)); memset(segments,0,sizeof(segments)); memset(mappings,0,sizeof(mappings));
    TaskHandle_t a=xTaskCreateStaticAffinitySet(client,"client",2048,NULL,2,stacks[0],&tasks[0],1);
    TaskHandle_t b=xTaskCreateStaticAffinitySet(peer,"peer",2048,NULL,2,stacks[1],&tasks[1],2);
    TaskHandle_t c=xTaskCreateStaticAffinitySet(observer,"observer",2048,NULL,2,stacks[2],&tasks[2],2);
    configASSERT(a && b && c);
    configASSERT(dma_service_bind(&service,0,a,63));
    configASSERT(dma_service_bind(&service,1,b,7));
    vTaskStartScheduler(); platform_panic("scheduler returned",0);
}
