/* Radio station entry of the integrated OS image (docs/os-image.md). The
 * board composition owns the radio in workload 1: main.c starts the radio
 * owner and the network client, with the crypto engine entropy source that
 * the WPA2 supplicant uses, before the start list runs. It refuses radio and
 * crypto engine owners equal to a board owner, but it cannot see the owners
 * the OS session adds. This entry refuses those collisions before any
 * provider is constructed. */
#include "os.h"
#include "cpu-return.h"
#include "quiesce.h"
#include "radio-quiesce.h"
#include "../board/application.h"
#include "a733_mapping_check.h"
#include "a733_storage_digest.h"
#include "sunstone_crumb.h"
#include "sunstone_wifi_credential.h"
#include <string.h>
static enum os_quiesce_result radio_quiesce_step(struct os_quiesce *);
static struct os_quiesce radio_quiesce={
    .owner=OS_QUIESCE_RADIO,.order=OS_QUIESCE_DEVICES,.step=radio_quiesce_step
};
static unsigned shutdown_started,module_notified;
extern int os_headset_module_release_ready(void) __attribute__((weak));
extern void os_headset_module_down(int proved) __attribute__((weak));
static enum os_quiesce_result radio_quiesce_step(struct os_quiesce *q)
{
    if(os_quiesce_result(q)==OS_QUIESCE_DONE)return OS_QUIESCE_DONE;
    if(board.handles[5])xTaskNotifyGive(board.handles[5]);
    if(board.handles[6])xTaskNotifyGive(board.handles[6]);
    return os_quiesce_result(q);
}
int os_radio_quiesce_requested(void)
{return os_quiesce_requested(&radio_quiesce);}
int os_radio_quiesce_parked(void)
{return os_radio_quiesce_requested()&&os_quiesce_result(&radio_quiesce)!=OS_QUIESCE_PENDING;}
int os_radio_quiesce_fault(uint32_t value,const void *caller)
{return os_quiesce_fault(&radio_quiesce,SUNSTONE_CRUMB_FAIL_BOARD<<12|0x901u,value,caller);}
static void radio_quiesce_refuse(uint32_t value)
{
    if(!os_radio_quiesce_fault(value,SUNSTONE_CRUMB_HERE()))
        board_fail_at(SUNSTONE_CRUMB_FAIL_BOARD<<12|0x901u,value,SUNSTONE_CRUMB_HERE());
}
static void radio_module_stopped(struct board_radio *r)
{
    if(!module_notified&&os_headset_module_down&&os_granted(OS_GRANT_HEADSET)){
        os_headset_module_down(a733_radio_session_peer_stopped(&r->session));module_notified=1;
    }
}
static enum board_radio_result radio_terminal_result(struct board_radio *r,enum a733_radio_session_result result)
{
    enum board_radio_result terminal=board_radio_stop_result(r,result);
#ifdef SUNSTONE_A733_OS_RADIO_SCAN
    os_radio_scan_step(r,(int)terminal);
#else
    (void)terminal;
#endif
    return terminal;
}
/* The owner bypasses ordinary board/session polling as soon as return is
 * requested. The accepted SDIO transfer drains while Bluetooth closes; only
 * its protocol release authorizes the shared module's physical shutdown. */
