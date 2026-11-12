/* Bluetooth headset capture for the integrated OS image (docs/os-image.md).
 * The Bluetooth application (apps/bluetooth) runs on the board's shared
 * providers: UART1 H4/HCI, secure pairing, the hands-free audio gateway and
 * CVSD or wideband mSBC audio, over SCO or eSCO. Its repeated profile (mode 2
 * for CVSD, mode 3 for mSBC with CVSD fallback) publishes fixed-length
 * segments into two slots. This file, the Bluetooth stack and the mSBC codec
 * are one object library, linked into an image only when its application
 * declares the audio-headset device; every entry does nothing for a session
 * that does not grant it.
 *
 * When the session does not grant the radio (workloads 0 and 4), the
 * application owns the AIC8800 module: one Bluetooth owner task runs its
 * firmware bootstrap over SDIO and its HCI, as in the Bluetooth image. When it
 * grants the radio (workload 1), the board's radio owner owns the module and
 * loads the firmware once; the application attaches to that module as a
 * second client and the radio owner task polls it after each radio poll
 * (os_module_poll). The Bluetooth client then owns only UART1, H4/HCI and its
 * upper layers, with its own recovery latch: its failure leaves the module and
 * Wi-Fi to the radio, and a module failure ends the Bluetooth client.
 *
 * One capture client task is the segment consumer. The headset records one
 * segment per voice-recognition activation, and the client requests one only
 * while a recognition generation waits for audio: in workload 0 it admits the
 * headset button (ACCEPT); in workload 4 each new generation, started by the
 * Enter key, starts recording at once (START), and if that activation ends
 * without a segment, as after a headset cancel, the button records it
 * instead. The next published segment is converted from the borrowed slot to
 * a 16 kHz WAV for the newest generation (apps/os/recognition.patch) and
 * acknowledged. A lost packet stops capture, and its stop flag, like any
 * stop but the supervised capture window, ends the session through
 * board_fail, as do a refused take, a failed Bluetooth owner and a failed
 * board. During a return the original owner stops the protocol, waits for
 * authenticated module reset, and drains UART. Uncertain resources stay
 * retained and the supervisor records the owner's fault. */
#include "os.h"
#include "cpu-return.h"
#include "quiesce.h"
#include "../board/application.h"
#include "../bluetooth/bluetooth.h"
#include "../bluetooth/headset.h"
#include "../bluetooth/pcm.h"
#include "../bluetooth/msbc.h"
#include "owner-memory.h"
#include "a733_cpu_coherence.h"
#include "a733_mapping_check.h"
#include "a733_storage_digest.h"
#include "sunstone_crumb.h"
#include <string.h>
/* The headset's memory, taken from the ledger at admission in a session that grants the headset (os_headset_memory): the
 * Bluetooth application, the task stacks and the WAV, followed by the library's state (bluetooth_headset_memory). The capture
 * client runs at EL0 and reaches the application and the library's state, and the recognizer reads the WAV, so the extent is
 * readable and writable by the application's tasks, as the image's .bss that held it was. app is its first member: the address
 * is null in a session that does not grant the headset, and the entries of apps/bluetooth/headset.c refuse a null application. */
struct headset_memory {
    struct bluetooth_application app;
    StaticTask_t owner_task;     /* the Bluetooth owner when the radio is not granted */
    StackType_t owner_stack[4096];
    StaticTask_t client_task;
    StackType_t client_stack[2048];
    _Alignas(4) uint8_t headroom[SS_CAPTURE_HEADROOM-44];
    uint8_t wav[BLUETOOTH_WAV_BYTES];
};
static struct headset_memory *memory;
#define app (memory->app)
/* The profile that os_headset_identity copied at EL3; its bytes field is zero until then. */
static struct bluetooth_profile profile;
static struct {
    atomic_uint latch; /* With the radio, the attached Bluetooth client's own recovery latch. */
    TaskHandle_t owner,client;
    atomic_uint failed,requested,captured;
    unsigned started; /* The newest generation that requested START. */
    size_t bytes;
} headset;
static enum os_quiesce_result headset_quiesce_step(struct os_quiesce *);
static struct os_quiesce headset_quiesce={.owner=OS_QUIESCE_HEADSET,
    .order=OS_QUIESCE_DEVICES,.step=headset_quiesce_step};
