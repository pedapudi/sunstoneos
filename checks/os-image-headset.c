/* The integrated OS image's headset capture client (apps/os/headset.c)
 * against a modeled Bluetooth application. The model publishes fixed-length
 * CVSD segments with the segment API's own copy, acknowledgment and ticket
 * rules (apps/bluetooth/headset.c); the client, its startup and pin entries
 * and the real 8-to-16 kHz conversion (apps/bluetooth/pcm.c) run unchanged.
 * The runner (checks/os-image-headset.py) executes each mode under unicorn.
 * Each failed expectation stops with its own code. */
#include "../apps/os/headset.c"
struct board_application board;
struct board_provider providers;
volatile unsigned os_check_result;
static void check(int ok,unsigned code){if(!ok){os_check_result=code;__asm__ volatile("brk #0");for(;;){}}}
static unsigned board_up=1,board_failed,stopped,bound,initialized,created,attributes=0xff,mode;
static struct os_session session;
static struct bluetooth_profile profile;
static const struct bluetooth_profile *published=&profile;
static uint64_t start_us,end_us;
int board_live(void *v){(void)v;return board_up;}
void board_fail(void *v){(void)v;board_failed=1;}
const struct os_session *os_session_admitted(void){return &session;}
const struct bluetooth_profile *os_headset_profile(void){return published;}
const struct board_radio_chip_id *board_radio_chip_id(void){return (const void *)&board;}
int aarch64_device_translate(uintptr_t address,int write,struct aarch64_translation *t)
{(void)write;*t=(struct aarch64_translation){address,attributes,3};return 1;}
/* The Bluetooth image's own profile rules are checked by its checks; here a
 * profile is valid when it carries the digest the session names. */
int bluetooth_profile_valid(const struct bluetooth_profile *p,const uint8_t digest[32])
{return p==&profile&&digest[0]==0x54;}
TaskHandle_t xTaskCreateStatic(TaskFunction_t f,const char *const name,const configSTACK_DEPTH_TYPE depth,
    void *const arg,UBaseType_t priority,StackType_t *const stack,StaticTask_t *const task)
{(void)f;(void)name;(void)depth;(void)arg;(void)priority;(void)stack;++created;return (TaskHandle_t)task;}
void vTaskDelay(TickType_t ticks){(void)ticks;}
int bluetooth_application_init(struct bluetooth_application *a,const struct bluetooth_profile *p,
    struct a733_native_usb *shared,const struct board_radio_chip_id *id,TaskHandle_t owner_task,atomic_uint *recovery,
    uint64_t start,uint64_t end,const uint8_t digest[32])
{
    check(a==&app&&p==&profile&&shared==&providers.shared&&id&&owner_task==headset.owner&&
        recovery==&board.watchdog_state&&digest==board.session.sid_evidence,80);
    start_us=start;end_us=end;++initialized;return 1;
}
int bluetooth_application_poll(struct bluetooth_application *a){(void)a;return 0;}
int bluetooth_application_retain(struct bluetooth_application *a){(void)a;return -1;}

/* Modeled segment producer: two slots, 3,000-byte CVSD segments whose bytes
 * encode their ticket. */
