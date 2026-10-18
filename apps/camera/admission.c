#include "session.h"
#include "boot.h"
#include "a733_storage_digest.h"
#include <string.h>
static int present(const void *v,size_t n)
{const uint8_t *p=v;unsigned b=0;for(size_t i=0;i<n;++i)b|=p[i];return b!=0;}
int camera_session_valid(const struct camera_session *s,uint64_t now)
{
    if(!s||s->magic!=CAMERA_SESSION_MAGIC||s->version!=1||s->bytes!=sizeof(*s)||
       !s->epoch||!s->generation||!s->lease||!present(s->image,32)||
       !present(s->evidence,32)||!present(s->memory,32)||!present(s->recovery,32)||
       s->deadline_us<=10000||s->reset_by_us>UINT64_MAX/24||
       !s->guard_us||s->minimum_reload_us<=s->guard_us||
       s->minimum_reload_us-s->guard_us<=1000000||s->maximum_recovery_us<s->minimum_reload_us||
       s->reset_by_us<=s->maximum_recovery_us||s->deadline_us>s->reset_by_us-s->maximum_recovery_us||
       s->reload_before_ticks/24>UINT64_MAX-s->minimum_reload_us||
       s->deadline_us>s->reload_before_ticks/24+s->minimum_reload_us-s->guard_us-1000||
       s->reload_after_ticks<s->reload_before_ticks||s->reload_after_ticks-s->reload_before_ticks>24000||
       now<s->reload_after_ticks||now>=s->deadline_us*24||s->pck_client>=16)return 0;
    const struct a733_camera_boot_record *b=&s->camera;
    if(b->version!=1||b->bytes!=sizeof(*b)||b->epoch!=s->epoch||b->generation!=s->generation||
       b->lease!=s->lease||b->deadline_us!=s->deadline_us||memcmp(b->build,s->image,32)||
       memcmp(b->evidence,s->evidence,32)||!b->owner||b->owner==s->shared.pmic.owner||
       b->owner==s->shared.usb.owner||s->shared.identity!=s->generation||
       s->shared.pmic.identity!=s->generation||s->shared.pmic.startup_deadline!=s->deadline_us||
       s->shared.usb.handoff!=s->generation||s->shared.usb.deadline_us!=s->deadline_us||
       s->media.owner!=b->owner||s->media.handoff!=s->lease||s->media.deadline_us!=s->deadline_us||
       s->media.groups!=(A733_MEDIA_VI|A733_MEDIA_IOMMU)||s->media.camera_mclk_mask!=2||
       memcmp(s->media.fabric,s->shared.usb.fabric,sizeof(s->media.fabric))||
       memcmp(s->media.rate_hz,s->shared.usb.rate_hz,sizeof(s->media.rate_hz)))return 0;
    return 1;
}
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
    const struct camera_session *s=(const void *)A733_STORAGE_SESSION_ADDRESS;
    uint8_t digest[32];
    if(!c||!camera_session_valid(s,now)||c->magic!=CAMERA_CONTRACT_MAGIC||c->version!=1||
       c->bytes!=sizeof(*c)||memcmp(s->image,a733_smp_build_id,32)||c->epoch!=s->epoch||c->generation!=s->generation||
       c->inverse_generation!=~c->generation||c->lease!=s->lease||memcmp(c->build_id,a733_smp_build_id,32)||
       c->reserved[0]!=A733_STORAGE_SESSION_ADDRESS||c->reserved[1]!=sizeof(*s)||
       c->armed_at!=s->reload_before_ticks||c->armed_at>UINT64_MAX-A733_SMP_COUNTER_HZ||
       c->startup_deadline!=c->armed_at+A733_SMP_COUNTER_HZ||now>=c->startup_deadline||
       c->startup_deadline>=c->diagnostic_deadline||c->diagnostic_deadline!=s->deadline_us*24||
       c->expires_at!=s->reset_by_us*24||c->watchdog_mode!=0x41||c->watchdog_configuration!=1||
       c->watchdog_output!=2||c->fel_flag!=0x5aa5a55a||c->redirect_flag!=0xfa50392f||
       c->redirect_entry!=0x20||c->alternate_flag)return 0;
    storage_digest_bytes(s,sizeof(*s),digest);
    return !memcmp(digest,&c->reserved[2],32);
}
/* Existing fixed secondary-core power allowlist; no application device authority. */
int a733_smp_power_address(unsigned core,uintptr_t address,int write)
{
    if(core<1||core>=8)return 0;
    uintptr_t cfg=UINT32_C(0x08001000)+(uintptr_t)core*0x1000;
    if(address==cfg||address==cfg+4||address==cfg+8||address==UINT32_C(0x07050200)+core*4||
       address==UINT32_C(0x07050220)+core*4)return 1;
    return !write&&address==UINT32_C(0x07052008)+(uintptr_t)core*0x1000;
}