int os_radio_quiesce_poll(struct board_radio *r,int result)
{
    (void)result;
    if(!os_radio_quiesce_requested())return 0;
    if(os_quiesce_result(&radio_quiesce)!=OS_QUIESCE_PENDING){
        if(os_quiesce_result(&radio_quiesce)==OS_QUIESCE_FAULT)
            (void)radio_terminal_result(r,A733_SESSION_RETAINED);
        radio_module_stopped(r);return 2;
    }
    if(!shutdown_started){
        uint64_t deadline=os_cpu_return_deadline_us();
        if(!deadline)return 1;
        if(r->config.entropy_only){
            radio_quiesce_refuse(0x905u);(void)radio_terminal_result(r,A733_SESSION_RETAINED);radio_module_stopped(r);return 2;
        }
        if(!a733_radio_session_stop(&r->session,deadline)){
            radio_quiesce_refuse(0x904u);(void)radio_terminal_result(r,A733_SESSION_RETAINED);radio_module_stopped(r);return 2;
        }
        shutdown_started=1;
    }
    if(os_granted(OS_GRANT_HEADSET)){
        int release=os_headset_module_release_ready?os_headset_module_release_ready():-1;
        if(release<0){
            (void)a733_radio_session_retain(&r->session);
            radio_quiesce_refuse(0x906u);(void)radio_terminal_result(r,A733_SESSION_RETAINED);radio_module_stopped(r);return 2;
        }
        if(release&&!a733_radio_session_stop_release(&r->session)){
            radio_quiesce_refuse(0x907u);(void)radio_terminal_result(r,A733_SESSION_RETAINED);radio_module_stopped(r);return 2;
        }
    }
    enum a733_radio_session_result stopped=a733_radio_session_poll(&r->session);
    if(stopped==A733_SESSION_RETAINED||stopped==A733_SESSION_INVALID){
        radio_quiesce_refuse(r->platform.fail_line?r->platform.fail_line<<16|(r->platform.fail_stage&0xffffu):
            r->session.fail_line<<16|0x100u|(r->session.fail_stage&0xfeffu));
        (void)radio_terminal_result(r,stopped);radio_module_stopped(r);return 2;
    }
    if(stopped==A733_SESSION_DOWN){
        if(radio_terminal_result(r,stopped)!=BOARD_RADIO_DOWN){
            radio_quiesce_refuse(0x908u);radio_module_stopped(r);return 2;
        }
        radio_module_stopped(r);
        /* Full station/association additionally owns CE entropy and DMA.
         * WLAN shutdown supplies no global CE drain or memory-release proof. */
        if(!r->config.scan_only||r->config.associate){radio_quiesce_refuse(0x905u);return 2;}
        os_wifi_credential_poll();
        if(os_wifi_credential_wiped()){os_quiesce_complete(&radio_quiesce);return 2;}
    }
    return 1;
}
#ifdef SUNSTONE_A733_RADIO_IRQ
extern struct board_radio *os_board_radio(void);
static unsigned radio_wait_registered[2];
void os_radio_irq(void){board_radio_irq(os_board_radio());}
static int radio_blocked_until(unsigned service,unsigned index,uint64_t until)
{
    if(xTaskGetCurrentTaskHandle()!=board.handles[index]||!board.handles[index])return 0;
    if(!radio_wait_registered[index-5]){
        if(!board_service_deadline_register(service))return 0;
        radio_wait_registered[index-5]=1;
    }
    return board_service_blocked_until(service,until);
}
int os_radio_owner_blocked_until(uint64_t until)
{return radio_blocked_until(OS_SERVICE_RADIO,5,until);}
int os_radio_client_blocked_until(uint64_t until)
{return radio_blocked_until(OS_SERVICE_NETWORK,6,until);}
#endif
static struct board_radio_profile profile;
static unsigned admitted;
/* The board's own radio tasks are privileged by main.c. */
int os_radio_task_privileged(void (*entry)(void *),void *context){(void)entry;(void)context;return 0;}
int os_radio_pins(struct a733_pin_handoff *pins)
{
    const struct os_session *s=os_session_admitted();
    if(!pins||!s)return 0;
    if(!(s->granted&OS_GRANT_RADIO))return 1; /* no radio session: the pins and the profile stay unread */
    if(s->board.workload!=OS_RADIO_WORKLOAD||s->board.radio_profile_address!=UINT64_C(0x43000000)||
       s->board.radio_profile_bytes!=sizeof(profile))return 0;
    static const struct a733_mapping_rule wb={A733_MAPPING_READ,1,{{A733_MAIR_NORMAL_WB,A733_SH_INNER}},0,1};
    if(!a733_mapping_check_pages(aarch64_device_translate,0x43000000,0x43000000,sizeof(profile),&wb))return 0;
    /* The profile copy main.c authenticates later has the same owners. */
    const volatile uint8_t *source=(const void *)UINT64_C(0x43000000);
    for(size_t i=0;i<sizeof(profile);++i)((uint8_t *)&profile)[i]=source[i];
    uint8_t hash[32];storage_digest_bytes(&profile,sizeof(profile),hash);
    if(memcmp(hash,s->board.radio_profile_digest,32))return 0;
    uint64_t owners[]={s->thermal_owner,(s->granted&OS_GRANT_AUDIO)?s->audio.dma.owner:0};
    for(unsigned i=0;i<sizeof(owners)/sizeof(owners[0]);++i)
        if(owners[i]&&(owners[i]==profile.platform.owner||owners[i]==profile.ce.owner))return 0;
    admitted=1;return 1;
}
const struct board_radio_profile *os_radio_profile(void){return admitted?&profile:NULL;}
/* main.c created both radio tasks and started the radio before this list. */
int os_radio_start(void)
{
    const struct os_session *s=os_session_admitted();
    if(s&&!(s->granted&OS_GRANT_RADIO))return 1;
    if(!s||s->board.workload!=OS_RADIO_WORKLOAD||!board.handles[5]||!board.handles[6])return 0;
    return !os_returning()||os_quiesce_register(&radio_quiesce);
}
