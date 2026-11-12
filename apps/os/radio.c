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
static unsigned shutdown_started;
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
/* One owner pass. The existing board driver admits shutdown only after a
 * completed scan with an idle transport. A retained or unimplemented stop
 * never becomes a successful retirement merely because the scan has ended. */
int os_radio_quiesce_poll(struct board_radio *r,int result)
{
    if(!os_radio_quiesce_requested())return 0;
    if(os_quiesce_result(&radio_quiesce)!=OS_QUIESCE_PENDING)return 2;
    if(!r->config.scan_only||r->config.entropy_only){radio_quiesce_refuse(0x902u);return 2;}
    if(result==BOARD_RADIO_RETAINED||result==BOARD_RADIO_INVALID){
        radio_quiesce_refuse(r->platform.fail_line?r->platform.fail_line<<16|(r->platform.fail_stage&0xffffu):
            r->session.fail_line<<16|0x100u|(r->session.fail_stage&0xfeffu));
        return 2;
    }
    if(result==BOARD_RADIO_DOWN){
        /* DOWN is the normal driver's completed readback sequence. The
         * platform_clean predicate describes its separate failure cleanup. */
        if(r->platform.fault||r->platform.held||r->platform.retained||r->platform.pmic_request.id||
           r->session.transport.pending||r->session.transport.held){radio_quiesce_refuse(0x903u);return 2;}
        os_wifi_credential_poll();
        if(os_wifi_credential_wiped()){os_quiesce_complete(&radio_quiesce);return 2;}
        return 1;
    }
    if(!shutdown_started&&result!=BOARD_RADIO_SCANNED){radio_quiesce_refuse(0x904u);return 2;}
    if(result==BOARD_RADIO_SCANNED&&!shutdown_started){
        uint64_t now=a733_smp_counter()/24;
        uint64_t deadline=now+OS_FAILURE_RETURN_US;
        if(deadline>board.session.deadline_us-10000)deadline=board.session.deadline_us-10000;
        shutdown_started=board_radio_shutdown(r,deadline);
    }
    return 1;
}
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
