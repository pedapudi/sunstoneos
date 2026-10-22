/* Bluetooth headset capture for the integrated OS image (docs/os-image.md).
 * The Bluetooth application (apps/bluetooth) runs unchanged on the board's
 * shared providers, with one owner task as in the Bluetooth image: AIC8800
 * firmware bootstrap over SDIO, UART1 H4/HCI, secure pairing, the hands-free
 * audio gateway and CVSD audio over SCO or eSCO. Its repeated profile (mode 2)
 * publishes fixed-length 8 kHz segments into two slots.
 *
 * One capture client task is the segment consumer. It copies and acknowledges
 * every segment as it arrives, so the headset never stops for backlog. The
 * first segment that starts after a recognition generation was requested is
 * converted to a 16 kHz WAV for the recognizer (apps/os/recognition.patch);
 * every other segment is discarded. A segment starts after the request when
 * its ticket exceeds the segment being filled at the request, which is at most
 * one ticket past the published count. A refused copy, a lost packet, a failed
 * Bluetooth owner or a failed board ends the session through board_fail. The
 * Bluetooth owner then retains its transports and the client keeps its
 * buffers until manual power removal; this build is supervised only. */
#include "os.h"
#include "../board/application.h"
#include "../bluetooth/bluetooth.h"
#include "../bluetooth/headset.h"
#include "../bluetooth/pcm.h"
#include "a733_mapping_check.h"
#include "a733_storage_digest.h"
#include <string.h>
static struct bluetooth_application app;
static struct {
    StaticTask_t owner_task,client_task;
    StackType_t owner_stack[4096],client_stack[2048];
    TaskHandle_t owner,client;
    atomic_uint failed,requested,captured;
    unsigned generation;
    uint64_t first_ticket;
    size_t bytes;
    uint8_t pcm[BLUETOOTH_PCM_BYTES];
    _Alignas(4) uint8_t wav[BLUETOOTH_WAV_BYTES];
} headset;
static int live(void){return board_live(NULL)&&!atomic_load_explicit(&headset.failed,memory_order_acquire);}
static void fail(void)
{
    if(!atomic_exchange_explicit(&headset.failed,1,memory_order_acq_rel))(void)bluetooth_headset_segment_stop(&app);
    board_fail(NULL);
}
/* The Bluetooth image's own startup rules (apps/bluetooth/application.c) for
 * the OS workloads, limited to repeated CVSD capture. The board session's SID
 * evidence is the profile's SHA-256, as in the Bluetooth image. */
