#include "../apps/camera/session.h"
#include "boot.h"
#include "a733_storage_digest.h"
#include <string.h>
const unsigned char a733_smp_build_id[32]={1};
volatile unsigned camera_check_result;
static struct camera_session session;
static struct a733_smp_contract contract;
static void check(int ok,unsigned code){if(!ok){camera_check_result=code;__asm__ volatile("brk #0");for(;;){}}}
void camera_check_run(unsigned mode)
{
 (void)mode;
 struct camera_session *s=&session;
 s->magic=CAMERA_SESSION_MAGIC;s->version=1;s->bytes=sizeof(*s);
 s->epoch=1;s->generation=2;s->lease=3;memcpy(s->image,a733_smp_build_id,32);
 s->evidence[0]=2;s->memory[0]=3;s->recovery[0]=4;
 s->deadline_us=2000000;s->minimum_reload_us=4000000;s->maximum_recovery_us=4000000;
 s->guard_us=10000;s->reset_by_us=7000000;s->reload_before_ticks=2400;s->reload_after_ticks=2401;
 s->shared.identity=2;s->shared.pmic.owner=4;s->shared.pmic.identity=2;
 s->shared.pmic.startup_deadline=s->deadline_us;s->shared.usb.owner=5;
 s->shared.usb.handoff=2;s->shared.usb.deadline_us=s->deadline_us;
 s->camera.version=1;s->camera.bytes=sizeof(s->camera);s->camera.owner=6;
 s->camera.epoch=1;s->camera.generation=2;s->camera.lease=3;s->camera.deadline_us=s->deadline_us;
 memcpy(s->camera.build,s->image,32);memcpy(s->camera.evidence,s->evidence,32);
 s->media.owner=6;s->media.handoff=3;s->media.deadline_us=s->deadline_us;
 s->media.groups=A733_MEDIA_VI|A733_MEDIA_IOMMU;s->media.camera_mclk_mask=2;
 check(camera_session_valid(s,2402),10);
 s->media.camera_mclk_mask=4;check(camera_session_valid(s,2402),20);
 for(unsigned mask=0;mask<9;mask++){
  s->media.camera_mclk_mask=mask;
  check(camera_session_valid(s,2402)==(mask==2||mask==4),21);
 }
 s->media.camera_mclk_mask=4; /* The actual contract below authenticates CAM2. */
 struct a733_smp_contract *c=&contract;
 c->magic=CAMERA_CONTRACT_MAGIC;c->version=1;c->bytes=sizeof(*c);c->epoch=1;c->generation=2;
 c->inverse_generation=~c->generation;memcpy(c->build_id,a733_smp_build_id,32);
 c->armed_at=s->reload_before_ticks;c->startup_deadline=c->armed_at+A733_SMP_COUNTER_HZ;
 c->diagnostic_deadline=s->deadline_us*24;c->expires_at=s->reset_by_us*24;c->lease=3;
 c->watchdog_mode=0x41;c->watchdog_configuration=1;c->watchdog_output=2;
 c->fel_flag=0x5aa5a55a;c->redirect_flag=0xfa50392f;c->redirect_entry=0x20;
 c->reserved[0]=A733_STORAGE_SESSION_ADDRESS;c->reserved[1]=sizeof(*s);
 memcpy((void *)A733_STORAGE_SESSION_ADDRESS,s,sizeof(*s));
 storage_digest_bytes(s,sizeof(*s),(void *)&c->reserved[2]);
 check(a733_smp_contract_valid(c,2402),11);
 ((struct camera_session *)A733_STORAGE_SESSION_ADDRESS)->media.camera_mclk_mask=2;
 check(!a733_smp_contract_valid(c,2402),22);
 ((struct camera_session *)A733_STORAGE_SESSION_ADDRESS)->media.camera_mclk_mask=4;
 s->image[0]^=0x80;memcpy(s->camera.build,s->image,32);
 memcpy((void *)A733_STORAGE_SESSION_ADDRESS,s,sizeof(*s));storage_digest_bytes(s,sizeof(*s),(void *)&c->reserved[2]);
 check(!a733_smp_contract_valid(c,2402),12);
 s->image[0]^=0x80;memcpy(s->camera.build,s->image,32);
 s->camera.epoch++;check(!camera_session_valid(s,2402),13);s->camera.epoch--;
 s->media.fabric[0]++;check(!camera_session_valid(s,2402),14);s->media.fabric[0]--;
 check(!camera_session_valid(s,s->deadline_us*24),15);
 camera_check_result=1;__asm__ volatile("brk #0");for(;;){}
}