enum { SEGMENT_BYTES=3000 };
static uint64_t head,tail;
static unsigned failed_app,changed_copy;
static uint8_t slot[BLUETOOTH_SEGMENT_SLOTS][SEGMENT_BYTES];
static struct bluetooth_segment_snapshot snapshots[BLUETOOTH_SEGMENT_SLOTS];
static uint8_t value(uint64_t ticket,unsigned i){return (uint8_t)(ticket*17+i*3);}
static void publish(unsigned loss,unsigned bad_hash)
{
    check(head-tail<BLUETOOTH_SEGMENT_SLOTS,81);
    unsigned n=(unsigned)(head%BLUETOOTH_SEGMENT_SLOTS);uint64_t t=++head;
    for(unsigned i=0;i<SEGMENT_BYTES;++i)slot[n][i]=value(t,i);
    struct bluetooth_segment_snapshot *s=&snapshots[n];memset(s,0,sizeof(*s));
    s->ticket=t;s->first_sample=(t-1)*SEGMENT_BYTES/2;
    s->capture=(struct bluetooth_pcm_snapshot){.firmware=1,.uart=2,.acl=3,.audio=4,.state=1,.bytes=SEGMENT_BYTES,
        .records=12,.loss_packets=loss};
    storage_digest_bytes(slot[n],SEGMENT_BYTES,s->capture.sha256);
    if(bad_hash)s->capture.sha256[0]^=1;
}
int bluetooth_headset_segment_status(struct bluetooth_application *a,struct bluetooth_segment_status *out)
{
    if(a!=&app||failed_app)return -1;
    *out=(struct bluetooth_segment_status){head,tail,0,0};return 1;
}
int bluetooth_headset_segment_copy(struct bluetooth_application *a,uint64_t ticket,uint32_t offset,size_t bytes,
    uint8_t out[1024],size_t *copied,struct bluetooth_segment_snapshot *snapshot)
{
    if(a!=&app||failed_app||!bytes||bytes>1024)return -1;
    *copied=0;memset(snapshot,0,sizeof(*snapshot));
    if(head==tail)return 0;
    unsigned n=(unsigned)(tail%BLUETOOTH_SEGMENT_SLOTS);
    if((ticket&&ticket!=tail+1)||offset>SEGMENT_BYTES||bytes>SEGMENT_BYTES-offset)return -1;
    memcpy(out,slot[n]+offset,bytes);*copied=bytes;*snapshot=snapshots[n];
    /* Mode 3: the producer's record changes during a later copy. */
    if(changed_copy&&offset)snapshot->capture.records^=1;
    return 1;
}
int bluetooth_headset_segment_ack(struct bluetooth_application *a,uint64_t ticket)
{
    if(a!=&app||failed_app||ticket!=tail+1||head<=tail)return -1;
    ++tail;return 1;
}
int bluetooth_headset_segment_stop(struct bluetooth_application *a){(void)a;++stopped;return failed_app?-1:1;}
int bluetooth_headset_segment_bind(struct bluetooth_application *a,void *consumer)
{check(a==&app&&consumer==headset.client,82);++bound;return 1;}