int os_headset_profile_valid(const struct a733_board_session *s,const struct bluetooth_profile *p)
{
    if(!s||!p||!os_workload_valid(s->workload)||!s->sid_authority||s->sid_authority!=p->sid_authority||
       p->version!=3||p->headset.mode!=2||!bluetooth_profile_valid(p,s->sid_evidence)||p->topology!=s->topology||
       memcmp(p->radio.fabric,s->fabric,sizeof(s->fabric))||memcmp(p->radio.fabric_hz,s->fabric_hz,sizeof(s->fabric_hz)))
        return 0;
    uint64_t owners[]={s->pmic_owner,s->usb0_owner,s->typec_owner,s->usb2_owner,p->radio.owner,p->uart.owner};
    for(unsigned i=0;i<6;++i){if(!owners[i])return 0;for(unsigned j=0;j<i;++j)if(owners[i]==owners[j])return 0;}
    return 1;
}
/* Called by the Bluetooth owner when it retains its transports. */
void bluetooth_recognition_abort(void){fail();}
static void owner(void *unused)
{
    (void)unused;for(;;){
        if(!board_live(NULL))(void)bluetooth_application_retain(&app);
        else if(atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire))
            (void)bluetooth_application_poll(&app);
        vTaskDelay(1);
    }
}
/* Copies the whole oldest segment into headset.pcm; 1 copied, -1 refused. */
static int copy(const struct bluetooth_segment_snapshot *first)
{
    const struct bluetooth_pcm_snapshot *c=&first->capture;
    if(c->state!=1||!c->bytes||(c->bytes&1)||c->bytes>sizeof(headset.pcm)||c->loss_packets)return -1;
    uint8_t chunk[1024];
    for(uint32_t offset=0;offset<c->bytes;){
        size_t count=c->bytes-offset,copied=0;if(count>sizeof(chunk))count=sizeof(chunk);
        struct bluetooth_segment_snapshot s;
        if(bluetooth_headset_segment_copy(&app,first->ticket,offset,count,chunk,&copied,&s)!=1||copied!=count||
           memcmp(&s,first,sizeof(s)))return -1;
        memcpy(headset.pcm+offset,chunk,count);offset+=(uint32_t)count;
    }
    uint8_t hash[32];storage_digest_bytes(headset.pcm,c->bytes,hash);
    return memcmp(hash,c->sha256,32)?-1:1;
}
/* Client task only: one bounded step. 0 continue, -1 failed. */
static int step(void)
{
    unsigned g=atomic_load_explicit(&headset.requested,memory_order_acquire);
    if(g!=headset.generation){
        struct bluetooth_segment_status status;
        if(bluetooth_headset_segment_status(&app,&status)!=1)return -1;
        headset.generation=g;headset.first_ticket=status.published+2;
    }
    struct bluetooth_segment_snapshot segment;uint8_t chunk[1024];size_t copied;
    int r=bluetooth_headset_segment_copy(&app,0,0,2,chunk,&copied,&segment);
    if(r<=0)return r;
    if(segment.capture.loss_packets)return -1;
    int wanted=g&&atomic_load_explicit(&headset.captured,memory_order_relaxed)!=g&&segment.ticket>=headset.first_ticket;
    if(wanted&&copy(&segment)!=1)return -1;
    if(bluetooth_headset_segment_ack(&app,segment.ticket)!=1)return -1;
    if(!wanted)return 0;
    if(bluetooth_pcm_wav(headset.pcm,segment.capture.bytes,headset.wav,sizeof(headset.wav),&headset.bytes)!=1)return -1;
    atomic_store_explicit(&headset.captured,g,memory_order_release);
    return 0;
}
void os_headset_task(void *unused)
{
    (void)unused;
    for(;;){
        /* A failed capture keeps its buffers and stops. */
        if(!atomic_load_explicit(&headset.failed,memory_order_acquire)&&(!live()||step()<0))fail();
        vTaskDelay(1);
    }
}
int os_headset_capture(unsigned g,const uint8_t **wav,size_t *bytes)
{
    if(!live()||!g||!wav||!bytes)return -1;
    unsigned requested=atomic_load_explicit(&headset.requested,memory_order_relaxed);
    if(g<requested)return -1;
    if(g>requested)atomic_store_explicit(&headset.requested,g,memory_order_release);
    if(atomic_load_explicit(&headset.captured,memory_order_acquire)!=g)return 0;
    *wav=headset.wav;*bytes=headset.bytes;return live()?1:-1;
}
/* The Bluetooth owner drives the module, UART and pins; the client only
 * copies segments and runs unprivileged. */
int os_headset_task_privileged(void (*entry)(void *),void *context){return !context&&entry==owner;}
/* PG0..PG9 and PM0, PM1, PM3 and PM4 join the board pin manager's authority,
 * with the profile's PM latch, as in the Bluetooth image. */
int os_headset_pins(struct a733_pin_handoff *pins)
{
    const struct os_session *s=os_session_admitted();
    const struct bluetooth_profile *p=os_headset_profile();
    if(!pins||!s||!p||!os_headset_profile_valid(&s->board,p)||p->radio.owner==s->thermal_owner||
       p->uart.owner==s->thermal_owner||pins->authorized[A733_PIN_G]||pins->authorized[A733_PIN_M])return 0;
    static const struct a733_mapping_rule wb={A733_MAPPING_READ,1,{{A733_MAIR_NORMAL_WB,A733_SH_INNER}},0,1};
    if(!a733_mapping_check_pages(aarch64_device_translate,BLUETOOTH_PROFILE_ADDRESS,BLUETOOTH_PROFILE_ADDRESS,
           UINT64_C(0x43100000)-BLUETOOTH_PROFILE_ADDRESS,&wb))return 0;
    pins->authorized[A733_PIN_G]=0x3ff;pins->authorized[A733_PIN_M]=0x1b;pins->latch[1]=p->pm_latch;
    return 1;
}
int os_headset_start(void)
{
    const struct os_session *s=os_session_admitted();
    const struct bluetooth_profile *p=os_headset_profile();
    if(!s||!p||headset.owner||!board_live(NULL))return 0;
    headset.owner=xTaskCreateStatic(owner,"bluetooth",4096,NULL,4,headset.owner_stack,&headset.owner_task);
    headset.client=xTaskCreateStatic(os_headset_task,"os-headset",2048,NULL,4,headset.client_stack,&headset.client_task);
    return headset.owner&&headset.client&&
        bluetooth_application_init(&app,p,&providers.shared,board_radio_chip_id(),headset.owner,&board.watchdog_state,
            board.session.session_start_ticks/24,board.session.deadline_us,board.session.sid_evidence)==1&&
        bluetooth_headset_segment_bind(&app,headset.client)==1;
}
