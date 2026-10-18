#include "../apps/i2s/session.h"
#include "boot.h"
#include "a733_storage_digest.h"
#include <string.h>
const uint8_t i2s_recorded_pcm_digest[32]={9};
const uint8_t a733_smp_build_id[32]={1};
volatile unsigned i2s_check_result;
static struct i2s_session session;
static struct a733_smp_contract contract;
static void check(int ok,unsigned code){if(!ok){i2s_check_result=code;__asm__ volatile("brk #0");for(;;){}}}
void i2s_check_run(unsigned mode)
{
 (void)mode;struct i2s_session *s=&session;
 s->magic=I2S_SESSION_MAGIC;s->version=1;s->bytes=sizeof(*s);s->epoch=1;s->generation=2;s->lease=3;
 memcpy(s->image,a733_smp_build_id,32);s->evidence[0]=2;s->memory[0]=3;s->recovery[0]=4;
 memcpy(s->pcm,i2s_recorded_pcm_digest,32);s->shared.identity=2;s->shared.pmic.identity=2;
 s->shared.pmic.owner=4;s->shared.usb.owner=5;s->shared.usb.handoff=2;
 s->dma.allowed.owner=6;s->dma.allowed.handoff=s->dma.evidence=3;
 s->dma.established=A733_DMA_HANDOFF_SUPERVISED;s->dma.owned_channels=0xffff;
 s->dma.service_channels=1;s->dma.controller=0x04601000;
 check(i2s_session_valid(s),10);
 struct a733_smp_contract *c=&contract;
 c->magic=I2S_CONTRACT_MAGIC;c->version=3;c->bytes=sizeof(*c);c->epoch=1;c->generation=2;
 c->inverse_generation=~c->generation;c->lease=3;memcpy(c->build_id,a733_smp_build_id,32);
 c->armed_at=100;c->startup_deadline=100+A733_SMP_COUNTER_HZ;
 c->diagnostic_deadline=c->expires_at=100+I2S_SESSION_SECONDS*A733_SMP_COUNTER_HZ;
 c->reserved[0]=A733_STORAGE_SESSION_ADDRESS;c->reserved[1]=sizeof(*s);
 memcpy((void *)A733_STORAGE_SESSION_ADDRESS,s,sizeof(*s));storage_digest_bytes(s,sizeof(*s),(void *)&c->reserved[2]);
 check(a733_smp_contract_valid(c,101),11);
 s->image[0]^=2;memcpy((void *)A733_STORAGE_SESSION_ADDRESS,s,sizeof(*s));
 storage_digest_bytes(s,sizeof(*s),(void *)&c->reserved[2]);check(!a733_smp_contract_valid(c,101),12);s->image[0]^=2;
 s->dma.established=A733_DMA_HANDOFF_REQUIRED;check(!i2s_session_valid(s),13);
 s->dma.established=A733_DMA_HANDOFF_SUPERVISED;s->shared.usb.deadline_us=1;check(!i2s_session_valid(s),14);
 s->shared.usb.deadline_us=0;s->pcm[0]^=1;check(!i2s_session_valid(s),15);
 i2s_check_result=1;__asm__ volatile("brk #0");for(;;){}
}
