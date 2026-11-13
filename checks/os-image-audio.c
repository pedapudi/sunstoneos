/* Native OS capture and stream state machines against modeled DMA replies,
 * audio-parent readiness and session admission. Provider construction remains
 * at the existing fixture boundary; capture receives the real stream API. */
#include "../apps/os/audio.c"
#include "../apps/os/application.h"
#include "../apps/os/capture.h"
#include "../apps/os/owner-memory.h"
extern uint8_t *ss_capture_buffer;
#define AUDIO_STATE (*audio)
static unsigned event_waits,event_hints,event_available,event_idle;
#define AUDIO_MODEL_REPLY_EVENT() audio_event(audio,DMA_SERVICE_REPLY_EVENT)
#include "os-image-audio-model.h"
enum { CAPTURE_MAX_FRAMES=80000, CAPTURE_BYTES=2*CAPTURE_MAX_FRAMES };
uint8_t os_check_pcm[2][CAPTURE_BYTES];
/* The capture buffer, which admission takes from the ledger (os_capture_memory): readable and writable at EL0. */
static _Alignas(4) uint8_t capture_storage[SS_CAPTURE_HEADROOM+40*OS_AUDIO_PERIOD_CAPACITY];
const size_t ss_capture_bytes=sizeof(capture_storage),ss_capture_capacity=sizeof(capture_storage)-SS_CAPTURE_HEADROOM;
void *os_owner_memory(uint64_t bytes,uint64_t align,unsigned attributes)
{return bytes==sizeof(capture_storage)&&!align&&attributes==OS_MEMORY_EL0_WRITE?capture_storage:NULL;}
static const struct ss_manifest manifest={.audio={16000,1,16,16000,CAPTURE_MAX_FRAMES}};
const struct ss_application ss_application={.manifest=&manifest};
static struct os_session session;
static const struct ss_grant grant={.devices=SS_DEVICE_AUDIO_I2S};
const struct ss_grant *ss_grant(void){return &grant;}
int ss_live(void){return board_up&&!board_failed?1:-1;}
const struct os_session *os_session_admitted(void){return &session;}
int os_granted(unsigned mask){return mask&&(session.granted&mask)==mask;}
/* The provider, pin and DMA-pool constructors are outside this model, as in
 * the earlier stream fixture. Preserve the actual client's validation and
 * stream state machine while supplying their configured provider boundary. */
