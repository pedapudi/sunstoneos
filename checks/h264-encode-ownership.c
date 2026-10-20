#include "a733_video_encode_platform.h"
#include <assert.h>
#include <stdio.h>
static int now(void *v,uint64_t *out){(void)v;*out=1;return 1;}
int main(void)
{
 struct a733_clock_leases registry={0};struct a733_gpu_resource_manager power;
 struct a733_encode_platform_config c={.clocks=&registry,.power=&power,.owner=12,.handoff=13,.module=0x80000001,.module_hz=600000000,.fabric={0,0x80000000,0x80000000},.fabric_hz={24000000,24000000,24000000}};
 a733_clock_mask_t scopes;assert(a733_encode_platform_scopes(&c,&scopes));
 for(unsigned first=0;first<2;++first){
  registry=(struct a733_clock_leases){0};struct a733_clock_token jpeg={0},h264={0};assert(a733_clock_leases_init(&registry,now,0));
  struct a733_clock_token *a=first?&h264:&jpeg,*b=first?&jpeg:&h264;
  assert(a733_clock_acquire(&registry,first?12:11,scopes,A733_CLOCK_VE_ENC,100,a)==A733_CLOCK_OK);
  assert(a733_clock_acquire(&registry,first?11:12,scopes,A733_CLOCK_VE_ENC,100,b)==A733_CLOCK_BUSY);
  assert(!b->id&&a->id&&a733_clock_token_status(&registry,a)==A733_CLOCK_OK);
 }
 puts("PASS actual common encoder scopes and registry exclude simultaneous JPEG/H264 ownership in both orders; retained first lease.");
}
