/* I2S microphone capture stream of the integrated OS image (docs/os-image.md,
 * apps/os/audio-stream.h). The stream task drives the DMA service's I2S0
 * receive stream with the I2S image's composition (apps/i2s/main.c) and
 * client protocol (apps/i2s/client.c): it copies each completed DMA period
 * once into the next free slot of the client's ring, returns the period, and
 * at a stop cancels and waits until the service reports CANCELED with its
 * reservations held. Before the next run it reaps and closes the stream, then
 * opens it again. Any refusal ends the session through board_fail and keeps
 * every reservation, lease and the stream held. Manual power removal recovers
 * them in the supervised lifetime; in the resident lifetime the stream task
 * stops reporting progress and the watchdog resets the board. */
#include "os.h"
#include "cpu-return.h"
#include "quiesce.h"
#include "../board/application.h"
#include "a733_audio_leases.h"
#include "a733_i2s_platform_native.h"
#include "a733_dma_pool_session.h"
#include "dma_service_i2s_irq.h"
#include "a733_mapping_check.h"
#include "boot.h"
#include "umem.h"
#include "sunstone_crumb.h"
#include <string.h>
struct umem *os_large_umem(void);
_Static_assert(OS_AUDIO_DMA_ESTABLISHED(A733_LIFETIME_RESIDENT)==A733_DMA_HANDOFF_REQUIRED,"resident DMA handoff");
_Static_assert(OS_AUDIO_DMA_ESTABLISHED(A733_LIFETIME_SUPERVISED)==A733_DMA_HANDOFF_SUPERVISED,"supervised DMA handoff");
_Static_assert(A733_DMA0_INTID==143,"OS interrupt table DMA0 entry");
_Static_assert(OS_AUDIO_PERIOD_CAPACITY==A733_I2S_PERIOD_CAPACITY,"stream period capacity");
/* DMA periods: one page each, independent of the client's ring. */
enum { PERIODS=4 };
/* The board's wire: AUDIO1 DIV5 / 25 and two 32-bit slots, mono in slot 0. */
enum { MODULE_HZ=24576000, SLOT_BITS=32 };
enum { PREPARE, POOL, IDLE, OPEN, OPENING, SUBMIT, GET, CHECK, PUT, CANCEL, DRAIN,
       REAP, CLOSE, CLOSING };
/* The stream's state, with the DMA pool session and the task stack (119,680 bytes), comes from the unified ledger when
 * a session first opens the stream: an image whose session does not grant the I2S service holds none of it. */
static struct os_audio_state {
    struct a733_dma_handoff dma;
    struct a733_audio_leases leases;
    struct a733_audio_parent parent;
    struct a733_i2s_platform binding;
    struct dma_service_i2s_stream_profile stream;
    struct a733_dma_pool_config config;
    struct a733_dma_pool_session pool;
    struct dma_service_i2s_irq irq;
    StaticTask_t task;StackType_t stack[2048];TaskHandle_t handle;
    atomic_uint failed,ready;
    struct os_audio_stream_config client;
    unsigned period_frames;
    /* Runs: the client advances requested to start and sets halted to the
     * current run to stop it; the stream task publishes stopped once the DMA
     * has drained. Both equal: no run is active. */
    atomic_uint requested,halted,stopped;
    /* Ring: the stream task publishes written after the period's slot and
     * descriptor; the client publishes released. Each run's first period
     * takes slot 0: the stream task keeps that run's first index in first,
     * the client in base. read is the client's own. */
    _Atomic uint64_t written,released;
    uint64_t read,base;
    struct os_audio_period ring[OS_AUDIO_RING_CAPACITY];
    /* Stream task only. */
    unsigned stage,run,reap;
    uint64_t pending,held,first,frames;
    struct dma_service_reply reply;
} *audio;
static enum os_quiesce_result audio_quiesce_step(struct os_quiesce *);
static struct os_quiesce audio_quiesce={.owner=OS_QUIESCE_AUDIO,
    .order=OS_QUIESCE_DEVICES,.step=audio_quiesce_step};
