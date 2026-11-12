/* The integrated OS image's headset capture client (apps/os/headset.c)
 * against a modeled Bluetooth application. The model publishes fixed-length
 * CVSD or mSBC segments with the segment API's request word, borrow,
 * acknowledgment and ticket rules (apps/bluetooth/headset.c); the client, its
 * startup and pin entries and the real WAV conversions (apps/bluetooth/pcm.c)
 * run unchanged. The same model covers both owner modes and the build's
 * options: a session that grants the radio attaches the Bluetooth client to
 * the radio's module, one that does not runs its own Bluetooth owner, a
 * session that does not grant the headset leaves every entry inert, and the
 * resident build records service progress. CVSD (mode 2) and mSBC (mode 3)
 * are both admitted: the codec is always linked.
 * The runner (checks/os-image-headset.py) executes each mode under unicorn,
 * with bit 8 of the mode selecting the radio session. Each failed expectation
 * stops with its own code. */
#include "../apps/os/headset.c"
struct board_application board;
struct board_provider providers;
volatile unsigned os_check_result;
static void check(int ok,unsigned code){if(!ok){os_check_result=code;__asm__ volatile("brk #0");for(;;){}}}
static unsigned board_up=1,board_failed,stopped,bound,initialized,created,attributes=0xff,mode,polled,retained;
static uint32_t segment_flags,segment_disconnected;
static unsigned model_failing,quiesce_notes,quiesce_registered;
static unsigned on_owner,owner_notified,stop_calls,release_calls,down_calls,deadline_calls;
static int stop_reply,module_release,module_down=-1;
static const uint64_t return_deadline=UINT64_C(12345678);
int os_quiesce_register(struct os_quiesce *q)
{check(!created,130);q->registered=1;++quiesce_registered;return 1;}
int os_quiesce_requested(const struct os_quiesce *q){return atomic_load(&q->requested)!=0;}
enum os_quiesce_result os_quiesce_result(const struct os_quiesce *q){return atomic_load(&q->result);}
void os_quiesce_complete(struct os_quiesce *q)
{unsigned pending=OS_QUIESCE_PENDING;(void)atomic_compare_exchange_strong(&q->result,&pending,OS_QUIESCE_DONE);}
int os_quiesce_fault(struct os_quiesce *q,uint32_t site,uint32_t value,const void *caller)
{
    if(!model_failing&&!os_quiesce_requested(q))return 0;
    if(!atomic_exchange(&q->fault_state,2)){q->site=site;q->value=value;q->caller=caller;++quiesce_notes;}
    atomic_store(&q->result,OS_QUIESCE_FAULT);return 1;
}
uint64_t os_cpu_return_deadline_us(void){check(on_owner,150);++deadline_calls;return return_deadline;}
static struct os_session session;
static unsigned radio_granted; /* the session grants the radio: the headset shares the radio's module owner */
static uint64_t start_us,end_us;
/* The ledger (apps/os/owner-memory.h): one extent, readable and writable at EL0, of the headset's memory and the library's state. */
#define STATE_BYTES 4096u
const size_t bluetooth_headset_state_bytes=STATE_BYTES;
static _Alignas(64) uint8_t extent[((sizeof(struct headset_memory)+63)&~(size_t)63)+STATE_BYTES];
static unsigned taken,given;
void *os_owner_memory(uint64_t bytes,uint64_t align,unsigned attributes)
{
    check(!taken&&bytes==sizeof(extent)&&align==64&&attributes==OS_MEMORY_EL0_WRITE,90);
    ++taken;return extent;
}
int bluetooth_headset_memory(void *m)
{check(m==extent+((sizeof(struct headset_memory)+63)&~(size_t)63)&&!given,91);++given;return 1;}
int board_live(void *v){(void)v;return board_up;}
void board_fail(void *v){(void)v;board_failed=1;}
const struct os_session *os_session_admitted(void){return &session;}
int os_granted(unsigned mask){return mask&&(session.granted&mask)==mask;}
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
/* os_headset_identity runs at EL3 in the image (checks/os-image-identity.py executes it); here it is never called. */
int a733_cpu_coherence_publish(uintptr_t address,size_t bytes,uint64_t deadline)
{(void)address;(void)bytes;(void)deadline;check(0,99);return 0;}
/* The radio entry's authenticated profile and the board's radio. */
static struct board_radio_profile radio_profile;
static struct board_radio radio;
static unsigned radio_admitted=1;
const struct board_radio_profile *os_radio_profile(void){return radio_admitted?&radio_profile:NULL;}
struct board_radio *os_board_radio(void){return &radio;}
int bluetooth_application_attach(struct bluetooth_application *a,const struct bluetooth_profile *p,
    struct a733_native_usb *shared,const struct board_radio_chip_id *id,TaskHandle_t owner_task,atomic_uint *latch,
    uint64_t start,uint64_t end,const uint8_t digest[32],struct a733_radio_platform_native *native,
    struct a733_radio_session *module)
{
    /* The radio owner serves the client, which has its own latch. */
    check(a==&app&&p==&profile&&shared==&providers.shared&&id&&owner_task==board.handles[5]&&owner_task==headset.owner&&
        latch==&headset.latch&&digest==board.session.sid_evidence&&native==&radio.native&&module==&radio.session,80);
    check(radio_granted,89);
    start_us=start;end_us=end;++initialized;return 1;
}
int bluetooth_application_init(struct bluetooth_application *a,const struct bluetooth_profile *p,
    struct a733_native_usb *shared,const struct board_radio_chip_id *id,TaskHandle_t owner_task,atomic_uint *recovery,
    uint64_t start,uint64_t end,const uint8_t digest[32])
{
    check(a==&app&&p==&profile&&shared==&providers.shared&&id&&owner_task==headset.owner&&
        recovery==&board.watchdog_state&&digest==board.session.sid_evidence,80);
    check(!radio_granted,89);
    start_us=start;end_us=end;++initialized;return 1;
}
static unsigned codec_ready;
int bluetooth_msbc_init(void){++codec_ready;return 1;}
/* The admitted lifetime of the modeled session (enum a733_lifetime). */
static unsigned model_lifetime=A733_LIFETIME_SUPERVISED;
unsigned board_session_lifetime(void){return model_lifetime;}
unsigned os_lifetime(void){return model_lifetime;}
static unsigned progress[OS_SERVICES];
void board_service_progress(unsigned service){check(service==OS_SERVICE_BLUETOOTH||service==OS_SERVICE_HEADSET,84);++progress[service];}
int bluetooth_application_poll(struct bluetooth_application *a){check(a==&app&&on_owner,85);++polled;return 0;}
int bluetooth_application_retain(struct bluetooth_application *a){check(a==&app&&on_owner,86);++retained;return -1;}

