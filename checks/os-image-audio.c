/* The integrated OS image's capture client (apps/os/audio.c) against a modeled
 * DMA service, audio parent and board. Its own state machine, request
 * protocol, WAV publication, generation handoff and resident progress
 * reports run unchanged; the runner (checks/os-image-audio.py) executes each
 * mode under unicorn. */
#include "../apps/os/audio.c"
struct board_application board;
struct board_provider providers;
volatile unsigned os_check_result;
static unsigned board_up=1,board_failed,parent_polls,mode,phase,produced,puts_done,reaps,closes,silent,reports;
static uint64_t sent,held_ticket;
static struct dma_service_request queued;
static uint8_t memory[PERIODS][PERIOD_BYTES];
static char log[64];static unsigned logged;
int board_live(void *v){(void)v;return board_up;}
void board_fail(void *v){(void)v;board_failed=1;}
static void check(int ok,unsigned code);
#ifdef SUNSTONE_BOARD_RESIDENT
void board_service_progress(unsigned service){check(service==OS_SERVICE_AUDIO,80);++reports;}
#define REPORTS(n) (n)
#else
#define REPORTS(n) 0u /* The supervised lifetime keeps no progress record. */
#endif
_Noreturn void platform_panic(const char *file,unsigned line)
{(void)file;os_check_result=200+line;__asm__ volatile("brk #0");for(;;){}}
void vTaskDelay(TickType_t ticks){(void)ticks;}
enum a733_i2s_resource_result a733_audio_parent_poll(struct a733_audio_parent *p)
{(void)p;return ++parent_polls<3?A733_I2S_RESOURCE_PENDING:A733_I2S_RESOURCE_READY;}
int a733_audio_parent_ready(const struct a733_audio_parent *p){(void)p;return parent_polls>=3;}
static void check(int ok,unsigned code){if(!ok){os_check_result=code;__asm__ volatile("brk #0");for(;;){}}}
static uint8_t pattern(unsigned n){return (uint8_t)(0x40+16*atomic_load(&audio.requested)+n);}
int dma_service_send(struct dma_service *s,const struct dma_service_request *r,TickType_t wait,uint64_t *ticket)
{
    check(s==&audio.pool.service&&!wait&&r->job==0&&logged<sizeof(log)-1,10);
    static const char op[]="SOMCRLGPE";
    queued=*r;*ticket=++sent;log[logged++]=op[r->operation];return 1;
}
int dma_service_collect(struct dma_service *s,uint64_t ticket,struct dma_service_reply *out,TickType_t wait)
{
    check(s==&audio.pool.service&&ticket==sent&&!wait,11);
    if(silent)return 0; /* The worker has not answered yet. */
    memset(out,0,sizeof(*out));out->ticket=ticket;out->result=DMA_SERVICE_OK;
    switch(queued.operation){
    case DMA_SERVICE_OPEN:check(phase==DMA_SERVICE_CLOSED&&queued.timeout,12);phase=DMA_SERVICE_OPENING;produced=0;break;
    case DMA_SERVICE_STATUS:
        if(phase==DMA_SERVICE_OPENING)phase=DMA_SERVICE_READY;
        else if(phase==DMA_SERVICE_DRAINING)phase=DMA_SERVICE_RESULT;
        else if(phase==DMA_SERVICE_CLOSING)phase=DMA_SERVICE_CLOSED;
        out->phase=phase;out->outcome=mode==2?DMA_SERVICE_FAILED:DMA_SERVICE_CANCELED;
        out->holds_buffers=phase==DMA_SERVICE_RESULT||phase==DMA_SERVICE_RUNNING;break;
    case DMA_SERVICE_SUBMIT:check(phase==DMA_SERVICE_READY,13);phase=DMA_SERVICE_RUNNING;break;
    case DMA_SERVICE_PERIOD_GET:{
        check(phase==DMA_SERVICE_RUNNING,14);
        /* Mode 2: the stream stops by itself after two periods. */
        if(mode==2&&produced==2){out->result=DMA_SERVICE_BUSY;phase=DMA_SERVICE_RESULT;break;}
        unsigned n=produced++;
        memset(memory[n%PERIODS],pattern(n),PERIOD_BYTES);
        out->period=(struct a733_i2s_period){.ticket=1000+sent,.slot=n%PERIODS,.frames=PERIOD_FRAMES,
            .first_frame=(uint64_t)(mode==1&&n==2?n+1:n)*PERIOD_FRAMES,.kind=A733_I2S_CAPTURED};
        held_ticket=out->period.ticket;break;}
    case DMA_SERVICE_PERIOD_PUT:check(queued.period_ticket==held_ticket,15);++puts_done;held_ticket=0;break;
    case DMA_SERVICE_CANCEL:check(phase==DMA_SERVICE_RUNNING,16);phase=DMA_SERVICE_DRAINING;break;
    case DMA_SERVICE_REAP:check(phase==DMA_SERVICE_RESULT&&!held_ticket,17);++reaps;phase=DMA_SERVICE_READY;break;
    case DMA_SERVICE_CLOSE:check(phase==DMA_SERVICE_READY,18);++closes;phase=DMA_SERVICE_CLOSING;break;
    default:out->result=DMA_SERVICE_INVALID;
    }
    return 1;
}
/* Steps until the given generation is captured, or step refuses. */
static int run(unsigned generation)
{
    for(unsigned n=0;n<400;++n){
        if(atomic_load(&audio.captured)==generation)return 1;
        int r=step();if(r<0)return -1;
    }
    return 0;
}
static uint32_t get32(const uint8_t *p){return p[0]|p[1]<<8|p[2]<<16|(uint32_t)p[3]<<24;}
static void captured(unsigned generation)
{
    const uint8_t *wav;size_t bytes;
    check(os_audio_capture(generation,&wav,&bytes)==1&&wav==audio.wav&&bytes==44+2*audio.samples,30);
    check(!memcmp(wav,"RIFF",4)&&get32(wav+4)==36+2*audio.samples&&!memcmp(wav+8,"WAVEfmt ",8)&&
        get32(wav+16)==16&&get32(wav+20)==0x10001&&get32(wav+24)==16000&&get32(wav+28)==32000&&
        get32(wav+32)==0x100002&&!memcmp(wav+36,"data",4)&&get32(wav+40)==2*audio.samples,31);
    for(unsigned i=0;i<2*audio.samples;++i)check(wav[44+i]==(uint8_t)(0x40+16*generation+i/PERIOD_BYTES),32);
}
void os_check_run(unsigned selected)
{
    mode=selected;audio.samples=16000;phase=DMA_SERVICE_CLOSED;
    for(unsigned i=0;i<PERIODS;++i)audio.pool.stream_mappings[i].cpu=memory[i];
    atomic_store(&audio.pool.state,A733_DMA_POOL_READY);
    const uint8_t *wav;size_t bytes;
    if(mode==4){
        /* Each task pass without an unanswered request reports progress; a
         * pass waiting for the worker or after a failure reports none. */
        for(unsigned n=0;n<10;++n)pass();
        check(audio.stage==IDLE&&reports==REPORTS(10),70);
        check(!os_audio_capture(1,&wav,&bytes),71);
        pass();check(audio.stage==OPEN&&!audio.pending&&reports==REPORTS(11),72);
        silent=1;pass();pass();
        check(audio.stage==OPEN&&audio.pending&&reports==REPORTS(11),73);
        silent=0;pass();
        check(audio.stage==OPENING&&!audio.pending&&reports==REPORTS(12),74);
        board_up=0;pass();pass();
        check(board_failed&&atomic_load(&audio.failed)&&reports==REPORTS(12),75);
        os_check_result=1;return;
    }
    if(mode==3){
        check(os_audio_capture(0,&wav,&bytes)<0&&os_audio_capture(1,NULL,&bytes)<0,40);
        check(!os_audio_capture(2,&wav,&bytes)&&os_audio_capture(1,&wav,&bytes)<0,41);
        board_up=0;check(os_audio_capture(2,&wav,&bytes)<0,42);
        os_check_result=1;return;
    }
    /* The parent becomes ready on the third poll; nothing is requested yet. */
    for(unsigned n=0;n<10;++n)check(step()==0,50);
    check(atomic_load(&audio.ready)&&audio.stage==IDLE&&!logged,51);
    check(!os_audio_capture(1,&wav,&bytes),52);
    int r=run(1);
    if(mode){
        /* A discontinuous period or a stream that stopped by itself refuses
         * the capture with the stream, its final period and leases held. */
        check(r<0&&!atomic_load(&audio.captured)&&!reaps&&!closes,60+mode);
        os_check_result=1;return;
    }
    /* 32,000 bytes: seven full periods and part of an eighth, then CANCEL
     * with the final period still held. */
    check(r==1&&produced==8&&puts_done==7&&held_ticket,53);
    check(logged==20&&!memcmp(log,"OSMGPGPGPGPGPGPGPGCS",20),54);
    captured(1);
    /* The next generation returns the final period, reaps, closes and
     * opens again before it captures. */
    check(!os_audio_capture(2,&wav,&bytes)&&run(2)==1&&reaps==1&&closes==1&&puts_done==15,55);
    check(logged==44&&!memcmp(log+20,"PRLSOSMGPGPGPGPGPGPGPGCS",24),56);
    captured(2);check(os_audio_capture(1,&wav,&bytes)<0&&!board_failed,57);
    os_check_result=1;
}