/* Local stream CLOSE proves capture stopped. The DMA pool's retirement API
 * rejects streams, and the audio parent has no release API. Keep those
 * resources and report the missing retirement proof instead of DONE. */
enum { AUDIO_RETURN_POOL=1u, AUDIO_RETURN_PARENT=2u };
static int return_retained(void)
{
    (void)os_quiesce_fault(&audio_quiesce,SUNSTONE_CRUMB_FAIL_SITE(BOARD),
        AUDIO_RETURN_POOL|AUDIO_RETURN_PARENT,SUNSTONE_CRUMB_HERE());
    return 0;
}
static enum os_quiesce_result audio_quiesce_step(struct os_quiesce *q)
{
    if(os_quiesce_result(q)==OS_QUIESCE_DONE)return OS_QUIESCE_DONE;
    if(audio&&audio->handle)xTaskNotifyGive(audio->handle);
    return os_quiesce_result(q);
}
static int live(void){return board_live(NULL)&&!atomic_load_explicit(&audio->failed,memory_order_acquire);}
static void fail(void)
{
    atomic_store_explicit(&audio->failed,1,memory_order_release);
    if(!os_quiesce_fault(&audio_quiesce,SUNSTONE_CRUMB_FAIL_SITE(BOARD),
        0x100u,SUNSTONE_CRUMB_HERE()))board_fail(NULL);
}
static _Noreturn void dma_fatal(void *unused)
{
    (void)unused;fail();
    if(os_cpu_return_failing()||os_quiesce_requested(&audio_quiesce))
        for(;;)vTaskSuspend(NULL);
    platform_panic("OS retained DMA",1);
}
/* Before the parent is ready only the client task enters the backend; then
 * only the DMA worker does (apps/i2s/main.c). */
static int worker(void)
{return live()&&xTaskGetCurrentTaskHandle()==
    (atomic_load_explicit(&audio->ready,memory_order_acquire)?audio->pool.service.worker:audio->handle);}
