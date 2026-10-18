#include "a733_board_application.h"
#include "session.h"
#include "a733_device_memory_layout.h"
#include "a733_cpu_coherence.h"
#include "a733_storage_digest.h"
#include <string.h>
static int present(const uint8_t *p,size_t n)
{unsigned v=0;for(size_t i=0;i<n;++i)v|=p[i];return v!=0;}
#ifdef SUNSTONE_GPU_SUPERVISED
int a733_board_session_valid(const struct a733_board_session *s,uint64_t now)
{
    (void)now;
    if(!s||s->magic!=A733_BOARD_SUPERVISED_SESSION_MAGIC||s->version!=2||s->bytes!=sizeof(*s)||
       !s->epoch||!s->generation||!s->lease||!present(s->image,32)||!present(s->qualification,32)||
       !present(s->recovery,32)||!present(s->wiring,32)||!present(s->memory,32)||
       !s->topology||!s->cold_master||!s->memory_authority||!s->recovery_authority||
       s->minimum_reload_us||s->maximum_recovery_us||s->guard_us||s->reset_by_us||
       s->rtc_control||s->rtc_divider||s->rtc_source||s->reserved0||s->deadline_us||
       s->session_start_ticks||s->reload_after_ticks||s->workload!=2||s->reserved_workload||
       s->sid_authority||present(s->sid_evidence,32)||s->radio_profile_address||s->radio_profile_bytes||
       present(s->radio_profile_digest,32)||s->latch_m_identity||s->latch_m_owner||s->latch_m_known_mask||s->latch_m_value||
       s->latch_identity||s->latch_owner||s->latch_known_mask||s->latch_value||s->typec_owner||s->usb2_owner||
       s->source_min_mv||s->source_max_mv||s->source_ma||s->zero_threshold_max_mv||s->fusb_device_id||
       s->usb2_revision||s->pck_client||s->usb_supply_uv[4]||present(&s->typec_pins[0][0],sizeof(s->typec_pins))||
       present(s->reserved,sizeof(s->reserved))||!s->pmic_owner||!s->usb0_owner||s->pmic_owner==s->usb0_owner||
#ifdef SUNSTONE_GPU_SINGLE_CORE
       (s->usb0_cold_reference!=1&&s->usb0_cold_reference!=2)||
#else
       s->usb0_cold_reference!=1||
#endif
       s->usb_supply_uv[0]!=3300000||s->usb_supply_uv[1]!=3300000)return 0;
    for(unsigned i=0;i<3;++i)if(!s->fabric_hz[i])return 0;
    return 1;
}
#else
int a733_board_session_valid(const struct a733_board_session *s,uint64_t now)
{
    if(!s||s->magic!=A733_BOARD_SESSION_MAGIC||s->version!=1||s->bytes!=sizeof(*s)||
       !s->epoch||!s->generation||!s->lease||!present(s->image,32)||
       !present(s->qualification,32)||!present(s->wiring,32)||!present(s->memory,32)||
       !present(s->recovery,32)||memcmp(s->qualification,s->recovery,32)||
       s->reserved0||s->reserved_workload||s->workload>1||present(s->reserved,sizeof(s->reserved))||
       !s->latch_identity||!s->latch_owner||s->latch_known_mask!=0x3fff||(s->latch_value&~UINT32_C(0x3fff))||
       !s->sid_authority||!present(s->sid_evidence,32)||!s->topology||!s->cold_master||!s->memory_authority||!s->recovery_authority||
       !s->guard_us||s->minimum_reload_us<=s->guard_us||
       s->minimum_reload_us-s->guard_us<=1000000||
       s->maximum_recovery_us<s->minimum_reload_us||s->reset_by_us<=s->maximum_recovery_us||
       s->reset_by_us>UINT64_MAX/24||s->deadline_us<=10000||
       s->deadline_us>s->reset_by_us-s->maximum_recovery_us||now>=s->deadline_us*24||
       s->reload_after_ticks<s->reload_before_ticks||s->reload_after_ticks-s->reload_before_ticks>24000||
       now<s->reload_after_ticks||s->reload_before_ticks/24>UINT64_MAX-s->minimum_reload_us||
       (!s->workload&&s->deadline_us>s->reload_before_ticks/24+s->minimum_reload_us-s->guard_us-1000)||
       s->source_min_mv<4750||s->source_max_mv>5500||s->source_min_mv>s->source_max_mv||
       s->source_ma<500||!s->zero_threshold_max_mv||s->zero_threshold_max_mv>800||
       s->fusb_device_id<0x80||s->fusb_device_id>0x82||s->pck_client>=16||
       s->usb0_cold_reference!=1||!s->usb2_revision)return 0;
    if(s->workload){
        if(s->radio_profile_address!=UINT64_C(0x43000000)||!s->radio_profile_bytes||
           s->radio_profile_bytes>65536||!present(s->radio_profile_digest,32)||
           !s->latch_m_identity||!s->latch_m_owner||s->latch_m_known_mask!=0x3f||
           (s->latch_m_value&~UINT32_C(0x3f)))return 0;
    }else if(s->radio_profile_address||s->radio_profile_bytes||present(s->radio_profile_digest,32)||
             s->latch_m_identity||s->latch_m_owner||s->latch_m_known_mask||s->latch_m_value)return 0;
    uint64_t owner[]={s->pmic_owner,s->usb0_owner,s->typec_owner,s->usb2_owner};
    for(unsigned i=0;i<4;++i){if(!owner[i])return 0;for(unsigned j=0;j<i;++j)if(owner[i]==owner[j])return 0;}
    if(s->usb_supply_uv[0]!=3300000||s->usb_supply_uv[1]!=3300000)return 0;
    for(unsigned i=0;i<3;++i)if(!s->fabric_hz[i])return 0;
    return 1;
}
#endif
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
    const struct a733_board_session *s=(const void *)A733_BOARD_SESSION_ADDRESS;
    const struct a733_device_memory_layout layout={A733_DEVICE_MEMORY_LAYOUT,
        A733_DEVICE_WB_START,A733_DEVICE_WB_END,A733_DEVICE_NC_START,A733_DEVICE_NC_END};
    if(!c||c->magic!=A733_SMP_CONTRACT_MAGIC||c->version!=2||c->bytes!=sizeof(struct gpu_boot_contract)||
       !a733_device_memory_handoff_valid(&layout,c->reserved)||!a733_board_session_valid(s,now)||
#ifndef SUNSTONE_GPU_SUPERVISED
       s->workload||
#else
       s->workload!=2||
#endif

       c->epoch!=s->epoch||c->generation!=s->generation||c->inverse_generation!=~c->generation||
       c->lease!=s->lease||memcmp(c->build_id,a733_smp_build_id,32)||memcmp(s->image,a733_smp_build_id,32)||
#ifndef SUNSTONE_GPU_SUPERVISED
       c->armed_at!=s->reload_before_ticks||c->startup_deadline!=c->armed_at+A733_SMP_COUNTER_HZ||
       now>=c->startup_deadline||c->startup_deadline>=c->diagnostic_deadline||
       c->diagnostic_deadline!=s->deadline_us*24||c->expires_at!=s->reset_by_us*24||
       c->watchdog_mode!=0x41||c->watchdog_configuration!=1||c->watchdog_output!=2||
       c->fel_flag!=0x5aa5a55a||c->redirect_flag!=0xfa50392f||c->redirect_entry!=0x20||c->alternate_flag)return 0;