static unsigned stream_opens;
int __wrap_os_audio_stream_open(const struct os_audio_stream_config *c){++stream_opens;return configure(c);}
static unsigned model_failing,quiesce_notes;
static unsigned service_retired,pool_retired,parent_retired,retire_block;
static unsigned lock_attempts,lock_late,lock_counter_fail,return_deadline_pending,lock_delay_ticks;
static uint64_t lock_deadline;
static TaskHandle_t mutex_owner,current_task;
static unsigned mask_mode,mask_attempts,mask_reads,mask_publications,late_irqs;
int sunstone_deadline_register(void){return 1;}
int board_service_deadline_register(unsigned service){check(service==OS_SERVICE_AUDIO,220);return 1;}
int board_service_blocked_until(unsigned service,uint64_t until)
{check(service==OS_SERVICE_AUDIO&&until<=board.session.deadline_us&&until>model_ticks/24,221);return 1;}
uint64_t a733_audio_parent_sample_at(const struct a733_audio_parent *p){(void)p;return 0;}
int sunstone_deadline_wait_until(uint64_t until)
{
    if(mode!=19&&mode!=20&&mode!=21){check(until>model_ticks/24&&!model_critical,222);++model_delays;model_ticks=until*24;return 1;}
    ++event_waits;
    if(!event_idle){
        check(audio->stage==IDLE&&until==board.session.deadline_us&&!sent&&!event_hints,223);
        /* One second of scheduler ticks produces no owner return. The
         * admitted wait stays armed until this application request. */
        model_ticks=UINT64_C(1000127)*24;event_idle=1;
        check(os_audio_stream_start()==1,224);event_hints=0;return 0;
    }
    check(until>model_ticks/24&&until<=model_ticks/24+1000000,225);
    if(event_hints){event_hints=0;return 0;} /* Old reply hints carry no device progress. */
    if(mode==21){check(sent==4&&audio->await_event&&!produced,232);model_ticks=until*24;return 1;}
    if(!event_available){
        check(sent==(mode==20?5u:4u)&&audio->await_event&&!produced&&!puts_done,226);
        /* A late DMA publication must survive a reply hint that was already
         * consumed. It authorizes the next GET and no earlier resubmission. */
        event_available=1;audio_event(audio,DMA_SERVICE_PROGRESS_EVENT);event_hints=0;return 0;
    }
    check(produced==1&&puts_done==1&&sent==(mode==20?8u:7u)&&event_waits>=4,227);
    os_check_result=1;__asm__ volatile("brk #0");for(;;){}
}
void vTaskSuspend(TaskHandle_t task){
    (void)task;
    check(mode==21&&atomic_load(&audio->failed)&&board_failed&&sent==4&&model_ticks/24==2000127,228);
    os_check_result=1;__asm__ volatile("brk #0");for(;;){}
}
uint32_t ulTaskGenericNotifyTake(UBaseType_t index,BaseType_t clear,TickType_t ticks)
{(void)index;(void)clear;(void)ticks;check(0,229);return 0;}
void vTaskEnterCritical(void){check(!model_critical,208);model_critical=1;}
void vTaskExitCritical(void){check(model_critical&&audio->irq.masked,209);model_critical=0;++mask_publications;}
int gicv3_spi_mask(const struct gicv3_spi *spi)
{check(spi==&audio->irq.spi,201);++mask_attempts;return mask_mode==1?0:1;}
int gicv3_spi_masked(const struct gicv3_spi *spi)
{
    check(spi==&audio->irq.spi&&!model_critical,202);++mask_reads;
    if(mask_mode==3&&mask_reads==1)return 0;
    if(mask_mode==4)return 0;
    /* A late notifier still owns the plain masked field under the scheduler
     * critical section, even after SERVICE_RETIRING closes its service call. */
    if(mask_mode==5){check(!model_critical,210);model_critical=1;audio->irq.masked=1;model_critical=0;++late_irqs;}
    return mask_mode==2?-1:1;
}
volatile uint64_t ullPortInterruptNestings[8],ullCriticalNestings[8];
TaskHandle_t xTaskGetCurrentTaskHandle(void){return current_task?current_task:audio->handle;}
BaseType_t xTaskGetSchedulerState(void){return taskSCHEDULER_RUNNING;}
TaskHandle_t xQueueGetMutexHolder(QueueHandle_t q){(void)q;return mutex_owner;}
int a733_counter_clock_now(void *cookie,uint64_t *out)
{check(cookie==&providers.shared.counter,192);if(atomic_load(&providers.shared.counter.failed))return 0;*out=model_ticks/24;return 1;}
int a733_platform_lock_enter(struct a733_platform_locks *locks,enum a733_platform_lock which,uint64_t d)
{
    check(locks==&providers.shared.locks&&which==A733_PLATFORM_REGISTER_LOCK,193);
    ++lock_attempts;lock_deadline=d;
    if(lock_counter_fail)atomic_store(&providers.shared.counter.failed,1);
    if(lock_late)model_ticks=(d+1)*24;
    if(lock_delay_ticks){model_ticks+=(uint64_t)lock_delay_ticks*24000000/configTICK_RATE_HZ;return model_ticks/24<d;}
    return 0; /* Ordinary contention or an already released late acquisition. */
}
int os_cpu_return_requested(void){return model_failing!=0;}
int os_cpu_return_failing(void){return model_failing!=0;}
uint64_t os_cpu_return_deadline_us(void){return return_deadline_pending?0:3000000;}
void os_quiesce_complete(struct os_quiesce *q)
{check(service_retired&&pool_retired&&parent_retired&&!audio->pending&&!held_ticket&&phase==DMA_SERVICE_CLOSED,182);atomic_store(&q->result,OS_QUIESCE_DONE);}
int a733_dma_pool_session_return_limit(struct a733_dma_pool_session *s,uint64_t d)
{check(s==&audio->pool&&d==3000000,183);return 1;}
int dma_service_retired(const struct dma_service *s)
{check(s==&audio->pool.service,184);return service_retired;}
int dma_service_quiesce(struct dma_service *s)
{
    check(s==&audio->pool.service&&!audio->pending&&!held_ticket&&phase==DMA_SERVICE_CLOSED,185);
    if(retire_block==1)return -1;
    service_retired=1;return 1;
}
int a733_dma_pool_session_retire(struct a733_dma_pool_session *s)
{
    check(s==&audio->pool&&service_retired,186);
    if(retire_block==2)return 0;
    if(retire_block==3)return -1;
    pool_retired=1;return 1;
}
enum a733_i2s_resource_result a733_audio_parent_retire(struct a733_audio_parent *p,uint64_t d)
{
    check(p==&audio->parent&&service_retired&&pool_retired&&d==3000000,187);
    if(retire_block==4)return A733_I2S_RESOURCE_PENDING;
    if(retire_block==5)return A733_I2S_RESOURCE_RETAINED;
    parent_retired=1;return A733_I2S_RESOURCE_READY;
}
int os_quiesce_register(struct os_quiesce *q){q->registered=1;return 1;}
int os_quiesce_requested(const struct os_quiesce *q){return atomic_load(&q->requested)!=0;}
enum os_quiesce_result os_quiesce_result(const struct os_quiesce *q){return atomic_load(&q->result);}
int os_quiesce_fault(struct os_quiesce *q,uint32_t site,uint32_t value,const void *caller)
{
    if(!model_failing&&!os_quiesce_requested(q))return 0;
    if(!atomic_exchange(&q->fault_state,2)){q->site=site;q->value=value;q->caller=caller;++quiesce_notes;}
    atomic_store(&q->result,OS_QUIESCE_FAULT);return 1;
}
BaseType_t xTaskGenericNotify(TaskHandle_t task,UBaseType_t index,uint32_t value,eNotifyAction action,uint32_t *previous)
{(void)value;check(task==audio->handle&&!index&&action==eIncrement&&!previous,181);++event_hints;return pdPASS;}
/* Mode 5: candidate rates and whether the stream accepts each one for
 * 16-bit mono capture with 4,096-byte periods; the runner compares them with
 * the vendor's table. */