/* Modeled segment producer: two slots, 3,000-byte CVSD segments whose bytes
 * encode their ticket, and the request word. An activation starts only on a
 * request and ends by storing 0, with or without a segment. */
enum { SEGMENT_BYTES=3000 };
static uint64_t head,tail;
static unsigned failed_app,refuse_take,refuse_request,word,notified,publish_in_take;
int bluetooth_application_stop(struct bluetooth_application *a,uint64_t deadline)
{check(a==&app&&on_owner&&deadline==return_deadline,151);++stop_calls;return failed_app?-1:stop_reply;}
int bluetooth_application_module_release(struct bluetooth_application *a)
{check(a==&app&&on_owner,152);++release_calls;return module_release;}
void bluetooth_application_module_down(struct bluetooth_application *a,int proved)
{check(a==&app&&on_owner&&(proved==0||proved==1),153);++down_calls;module_down=proved;}
static uint8_t slot[BLUETOOTH_SEGMENT_SLOTS][SEGMENT_BYTES];
static struct bluetooth_segment_snapshot snapshots[BLUETOOTH_SEGMENT_SLOTS];
static uint8_t value(uint64_t ticket,unsigned i){return (uint8_t)(ticket*17+i*3);}
static void publish_state(unsigned loss,unsigned state)
{
    check(head-tail<BLUETOOTH_SEGMENT_SLOTS,81);
    unsigned n=(unsigned)(head%BLUETOOTH_SEGMENT_SLOTS);uint64_t t=++head;
    for(unsigned i=0;i<SEGMENT_BYTES;++i)slot[n][i]=value(t,i);
    struct bluetooth_segment_snapshot *s=&snapshots[n];memset(s,0,sizeof(*s));
    s->ticket=t;s->first_sample=(t-1)*SEGMENT_BYTES/2;
    s->capture=(struct bluetooth_pcm_snapshot){.firmware=1,.uart=2,.acl=3,.audio=4,.state=state,.bytes=SEGMENT_BYTES,
        .records=12,.loss_packets=loss};
    storage_digest_bytes(slot[n],SEGMENT_BYTES,s->capture.sha256);
}
static void publish(unsigned loss){publish_state(loss,1);}
/* One activation on the current request; segment 0 ends it without one, as
 * after a headset cancel. */