static int now(void *v,uint64_t *out)
{return v==audio&&worker()&&a733_counter_clock_now(&providers.shared.counter,out);}
static int enter(void *v,uint64_t d)
{return v==audio&&worker()&&a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,d);}
static void leave(void *v)
{configASSERT(v==audio);a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);}
static int mapped(uintptr_t a,int write)
{
    static const struct a733_mapping_rule readable={A733_MAPPING_READ,1,{{A733_MAIR_DEVICE_NGNRNE,A733_SH_ANY}},0,1},
        writable={A733_MAPPING_WRITE,1,{{A733_MAIR_DEVICE_NGNRNE,A733_SH_ANY}},0,1};
    return a733_mapping_check(aarch64_device_translate,a,a,write?&writable:&readable);
}
static int read_reg(void *v,uintptr_t a,uint32_t *out)
{return v==audio&&worker()&&mapped(a,0)&&a733_audio_native_read(NULL,a,out)==1;}
static int write_reg(void *v,uintptr_t a,uint32_t value)
{return v==audio&&worker()&&mapped(a,1)&&a733_audio_native_write(NULL,a,value)==1;}
/* One copied request at a time; 1 OK, 0 not yet answered or BUSY, -1 refused. */
static int request(enum dma_service_operation operation,uint64_t timeout,uint64_t period)
{
    if(!audio->pending){
        struct dma_service_request r={.operation=operation,.job=0,.timeout=timeout,.period_ticket=period};
        if(!dma_service_send(&audio->pool.service,&r,0,&audio->pending))return 0;
    }
    if(!dma_service_collect(&audio->pool.service,audio->pending,&audio->reply,0))return 0;
    audio->pending=0;
    if(audio->reply.result==DMA_SERVICE_BUSY)return 0;
    return audio->reply.result==DMA_SERVICE_OK?1:-1;
}
/* Client task only: one bounded step. 0 continue, -1 failed. */
static int step(void)
{
    int r;
    const int returning=os_quiesce_requested(&audio_quiesce);
    if(returning){
        atomic_store_explicit(&audio->halted,atomic_load_explicit(&audio->requested,memory_order_acquire),memory_order_release);
        /* No stream exists during parent/pool setup. Do not start one only
         * to stop it; the retained parents still prevent terminal admission. */
        if(audio->stage==PREPARE||audio->stage==POOL)return return_retained();
    }
    switch(audio->stage){
    case PREPARE:{
        enum a733_i2s_resource_result p=a733_audio_parent_poll(&audio->parent);
        if(p==A733_I2S_RESOURCE_BUSY||p==A733_I2S_RESOURCE_PENDING)return 0;
        if(p!=A733_I2S_RESOURCE_READY||!a733_audio_parent_ready(&audio->parent))return -1;
        /* OPEN is queued only after this publication; from here only the DMA
         * worker enters the backend. */
        atomic_store_explicit(&audio->ready,1,memory_order_release);audio->stage=POOL;return 0;}
    case POOL:{
        unsigned state=atomic_load_explicit(&audio->pool.state,memory_order_acquire);
        if(state==A733_DMA_POOL_FAILED)return -1;
        if(state==A733_DMA_POOL_READY)audio->stage=IDLE;
        return 0;}
    case IDLE:{
        if(returning){if(audio->reap){audio->stage=REAP;return 0;}return return_retained();}
        unsigned run=atomic_load_explicit(&audio->requested,memory_order_acquire);
        if(run==audio->run)return 0;
        audio->run=run;
        /* Stopped before the stream task saw the start: nothing to capture. */
        if(atomic_load_explicit(&audio->halted,memory_order_acquire)==run){
            atomic_store_explicit(&audio->stopped,run,memory_order_release);return 0;}
        audio->first=atomic_load_explicit(&audio->written,memory_order_relaxed);
        audio->stage=audio->reap?REAP:OPEN;return 0;}
    case OPEN:
        if(returning&&!audio->pending)return return_retained();
        r=request(DMA_SERVICE_OPEN,100000,0);if(r<0)return -1;if(!r)return 0;
        audio->stage=OPENING;return 0;
    case OPENING:
        r=request(DMA_SERVICE_STATUS,0,0);if(r<0)return -1;if(!r)return 0;
        if(audio->reply.phase==DMA_SERVICE_OPENING)return 0;
        if(audio->reply.phase!=DMA_SERVICE_READY)return -1;
        audio->frames=0;audio->held=0;audio->stage=SUBMIT;return 0;
    case SUBMIT:
        if(returning&&!audio->pending){audio->stage=CLOSE;return 0;}
        /* Period timeout in DMA counter ticks: one second. */
        r=request(DMA_SERVICE_SUBMIT,A733_SMP_COUNTER_HZ,0);if(r<0)return -1;if(!r)return 0;
        audio->stage=GET;return 0;
    case GET:{
        if(!audio->pending&&atomic_load_explicit(&audio->halted,memory_order_acquire)==audio->run){
            audio->stage=CANCEL;return 0;}
        r=request(DMA_SERVICE_PERIOD_GET,0,0);if(r<0)return -1;
        /* No completed period: a stream that stopped by itself has failed. */
        if(!r){if(!audio->pending&&audio->reply.result==DMA_SERVICE_BUSY)audio->stage=CHECK;return 0;}
        struct a733_i2s_period p=audio->reply.period;
        if(p.kind!=(audio->client.cyclic?A733_I2S_CAPTURED_COPY:A733_I2S_CAPTURED)||
           p.slot>=PERIODS||p.frames!=audio->period_frames||!p.ticket||
           p.ticket<=audio->held||p.first_frame!=audio->frames||audio->frames>UINT64_MAX-p.frames||
           p.observed_before_us>p.observed_after_us)return -1;
        audio->frames+=p.frames;audio->held=p.ticket;audio->stage=PUT;
        /* The next slot is free once the client has released the period that
         * last used it; otherwise this period is dropped. */
        uint64_t n=atomic_load_explicit(&audio->written,memory_order_relaxed);
        if(n-atomic_load_explicit(&audio->released,memory_order_acquire)>=audio->client.periods)return 0;
        uint8_t *slot=audio->client.buffer+(size_t)((n-audio->first)%audio->client.periods)*audio->client.period_bytes;
        if(audio->client.cyclic){
            if(!dma_service_copy_period(&audio->pool.service,0,p.ticket,slot,audio->client.period_bytes))return -1;
        }else memcpy(slot,audio->pool.stream_mappings[p.slot].cpu,audio->client.period_bytes);
        audio->ring[(n-audio->first)%audio->client.periods]=(struct os_audio_period){slot,audio->client.period_bytes,
            p.frames,p.first_frame,p.observed_before_us,p.observed_after_us};
        atomic_store_explicit(&audio->written,n+1,memory_order_release);return 0;}
    case CHECK:
        r=request(DMA_SERVICE_STATUS,0,0);if(r<0)return -1;if(!r)return 0;
        if(audio->reply.phase!=DMA_SERVICE_RUNNING&&audio->reply.phase!=DMA_SERVICE_READY)return -1;
        audio->stage=GET;return 0;
    case PUT:
        r=request(DMA_SERVICE_PERIOD_PUT,0,audio->held);if(r<0)return -1;if(!r)return 0;
        audio->stage=GET;return 0;
    case CANCEL:
        r=request(DMA_SERVICE_CANCEL,0,0);if(r<0)return -1;
        /* BUSY: the stream is no longer running; DRAIN reads its outcome. */
        if(!r&&(audio->pending||audio->reply.result!=DMA_SERVICE_BUSY))return 0;
        audio->stage=DRAIN;return 0;
    case DRAIN:
        r=request(DMA_SERVICE_STATUS,0,0);if(r<0)return -1;if(!r)return 0;
        if(audio->reply.phase==DMA_SERVICE_DRAINING)return 0;
        if(audio->reply.phase==DMA_SERVICE_RUNNING){audio->stage=CANCEL;return 0;}
        if(audio->reply.phase!=DMA_SERVICE_RESULT||audio->reply.outcome!=DMA_SERVICE_CANCELED||
           !audio->reply.holds_buffers)return -1;
        audio->reap=1;audio->stage=IDLE;
        atomic_store_explicit(&audio->stopped,audio->run,memory_order_release);return 0;
    case REAP:
        r=request(DMA_SERVICE_REAP,0,0);if(r<0)return -1;if(!r)return 0;
        audio->reap=0;audio->stage=CLOSE;return 0;
    case CLOSE:
        r=request(DMA_SERVICE_CLOSE,100000,0);if(r<0)return -1;if(!r)return 0;
        audio->stage=CLOSING;return 0;
    case CLOSING:
        r=request(DMA_SERVICE_STATUS,0,0);if(r<0)return -1;if(!r)return 0;
        if(audio->reply.phase==DMA_SERVICE_CLOSING)return 0;
        if(audio->reply.phase!=DMA_SERVICE_CLOSED)return -1;
        if(returning)return return_retained();
        audio->stage=OPEN;return 0;
    default:return -1;
    }
}
/* One bounded pass of the client task. The DMA worker answers each request
 * within its next iteration, so a pass that ends with no unanswered request
 * reports resident progress; a stalled worker or a failed capture does not. */
