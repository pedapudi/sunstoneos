#include "session.h"
#include "boot.h"
#include "a733_storage_digest.h"
#include <string.h>
#if SUNSTONE_HDMI_AUDIO
#include "../hdmi-audio/native.h"
#endif
static int present(const uint8_t *p)
{unsigned b=0;for(unsigned i=0;i<32;++i)b|=p[i];return b!=0;}
int hdmi_session_valid(const struct hdmi_session *s)
{
    return s&&s->magic==HDMI_SESSION_MAGIC&&s->version==
#if SUNSTONE_HDMI_AUDIO_STREAM
        3&&s->bytes==sizeof(*s)&&present(s->manifest_sha256)
#elif SUNSTONE_HDMI_AUDIO_USB
        2&&s->bytes==sizeof(*s)
#else
        1&&s->bytes==sizeof(*s)
#endif
        &&s->epoch&&s->generation&&s->lease&&s->supervisor&&s->owner&&!s->reserved&&
        present(s->image)&&present(s->evidence)&&present(s->analog)&&present(s->power)&&
        present(s->supervision)&&s->pck_client<A733_GPU_RESOURCE_CLIENTS&&
        s->request.mode==A733_HDMI_720P60&&s->request.range==A733_HDMI_LIMITED&&
        s->request.initial.id&&s->request.initial.generation&&
        (s->request.phy_reference_hz==24000000||s->request.phy_reference_hz==26000000)&&
        s->pmic.owner&&s->pmic.owner!=s->owner&&s->pmic.identity==s->generation&&
        !s->pmic.startup_deadline
#if SUNSTONE_HDMI_FRAMEBUFFER
        &&s->request.source==A733_HDMI_FRAMEBUFFER&&hdmi_framebuffer_session_valid(&s->framebuffer)
#else
        &&s->request.source==A733_HDMI_PATTERN
#endif
#if SUNSTONE_HDMI_AUDIO
        &&hdmi_audio_session_valid(&s->audio)
#if SUNSTONE_HDMI_AUDIO_USB
        &&s->usb.owner&&s->usb.handoff&&s->usb.owner!=s->owner&&s->usb.owner!=s->pmic.owner&&
        s->usb.owner!=s->audio.authority&&s->usb.owner!=s->audio.dma.allowed.owner&&
        present(s->usb.initial)&&present(s->usb.electrical)&&present(s->usb.supervision)&&
        s->usb.cold_reference==1&&!s->usb.reserved&&s->usb.supply_uv[0]==3300000&&
        s->usb.supply_uv[1]==3300000&&s->usb.supply_uv[2]==1200000&&s->usb.supply_uv[3]==900000&&
        !memcmp(s->usb.fabric,s->audio.dma.allowed.fabric,sizeof(s->usb.fabric))&&
        !memcmp(s->usb.fabric_hz,s->audio.dma.allowed.rate_hz,sizeof(s->usb.fabric_hz))
#endif
#endif
        ;
}
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
    const struct hdmi_session *s=(const void *)A733_STORAGE_SESSION_ADDRESS;
    uint8_t digest[32];
    /* Authenticate the complete immutable session before using provider input. */
    if(!c||c->magic!=HDMI_CONTRACT_MAGIC||c->version!=3||c->bytes!=sizeof(*c)||
       !c->epoch||!c->generation||c->inverse_generation!=~c->generation||!c->lease||
       memcmp(c->build_id,a733_smp_build_id,32)||!c->reserved[0]||c->reserved[1])return 0;
    storage_digest_bytes(s,sizeof(*s),digest);
    if(memcmp(digest,&c->reserved[2],32)||!hdmi_session_valid(s)||
       memcmp(s->image,a733_smp_build_id,32)||c->epoch!=s->epoch||c->generation!=s->generation||
       c->lease!=s->lease||c->reserved[0]!=s->supervisor||c->armed_at>now||
       c->armed_at>UINT64_MAX-HDMI_WINDOW_TICKS||
       c->startup_deadline!=c->armed_at+A733_SMP_COUNTER_HZ||now>=c->startup_deadline||
       c->diagnostic_deadline!=c->armed_at+HDMI_WINDOW_TICKS||
       c->expires_at!=c->diagnostic_deadline)return 0;
    return !c->watchdog_mode&&!c->watchdog_configuration&&!c->watchdog_output&&
        !c->fel_flag&&!c->redirect_flag&&!c->redirect_entry&&!c->alternate_flag;
}
int a733_smp_power_address(unsigned core,uintptr_t address,int write)
{
    if(core<1||core>=8)return 0;
    uintptr_t cfg=UINT32_C(0x08001000)+(uintptr_t)core*0x1000;
    if(address==cfg||address==cfg+4||address==cfg+8||address==UINT32_C(0x07050200)+core*4||
       address==UINT32_C(0x07050220)+core*4)return 1;
    return !write&&address==UINT32_C(0x07052008)+(uintptr_t)core*0x1000;
}
