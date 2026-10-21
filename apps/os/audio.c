/* I2S microphone receive for the integrated OS image (docs/os-image.md). Each
 * recognition generation captures one finite 16 kHz mono utterance through
 * the DMA service's I2S0 receive stream, with the I2S image's composition
 * (apps/i2s/main.c) and client protocol (apps/i2s/client.c): copy each
 * completed period into a WAV buffer, cancel, and publish the buffer once the
 * service reports CANCELED with its reservations held. The recognizer reads
 * the buffer at EL0. Before the next capture the client returns the final
 * period, reaps and closes the stream, then opens it again. Any refusal ends
 * the session through board_fail and keeps every reservation, lease and the
 * stream held; the supervised session's manual power removal recovers them. */
#include "os.h"
#include "../board/application.h"
#include "a733_audio_leases.h"
#include "a733_i2s_platform_native.h"
#include "a733_dma_pool_session.h"
#include "dma_service_i2s_irq.h"
#include "aarch64_device_memory.h"
#include "boot.h"
#include <string.h>
_Static_assert(OS_AUDIO_DMA_SUPERVISED==A733_DMA_HANDOFF_SUPERVISED,"supervised DMA handoff");
_Static_assert(A733_DMA0_INTID==143,"OS interrupt table DMA0 entry");
enum { PERIODS=4, PERIOD_BYTES=4096, PERIOD_FRAMES=PERIOD_BYTES/2 };
enum { PREPARE, POOL, IDLE, OPEN, OPENING, SUBMIT, GET, CHECK, PUT, CANCEL, DRAIN, CAPTURED,
       RETURN, REAP, CLOSE, CLOSING };
static struct {
    struct a733_dma_handoff dma;
    struct a733_audio_leases leases;
    struct a733_audio_parent parent;
    struct a733_i2s_platform binding;
    struct dma_service_i2s_stream_profile stream;
    struct a733_dma_pool_config config;
    struct a733_dma_pool_session pool;
    struct dma_service_i2s_irq irq;
    StaticTask_t task;StackType_t stack[2048];TaskHandle_t handle;
    atomic_uint failed,ready,requested,captured;
    unsigned stage,generation,samples,bytes,periods;
    uint64_t pending,held;
    struct dma_service_reply reply;
    _Alignas(4) uint8_t wav[44+2*OS_AUDIO_MAX_SAMPLES];
} audio;
static int live(void){return board_live(NULL)&&!atomic_load_explicit(&audio.failed,memory_order_acquire);}
static void fail(void){atomic_store_explicit(&audio.failed,1,memory_order_release);board_fail(NULL);}
static _Noreturn void dma_fatal(void *unused){(void)unused;fail();platform_panic("OS retained DMA",1);}
/* Before the parent is ready only the client task enters the backend; then
 * only the DMA worker does (apps/i2s/main.c). */
static int worker(void)
{return live()&&xTaskGetCurrentTaskHandle()==
    (atomic_load_explicit(&audio.ready,memory_order_acquire)?audio.pool.service.worker:audio.handle);}