static void pass(void)
{
    /* A failed capture parks with its request, leases and periods held. */
    if(atomic_load_explicit(&audio->failed,memory_order_acquire)||
       os_quiesce_result(&audio_quiesce)==OS_QUIESCE_FAULT)return;
    if(!live()||step()<0){fail();return;}
    if(!audio->pending)BOARD_PROGRESS(OS_SERVICE_AUDIO);
}
void os_audio_task(void *unused)
{
    (void)unused;
    for(;;){pass();(void)ulTaskNotifyTake(pdTRUE,1);}
}
int os_audio_stream_start(void)
{
    if(!live()||os_quiesce_requested(&audio_quiesce))return -1;
    unsigned run=atomic_load_explicit(&audio->requested,memory_order_relaxed);
    if(atomic_load_explicit(&audio->stopped,memory_order_acquire)!=run)
        return atomic_load_explicit(&audio->halted,memory_order_relaxed)==run?0:1;
    /* No run is active, so written is final: release every earlier period. */
    uint64_t n=atomic_load_explicit(&audio->written,memory_order_acquire);
    audio->read=audio->base=n;atomic_store_explicit(&audio->released,n,memory_order_release);
    atomic_store_explicit(&audio->requested,run+1,memory_order_release);
    return live()?1:-1;
}
int os_audio_stream_read(struct os_audio_period *out)
{
    if(!live()||!out)return -1;
    uint64_t n=atomic_load_explicit(&audio->written,memory_order_acquire);
    if(audio->read==n)return 0;
    *out=audio->ring[(audio->read-audio->base)%audio->client.periods];++audio->read;
    return 1;
}
int os_audio_stream_release(void)
{
    uint64_t n=atomic_load_explicit(&audio->released,memory_order_relaxed);
    if(n==audio->read)return -1;
    atomic_store_explicit(&audio->released,n+1,memory_order_release);return 1;
}
int os_audio_stream_stop(void)
{
    if(!live())return -1;
    unsigned run=atomic_load_explicit(&audio->requested,memory_order_relaxed);
    if(atomic_load_explicit(&audio->stopped,memory_order_acquire)==run)return 1;
    atomic_store_explicit(&audio->halted,run,memory_order_release);return 0;
}
void os_audio_irq(void){dma_service_i2s_irq_handler(&audio->irq);}
int os_audio_task_privileged(void (*entry)(void *),void *context)
{return (!context&&entry==os_audio_task)||a733_dma_pool_task_privileged(entry,context)==pdTRUE;}
/* PB5, PB6 and PB8 join the board pin manager's authority. */
int os_audio_pins(struct a733_pin_handoff *pins)
{
    if(!pins||!os_session_admitted())return 0;
    if(!os_granted(OS_GRANT_AUDIO))return 1; /* the pins stay with the board pin manager */
    if(pins->authorized[A733_PIN_B]&((1u<<5)|(1u<<6)|(1u<<8)))return 0;
    pins->authorized[A733_PIN_B]|=(1u<<5)|(1u<<6)|(1u<<8);return 1;
}
/* Validate the client's configuration without I/O and keep it. The stream
 * profile follows the board's wire with the client's rate, channels and
 * sample size; a733_i2s_plan proves an exact bit-clock divider. */
