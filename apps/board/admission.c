#include "a733_board_application.h"
#include "boot.h"
#include "a733_storage_digest.h"
#include "a733_admission_refusal.h"
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
       s->reserved0||s->usb_topology>1||(s->usb_topology&&
         s->workload!=6&&s->workload!=8&&s->workload!=10&&s->workload!=12)||s->workload==5||s->workload==7||s->workload==9||s->workload==11||s->workload==13||s->workload>
#if defined(SUNSTONE_BOARD_SUPERVISED) && defined(SUNSTONE_BOARD_USB_AUDIO)
       14
#elif defined(SUNSTONE_BOARD_SUPERVISED) && defined(SUNSTONE_BOARD_USB3)
       12
#elif defined(SUNSTONE_BOARD_SUPERVISED) && defined(SUNSTONE_BOARD_USB2_DUAL)
       4
#elif defined(SUNSTONE_BOARD_SUPERVISED)
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
       s->source_ma<((s->workload==6||s->workload==8||s->workload==10||s->workload==12)?900u:500u)||!s->zero_threshold_max_mv||s->zero_threshold_max_mv>800||
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
           !s->latch_m_identity||!s->latch_m_owner||
           (s->latch_m_known_mask!=0x3f
#ifdef SUNSTONE_BOARD_SUPERVISED
            &&s->latch_m_known_mask!=0
#endif
           )||(s->latch_m_value&~s->latch_m_known_mask))return 0;
    }else if(s->radio_profile_address||s->radio_profile_bytes||present(s->radio_profile_digest,32)||
             s->latch_m_identity||s->latch_m_owner||s->latch_m_known_mask||s->latch_m_value)return 0;
    uint64_t owner[]={s->pmic_owner,s->usb0_owner,s->typec_owner,s->usb2_owner};
    for(unsigned i=0;i<(a733_board_host_workload(s->workload)?4u:2u);++i){if(!owner[i])return 0;for(unsigned j=0;j<i;++j)if(owner[i]==owner[j])return 0;}
    if(s->usb_supply_uv[0]!=3300000||s->usb_supply_uv[1]!=3300000)return 0;
    for(unsigned i=0;i<3;++i)if(!s->fabric_hz[i])return 0;
    return 1;
}
#ifdef A733_BOARD_ADMITS_RESIDENT
#ifndef SUNSTONE_BOARD_SUPERVISED
#error The resident lifetime uses the supervised session layout
#endif
_Static_assert(A733_BOARD_STARTUP_SECONDS<4,"resident startup must end before the first watchdog expiry");
#endif
/* The lifetime that a board contract magic names, among those this image admits; 0 for any other magic. A board image
 * admits its own magic alone. The OS image (apps/os/admission.c) also admits the resident magic. */
static unsigned board_lifetime(uint64_t magic)
{
    if(magic==A733_BOARD_CONTRACT_MAGIC)return A733_LIFETIME_SUPERVISED;
#ifdef A733_BOARD_ADMITS_RESIDENT
    if(magic==A733_BOARD_RESIDENT_CONTRACT_MAGIC)return A733_LIFETIME_RESIDENT;
#endif
    return 0;
}
/* Zero when the contract binds this session; otherwise the named refusal (include/a733_admission_refusal.h),
 * checked in the order of the stop detail's terms. */
unsigned a733_board_contract_refusal(const struct a733_smp_contract *c,const struct a733_board_session *s,
    const uint8_t id[32],uint64_t now)
{
    uint8_t digest[32];
    if(!c||!s)return A733_REFUSAL(A733_REFUSAL_CONTRACT_FIELD,0);
    if(!a733_board_session_valid(s,now))return A733_REFUSAL(A733_REFUSAL_SESSION_BIND,A733_BIND_BOARD_PREFIX);
    const unsigned lifetime=board_lifetime(c->magic);
    if(!lifetime)return A733_REFUSAL(A733_REFUSAL_MAGIC_VERSION,A733_MAGIC_MAGIC);
    if(c->version!=A733_BOARD_CONTRACT_VERSION)return A733_REFUSAL(A733_REFUSAL_MAGIC_VERSION,A733_MAGIC_VERSION);
    if(c->bytes!=sizeof(*c))return A733_REFUSAL(A733_REFUSAL_MAGIC_VERSION,A733_MAGIC_SIZE);
    if(c->epoch!=s->epoch||c->generation!=s->generation||c->inverse_generation!=~c->generation||c->lease!=s->lease)
        return A733_REFUSAL(A733_REFUSAL_CONTRACT_FIELD,A733_FIELD_IDENTITY);
    if(memcmp(c->build_id,id,32))return A733_REFUSAL(A733_REFUSAL_BUILD_ID,0);
    if(c->reserved[0]!=A733_BOARD_SESSION_ADDRESS||c->reserved[1]!=sizeof(*s))
        return A733_REFUSAL(A733_REFUSAL_SESSION_BIND,A733_BIND_CONTRACT);
#ifndef SUNSTONE_BOARD_SUPERVISED
    (void)lifetime;
    if(c->armed_at!=s->reload_before_ticks||c->armed_at>UINT64_MAX-A733_SMP_COUNTER_HZ)
        return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_ARMED);
    if(c->startup_deadline!=c->armed_at+A733_SMP_COUNTER_HZ||now>=c->startup_deadline)
        return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_STARTUP);
    if(c->startup_deadline>=c->diagnostic_deadline||c->diagnostic_deadline!=s->deadline_us*24)
        return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_DIAGNOSTIC);
    if(c->expires_at!=s->reset_by_us*24)return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_EXPIRY);
    if(c->watchdog_mode!=0x41||c->watchdog_configuration!=1||c->watchdog_output!=2||c->fel_flag!=0x5aa5a55a||
       c->redirect_flag!=0xfa50392f||c->redirect_entry!=0x20||c->alternate_flag)
        return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_WATCHDOG_FIELDS);