static int live(void){return board_live(NULL)&&!atomic_load_explicit(&headset.failed,memory_order_acquire);}
/* The session grants the radio: the board's radio owner owns the module and the headset attaches to it. */
static int shared(void){return os_granted(OS_GRANT_RADIO);}
static int granted(void){return os_granted(OS_GRANT_HEADSET);}
int os_headset_quiesce_pending(void)
{return os_quiesce_requested(&headset_quiesce)&&os_quiesce_result(&headset_quiesce)==OS_QUIESCE_PENDING;}
static enum os_quiesce_result headset_quiesce_step(struct os_quiesce *q)
{
    if(os_quiesce_result(q)!=OS_QUIESCE_PENDING)return os_quiesce_result(q);
    /* The supervisor never runs protocol or device work on the wrong task. */
    if(!headset.owner)(void)os_quiesce_fault(q,SUNSTONE_CRUMB_FAIL_SITE(BOARD),0x100u,SUNSTONE_CRUMB_HERE());
    else xTaskNotifyGive(headset.owner);
    if(headset.client)xTaskNotifyGive(headset.client);
    return os_quiesce_result(q);
}
static void fail(void)
{
    if(!atomic_exchange_explicit(&headset.failed,1,memory_order_acq_rel))(void)bluetooth_headset_segment_stop(&app);
    if(!os_quiesce_fault(&headset_quiesce,SUNSTONE_CRUMB_FAIL_SITE(BOARD),0x102u,SUNSTONE_CRUMB_HERE()))board_fail(NULL);
}
/* The Bluetooth image's own startup rules (apps/bluetooth/application.c) for
 * the OS workloads, limited to the repeated capture modes. The board
 * session's SID evidence is the profile's SHA-256, as in the Bluetooth image. */
