#include "usb-platform-fixture.h"
static void actual(struct fixture *f,unsigned cold)
{
 init(f,cold);memset(&f->p,0,sizeof(f->p));
 f->config.fabric[0]=0x03000002;f->config.fabric[1]=0x83000000;f->config.fabric[2]=0x81000000;
 f->config.rate_hz[0]=200000000;f->config.rate_hz[1]=f->config.rate_hz[2]=600000000;
 *reg(f,0x07090160)=0x983fb0f7;*reg(f,C)=0xf8675f00;
 *reg(f,C+0x500)=f->config.fabric[0];*reg(f,C+0x580)=0xc3000000;*reg(f,C+0x588)=f->config.fabric[2];
 *reg(f,C+0x1340)=0x80000000;
 *reg(f,C+0xa0)=0xff126310;*reg(f,C+0xa8)=0;*reg(f,C+0xac)=0x30000000;
 *reg(f,C+0x1908)=0x8fff0fff;*reg(f,C+0x1988)=0x0fff0000;
 assert(a733_usb_platform_init(&f->p,&f->config,&ops,f,&f->registry));
}
int main(void)
{
 struct fixture f;actual(&f,0);assert(run(&f)==A733_USB_PLATFORM_PREPARED&&!f.retains);
 actual(&f,1);assert(run(&f)==A733_USB_PLATFORM_PREPARED&&!f.retains);
 assert((*reg(&f,C+0x1340)&0x87000000)==0x80000000 && f.ref_enabled);
 actual(&f,0);*reg(&f,C+0x1340)=0x81000000;
 assert(run(&f)==A733_USB_PLATFORM_RETAINED&&!f.writes);terminal(&f);
 for(unsigned loss=0;loss<2;++loss){
  actual(&f,0);*reg(&f,0x07090160)&=~(loss?(1u<<28):2u);
  assert(run(&f)==A733_USB_PLATFORM_RETAINED&&!f.writes);terminal(&f);
 }
 /* Change selected reference after baseline, before final verification. */
 actual(&f,0);
 while(!f.writes){assert(a733_usb_platform_poll(&f.p)==A733_USB_PLATFORM_PENDING);f.now+=100;}
 *reg(&f,C+0x1340)=0x81000000;
 assert(run(&f)==A733_USB_PLATFORM_RETAINED);terminal(&f);
 /* Previously admitted raw24 may not become raw26, even with valid REF24. */
 init(&f,0);
 while(!f.writes){assert(a733_usb_platform_poll(&f.p)==A733_USB_PLATFORM_PENDING);f.now+=100;}
 *reg(&f,0x07090160)=0x10008002;*reg(&f,C)=0xf8675f00;
 assert(run(&f)==A733_USB_PLATFORM_RETAINED);terminal(&f);
 puts("PASS USB SYS24 cold/adopted26, raw26 refusal, retained selector/raw-rate/enable/stability loss");
}