static void activation(unsigned segment)
{
    check(word==BLUETOOTH_SEGMENT_ACCEPT||word==BLUETOOTH_SEGMENT_START,87);
    if(segment)publish(0);
    word=0;
}
int bluetooth_headset_segment_status(struct bluetooth_application *a,struct bluetooth_segment_status *out)
{
    if(a!=&app||failed_app)return -1;
    *out=(struct bluetooth_segment_status){head,tail,segment_flags,segment_disconnected};return 1;
}
int bluetooth_headset_segment_request(struct bluetooth_application *a,unsigned request,uint64_t observed)
{
    if(a!=&app||failed_app||refuse_request||(request!=BLUETOOTH_SEGMENT_ACCEPT&&request!=BLUETOOTH_SEGMENT_START))return -1;
    if(word==request)return 1;
    if(word>request||head!=observed)return 0;
    word=request;return 1;
}
int bluetooth_headset_segment_take(struct bluetooth_application *a,const uint8_t **pcm,struct bluetooth_segment_snapshot *snapshot)
{
    if(a!=&app||failed_app||refuse_take)return -1;
    *pcm=NULL;memset(snapshot,0,sizeof(*snapshot));
    /* An activation may end with a segment right after take sees none. */
    if(head==tail){if(publish_in_take){publish_in_take=0;publish(0);word=0;}return 0;}
    unsigned n=(unsigned)(tail%BLUETOOTH_SEGMENT_SLOTS);*pcm=slot[n];*snapshot=snapshots[n];return 1;
}
int bluetooth_headset_segment_ack(struct bluetooth_application *a,uint64_t ticket)
{
    if(a!=&app||failed_app||ticket!=tail+1||head<=tail)return -1;
    ++tail;return 1;
}
int bluetooth_headset_segment_stop(struct bluetooth_application *a){(void)a;++stopped;return failed_app?-1:1;}
int bluetooth_headset_segment_bind(struct bluetooth_application *a,void *consumer)
{check(a==&app&&consumer==headset.client,82);++bound;return 1;}
/* A new generation wakes the client. */
BaseType_t xTaskGenericNotify(TaskHandle_t task,UBaseType_t index,uint32_t v,eNotifyAction action,uint32_t *previous)
{(void)v;check(task&&(task==headset.client||task==headset.owner)&&!index&&action==eIncrement&&!previous,88);
 if(task==headset.client)++notified;else ++owner_notified;return pdPASS;}
uint32_t ulTaskGenericNotifyTake(UBaseType_t index,BaseType_t clear,TickType_t ticks){(void)index;(void)clear;(void)ticks;return 0;}

static void steps(unsigned n){for(unsigned i=0;i<n;++i)check(step()==0,83);}
static uint32_t get32(const uint8_t *p){return p[0]|p[1]<<8|p[2]<<16|(uint32_t)p[3]<<24;}
static int16_t at(const uint8_t *p){return (int16_t)(p[0]|p[1]<<8);}
/* The published WAV is the 16 kHz conversion of segment `ticket`. */
static void converted(unsigned g,uint64_t ticket)
{
    const uint8_t *wav;size_t bytes;
    check(os_headset_capture(g,&wav,&bytes)==1&&wav==memory->wav&&bytes==44+2*SEGMENT_BYTES,30);
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
    b->workload=radio_granted?1:4;session.granted=OS_GRANT_HEADSET|(radio_granted?OS_GRANT_RADIO:0);
    b->sid_authority=21;memset(b->sid_evidence,0x54,32);b->topology=1;
    b->pmic_owner=11;b->usb0_owner=12;b->typec_owner=13;b->usb2_owner=14;session.thermal_owner=15;
    for(unsigned i=0;i<3;++i)b->fabric_hz[i]=200000000;
    profile.bytes=832;profile.version=3;profile.headset.mode=2;profile.sid_authority=21;profile.topology=1;
    profile.radio.owner=31;profile.uart.owner=32;profile.pm_latch=(struct a733_pin_latch_handoff){41,42,0x3f,0};
    memcpy(profile.radio.fabric_hz,b->fabric_hz,sizeof(b->fabric_hz));
    board.session=*b;attributes=0xff;
    memset(&radio_profile,0,sizeof(radio_profile));radio_admitted=1;
    radio_profile.platform.owner=profile.radio.owner;radio_profile.session_owner=profile.session_owner=33;radio_profile.ce.owner=34;
}
/* The pin authority before the headset entry: none, or with the radio the
 * board's PG0..PG5, PM0, PM1 and the session's PM latch for the module. */