static int headset_workload_valid(unsigned workload)
{
    return workload==0||workload==1||workload==4;
}
int os_headset_profile_valid(const struct a733_board_session *s,const struct bluetooth_profile *p)
{
    if(!s||!p||!headset_workload_valid(s->workload)||!s->sid_authority||s->sid_authority!=p->sid_authority||
       (p->version!=3&&p->version!=4)||!os_headset_mode_valid(p->headset.mode)||!bluetooth_profile_valid(p,s->sid_evidence)||p->topology!=s->topology||
       memcmp(p->radio.fabric,s->fabric,sizeof(s->fabric))||memcmp(p->radio.fabric_hz,s->fabric_hz,sizeof(s->fabric_hz)))
        return 0;
    uint64_t owners[]={s->pmic_owner,s->usb0_owner,s->typec_owner,s->usb2_owner,p->radio.owner,p->uart.owner};
    for(unsigned i=0;i<6;++i){if(!owners[i])return 0;for(unsigned j=0;j<i;++j)if(owners[i]==owners[j])return 0;}
    return 1;
}
/* EL3 with translation and caches disabled, for a session whose device grant names the headset (apps/os/identity.c). */
int os_headset_identity(const struct a733_board_session *s,uint64_t deadline)
{
    const volatile struct bluetooth_profile *source=(const void *)OS_HEADSET_PROFILE_ADDRESS;
    uint32_t bytes=source->bytes;
    if(bytes!=832&&bytes!=sizeof(profile))return 0;
    for(uint32_t i=0;i<bytes;++i)((uint8_t *)&profile)[i]=((const volatile uint8_t *)source)[i];
    if(profile.bytes!=bytes||!os_headset_profile_valid(s,&profile)||
       a733_cpu_coherence_publish((uintptr_t)&profile,sizeof(profile),deadline)!=1||
       a733_cpu_coherence_publish(BLUETOOTH_PROFILE_ADDRESS,0x100000,deadline)!=1){profile.bytes=0;return 0;}
    return 1;
}
const struct bluetooth_profile *os_headset_profile(void)
{return board_radio_chip_id()&&profile.bytes?&profile:NULL;}
int os_headset_capture_frames(uint32_t *frames)
{
    const struct bluetooth_profile *p=os_headset_profile();
    if(!frames||!p||p->headset.samples>UINT32_MAX/2)return 0;
    *frames=2*p->headset.samples;return 1;
}
/* Called by the Bluetooth owner when it retains its transports. */
void bluetooth_recognition_abort(void){fail();}
/* One Bluetooth owner iteration, on the module owner's task. */
static void serve(void)
{
    if(os_quiesce_result(&headset_quiesce)!=OS_QUIESCE_PENDING)return;
    if(os_quiesce_requested(&headset_quiesce)){
        int stopped=bluetooth_application_stop(&app,os_cpu_return_deadline_us());
        if(stopped<0)(void)os_quiesce_fault(&headset_quiesce,SUNSTONE_CRUMB_FAIL_SITE(BOARD),
            0x110u|(app.stop_fault<<12),SUNSTONE_CRUMB_HERE());
        else if(stopped)os_quiesce_complete(&headset_quiesce);
        if(stopped>=0)BOARD_PROGRESS(OS_SERVICE_BLUETOOTH);
        return;
    }
    if(!board_live(NULL))(void)bluetooth_application_retain(&app);
    else if(atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire))
        (void)bluetooth_application_poll(&app);
    /* A failed owner stops reporting, so the resident watchdog resets the board. */
    if(!atomic_load_explicit(&app.failed,memory_order_acquire))BOARD_PROGRESS(OS_SERVICE_BLUETOOTH);
}
void os_module_poll(void){serve();}
int os_headset_module_release_ready(void)
{
    if(!granted())return 1;
    if(os_quiesce_result(&headset_quiesce)==OS_QUIESCE_FAULT)return -1;
    return bluetooth_application_module_release(&app);
}
void os_headset_module_down(int proved)
{if(granted())bluetooth_application_module_down(&app,proved);}
static void owner(void *unused){(void)unused;for(;;){serve();vTaskDelay(1);}}
/* Client task only: one bounded step. 0 continue, -1 failed. */
static int step(void)
{
    if(os_quiesce_requested(&headset_quiesce))return 0;
    struct bluetooth_segment_status status;
    if(bluetooth_headset_segment_status(&app,&status)!=1)return -1;
    /* Capture stopped for any reason but the supervised window would leave
     * later generations without a capture. */
    if(status.flags&~BLUETOOTH_SEGMENT_WINDOW)return -1;
    unsigned g=atomic_load_explicit(&headset.requested,memory_order_acquire);
    struct bluetooth_segment_snapshot segment;const uint8_t *pcm;
    int r=bluetooth_headset_segment_take(&app,&pcm,&segment);
    if(r<0)return -1;
    if(r){
        const struct bluetooth_pcm_snapshot *c=&segment.capture;
        /* State 1 is CVSD at 8 kHz; state 2 is mSBC at 16 kHz. */
        if(c->state!=1&&c->state!=2)return -1;
        /* Requests are made only while a generation waits, so the segment is
         * the newest one's; the WAV of a generation already captured, which
         * the recognizer may be reading, is never rewritten. */
        unsigned wanted=g&&atomic_load_explicit(&headset.captured,memory_order_relaxed)!=g;
        /* mSBC segments are already 16 kHz; CVSD segments are interpolated. */
        if((wanted&&(c->state==2?bluetooth_pcm16_wav(pcm,c->bytes,memory->wav,sizeof(memory->wav),&headset.bytes):
            bluetooth_pcm_wav(pcm,c->bytes,memory->wav,sizeof(memory->wav),&headset.bytes))!=1)||
           bluetooth_headset_segment_ack(&app,segment.ticket)!=1)return -1;
        if(wanted)atomic_store_explicit(&headset.captured,g,memory_order_release);
        return 0;
    }
    if(!g||atomic_load_explicit(&headset.captured,memory_order_relaxed)==g)return 0;
    /* A new keyboard generation upgrades the word to START once; otherwise
     * the client admits the headset button whenever the word is 0. With no
     * segment taken, status.published is the head that take saw, so the
     * request is refused if an activation published after it. START counts
     * only once the word holds it: an activation that was running instead
     * may still end without a segment. */
    unsigned start=board.session.workload==4&&headset.started!=g;
    int requested=bluetooth_headset_segment_request(&app,start?BLUETOOTH_SEGMENT_START:BLUETOOTH_SEGMENT_ACCEPT,status.published);
    if(requested<0)return -1;
    if(start&&requested)headset.started=g;
    return 0;
}
void os_headset_task(void *unused)
{
    (void)unused;
    for(;;){
        /* A failed capture keeps its buffers, stops and stops reporting. */
        if(os_quiesce_result(&headset_quiesce)!=OS_QUIESCE_FAULT){
            if(!atomic_load_explicit(&headset.failed,memory_order_acquire)&&(!live()||step()<0))fail();
            if(!atomic_load_explicit(&headset.failed,memory_order_acquire))BOARD_PROGRESS(OS_SERVICE_HEADSET);
        }
        /* Woken by the Bluetooth owner and by a new generation; the wait stays
         * inside the 1 s progress window. */
        (void)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(250));
    }
}
int os_headset_capture(unsigned g,const uint8_t **wav,size_t *bytes)
{
    if(!live()||os_quiesce_requested(&headset_quiesce)||!g||!wav||!bytes)return -1;
    unsigned requested=atomic_load_explicit(&headset.requested,memory_order_relaxed);
    if(g<requested)return -1;
    if(g>requested){atomic_store_explicit(&headset.requested,g,memory_order_release);if(headset.client)xTaskNotifyGive(headset.client);}
    if(atomic_load_explicit(&headset.captured,memory_order_acquire)!=g)return 0;
    *wav=memory->wav;*bytes=headset.bytes;return live()?1:-1;
}
/* The Bluetooth owner drives the module, UART and pins; the client only
 * copies segments and runs unprivileged. With the radio, the board's radio
 * owner is the Bluetooth owner, and main.c already runs it privileged; the
 * headset then creates no owner task. */
