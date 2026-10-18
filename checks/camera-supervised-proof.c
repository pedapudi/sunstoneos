#include "a733_camera_platform.h"
#include <assert.h>
#include <stdio.h>
static uint32_t proof;
static unsigned calls;
static int now(void *v,uint64_t *out){(void)v;*out=10;return 1;}
static int admit(void *v,uint64_t deadline,struct a733_camera_admission *out)
{(void)v;(void)deadline;*out=(struct a733_camera_admission){1,proof,A733_CAMERA_COLD_RESET};return 1;}
static int read_reg(void *v,uint64_t t,uint16_t offset,uint32_t *out,uint64_t d)
{(void)v;(void)t;(void)offset;(void)out;(void)d;++calls;return -1;}
static int write_reg(void *v,uint64_t t,uint16_t offset,uint32_t x,uint64_t d)
{(void)v;(void)t;(void)offset;(void)x;(void)d;++calls;return -1;}
int main(void)
{
 struct a733_camera_platform_ops ops={now,admit,read_reg,write_reg};
 for(unsigned mode=0;mode<2;++mode)for(unsigned manual=0;manual<2;++manual){
  struct a733_camera_platform object={0};proof=manual?A733_CAMERA_SUPERVISED_PROOF:A733_CAMERA_REQUIRED_PROOF;
  assert((mode?a733_camera_platform_supervised_init(&object,&ops,NULL):a733_camera_platform_init(&object,&ops,NULL))==1);
  assert(a733_camera_platform_open(&object,100)==1);
  int r=a733_camera_platform_poll(&object);
  assert(mode==manual?(r==0&&object.state==A733_CAMERA_PLATFORM_CHECK):(r==-1&&object.state==A733_CAMERA_PLATFORM_FAULT));
  assert(!calls);
 }
 struct a733_camera_platform missing={0};proof=A733_CAMERA_SUPERVISED_PROOF&~A733_CAMERA_TRANSACTIONS_DRAINED;
 assert(a733_camera_platform_supervised_init(&missing,&ops,NULL)==1);
 assert(a733_camera_platform_open(&missing,100)==1&&a733_camera_platform_poll(&missing)==-1&&!calls);
 puts("PASS explicit recovery mode separation before camera MMIO");
 return 0;
}