static void board_pins(struct a733_pin_handoff *pins)
{
    memset(pins,0,sizeof(*pins));
    if(radio_granted){pins->authorized[A733_PIN_G]=0x3f;pins->authorized[A733_PIN_M]=3;pins->latch[1]=profile.pm_latch;}
}
static void pins_cases(void)
{
    struct a733_pin_handoff pins;
    nominal_session();board_pins(&pins);check(os_headset_pins(&pins)==1,10);
    check(pins.authorized[A733_PIN_G]==0x3ff&&pins.authorized[A733_PIN_M]==0x1b&&pins.latch[1].identity==41,11);
    /* An earlier entry already holds UART1 or the wake pins. */
    check(!os_headset_pins(&pins),12);
    struct a733_pin_handoff clear;
#define REFUSED(change,code) do{nominal_session();board_pins(&clear);change; \
    check(!os_headset_pins(&clear)&&(clear.authorized[A733_PIN_G]&0x3ff)!=0x3ff,code);}while(0)
    REFUSED(session.board.workload=2,13);
    REFUSED(session.board.workload=3,14);
    if(radio_granted){
        /* The radio entry must have admitted the module, its pins and latch, and
         * the profile must name the radio's owners and its own UART owner. */
        REFUSED(radio_admitted=0,90);
        REFUSED(clear.authorized[A733_PIN_G]=0,91);
        REFUSED(clear.authorized[A733_PIN_M]=0,92);
        REFUSED(clear.latch[1].value^=1,93);
        REFUSED(radio_profile.platform.owner^=1,94);
        REFUSED(radio_profile.session_owner^=1,95);
        REFUSED(radio_profile.ce.owner=profile.uart.owner,96);
    }else{
        /* Without the radio no earlier entry holds the module's pins. */
        REFUSED(clear.authorized[A733_PIN_M]=3,29);
        REFUSED(clear.authorized[A733_PIN_G]=0x3f,98);
    }
    REFUSED(profile.headset.mode=1,15);
    nominal_session();profile.headset.mode=3;board_pins(&clear);check(os_headset_pins(&clear)==1,16);
    REFUSED(profile.headset.mode=5,16);
    REFUSED(profile.version=2,17);
    nominal_session();profile.version=4;profile.bytes=sizeof(profile);board_pins(&clear);
    check(os_headset_pins(&clear)==1&&clear.authorized[A733_PIN_M]==0x1f,154);
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
    REFUSED(profile.bytes=0,28);
    /* The capture length is twice the profile's 8 kHz sample count, and there is none without a copied profile. */
    uint32_t frames=0;
    nominal_session();profile.headset.samples=24000;
    check(os_headset_capture_frames(&frames)==1&&frames==48000,35);
    profile.bytes=0;check(!os_headset_capture_frames(&frames)&&!os_headset_capture_frames(NULL),36);
    /* A session that does not grant the headset leaves the pins and the tasks alone. */
    nominal_session();session.granted&=~OS_GRANT_HEADSET;board_pins(&clear);
    struct a733_pin_handoff before=clear;
    check(os_headset_pins(&clear)==1&&!memcmp(&clear,&before,sizeof(clear)),37);
    check(os_headset_memory()==1&&!taken&&!memory,39);
    check(os_headset_start()==1&&!created&&!initialized&&!headset.client,38);
}
static void start_case(void)
{
    nominal_session();
    profile.headset.mode=3;
    board.session.session_start_ticks=24*UINT64_C(7000000);
    board.session.deadline_us=model_lifetime==A733_LIFETIME_RESIDENT?UINT64_MAX/24:7000000+60000000;
    if(radio_granted){
        /* The radio owner (handle 5) is the Bluetooth owner; only the client is created. */
        board.handles[5]=(TaskHandle_t)&radio;
        check(os_headset_start()==1&&created==1&&initialized==1&&bound==1,40);
        check(!os_headset_task_privileged(os_headset_task,NULL)&&!os_headset_task_privileged(os_headset_task,&app)&&
            !os_headset_task_privileged(owner,NULL),43);
    }else{
        check(os_headset_start()==1&&created==2&&initialized==1&&bound==1,40);
        /* Only the Bluetooth owner runs privileged. */
        check(os_headset_task_privileged(owner,NULL)==1&&!os_headset_task_privileged(os_headset_task,NULL)&&
            !os_headset_task_privileged(owner,&app),43);
    }
    check(start_us==7000000&&end_us==board.session.deadline_us,41);
    check(codec_ready==1,44);
    check(quiesce_registered==1,131);
    check(!os_headset_start()&&initialized==1,42);
}
/* One owner iteration polls a live application, retains it once the board
 * fails, and reports resident progress only while the application has not
 * failed. With the radio the board's radio owner runs it after its own poll. */