const uint32_t os_check_rates[]={7999,8000,11025,12000,16000,22050,24000,32000,44100,48000,
    64000,88200,96000,128000,176400,192000,192001,384000};
uint8_t os_check_accepted[sizeof(os_check_rates)/sizeof(os_check_rates[0])];
static _Alignas(8) uint8_t ring_buffer[OS_AUDIO_RING_CAPACITY*OS_AUDIO_PERIOD_CAPACITY];

static void reset(void)
{
    /* The stream's state, which the image takes from the ledger at its first open (outside this model). */
    static struct os_audio_state state;
    audio=&state;memset(audio,0,sizeof(*audio));
    atomic_store(&audio_quiesce.requested,0);atomic_store(&audio_quiesce.result,OS_QUIESCE_PENDING);
    atomic_store(&audio_quiesce.fault_state,0);model_failing=quiesce_notes=0;
    model_ticks=0;service_retired=pool_retired=parent_retired=retire_block=0;
    event_waits=event_hints=event_available=event_idle=0;board.session.deadline_us=60000000;
    lock_attempts=lock_late=lock_counter_fail=return_deadline_pending=lock_delay_ticks=0;lock_deadline=0;mutex_owner=current_task=NULL;
    mask_mode=mask_attempts=mask_reads=mask_publications=late_irqs=model_critical=model_delays=0;
    memset(&providers,0,sizeof(providers));audio->handle=(TaskHandle_t)(uintptr_t)1;
    phase=DMA_SERVICE_CLOSED;produced=puts_done=reaps=closes=opens=silent=logged=0;
    sent=held_ticket=0;board_up=1;board_failed=0;parent_polls=0;
    for(unsigned i=0;i<PERIODS;++i)audio->pool.stream_mappings[i].cpu=memory[i];
    atomic_store(&audio->pool.state,A733_DMA_POOL_READY);
    check(os_capture_memory()==1&&ss_capture_buffer==capture_storage,161);
}
static struct os_audio_stream_config config(uint32_t rate,unsigned channels,unsigned bits,
    uint32_t period_bytes,uint32_t periods)
{return (struct os_audio_stream_config){{rate,(uint8_t)channels,(uint8_t)bits,{0}},period_bytes,periods,ring_buffer,0};}
static unsigned count(char op){unsigned n=0;for(unsigned i=0;i<logged;++i)n+=log[i]==op;return n;}
/* Stream task steps and adapter requests in turn, until generation g is
 * captured: 1, or -1 once the adapter refuses. */
