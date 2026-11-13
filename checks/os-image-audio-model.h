/* Modeled DMA service replies, audio parent and board for the stream task of
 * apps/os/audio.c. checks/os-image-audio.c uses it with the current capture
 * stream and generic PCM capture. checks/os-image-audio-baseline.c uses an
 * earlier capture client, so both see the same period bytes. Include it after
 * audio.c, which defines PERIODS and the audio object. AUDIO_STATE names that
 * object: the current audio.c keeps a pointer to it, an earlier one the object. */
#ifndef AUDIO_STATE
#define AUDIO_STATE audio
#endif
struct board_application board;
struct board_provider providers;
volatile unsigned os_check_result;
static unsigned board_up=1,board_failed,parent_polls,mode,phase,produced,puts_done,reaps,closes,opens,silent,reports;
/* Period payload and frame size of the modeled stream. */
static unsigned model_bytes=4096,model_frame=2;
static uint64_t sent,held_ticket;
static struct dma_service_request queued;
static uint8_t memory[PERIODS][4096];
static uint8_t staging[4096];
static char log[512];static unsigned logged;
int board_live(void *v){(void)v;return board_up;}
static uint64_t model_ticks;
static unsigned model_critical,model_delays;
uint64_t a733_smp_counter(void){return model_ticks;} /* Replay is not selected in this model. */
void board_fail(void *v){(void)v;board_failed=1;}
static void check(int ok,unsigned code){if(!ok){os_check_result=code;__asm__ volatile("brk #0");for(;;){}}}
/* The admitted lifetime of the modeled session (enum a733_lifetime), at EL1 and in admission alike. */
static unsigned model_lifetime=A733_LIFETIME_RESIDENT;
unsigned board_session_lifetime(void){return model_lifetime;}
unsigned os_lifetime(void){return model_lifetime;}
void board_service_progress(unsigned service){check(service==OS_SERVICE_AUDIO,80);++reports;}
/* The supervised lifetime keeps no progress record. */
#define REPORTS(n) (model_lifetime==A733_LIFETIME_RESIDENT?(n):0u)
_Noreturn void platform_panic(const char *file,unsigned line)
{(void)file;os_check_result=200+line;__asm__ volatile("brk #0");for(;;){}}
void vTaskDelay(TickType_t ticks)
{check(!model_critical,207);++model_delays;model_ticks+=(uint64_t)ticks*24000000/configTICK_RATE_HZ;}
enum a733_i2s_resource_result a733_audio_parent_poll(struct a733_audio_parent *p)
{(void)p;return ++parent_polls<3?A733_I2S_RESOURCE_PENDING:A733_I2S_RESOURCE_READY;}
int a733_audio_parent_ready(const struct a733_audio_parent *p){(void)p;return parent_polls>=3;}
/* Byte i of period n of the stream's open number r. */
static uint8_t sample(unsigned r,unsigned n,unsigned i)
{
    uint32_t x=r*0x9e3779b1u^n*0x85ebca77u^i*0xc2b2ae3du;
    x^=x>>15;x*=0x2c1b3c6du;x^=x>>12;return (uint8_t)x;
}
static uint64_t model_before(unsigned n){return 1000u*n+7;}
static uint64_t model_after(unsigned n){return 1000u*n+9;}
int dma_service_send(struct dma_service *s,const struct dma_service_request *r,TickType_t wait,uint64_t *ticket)
{
    check(s==&AUDIO_STATE.pool.service&&!wait&&r->job==0&&logged<sizeof(log)-1,10);
    static const char op[]="SOMCRLGPE";
    queued=*r;*ticket=++sent;log[logged++]=op[r->operation];return 1;
}
int dma_service_collect(struct dma_service *s,uint64_t ticket,struct dma_service_reply *out,TickType_t wait)
{
    check(s==&AUDIO_STATE.pool.service&&ticket==sent&&!wait,11);
    if(silent)return 0; /* The worker has not answered yet. */
    memset(out,0,sizeof(*out));out->ticket=ticket;out->result=DMA_SERVICE_OK;
    switch(queued.operation){
    case DMA_SERVICE_OPEN:check(phase==DMA_SERVICE_CLOSED&&queued.timeout,12);phase=DMA_SERVICE_OPENING;produced=0;++opens;break;
    case DMA_SERVICE_STATUS:
        if(phase==DMA_SERVICE_OPENING)phase=DMA_SERVICE_READY;
        else if(phase==DMA_SERVICE_DRAINING)phase=DMA_SERVICE_RESULT;
        else if(phase==DMA_SERVICE_CLOSING)phase=DMA_SERVICE_CLOSED;
        out->phase=phase;out->outcome=mode==2?DMA_SERVICE_FAILED:DMA_SERVICE_CANCELED;
        out->holds_buffers=phase==DMA_SERVICE_RESULT||phase==DMA_SERVICE_RUNNING;break;
    case DMA_SERVICE_SUBMIT:check(phase==DMA_SERVICE_READY,13);phase=DMA_SERVICE_RUNNING;break;
    case DMA_SERVICE_PERIOD_GET:{
        check(phase==DMA_SERVICE_RUNNING,14);
#ifdef AUDIO_MODEL_REPLY_EVENT
        if(mode==19||mode==20||mode==21){
            AUDIO_MODEL_REPLY_EVENT();
            if(mode==20&&sent==4)audio_event(audio,DMA_SERVICE_PROGRESS_EVENT);
            if(!event_available||produced){out->result=DMA_SERVICE_BUSY;break;}
        }
#endif
        /* Mode 2: the stream stops by itself after two periods. */
        if(mode==2&&produced==2){out->result=DMA_SERVICE_BUSY;phase=DMA_SERVICE_RESULT;break;}
        unsigned n=produced++,frames=model_bytes/model_frame;
        for(unsigned i=0;i<model_bytes;++i){
            memory[n%PERIODS][i]=AUDIO_STATE.client.cyclic?0xee:sample(opens,n,i);
            staging[i]=sample(opens,n,i);
        }
        out->period=(struct a733_i2s_period){.ticket=1000+sent,.slot=n%PERIODS,.frames=frames,
            .first_frame=(uint64_t)(mode==1&&n==2?n+1:n)*frames,.observed_before_us=model_before(n),
            .observed_after_us=mode==10?model_before(n)-1:model_after(n),
            .kind=AUDIO_STATE.client.cyclic?A733_I2S_CAPTURED_COPY:A733_I2S_CAPTURED};
        held_ticket=out->period.ticket;break;}
    case DMA_SERVICE_PERIOD_PUT:check(queued.period_ticket==held_ticket,15);++puts_done;held_ticket=0;break;
    case DMA_SERVICE_CANCEL:check(phase==DMA_SERVICE_RUNNING,16);phase=DMA_SERVICE_DRAINING;break;
    case DMA_SERVICE_REAP:check(phase==DMA_SERVICE_RESULT&&!held_ticket,17);++reaps;phase=DMA_SERVICE_READY;break;
    case DMA_SERVICE_CLOSE:check(phase==DMA_SERVICE_READY,18);++closes;phase=DMA_SERVICE_CLOSING;break;
    default:out->result=DMA_SERVICE_INVALID;
    }
    out->phase=phase;
    return 1;
}
int dma_service_copy_period(struct dma_service *s,unsigned job,uint64_t ticket,void *out,size_t bytes)
{
    check(s==&AUDIO_STATE.pool.service&&!job&&!AUDIO_STATE.pending&&AUDIO_STATE.client.cyclic&&
          ticket==held_ticket&&out&&bytes==model_bytes,19);
    if(mode==9)return 0;
    memcpy(out,staging,bytes);return 1;
}
