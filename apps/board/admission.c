#include "a733_board_application.h"
#include "boot.h"
#include "a733_storage_digest.h"
#include <string.h>
static int present(const uint8_t *p,size_t n)
{unsigned v=0;for(size_t i=0;i<n;++i)v|=p[i];return v!=0;}
int a733_board_session_valid(const struct a733_board_session *s,uint64_t now)
{
#ifdef SUNSTONE_BOARD_SUPERVISED
    (void)now;
#endif
    if(!s||s->magic!=A733_BOARD_SESSION_MAGIC||s->version!=A733_BOARD_SESSION_VERSION||s->bytes!=sizeof(*s)||
       !s->epoch||!s->generation||!s->lease||!present(s->image,32)||
       !present(s->qualification,32)||!present(s->wiring,32)||!present(s->memory,32)||
       !present(s->recovery,32)||memcmp(s->qualification,s->recovery,32)||
       s->reserved0||s->reserved_workload||s->workload>
#ifdef SUNSTONE_BOARD_SUPERVISED
       3
#else
       1
#endif
       ||present(s->reserved,sizeof(s->reserved))||
#ifndef SUNSTONE_BOARD_SUPERVISED
       !s->sid_authority||!present(s->sid_evidence,32)||
#endif
       !s->topology||!s->cold_master||!s->memory_authority||!s->recovery_authority||
#ifndef SUNSTONE_BOARD_SUPERVISED
       !s->guard_us||s->minimum_reload_us<=s->guard_us||
       s->minimum_reload_us-s->guard_us<=1000000||
       s->maximum_recovery_us<s->minimum_reload_us||s->reset_by_us<=s->maximum_recovery_us||
       s->reset_by_us>UINT64_MAX/24||s->deadline_us<=10000||
       s->deadline_us>s->reset_by_us-s->maximum_recovery_us||now>=s->deadline_us*24||
       s->reload_after_ticks<s->reload_before_ticks||s->reload_after_ticks-s->reload_before_ticks>24000||
       now<s->reload_after_ticks||s->reload_before_ticks/24>UINT64_MAX-s->minimum_reload_us||
       (!s->workload&&s->deadline_us>s->reload_before_ticks/24+s->minimum_reload_us-s->guard_us-1000)||
#else
       s->minimum_reload_us||s->maximum_recovery_us||s->guard_us||s->reset_by_us||
       s->rtc_control||s->rtc_divider||s->rtc_source||s->deadline_us||s->session_start_ticks||s->reload_after_ticks||
#endif
#if defined(SUNSTONE_BOARD_SUPERVISED) && defined(SUNSTONE_SD_SINGLE_CORE)
       (s->usb0_cold_reference!=1 && (s->usb0_cold_reference!=2 || s->workload!=2))
#else
       s->usb0_cold_reference!=1
#endif
       )return 0;
    if(a733_board_host_workload(s->workload)){
        if(!s->latch_identity||!s->latch_owner||s->latch_known_mask!=0x3fff||(s->latch_value&~UINT32_C(0x3fff))||
       s->source_min_mv<4750||s->source_max_mv>5500||s->source_min_mv>s->source_max_mv||
       s->source_ma<500||!s->zero_threshold_max_mv||s->zero_threshold_max_mv>800||
       s->fusb_device_id<0x80||s->fusb_device_id>0x82||s->pck_client>=16||
       !s->usb2_revision||!s->typec_owner||!s->usb2_owner)return 0;
    }else if(s->latch_identity||s->latch_owner||s->latch_known_mask||s->latch_value||
       s->typec_owner||s->usb2_owner||s->source_min_mv||s->source_max_mv||s->source_ma||
       s->zero_threshold_max_mv||s->fusb_device_id||s->usb2_revision||s->pck_client||s->usb_supply_uv[4]||
       present(&s->typec_pins[0][0],sizeof(s->typec_pins)))return 0;
    if(a733_board_radio_workload(s->workload)){
        if(!s->sid_authority||!present(s->sid_evidence,32))return 0;
        if(s->radio_profile_address!=UINT64_C(0x43000000)||!s->radio_profile_bytes||
           s->radio_profile_bytes>65536||!present(s->radio_profile_digest,32)||
           !s->latch_m_identity||!s->latch_m_owner||s->latch_m_known_mask!=0x3f||
           (s->latch_m_value&~UINT32_C(0x3f)))return 0;
    }else if(s->radio_profile_address||s->radio_profile_bytes||present(s->radio_profile_digest,32)||
             s->latch_m_identity||s->latch_m_owner||s->latch_m_known_mask||s->latch_m_value)return 0;
    uint64_t owner[]={s->pmic_owner,s->usb0_owner,s->typec_owner,s->usb2_owner};
    for(unsigned i=0;i<(a733_board_host_workload(s->workload)?4u:2u);++i){if(!owner[i])return 0;for(unsigned j=0;j<i;++j)if(owner[i]==owner[j])return 0;}
    if(s->usb_supply_uv[0]!=3300000||s->usb_supply_uv[1]!=3300000)return 0;
    for(unsigned i=0;i<3;++i)if(!s->fabric_hz[i])return 0;
    return 1;
}
int a733_board_contract_valid(const struct a733_smp_contract *c,const struct a733_board_session *s,
    const uint8_t id[32],uint64_t now)
{
    uint8_t digest[32];
    if(!c||!a733_board_session_valid(s,now)||c->magic!=A733_BOARD_CONTRACT_MAGIC||
       c->version!=A733_BOARD_CONTRACT_VERSION||c->bytes!=sizeof(*c)||c->epoch!=s->epoch||c->generation!=s->generation||
       c->inverse_generation!=~c->generation||c->lease!=s->lease||memcmp(c->build_id,id,32)||
       c->reserved[0]!=A733_BOARD_SESSION_ADDRESS||c->reserved[1]!=sizeof(*s)||
#ifndef SUNSTONE_BOARD_SUPERVISED
       c->armed_at!=s->reload_before_ticks||c->armed_at>UINT64_MAX-A733_SMP_COUNTER_HZ||
       c->startup_deadline!=c->armed_at+A733_SMP_COUNTER_HZ||now>=c->startup_deadline||
       c->startup_deadline>=c->diagnostic_deadline||c->diagnostic_deadline!=s->deadline_us*24||
       c->expires_at!=s->reset_by_us*24||c->watchdog_mode!=0x41||c->watchdog_configuration!=1||
       c->watchdog_output!=2||c->fel_flag!=0x5aa5a55a||c->redirect_flag!=0xfa50392f||
       c->redirect_entry!=0x20||c->alternate_flag)return 0;
#else
       c->armed_at>now||c->armed_at>UINT64_MAX-A733_BOARD_SUPERVISED_SECONDS*A733_SMP_COUNTER_HZ||
       c->startup_deadline!=c->armed_at+A733_BOARD_STARTUP_SECONDS*A733_SMP_COUNTER_HZ||now>=c->startup_deadline||
       c->diagnostic_deadline!=c->armed_at+A733_BOARD_SUPERVISED_SECONDS*A733_SMP_COUNTER_HZ||
       c->expires_at!=c->diagnostic_deadline||c->watchdog_mode||c->watchdog_configuration||
       c->watchdog_output||c->fel_flag||c->redirect_flag||c->redirect_entry||c->alternate_flag)return 0;
#endif
    storage_digest_bytes(s,sizeof(*s),digest);
    return !memcmp(digest,&c->reserved[2],32);
}
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{return a733_board_contract_valid(c,(const void *)A733_BOARD_SESSION_ADDRESS,a733_smp_build_id,now);}
/* Existing fixed secondary-core power allowlist; no application device authority. */
int a733_smp_power_address(unsigned core,uintptr_t address,int write)
{
    if(core<1||core>=8)return 0;
    uintptr_t cfg=UINT32_C(0x08001000)+(uintptr_t)core*0x1000;
    if(address==cfg||address==cfg+4||address==cfg+8||address==UINT32_C(0x07050200)+core*4||
       address==UINT32_C(0x07050220)+core*4)return 1;
    return !write&&address==UINT32_C(0x07052008)+(uintptr_t)core*0x1000;
}