static int capture_run(unsigned g,void **pcm,size_t *bytes)
{
    for(unsigned n=0;n<2000;++n){
        int r=ss_capture(g,pcm,bytes);if(r)return r;
        if(step()<0){fail();}
    }
    return 0;
}
static void check_pcm(unsigned g,unsigned frames,const void *data,size_t bytes)
{
    const uint8_t *pcm=data;
    check(bytes==2*frames&&pcm==ss_capture_buffer+SS_CAPTURE_HEADROOM&&audio->client.buffer==pcm,30);
    for(unsigned i=0;i<SS_CAPTURE_HEADROOM;++i)check(ss_capture_buffer[i]==0xa5,31);
    for(unsigned i=0;i<2*frames;++i)check(pcm[i]==sample(g,i/4096,i%4096),32);
    memcpy(os_check_pcm[g-1],pcm,bytes);
}
/* One run of a stream configuration through the generic interface: read and
 * release 2 * periods periods, check their place, order, timestamps and
 * bytes, stop, and start a second run that begins at slot 0 again. */
static void delivery(struct os_audio_stream_config c,unsigned frame)
{
    reset();check(configure(&c),100);
    model_bytes=c.period_bytes;model_frame=frame;
    unsigned frames=c.period_bytes/frame;
    struct os_audio_period p;
    for(unsigned run=1;run<=2;++run){
        check(os_audio_stream_start()==1,101);
        for(unsigned k=0;k<2*c.periods;++k){
            unsigned n=0;int r;
            while(!(r=os_audio_stream_read(&p))){check(step()==0&&++n<20,102);}
            check(r==1&&p.data==c.buffer+(size_t)(k%c.periods)*c.period_bytes&&p.bytes==c.period_bytes&&
                p.frames==frames&&p.first_frame==(uint64_t)k*frames&&
                p.observed_before_us==model_before(k)&&p.observed_after_us==model_after(k),103);
            for(unsigned i=0;i<c.period_bytes;++i)check(p.data[i]==sample(run,k,i),104);
            check(os_audio_stream_release()==1&&os_audio_stream_release()==-1,105);
        }
        check(os_audio_stream_stop()==0,106);
        unsigned n=0;
        while(os_audio_stream_stop()!=1)check(step()==0&&++n<20,107);
        check(phase==DMA_SERVICE_RESULT&&!board_failed&&opens==run,108);
    }
    check(reaps==1&&closes==1&&count('G')==count('P'),109);
}
void os_check_run(unsigned selected,unsigned samples)
{
    /* Mode 12 is mode 4 in the supervised lifetime; every other mode runs in the resident one. */
    if(selected==12){selected=4;model_lifetime=A733_LIFETIME_SUPERVISED;}
    mode=selected;reset();
    void *pcm;size_t bytes;
    if(mode==19||mode==20||mode==21){
        struct os_audio_stream_config c=config(16000,1,16,4096,4);
        check(configure(&c),230);audio->stage=IDLE;
        os_audio_task(NULL);check(0,231);
    }
    if(mode==13){
        /* Request return at each client boundary, including a held period.
         * Every opened stream drains and closes, with no replacement open.
         * Parent shutdown follows the final worker and pool publications. */
        for(unsigned cyclic=0;cyclic<2;++cyclic)for(unsigned before=0;before<40;++before){
            reset();struct os_audio_stream_config c=config(16000,1,16,4096,4);
            c.cyclic=cyclic;
            check(configure(&c)&&os_audio_stream_start()==1,160);
            for(unsigned i=0;i<before;++i)check(step()==0,161);
            unsigned opened=opens;
            model_failing=1;atomic_store(&audio_quiesce.requested,1);
            for(unsigned i=0;i<40&&os_quiesce_result(&audio_quiesce)==OS_QUIESCE_PENDING;++i)pass();
            check(os_quiesce_result(&audio_quiesce)==OS_QUIESCE_DONE&&!quiesce_notes&&!board_failed,162);
            check(service_retired&&pool_retired&&parent_retired,163);
            check(opens==opened&&(!opened||phase==DMA_SERVICE_CLOSED)&&!held_ticket,164);
            check(os_audio_stream_start()==-1,165);
        }
        os_check_result=1;return;
    }
    if(mode==14){
        /* An outstanding request is collected before cancellation. A late
         * worker must not be reported drained or treated as an early fault. */
        struct os_audio_stream_config c=config(16000,1,16,4096,4);
        check(configure(&c)&&os_audio_stream_start()==1,170);
        for(unsigned i=0;audio->stage!=GET&&i<20;++i)check(step()==0,171);
        check(audio->stage==GET,172);silent=1;check(step()==0&&audio->pending,173);
        model_failing=1;atomic_store(&audio_quiesce.requested,1);
        for(unsigned i=0;i<10;++i)pass();
        check(os_quiesce_result(&audio_quiesce)==OS_QUIESCE_PENDING&&!board_failed&&audio->pending,174);
        silent=0;
        for(unsigned i=0;i<40&&os_quiesce_result(&audio_quiesce)==OS_QUIESCE_PENDING;++i)pass();
        check(os_quiesce_result(&audio_quiesce)==OS_QUIESCE_DONE&&phase==DMA_SERVICE_CLOSED&&!held_ticket&&!board_failed,175);
        os_check_result=1;return;
    }
    if(mode==15){
        /* A device failure can precede this owner's ordered step. */
        model_failing=1;fail();
        check(atomic_load(&audio->failed)&&os_quiesce_result(&audio_quiesce)==OS_QUIESCE_FAULT&&!board_failed,180);
        os_check_result=1;return;
    }
    if(mode==16){
        for(unsigned blocked=1;blocked<=5;++blocked){
            reset();retire_block=blocked;model_failing=1;atomic_store(&audio_quiesce.requested,1);
            for(unsigned i=0;i<20;++i)pass();
            check(!parent_retired&&!board_failed,188);
            if(blocked==2||blocked==4){
                check(os_quiesce_result(&audio_quiesce)==OS_QUIESCE_PENDING,189);
                model_ticks=3000000u*24u;pass();
            }
            check(os_quiesce_result(&audio_quiesce)==OS_QUIESCE_FAULT&&quiesce_notes==1,190);
            unsigned before=logged;for(unsigned i=0;i<4;++i)pass();check(logged==before,191);
        }
        os_check_result=1;return;
    }
    if(mode==17){
        __asm__ volatile("msr daifclr, #2");
        providers.shared.locks.initialized=1;
        const uint64_t tick=(1000000u+configTICK_RATE_HZ-1u)/configTICK_RATE_HZ;
        check(!enter(audio,60000000)&&lock_attempts==1&&lock_deadline==tick&&!atomic_load(&audio->failed),194);
        model_failing=1;return_deadline_pending=1;
        check(!enter(audio,60000000)&&lock_attempts==1&&!atomic_load(&audio->failed),195);
        return_deadline_pending=0;model_ticks=(3000000-10)*24;lock_late=1;
        check(!enter(audio,60000000)&&lock_attempts==2&&lock_deadline==3000000&&!mutex_owner,196);
        model_ticks=1;lock_late=0;atomic_store(&providers.shared.locks.retained,1u<<A733_PLATFORM_REGISTER_LOCK);
        check(!enter(audio,60000000)&&lock_attempts==2&&os_quiesce_result(&audio_quiesce)==OS_QUIESCE_FAULT,197);
        reset();providers.shared.locks.initialized=1;model_failing=1;mutex_owner=audio->handle;
        check(!enter(audio,60000000)&&!lock_attempts&&os_quiesce_result(&audio_quiesce)==OS_QUIESCE_FAULT,198);
        reset();providers.shared.locks.initialized=1;model_failing=1;lock_counter_fail=1;
        uint64_t t;
        check(!enter(audio,60000000)&&lock_attempts==1&&!now(audio,&t),199);
        reset();providers.shared.locks.initialized=1;atomic_store(&audio->ready,1);
        audio->pool.service.worker=current_task=(TaskHandle_t)(uintptr_t)2;lock_delay_ticks=2;
        check(enter(audio,10000)==1&&lock_deadline==10000&&model_ticks/24==2*tick&&
              !atomic_load(&audio->failed),211);
        os_check_result=1;return;
    }
    if(mode==18){
        __asm__ volatile("msr daifclr, #2");
        for(unsigned m=0;m<4;++m){
            reset();mask_mode=m;audio->pool.service.worker=audio->handle;
            int r=mask(&audio->irq);
            unsigned success=m==0||m==3;
            check(r==(success?1:-1)&&mask_attempts==1&&audio->irq.masked==success&&mask_publications==success,203);
            check(mask_reads==(m==1?0u:m==3?2u:1u)&&model_delays==(m==3),204);
        }
        reset();audio->pool.service.worker=audio->handle;model_ticks=3000000u*24u;
        check(mask(&audio->irq)==-1&&!mask_attempts&&!mask_reads,205);
        reset();audio->pool.service.worker=audio->handle;model_ticks=(3000000u-10u)*24u;mask_mode=4;
        check(mask(&audio->irq)==-1&&mask_attempts==1&&mask_reads==1&&!audio->irq.masked,206);
        reset();audio->pool.service.worker=audio->handle;return_deadline_pending=1;
        check(mask(&audio->irq)==-1&&!mask_attempts,212);
        reset();audio->pool.service.worker=audio->handle;mask_mode=5;
        check(mask(&audio->irq)==1&&late_irqs==1&&mask_publications==1,213);
        os_check_result=1;return;
    }
    if(mode==11){
        /* A session that does not grant the I2S service: the start entry succeeds without opening the stream, which is
         * the only path to the pins, the clocks, the DMA channel and the interrupt. The device bit and the record are the
         * grant (apps/os/session.c); neither is present. */
        session.audio.capture_samples=samples;session.granted=0;
        stream_opens=0;check(os_capture_start()==1&&stream_opens==0,150);
        check(ss_capture(1,&pcm,&bytes)<0,151);
        session.granted=OS_GRANT_AUDIO;
        check(os_capture_start()==1&&stream_opens==1,152);
        os_check_result=1;return;
    }
    if(mode==5){
        /* Rates: exactly those whose bit clock divides 24.576 MHz. */
        for(unsigned i=0;i<sizeof(os_check_rates)/sizeof(os_check_rates[0]);++i){
            struct os_audio_stream_config c=config(os_check_rates[i],1,16,4096,4);
            reset();os_check_accepted[i]=(uint8_t)configure(&c);
        }
        /* Formats, periods and buffers the stream refuses. */
        struct os_audio_stream_config bad[]={config(16000,3,16,4096,4),config(16000,1,24,4096,4),
            config(16000,0,16,4096,4),config(16000,1,16,0,4),config(16000,1,16,4100,4),
            config(16000,1,16,4098,4),config(16000,1,16,1000,4),config(48000,2,16,1002,4),
            config(48000,2,32,4088,4),config(16000,1,16,4096,1),
            config(16000,1,16,4096,OS_AUDIO_RING_CAPACITY+1),config(16000,1,16,4096,4),
            config(16000,1,16,4096,4),config(16000,1,16,4096,4),config(44100,2,16,4096,4),
            /* Shorter than 8 ms: 7 ms at 8 kHz, 5.3 ms at 192 kHz. */
            config(8000,1,16,112,2),config(192000,2,32,4096,8),config(192000,1,32,4096,8)};
        bad[11].buffer=NULL;bad[12].buffer=ring_buffer+2;bad[13].format.reserved[1]=1;
        for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i){reset();check(!configure(&bad[i]),90+i);}
        struct os_audio_stream_config inside=config(16000,1,16,4096,2);
        inside.buffer=(uint8_t *)audio;reset();check(!configure(&inside),110);
        /* Valid edges: the capacity, the most slots, small stereo periods. */
        struct os_audio_stream_config good[]={config(16000,1,16,4096,OS_AUDIO_RING_CAPACITY),
            config(8000,1,16,128,2),config(192000,1,16,4096,8),config(96000,2,16,4096,3)};
        for(unsigned i=0;i<sizeof(good)/sizeof(good[0]);++i){reset();check(configure(&good[i]),111+i);}
        /* Configured once only. */
        check(!configure(&good[0]),115);
        os_check_result=1;return;
    }
    if(mode==6){
        /* Rates and period sizes through the generic interface. */
        delivery(config(8000,1,16,512,2),2);
        delivery(config(16000,1,16,4096,4),2);
        delivery(config(16000,1,32,2048,5),4);
        delivery(config(48000,2,16,2048,3),4);
        delivery(config(96000,1,32,4096,6),4);
        delivery(config(192000,1,16,4096,8),2);
        os_check_result=1;return;
    }
    if(mode==8||mode==9||mode==10){
        struct os_audio_stream_config c=config(16000,1,16,4096,4);c.cyclic=1;
        if(mode==8){delivery(c,2);os_check_result=1;return;}
        check(configure(&c)&&os_audio_stream_start()==1,140);
        int r=0;for(unsigned i=0;i<20&&r==0;++i)r=step();
        check(r<0&&held_ticket&&!puts_done&&!reaps&&!closes,141);
        fail();struct os_audio_period p;check(os_audio_stream_read(&p)<0,142);
        os_check_result=1;return;
    }
    if(mode==7){
        /* A full ring drops periods; the next delivered period shows the
         * gap. Release follows read order; start and stop handshakes. */
        struct os_audio_stream_config c=config(16000,1,16,4096,2);
        check(configure(&c),120);
        struct os_audio_period p;
        check(os_audio_stream_release()==-1&&os_audio_stream_read(&p)==0&&os_audio_stream_stop()==1,121);
        check(os_audio_stream_start()==1&&os_audio_stream_start()==1,122);
        for(unsigned n=0;n<40&&produced<6;++n)check(step()==0,123);
        check(produced==6&&atomic_load(&audio->written)==2,124);
        check(os_audio_stream_read(&p)==1&&p.first_frame==0&&os_audio_stream_read(&p)==1&&p.first_frame==2048,125);
        check(os_audio_stream_read(&p)==0&&os_audio_stream_release()==1,126);
        while(produced<8)check(step()==0,127);
        unsigned gap=0;int r;
        while(!(r=os_audio_stream_read(&p)))check(step()==0&&++gap<20,128);
        check(r==1&&p.first_frame>=6*2048&&p.first_frame%2048==0&&p.data==ring_buffer,129);
        for(unsigned i=0;i<4096;++i)check(p.data[i]==sample(1,(unsigned)(p.first_frame/2048),i),130);
        /* A stop in progress refuses a new start until the DMA drained. */
        check(os_audio_stream_stop()==0&&os_audio_stream_start()==0,131);
        while(os_audio_stream_stop()!=1)check(step()==0,132);
        check(!board_failed&&count('C')==1,133);
        /* Stopped before the stream task saw the start: no DMA request. */
        unsigned before=logged;
        check(os_audio_stream_start()==1&&os_audio_stream_stop()==0,134);
        for(unsigned n=0;n<5;++n)check(step()==0,135);
        check(os_audio_stream_stop()==1&&logged==before,136);
        os_check_result=1;return;
    }
    session.audio.capture_samples=samples;session.granted=OS_GRANT_AUDIO;
    memset(ss_capture_buffer,0xa5,SS_CAPTURE_HEADROOM);
    check(os_capture_start()&&!os_capture_start(),20);
    if(mode==4){
        /* Each task pass without an unanswered request reports progress; a
         * pass waiting for the worker or after a failure reports none. */
        for(unsigned n=0;n<10;++n)pass();
        check(audio->stage==IDLE&&reports==REPORTS(10),70);
        check(!ss_capture(1,&pcm,&bytes),71);
        pass();check(audio->stage==OPEN&&!audio->pending&&reports==REPORTS(11),72);
        silent=1;pass();pass();
        check(audio->stage==OPEN&&audio->pending&&reports==REPORTS(11),73);
        silent=0;pass();
        check(audio->stage==OPENING&&!audio->pending&&reports==REPORTS(12),74);
        board_up=0;pass();pass();
        check(board_failed&&atomic_load(&audio->failed)&&reports==REPORTS(12),75);
        os_check_result=1;return;
    }
    if(mode==3){
        check(os_audio_client_admit(16000)&&os_audio_client_admit(CAPTURE_MAX_FRAMES)&&
            !os_audio_client_admit(15999)&&!os_audio_client_admit(CAPTURE_MAX_FRAMES+1),43);
        check(ss_capture(0,&pcm,&bytes)<0&&ss_capture(1,NULL,&bytes)<0,40);
        check(!ss_capture(2,&pcm,&bytes)&&ss_capture(1,&pcm,&bytes)<0,41);
        board_up=0;check(ss_capture(2,&pcm,&bytes)<0,42);
        os_check_result=1;return;
    }
    /* The parent becomes ready on the third poll; nothing is requested yet. */
    for(unsigned n=0;n<10;++n)check(step()==0,50);
    check(atomic_load(&audio->ready)&&audio->stage==IDLE&&!logged,51);
    int r=capture_run(1,&pcm,&bytes);
    if(mode){
        /* A discontinuous period or a stream that stopped by itself refuses
         * the capture with the stream and its leases held. */
        check(r<0&&board_failed&&!reaps&&!closes&&ss_capture(1,&pcm,&bytes)<0,60+mode);
        os_check_result=1;return;
    }
    unsigned periods=(2*samples+4095)/4096;
    check(r==1&&produced>=periods&&count('G')==produced&&count('P')==produced&&count('C')==1,53);
    check(!memcmp(log,"OSM",3)&&!memcmp(log+logged-2,"CS",2)&&!reaps,54);
    check_pcm(1,samples,pcm,bytes);
    /* The next generation reaps, closes and opens again before it captures;
     * the first generation's request is then refused. */
    unsigned first=logged;
    check(!ss_capture(2,&pcm,&bytes)&&capture_run(2,&pcm,&bytes)==1&&reaps==1&&closes==1,55);
    check(!memcmp(log+first,"RLSOSM",6)&&!memcmp(log+logged-2,"CS",2),56);
    check_pcm(2,samples,pcm,bytes);
    check(ss_capture(1,&pcm,&bytes)<0&&!board_failed,57);
    os_check_result=1;
}