static int configure(const struct os_audio_stream_config *c)
{
    struct a733_i2s_plan plan;
    if(!c||audio->client.buffer||c->format.reserved[0]||c->format.reserved[1]||c->cyclic>1)return 0;
    const struct a733_i2s_format f={MODULE_HZ,c->format.rate_hz,0,c->format.bits,SLOT_BITS,c->format.channels,0};
    if(!a733_i2s_plan(&f,&plan)||!c->period_bytes||c->period_bytes>OS_AUDIO_PERIOD_CAPACITY||
       c->period_bytes%plan.frame_bytes||c->period_bytes%(plan.dma_width*8)||
       (uint64_t)(c->period_bytes/plan.frame_bytes)*1000000<(uint64_t)OS_AUDIO_MIN_PERIOD_US*c->format.rate_hz||
       c->periods<2||c->periods>OS_AUDIO_RING_CAPACITY||!c->buffer||(uintptr_t)c->buffer%4)return 0;
    uintptr_t start=(uintptr_t)c->buffer;size_t bytes=(size_t)c->periods*c->period_bytes;
    if(start>UINTPTR_MAX-bytes||(start<(uintptr_t)(audio+1)&&(uintptr_t)audio<start+bytes))return 0;
    audio->client=*c;audio->period_frames=c->period_bytes/plan.frame_bytes;audio->stream.stream.format=f;
    return 1;
}
/* The state's pages, zeroed, from the ledger the application preparation set up. The DMA pool session accepts them as
 * its storage and refuses DMA transfers into them, as it does for the image. Serialized boot only: the start list runs
 * before the scheduler. */