static void steps(unsigned n){for(unsigned i=0;i<n;++i)check(step()==0,83);}
static uint32_t get32(const uint8_t *p){return p[0]|p[1]<<8|p[2]<<16|(uint32_t)p[3]<<24;}
static int16_t at(const uint8_t *p){return (int16_t)(p[0]|p[1]<<8);}
/* The published WAV is the 16 kHz conversion of segment `ticket`. */
static void converted(unsigned g,uint64_t ticket)
{
    const uint8_t *wav;size_t bytes;
    check(os_headset_capture(g,&wav,&bytes)==1&&wav==headset.wav&&bytes==44+2*SEGMENT_BYTES,30);
    check(!memcmp(wav,"RIFF",4)&&!memcmp(wav+8,"WAVEfmt ",8)&&get32(wav+24)==16000&&get32(wav+40)==2*SEGMENT_BYTES,31);
    for(unsigned k=0;k+2<SEGMENT_BYTES;k+=2){
        uint8_t a[2]={value(ticket,k),value(ticket,k+1)},b[2]={value(ticket,k+2),value(ticket,k+3)};
        check(at(wav+44+2*k)==at(a)&&at(wav+46+2*k)==(int16_t)((at(a)+at(b))/2),32);
    }
}
static void nominal_session(void)
{
    memset(&session,0,sizeof(session));memset(&profile,0,sizeof(profile));
    struct a733_board_session *b=&session.board;
    b->workload=4;b->sid_authority=21;memset(b->sid_evidence,0x54,32);b->topology=1;
    b->pmic_owner=11;b->usb0_owner=12;b->typec_owner=13;b->usb2_owner=14;session.thermal_owner=15;
    for(unsigned i=0;i<3;++i)b->fabric_hz[i]=200000000;
    profile.version=3;profile.headset.mode=2;profile.sid_authority=21;profile.topology=1;
    profile.radio.owner=31;profile.uart.owner=32;profile.pm_latch=(struct a733_pin_latch_handoff){41,42,0x3f,0};
    memcpy(profile.radio.fabric_hz,b->fabric_hz,sizeof(b->fabric_hz));
    board.session=*b;attributes=0xff;published=&profile;
}
static void pins_cases(void)
{
    struct a733_pin_handoff pins;memset(&pins,0,sizeof(pins));
    nominal_session();check(os_headset_pins(&pins)==1,10);
    check(pins.authorized[A733_PIN_G]==0x3ff&&pins.authorized[A733_PIN_M]==0x1b&&pins.latch[1].identity==41,11);
    /* The radio station or an earlier entry already holds PG or PM. */
    check(!os_headset_pins(&pins),12);
    struct a733_pin_handoff clear;
#define REFUSED(change,code) do{nominal_session();memset(&clear,0,sizeof(clear));change; \
    check(!os_headset_pins(&clear)&&!clear.authorized[A733_PIN_G],code);}while(0)
    REFUSED(session.board.workload=2,13);
    REFUSED(session.board.workload=1,14);
    REFUSED(profile.headset.mode=1,15);
    REFUSED(profile.headset.mode=3,16);
    REFUSED(profile.version=2,17);
    REFUSED(profile.sid_authority=22,18);
    REFUSED(session.board.sid_evidence[0]=0x55,19);
    REFUSED(profile.radio.owner=session.board.usb2_owner,20);
    REFUSED(profile.uart.owner=session.board.pmic_owner,21);
    REFUSED(profile.uart.owner=profile.radio.owner,22);
    REFUSED(profile.radio.owner=session.thermal_owner,23);
    REFUSED(profile.uart.owner=session.thermal_owner,24);
    REFUSED(profile.topology=2,25);
    REFUSED(profile.radio.fabric_hz[1]^=1,26);
    REFUSED(attributes=0x44,27);
    REFUSED(published=NULL,28);
    REFUSED(clear.authorized[A733_PIN_M]=3,29);
}
static void start_case(void)
{
    nominal_session();
    board.session.session_start_ticks=24*UINT64_C(7000000);board.session.deadline_us=7000000+60000000;
    check(os_headset_start()==1&&created==2&&initialized==1&&bound==1,40);
    check(start_us==7000000&&end_us==67000000,41);
    check(!os_headset_start()&&initialized==1,42);
    /* Only the Bluetooth owner runs privileged. */
    check(os_headset_task_privileged(owner,NULL)==1&&!os_headset_task_privileged(os_headset_task,NULL)&&
        !os_headset_task_privileged(owner,&app),43);
}
void os_check_run(unsigned selected)
{
    mode=selected;
    const uint8_t *wav;size_t bytes;
    if(mode==0){pins_cases();start_case();os_check_result=1;return;}
    if(mode==4){
        check(os_headset_capture(0,&wav,&bytes)<0&&os_headset_capture(1,NULL,&bytes)<0,50);
        check(!os_headset_capture(2,&wav,&bytes)&&os_headset_capture(1,&wav,&bytes)<0,51);
        board_up=0;check(os_headset_capture(2,&wav,&bytes)<0,52);
        os_check_result=1;return;
    }
    if(mode==1){
        /* Two generations. Segments before a request are discarded. */
        publish(0,0);steps(2);publish(0,0);steps(2);
        check(head==2&&tail==2&&!atomic_load(&headset.captured),60);
        /* Segment 3 is being filled when generation 1 is requested; the first
         * segment that starts after the request is 4. */
        check(!os_headset_capture(1,&wav,&bytes),61);
        steps(1);check(headset.first_ticket==4,62);
        publish(0,0);steps(1);check(tail==3&&!atomic_load(&headset.captured),63);
        publish(0,0);steps(1);check(tail==4,64);converted(1,4);
        /* While the recognizer holds generation 1, later segments are discarded
         * without touching its WAV; two at once drain over two steps. */
        publish(0,0);publish(0,0);steps(2);check(tail==6,65);converted(1,4);
        check(!os_headset_capture(2,&wav,&bytes),66);
        steps(1);check(headset.first_ticket==8,67);
        publish(0,0);publish(0,0);steps(2);check(tail==8,68);converted(2,8);
        check(os_headset_capture(1,&wav,&bytes)<0&&!board_failed&&!stopped,69);
        os_check_result=1;return;
    }
    /* Refusals: a lost packet, a wrong digest or a record that changes during
     * the copy ends the session and stops capture; a failed application too. */
    check(!os_headset_capture(1,&wav,&bytes),70);steps(1);check(headset.first_ticket==2,74);
    /* Segment 1 started before the request and is discarded unread; a lost
     * packet refuses any segment. */
    if(mode!=2){publish(0,0);steps(1);check(tail==1,75);}
    if(mode==2)publish(1,0);
    else if(mode==3){changed_copy=1;publish(0,0);}
    else if(mode==5)publish(0,1);
    else failed_app=1;
    check(step()<0,71);fail();
    check(board_failed&&stopped==1&&atomic_load(&headset.failed)&&os_headset_capture(1,&wav,&bytes)<0,72);
    /* The refused segment stays unacknowledged. */
    check(tail==(mode==2?0u:1u),73);
    os_check_result=1;
}