int os_headset_task_privileged(void (*entry)(void *),void *context)
{
    return !context&&entry==owner&&!shared();
}
static int same_latch(const struct a733_pin_latch_handoff *a,const struct a733_pin_latch_handoff *b)
{return a->identity==b->identity&&a->owner==b->owner&&a->known_mask==b->known_mask&&a->value==b->value;}
/* PG0..PG9 and PM0, PM1, PM3 and PM4 join the board pin manager's authority,
 * with the profile's PM latch, as in the Bluetooth image. With the radio, the
 * board already holds PG0..PG5, PM0, PM1 and the session's PM latch for the
 * module; the profile must name that latch and the radio's platform and
 * session owners, and adds UART1 (PG6..PG9) and the wake pins PM3 and PM4. */
int os_headset_pins(struct a733_pin_handoff *pins)
{
    const struct os_session *s=os_session_admitted();
    if(s&&!granted())return 1; /* no headset session: the pins stay unread */
    const struct bluetooth_profile *p=os_headset_profile();
    if(!pins||!s||!p||!os_headset_profile_valid(&s->board,p)||p->radio.owner==s->thermal_owner||
       p->uart.owner==s->thermal_owner)return 0;
    if(shared()){
        const struct board_radio_profile *r=os_radio_profile();
        if(!r||pins->authorized[A733_PIN_G]!=0x3f||pins->authorized[A733_PIN_M]!=3||!same_latch(&pins->latch[1],&p->pm_latch)||
           p->radio.owner!=r->platform.owner||p->session_owner!=r->session_owner||p->uart.owner==r->ce.owner)return 0;
    }else if(pins->authorized[A733_PIN_G]||pins->authorized[A733_PIN_M])return 0;
    static const struct a733_mapping_rule wb={A733_MAPPING_READ,1,{{A733_MAIR_NORMAL_WB,A733_SH_INNER}},0,1};
    if(!a733_mapping_check_pages(aarch64_device_translate,BLUETOOTH_PROFILE_ADDRESS,BLUETOOTH_PROFILE_ADDRESS,
           UINT64_C(0x43100000)-BLUETOOTH_PROFILE_ADDRESS,&wb))return 0;
    pins->authorized[A733_PIN_G]=0x3ff;pins->authorized[A733_PIN_M]=p->version==4?0x1f:0x1b;pins->latch[1]=p->pm_latch;
    return 1;
}
int os_headset_memory(void)
{
    if(!granted()||memory)return 1;
    size_t own=(sizeof(struct headset_memory)+63)&~(size_t)63;
    uint8_t *m=os_owner_memory(own+bluetooth_headset_state_bytes,64,OS_MEMORY_EL0_WRITE);
    if(!m||bluetooth_headset_memory(m+own)!=1)return 0;
    memory=(struct headset_memory *)(void *)m;return 1;
}
int os_headset_start(void)
{
    const struct os_session *s=os_session_admitted();
    if(s&&!granted())return 1;
    const struct bluetooth_profile *p=os_headset_profile();
    if(!s||!p||!memory||headset.client||!board_live(NULL))return 0;
    /* The recognizer's start entry ran runtime_init, so libsbc can allocate. */
    if(p->headset.mode==3&&bluetooth_msbc_init()!=1)return 0;
    if(!os_quiesce_register(&headset_quiesce))return 0;
    headset.client=xTaskCreateStatic(os_headset_task,"os-headset",2048,NULL,4,memory->client_stack,&memory->client_task);
    if(!headset.client)return 0;
    if(shared()){
        /* main.c built the radio and created its owner (handle 5) before this list. */
        struct board_radio *radio=os_board_radio();
        headset.owner=board.handles[5];
        atomic_init(&headset.latch,0);
        if(!radio||!headset.owner||bluetooth_application_attach(&app,p,&providers.shared,board_radio_chip_id(),headset.owner,
               &headset.latch,board.session.session_start_ticks/24,board.session.deadline_us,board.session.sid_evidence,
               &radio->native,&radio->session)!=1)return 0;
    }else{
        headset.owner=xTaskCreateStatic(owner,"bluetooth",4096,NULL,4,memory->owner_stack,&memory->owner_task);
        if(!headset.owner||bluetooth_application_init(&app,p,&providers.shared,board_radio_chip_id(),headset.owner,
               &board.watchdog_state,board.session.session_start_ticks/24,board.session.deadline_us,board.session.sid_evidence)!=1)
            return 0;
    }
    return bluetooth_headset_segment_bind(&app,headset.client)==1;
}