static int allocate(void)
{
    if(audio)return 1;
    struct umem *ledger=os_large_umem();
    struct umem_request r={.bytes=(sizeof(*audio)+UMEM_PAGE-1)&~(UMEM_PAGE-1),.align=UMEM_PAGE,
                           .owner=UMEM_OWNER_KERNEL,.flags=UMEM_RAW,.policy=UMEM_WB};
    struct umem_buf b;
    if(!ledger||umem_alloc(ledger,&r,&b)!=UMEM_OK)return 0;
    memset((void *)(uintptr_t)b.base,0,sizeof(*audio));
    if(!a733_dma_pool_session_storage((const void *)(uintptr_t)b.base,(size_t)b.bytes))return 0;
    audio=(struct os_audio_state *)(uintptr_t)b.base;return 1;
}
int os_audio_stream_open(const struct os_audio_stream_config *c)
{
    const struct os_session *s=os_session_admitted();
    if(!s||!board_live(NULL)||!allocate()||audio->handle||!configure(c))return 0;
    if(!os_quiesce_register(&audio_quiesce))return 0;
    const struct os_audio_session *a=&s->audio;
    uint64_t deadline=board.session.deadline_us,owner=a->dma.owner;
    audio->dma=(struct a733_dma_handoff){a->dma,a->dma_controller,a->dma_evidence,a->dma_established,
        a->dma_owned_channels,a->dma_service_channels};
    audio->dma.allowed.deadline_us=deadline;
    audio->handle=xTaskCreateStatic(os_audio_task,"os-audio",2048,NULL,4,audio->stack,&audio->task);
    if(!audio->handle)return 0;
    struct a733_audio_operations backend={.enter=enter,.leave=leave,.now_us=now,.read=read_reg,.write=write_reg};
    struct a733_pin_request pins[3];
    static const unsigned pb[3]={5,6,8};
    for(unsigned i=0;i<3;++i)pins[i]=(struct a733_pin_request){pb[i],
        {a->inherited[i][0],a->inherited[i][1],a->inherited[i][2]},{3,0,0}};
    if(!a733_audio_leases_init(&audio->leases,&providers.shared.clocks,owner,&backend,audio)||
       !a733_audio_parent_init(&audio->parent,&audio->leases.operations,&audio->leases,a->audio_cold,deadline)||
       !a733_i2s_platform_stream_init(&audio->binding,&audio->parent,&providers.shared.pins,&audio->leases,
           owner,pins,3,A733_I2S_RECEIVE))return 0;
    audio->stream.stream=(struct a733_i2s_stream_profile){audio->stream.stream.format,
        a733_i2s_platform_stream_operations(),&audio->binding,A733_I2S_RECEIVE,10000,c->cyclic,A733_I2S_ROUTE_I2S0};
    audio->config=(struct a733_dma_pool_config){&audio->dma,&providers.shared.counter,&providers.shared.clocks,
        &providers.shared.locks,240000,4096,5,1,dma_fatal,audio};
    /* The admitted lifetime chose the handoff's established facts (OS_AUDIO_DMA_ESTABLISHED); the pool takes the same. */
    if(!(os_lifetime()==A733_LIFETIME_RESIDENT?
         a733_dma_pool_session_stream_init(&audio->pool,&audio->config,&audio->stream,PERIODS,c->period_bytes):
         a733_dma_pool_session_stream_supervised_init(&audio->pool,&audio->config,&audio->stream,PERIODS,c->period_bytes)))
        return 0;
    return dma_service_bind(&audio->pool.service,0,audio->handle,1)&&
        dma_service_i2s_irq_bind(&audio->irq,&audio->pool.service,A733_SMP_COUNTER_HZ);
}