#else
       c->armed_at>now||c->armed_at>UINT64_MAX-A733_GPU_SUPERVISED_TICKS||
       c->startup_deadline!=c->armed_at+A733_SMP_COUNTER_HZ||now>=c->startup_deadline||
       c->diagnostic_deadline!=c->armed_at+A733_GPU_SUPERVISED_TICKS||c->expires_at!=c->diagnostic_deadline||
       c->watchdog_mode||c->watchdog_configuration||c->watchdog_output||c->fel_flag||
       c->redirect_flag||c->redirect_entry||c->alternate_flag)return 0;
    const uint8_t *entry=gpu_application_entry_digest(c);
    if(!entry||memcmp(s->qualification,entry,32))return 0;
#endif
    uint8_t session_digest[32];storage_digest_bytes(s,sizeof(*s),session_digest);
    const uint8_t *bound=gpu_application_contract_digest(c);
    if(!bound)return 0;
    for(unsigned i=0;i<32;++i)if(session_digest[i]!=bound[i])return 0;
    uint64_t owner[]={s->pmic_owner,s->usb0_owner,s->typec_owner,s->usb2_owner};
    for(unsigned i=0;i<4;++i)if(owner[i]>=9&&owner[i]<=11)return 0;
    return 1;
}
int a733_smp_power_address(unsigned core,uintptr_t address,int write)
{
    if(core<1 || core>=8)return 0;
    uintptr_t cfg=UINT32_C(0x08001000)+(uintptr_t)core*0x1000;
    if(address==cfg || address==cfg+4 || address==cfg+8 ||
       address==UINT32_C(0x07050200)+core*4 ||
       address==UINT32_C(0x07050220)+core*4)return 1;
    return !write && address==UINT32_C(0x07052008)+(uintptr_t)core*0x1000;
}