#else
    if(lifetime==A733_LIFETIME_RESIDENT){
        /* The stage-2 loader armed the four-second watchdog at armed_at. The
         * watchdog is the only hang bound: no diagnostic deadline, no FEL return. */
        if(c->armed_at>now||c->armed_at>UINT64_MAX-4*A733_SMP_COUNTER_HZ)return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_ARMED);
        if(c->startup_deadline!=c->armed_at+A733_BOARD_STARTUP_SECONDS*A733_SMP_COUNTER_HZ||now>=c->startup_deadline)
            return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_STARTUP);
        if(c->diagnostic_deadline!=A733_SMP_NO_DEADLINE)return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_DIAGNOSTIC);
        if(c->expires_at!=c->armed_at+4*A733_SMP_COUNTER_HZ)return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_EXPIRY);
        if(c->watchdog_mode!=0x41||c->watchdog_configuration!=1||c->watchdog_output!=2||
           c->fel_flag||c->redirect_flag||c->redirect_entry||c->alternate_flag)
            return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_WATCHDOG_FIELDS);
    }else{
        if(c->armed_at>now||c->armed_at>UINT64_MAX-A733_BOARD_SUPERVISED_SECONDS*A733_SMP_COUNTER_HZ)
            return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_ARMED);
        if(c->startup_deadline!=c->armed_at+A733_BOARD_STARTUP_SECONDS*A733_SMP_COUNTER_HZ||now>=c->startup_deadline)
            return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_STARTUP);
        if(c->diagnostic_deadline!=c->armed_at+A733_BOARD_SUPERVISED_SECONDS*A733_SMP_COUNTER_HZ)
            return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_DIAGNOSTIC);
        if(c->expires_at!=c->diagnostic_deadline)return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_EXPIRY);
        if(c->watchdog_mode||c->watchdog_configuration||c->watchdog_output||c->fel_flag||c->redirect_flag||
           c->redirect_entry||c->alternate_flag)return A733_REFUSAL(A733_REFUSAL_TIMING,A733_TIMING_WATCHDOG_FIELDS);
    }
#endif
    storage_digest_bytes(s,sizeof(*s),digest);
    if(memcmp(digest,&c->reserved[2],32))return A733_REFUSAL(A733_REFUSAL_SESSION_BIND,A733_BIND_DIGEST);
    return 0;
}
int a733_board_contract_valid(const struct a733_smp_contract *c,const struct a733_board_session *s,
    const uint8_t id[32],uint64_t now)
{return !a733_board_contract_refusal(c,s,id,now);}
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{return a733_board_contract_valid(c,(const void *)A733_BOARD_SESSION_ADDRESS,a733_smp_build_id,now);}
/* Existing fixed secondary-core power allowlist; no application device authority. */
int a733_smp_power_address(unsigned core,uintptr_t address,int write)
{
    if(core<1||core>=8)return 0;
    uintptr_t cfg=UINT32_C(0x08001000)+(uintptr_t)core*0x1000;
    if(address==cfg||address==cfg+4||address==cfg+8||address==UINT32_C(0x07050200)+core*4||
       address==UINT32_C(0x07050220)+core*4)return 1;
#ifdef A733_BOARD_ADMITS_RESIDENT
    /* The resident boot holds each secondary core in static OFF through its power policy word; startup releases it
     * (docs/designs/cold-cpu-static-off.md). */
    if(a733_smp_lifetime==A733_LIFETIME_RESIDENT&&address==UINT32_C(0x07052000)+(uintptr_t)core*0x1000)return 1;
#endif
    return !write&&address==UINT32_C(0x07052008)+(uintptr_t)core*0x1000;
}
