#include "session.h"
#include "boot.h"
#include "a733_storage_digest.h"
#include <string.h>
extern const uint8_t i2s_recorded_pcm_digest[32];
static int present(const void *v,size_t n)
{const uint8_t *p=v;unsigned b=0;for(size_t i=0;i<n;++i)b|=p[i];return b!=0;}
int i2s_session_valid(const struct i2s_session *s)
{
 if(!s||s->magic!=I2S_SESSION_MAGIC||s->version!=I2S_SESSION_VERSION||s->bytes!=sizeof(*s)||
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
#ifdef SUNSTONE_I2S_TRANSCRIBE
 if(s->direction||s->reserved||s->capture_samples<I2S_CAPTURE_MIN_SAMPLES||
    s->capture_samples>I2S_CAPTURE_MAX_SAMPLES||!i2s_models_valid(&s->models))return 0;
#endif
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

#ifdef SUNSTONE_I2S_TRANSCRIBE
int i2s_models_valid(const struct i2s_models *p)
{
 static const uint64_t bytes[2]={7501968,6560592};
 static const uint8_t hashes[2][32]={{0xf7,0x3c,0xcd,0x02,0x46,0x45,0x38,0x7d,0x26,0x79,0xee,0xa9,0x7a,0xe1,0x1d,0x0c,0x33,0xf5,0xe4,0xbe,0x81,0xb3,0x9a,0x94,0x30,0xd1,0xf5,0xd2,0x85,0xe8,0x08,0x9d},{0xd8,0x77,0x60,0x4c,0x75,0x9b,0x68,0x8e,0x46,0xf7,0xf9,0xb4,0x34,0x6c,0x7d,0x87,0x49,0x6f,0xaf,0x6e,0xe6,0x4c,0xbb,0x8f,0x1f,0x98,0x37,0x65,0x2a,0xad,0xe6,0xb2}};
 return p&&!memcmp(p->bytes,bytes,sizeof(bytes))&&!memcmp(p->sha256,hashes,sizeof(hashes));
}
#endif
