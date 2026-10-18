#include "session.h"
#include "boot.h"
#include "a733_storage_digest.h"
#include <string.h>
extern const uint8_t i2s_recorded_pcm_digest[32];
static int present(const void *v,size_t n)
{const uint8_t *p=v;unsigned b=0;for(size_t i=0;i<n;++i)b|=p[i];return b!=0;}
int i2s_session_valid(const struct i2s_session *s)
{
 if(!s||s->magic!=I2S_SESSION_MAGIC||s->version!=1||s->bytes!=sizeof(*s)||
    !s->epoch||!s->generation||!s->lease||!present(s->image,32)||!present(s->evidence,32)||
    !present(s->memory,32)||!present(s->recovery,32)||s->direction>1||s->audio_cold>1||
    memcmp(s->pcm,i2s_recorded_pcm_digest,32)||s->shared.identity!=s->generation||
    s->shared.pmic.identity!=s->generation||s->shared.usb.handoff!=s->generation||
    s->shared.pmic.startup_deadline||s->shared.usb.deadline_us||s->dma.allowed.deadline_us||
    !s->dma.allowed.owner||s->dma.allowed.owner==s->shared.pmic.owner||
    s->dma.allowed.owner==s->shared.usb.owner||s->dma.allowed.handoff!=s->lease||
    s->dma.evidence!=s->lease||s->dma.established!=A733_DMA_HANDOFF_SUPERVISED||
    s->dma.owned_channels!=0xffff||s->dma.service_channels!=1||s->dma.controller!=0x04601000||
    memcmp(s->dma.allowed.fabric,s->shared.usb.fabric,sizeof(s->dma.allowed.fabric))||
    memcmp(s->dma.allowed.rate_hz,s->shared.usb.rate_hz,sizeof(s->dma.allowed.rate_hz)))return 0;
 return 1;
}
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
 const struct i2s_session *s=(const void *)A733_STORAGE_SESSION_ADDRESS;uint8_t digest[32];
 if(!c||!i2s_session_valid(s)||c->magic!=I2S_CONTRACT_MAGIC||c->version!=3||c->bytes!=sizeof(*c)||
    c->epoch!=s->epoch||c->generation!=s->generation||c->inverse_generation!=~c->generation||c->lease!=s->lease||
    memcmp(c->build_id,a733_smp_build_id,32)||memcmp(s->image,a733_smp_build_id,32)||
    c->reserved[0]!=A733_STORAGE_SESSION_ADDRESS||c->reserved[1]!=sizeof(*s)||
    c->armed_at>now||c->armed_at>UINT64_MAX-I2S_SESSION_SECONDS*A733_SMP_COUNTER_HZ||
    c->startup_deadline!=c->armed_at+A733_SMP_COUNTER_HZ||now>=c->startup_deadline||
    c->diagnostic_deadline!=c->armed_at+I2S_SESSION_SECONDS*A733_SMP_COUNTER_HZ||
    c->expires_at!=c->diagnostic_deadline||c->watchdog_mode||c->watchdog_configuration||
    c->watchdog_output||c->fel_flag||c->redirect_flag||c->redirect_entry||c->alternate_flag)return 0;
 storage_digest_bytes(s,sizeof(*s),digest);return !memcmp(digest,&c->reserved[2],32);
}
int a733_smp_power_address(unsigned core,uintptr_t address,int write)
{
 if(core<1||core>=8)return 0;
 uintptr_t cfg=UINT32_C(0x08001000)+(uintptr_t)core*0x1000;
 if(address==cfg||address==cfg+4||address==cfg+8||address==UINT32_C(0x07050200)+core*4||
    address==UINT32_C(0x07050220)+core*4)return 1;
 return !write&&address==UINT32_C(0x07052008)+(uintptr_t)core*0x1000;
}