static int now(void *v,uint64_t *out)
{return v==&audio&&worker()&&a733_counter_clock_now(&providers.shared.counter,out);}
static int enter(void *v,uint64_t d)
{return v==&audio&&worker()&&a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,d);}
static void leave(void *v)
{configASSERT(v==&audio);a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);}
static int mapped(uintptr_t a,int write)
{struct aarch64_translation t;return aarch64_device_translate(a,write,&t)&&t.physical==a&&t.attributes==0;}
static int read_reg(void *v,uintptr_t a,uint32_t *out)
{return v==&audio&&worker()&&mapped(a,0)&&a733_audio_native_read(NULL,a,out)==1;}
static int write_reg(void *v,uintptr_t a,uint32_t value)
{return v==&audio&&worker()&&mapped(a,1)&&a733_audio_native_write(NULL,a,value)==1;}
static void put16(uint8_t *p,unsigned v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i));}
/* One copied request at a time; 1 OK, 0 not yet answered or BUSY, -1 refused. */
static int request(enum dma_service_operation operation,uint64_t timeout,uint64_t period)
{
    if(!audio.pending){
        struct dma_service_request r={.operation=operation,.job=0,.timeout=timeout,.period_ticket=period};
        if(!dma_service_send(&audio.pool.service,&r,0,&audio.pending))return 0;
    }
    if(!dma_service_collect(&audio.pool.service,audio.pending,&audio.reply,0))return 0;
    audio.pending=0;
    if(audio.reply.result==DMA_SERVICE_BUSY)return 0;
    return audio.reply.result==DMA_SERVICE_OK?1:-1;
}
/* Client task only: one bounded step. 0 continue, -1 failed. */
static int step(void)
{
    int r;
    switch(audio.stage){
    case PREPARE:{
        enum a733_i2s_resource_result p=a733_audio_parent_poll(&audio.parent);
        if(p==A733_I2S_RESOURCE_BUSY||p==A733_I2S_RESOURCE_PENDING)return 0;
        if(p!=A733_I2S_RESOURCE_READY||!a733_audio_parent_ready(&audio.parent))return -1;
        /* OPEN is queued only after this publication; from here only the DMA
         * worker enters the backend. */
        atomic_store_explicit(&audio.ready,1,memory_order_release);audio.stage=POOL;return 0;}
    case POOL:{
        unsigned state=atomic_load_explicit(&audio.pool.state,memory_order_acquire);
        if(state==A733_DMA_POOL_FAILED)return -1;
        if(state==A733_DMA_POOL_READY)audio.stage=IDLE;
        return 0;}
    case IDLE:
    case CAPTURED:{
        unsigned g=atomic_load_explicit(&audio.requested,memory_order_acquire);
        if(g==audio.generation)return 0;
        audio.generation=g;audio.stage=audio.stage==IDLE?OPEN:RETURN;return 0;}
    case OPEN:
        r=request(DMA_SERVICE_OPEN,100000,0);if(r<0)return -1;if(!r)return 0;
        audio.stage=OPENING;return 0;
    case OPENING:
        r=request(DMA_SERVICE_STATUS,0,0);if(r<0)return -1;if(!r)return 0;
        if(audio.reply.phase==DMA_SERVICE_OPENING)return 0;
        if(audio.reply.phase!=DMA_SERVICE_READY)return -1;
        audio.bytes=audio.periods=0;audio.held=0;audio.stage=SUBMIT;return 0;
    case SUBMIT:
        /* Period timeout in DMA counter ticks: one second. */
        r=request(DMA_SERVICE_SUBMIT,A733_SMP_COUNTER_HZ,0);if(r<0)return -1;if(!r)return 0;
        audio.stage=GET;return 0;
    case GET:{
        r=request(DMA_SERVICE_PERIOD_GET,0,0);if(r<0)return -1;
        /* No completed period: a stream that stopped by itself has failed. */
        if(!r){if(!audio.pending&&audio.reply.result==DMA_SERVICE_BUSY)audio.stage=CHECK;return 0;}
        struct a733_i2s_period p=audio.reply.period;
        unsigned total=2*audio.samples;
        if(p.kind!=A733_I2S_CAPTURED||p.slot>=PERIODS||p.frames!=PERIOD_FRAMES||!p.ticket||
           p.ticket==audio.held||p.first_frame!=(uint64_t)audio.periods*PERIOD_FRAMES||audio.bytes>=total)return -1;
        unsigned bytes=total-audio.bytes;if(bytes>PERIOD_BYTES)bytes=PERIOD_BYTES;
        memcpy(audio.wav+44+audio.bytes,audio.pool.stream_mappings[p.slot].cpu,bytes);
        audio.bytes+=bytes;audio.held=p.ticket;++audio.periods;
        audio.stage=audio.bytes==total?CANCEL:PUT;return 0;}
    case CHECK:
        r=request(DMA_SERVICE_STATUS,0,0);if(r<0)return -1;if(!r)return 0;
        if(audio.reply.phase!=DMA_SERVICE_RUNNING&&audio.reply.phase!=DMA_SERVICE_READY)return -1;
        audio.stage=GET;return 0;
    case PUT:
        r=request(DMA_SERVICE_PERIOD_PUT,0,audio.held);if(r<0)return -1;if(!r)return 0;
        audio.stage=GET;return 0;
    case CANCEL:
        r=request(DMA_SERVICE_CANCEL,0,0);if(r<0)return -1;if(!r)return 0;
        audio.stage=DRAIN;return 0;
    case DRAIN:{
        r=request(DMA_SERVICE_STATUS,0,0);if(r<0)return -1;if(!r)return 0;
        if(audio.reply.phase==DMA_SERVICE_DRAINING||audio.reply.phase==DMA_SERVICE_RUNNING)return 0;
        if(audio.reply.phase!=DMA_SERVICE_RESULT||audio.reply.outcome!=DMA_SERVICE_CANCELED||
           !audio.reply.holds_buffers||audio.bytes!=2*audio.samples)return -1;
        uint8_t *w=audio.wav;memcpy(w,"RIFF",4);put32(w+4,36+audio.bytes);memcpy(w+8,"WAVEfmt ",8);
        put32(w+16,16);put16(w+20,1);put16(w+22,1);put32(w+24,16000);put32(w+28,32000);
        put16(w+32,2);put16(w+34,16);memcpy(w+36,"data",4);put32(w+40,audio.bytes);
        atomic_store_explicit(&audio.captured,audio.generation,memory_order_release);
        audio.stage=CAPTURED;return 0;}
    case RETURN:
        /* A later request releases the published buffer and the final period. */
        r=request(DMA_SERVICE_PERIOD_PUT,0,audio.held);if(r<0)return -1;if(!r)return 0;
        audio.stage=REAP;return 0;
    case REAP:
        r=request(DMA_SERVICE_REAP,0,0);if(r<0)return -1;if(!r)return 0;
        audio.stage=CLOSE;return 0;
    case CLOSE:
        r=request(DMA_SERVICE_CLOSE,100000,0);if(r<0)return -1;if(!r)return 0;
        audio.stage=CLOSING;return 0;
    case CLOSING:
        r=request(DMA_SERVICE_STATUS,0,0);if(r<0)return -1;if(!r)return 0;
        if(audio.reply.phase==DMA_SERVICE_CLOSING)return 0;
        if(audio.reply.phase!=DMA_SERVICE_CLOSED)return -1;
        audio.stage=OPEN;return 0;
    default:return -1;
    }
}
void os_audio_task(void *unused)
{
    (void)unused;
    for(;;){
        /* A failed capture parks with its request, leases and periods held. */
        if(!atomic_load_explicit(&audio.failed,memory_order_acquire)&&(!live()||step()<0))fail();
        vTaskDelay(1);
    }
}
int os_audio_capture(unsigned g,const uint8_t **wav,size_t *bytes)
{
    if(!live()||!g||!wav||!bytes)return -1;
    unsigned requested=atomic_load_explicit(&audio.requested,memory_order_relaxed);
    if(g<requested)return -1;
    if(g>requested)atomic_store_explicit(&audio.requested,g,memory_order_release);
    if(atomic_load_explicit(&audio.captured,memory_order_acquire)!=g)return 0;
    *wav=audio.wav;*bytes=44+2*audio.samples;return live()?1:-1;
}
void os_audio_irq(void){dma_service_i2s_irq_handler(&audio.irq);}
int os_audio_task_privileged(void (*entry)(void *),void *context)
{return (!context&&entry==os_audio_task)||a733_dma_pool_task_privileged(entry,context)==pdTRUE;}
/* PB5, PB6 and PB8 join the board pin manager's authority. */
int os_audio_pins(struct a733_pin_handoff *pins)
{
    if(!pins||!os_session_admitted()||(pins->authorized[A733_PIN_B]&((1u<<5)|(1u<<6)|(1u<<8))))return 0;
    pins->authorized[A733_PIN_B]|=(1u<<5)|(1u<<6)|(1u<<8);return 1;
}
int os_audio_start(void)
{
    const struct os_session *s=os_session_admitted();
    if(!s||!board_live(NULL))return 0;
    const struct os_audio_session *a=&s->audio;
    uint64_t deadline=board.session.deadline_us,owner=a->dma.owner;
    audio.samples=a->capture_samples;
    audio.dma=(struct a733_dma_handoff){a->dma,a->dma_controller,a->dma_evidence,a->dma_established,
        a->dma_owned_channels,a->dma_service_channels};
    audio.dma.allowed.deadline_us=deadline;
    audio.handle=xTaskCreateStatic(os_audio_task,"os-audio",2048,NULL,4,audio.stack,&audio.task);
    if(!audio.handle)return 0;
    struct a733_audio_operations backend={.enter=enter,.leave=leave,.now_us=now,.read=read_reg,.write=write_reg};
    struct a733_pin_request pins[3];
    static const unsigned pb[3]={5,6,8};
    for(unsigned i=0;i<3;++i)pins[i]=(struct a733_pin_request){pb[i],
        {a->inherited[i][0],a->inherited[i][1],a->inherited[i][2]},{3,0,0}};
    if(!a733_audio_leases_init(&audio.leases,&providers.shared.clocks,owner,&backend,&audio)||
       !a733_audio_parent_init(&audio.parent,&audio.leases.operations,&audio.leases,a->audio_cold,deadline)||
       !a733_i2s_platform_stream_init(&audio.binding,&audio.parent,&providers.shared.pins,&audio.leases,
           owner,pins,3,A733_I2S_RECEIVE))return 0;
    audio.stream.stream=(struct a733_i2s_stream_profile){{24576000,16000,0,16,32,1,0},
        a733_i2s_platform_stream_operations(),&audio.binding,A733_I2S_RECEIVE,10000,0};
    audio.config=(struct a733_dma_pool_config){&audio.dma,&providers.shared.counter,&providers.shared.clocks,
        &providers.shared.locks,240000,4096,5,1,dma_fatal,&audio};
    return a733_dma_pool_session_stream_supervised_init(&audio.pool,&audio.config,&audio.stream,PERIODS,PERIOD_BYTES)&&
        dma_service_bind(&audio.pool.service,0,audio.handle,1)&&
        dma_service_i2s_irq_bind(&audio.irq,&audio.pool.service,A733_SMP_COUNTER_HZ);
}