static void owner_case(void)
{
#define SERVE() do{on_owner=1;if(radio_granted)os_module_poll();else serve();on_owner=0;}while(0)
    atomic_store(&providers.shared.pmic_ready,1);
    SERVE();check(polled==1&&!retained,100);
    atomic_store(&app.failed,1);SERVE();check(polled==2,101);
    board_up=0;SERVE();check(polled==2&&retained==1,102);
    /* A supervised session reports no progress. */
    check(progress[OS_SERVICE_BLUETOOTH]==(model_lifetime==A733_LIFETIME_RESIDENT),103);
}
/* The recognizer requests generation g; the client is woken once. */
static void generation(unsigned g)
{
    const uint8_t *wav;size_t bytes;unsigned before=notified;
    check(!os_headset_capture(g,&wav,&bytes)&&notified==before+1,61);
    check(!os_headset_capture(g,&wav,&bytes)&&notified==before+1,62);
}
void os_check_run(unsigned selected)
{
    mode=selected&0xff;radio_granted=selected>>8&1;
    model_lifetime=selected>>9&1?A733_LIFETIME_RESIDENT:A733_LIFETIME_SUPERVISED;
    const uint8_t *wav;size_t bytes;
    if(mode==0){
        pins_cases();
        /* The headset has no memory before admission takes it, and starts nothing without it. */
        nominal_session();check(!os_headset_start()&&!created&&!initialized,45);
        check(os_headset_memory()==1&&taken==1&&given==1&&(uint8_t *)memory==extent&&os_headset_memory()==1&&taken==1,46);
        start_case();os_check_result=1;return;
    }
    /* Admission took the headset's memory (os_large_prepare). */
    session.granted=OS_GRANT_HEADSET;check(os_headset_memory()==1&&memory,47);session.granted=0;
    if(mode==7){owner_case();os_check_result=1;return;}
    /* The started client's handle; os_headset_start runs in mode 0. */
    headset.client=(TaskHandle_t)&memory->client_task;
    headset.owner=radio_granted?(TaskHandle_t)&radio:(TaskHandle_t)&memory->owner_task;
    board.session.workload=mode==10?0:4;
    if(mode==11||mode==14){
        /* Only the original owner advances cleanup. The supervisor wakes
         * both tasks, while pending protocol/device work keeps the descriptor
         * pending during either a normal or a failed return. */
        session.granted=OS_GRANT_HEADSET|(radio_granted?OS_GRANT_RADIO:0);
        model_failing=mode==11;board_up=!model_failing;atomic_store(&headset_quiesce.requested,1);
        check(os_headset_quiesce_pending(),132);
        atomic_store(&headset.requested,1);
        for(unsigned i=0;i<10;++i){
            check(headset_quiesce_step(&headset_quiesce)==OS_QUIESCE_PENDING,133);
            check(stop_calls==i&&deadline_calls==i&&!stopped&&!polled&&!retained,155);
            check(step()==0&&!word&&!board_failed&&owner_notified==i+1&&notified==i+1,134);
            SERVE();check(stop_calls==i+1&&os_headset_quiesce_pending(),156);
        }
        check(quiesce_notes==0&&os_headset_capture(1,&wav,&bytes)==-1,135);
        stop_reply=1;SERVE();
        check(headset_quiesce_step(&headset_quiesce)==OS_QUIESCE_DONE&&!os_headset_quiesce_pending(),136);
        check(!quiesce_notes&&!board_failed&&stop_calls==11&&deadline_calls==11,137);
        for(unsigned i=0;i<10;++i){SERVE();check(headset_quiesce_step(&headset_quiesce)==OS_QUIESCE_DONE,157);}
        check(stop_calls==11&&deadline_calls==11&&!polled&&!retained&&owner_notified==10&&notified==10,138);
        check(progress[OS_SERVICE_BLUETOOTH]==(model_lifetime==A733_LIFETIME_RESIDENT?11u:0u),158);
        os_check_result=1;return;
    }
    if(mode==12){
        /* A failure before the ordered callback still preserves admission. */
        model_failing=1;fail();
        check(os_quiesce_result(&headset_quiesce)==OS_QUIESCE_FAULT&&quiesce_notes==1&&!board_failed,139);
        os_quiesce_complete(&headset_quiesce);SERVE();
        check(headset_quiesce_step(&headset_quiesce)==OS_QUIESCE_FAULT&&!stop_calls&&!deadline_calls,159);
        os_check_result=1;return;
    }
    if(mode==13||mode==17){
        model_failing=mode==13;atomic_store(&headset_quiesce.requested,1);failed_app=1;app.stop_fault=3;
        check(headset_quiesce_step(&headset_quiesce)==OS_QUIESCE_PENDING&&!stop_calls&&!deadline_calls,140);
        SERVE();check(os_quiesce_result(&headset_quiesce)==OS_QUIESCE_FAULT&&headset_quiesce.value==(0x110u|3u<<12)&&
            quiesce_notes==1&&!board_failed&&stop_calls==1&&deadline_calls==1,160);
        os_quiesce_complete(&headset_quiesce);SERVE();
        check(headset_quiesce_step(&headset_quiesce)==OS_QUIESCE_FAULT&&stop_calls==1&&quiesce_notes==1,161);
        os_check_result=1;return;
    }
    if(mode==15){
        /* Radio forwards its independent peer-reset proof, including a
         * negative outcome. Neither direction infers Bluetooth retirement. */
        session.granted=OS_GRANT_HEADSET|OS_GRANT_RADIO;on_owner=1;
        check(os_headset_module_release_ready()==0&&release_calls==1,162);
        module_release=1;check(os_headset_module_release_ready()==1&&release_calls==2,163);
        os_headset_module_down(0);check(module_down==0&&down_calls==1,164);
        os_headset_module_down(1);check(module_down==1&&down_calls==2&&os_quiesce_result(&headset_quiesce)==OS_QUIESCE_PENDING,165);
        module_release=-1;check(os_headset_module_release_ready()==-1&&release_calls==3,166);
        model_failing=1;os_quiesce_fault(&headset_quiesce,1,2,NULL);
        check(os_headset_module_release_ready()==-1&&release_calls==3,167);
        session.granted=0;check(os_headset_module_release_ready()==1&&release_calls==3,168);
        os_headset_module_down(1);check(down_calls==2,169);on_owner=0;
        os_check_result=1;return;
    }
    if(mode==16){
        model_failing=1;atomic_store(&headset_quiesce.requested,1);headset.owner=NULL;
        check(headset_quiesce_step(&headset_quiesce)==OS_QUIESCE_FAULT&&headset_quiesce.value==0x100u&&
            !stop_calls&&!deadline_calls&&!board_failed,170);
        os_check_result=1;return;
    }
    if(mode==8){
        /* The supervised capture window ends capture without a fault; any other
         * stop reason leaves later generations without a capture. */
        segment_flags=BLUETOOTH_SEGMENT_WINDOW;steps(2);
        segment_flags|=BLUETOOTH_SEGMENT_DEADLINE;check(step()<0,110);
        segment_flags=BLUETOOTH_SEGMENT_BACKLOG;check(step()<0,111);
        segment_flags=BLUETOOTH_SEGMENT_LOSS;check(step()<0,112);
        os_check_result=1;return;
    }
    if(mode==9){
        /* A wideband segment is already 16 kHz; its WAV carries it unchanged. */
        generation(1);steps(1);publish_state(0,2);word=0;steps(1);check(tail==1,121);
        check(os_headset_capture(1,&wav,&bytes)==1&&bytes==44+SEGMENT_BYTES&&get32(wav+24)==16000&&
            get32(wav+40)==SEGMENT_BYTES,122);
        for(unsigned k=0;k<SEGMENT_BYTES;++k)check(wav[44+k]==value(1,k),123);
        /* A capture state other than CVSD or mSBC is refused. */
        generation(2);steps(1);publish_state(0,3);word=0;check(step()<0,125);
        os_check_result=1;return;
    }
    if(mode==4){
        check(os_headset_capture(0,&wav,&bytes)<0&&os_headset_capture(1,NULL,&bytes)<0,50);
        check(!os_headset_capture(2,&wav,&bytes)&&os_headset_capture(1,&wav,&bytes)<0&&notified==1,51);
        check(!os_headset_capture(2,&wav,&bytes)&&notified==1,53);
        board_up=0;check(os_headset_capture(2,&wav,&bytes)<0,52);
        os_check_result=1;return;
    }
    if(mode==1){
        /* Workload 4. No request is made while no generation waits. */
        steps(2);check(!word&&!head,60);
        /* A new generation starts recording at once. */
        generation(1);steps(1);check(word==BLUETOOTH_SEGMENT_START&&headset.started==1,63);
        steps(1);check(word==BLUETOOTH_SEGMENT_START,64);
        activation(1);steps(1);check(tail==1&&!word,65);converted(1,1);
        /* Once captured, the client requests nothing and a stray segment does
         * not touch the generation's WAV. */
        steps(2);check(!word,66);publish(0);steps(1);check(tail==2&&!word,67);converted(1,1);
        /* A headset cancel ends the START activation without a segment; the
         * button then records the generation. */
        generation(2);steps(1);check(word==BLUETOOTH_SEGMENT_START,68);
        activation(0);steps(1);check(word==BLUETOOTH_SEGMENT_ACCEPT,69);
        activation(1);steps(1);check(tail==3,70);converted(2,3);
        /* A newer generation upgrades a pending ACCEPT to START. */
        generation(3);steps(1);activation(0);steps(1);check(word==BLUETOOTH_SEGMENT_ACCEPT,71);
        generation(4);steps(1);check(word==BLUETOOTH_SEGMENT_START&&headset.started==4,72);
        activation(1);steps(1);converted(4,4);
        check(os_headset_capture(3,&wav,&bytes)<0&&!board_failed&&!stopped,73);
        os_check_result=1;return;
    }
    if(mode==10){
        /* Workload 0 admits the headset button only. */
        generation(1);steps(1);check(word==BLUETOOTH_SEGMENT_ACCEPT&&!headset.started,130);
        activation(0);steps(1);check(word==BLUETOOTH_SEGMENT_ACCEPT,131);
        activation(1);steps(1);check(tail==1&&!word,132);converted(1,1);
        os_check_result=1;return;
    }
    if(mode==2){
        /* An activation that publishes after the client's take found nothing
         * refuses its request, so ACCEPT is not armed again for a generation
         * that already has a segment. */
        generation(1);steps(1);check(word==BLUETOOTH_SEGMENT_START&&headset.started==1,140);
        word=BLUETOOTH_SEGMENT_ACTIVE;publish_in_take=1;steps(1);check(!word&&head==1&&!tail,141);
        steps(1);check(tail==1&&!word,142);converted(1,1);
        /* START refused while an earlier activation runs is requested again
         * when that activation ends without a segment. */
        generation(2);word=BLUETOOTH_SEGMENT_ACTIVE;steps(1);check(word==BLUETOOTH_SEGMENT_ACTIVE&&headset.started==1,143);
        word=0;steps(1);check(word==BLUETOOTH_SEGMENT_START&&headset.started==2,144);
        activation(1);steps(1);converted(2,2);check(!board_failed&&!stopped,145);
        os_check_result=1;return;
    }
    /* Refusals: a refused borrow or request and a failed application end the
     * session and stop capture. */
    generation(1);steps(1);check(word==BLUETOOTH_SEGMENT_START,74);
    if(mode==3){activation(1);refuse_take=1;}
    else if(mode==5){activation(0);refuse_request=1;}
    else failed_app=1;
    check(step()<0,75);fail();
    check(board_failed&&stopped==1&&atomic_load(&headset.failed)&&os_headset_capture(1,&wav,&bytes)<0,76);
    /* The refused segment stays unacknowledged. */
    check(!tail,77);
    os_check_result=1;
}
