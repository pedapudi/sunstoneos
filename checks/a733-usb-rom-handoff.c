/* Reuse the real provider and its established cold fixture. No device traffic. */
#define main cold_cases
#include "a733-usb-platform.c"
#undef main
static void actual(struct fixture *f)
{
    init(f,0);memset(&f->p,0,sizeof(f->p));f->rom=1;
    f->config.cold_reference=A733_USB_ROM_DEVICE;f->config.deadline_us=1000000;
    f->config.fabric[0]=0x03000002;f->config.fabric[1]=0x83000000;f->config.fabric[2]=0x81000000;
    f->config.rate_hz[0]=200000000;f->config.rate_hz[1]=f->config.rate_hz[2]=600000000;
    *reg(f,0x07090160)=0x983fb0f7;*reg(f,C)=0xf8675f00;
    *reg(f,C+0x500)=f->config.fabric[0];*reg(f,C+0x580)=0xc3000000;*reg(f,C+0x588)=f->config.fabric[2];
    *reg(f,C+0xa0)=0xff126310;*reg(f,C+0xa8)=0;*reg(f,C+0xac)=0x30000000;
    *reg(f,C+0x1908)=0x8fff0fff;*reg(f,C+0x1988)=0x0fff0000;
    *reg(f,C+0x1340)=0x80000000;*reg(f,C+0x1300)=0x40000000;*reg(f,C+0x1304)=0x01000100;
    *reg(f,C+0x1308)=*reg(f,C+0x130c)=0;
    *reg(f,U+0x40)=0x01400060; /* EP index plus high-speed/soft-connect. */
    *reg(f,U+0x48)=0x00040003;*reg(f,U+0x50)=255;*reg(f,U+0x500)=65535;
    *reg(f,U+0x400)=0x8000007f;f->device_enabled=0;
    assert(a733_usb_platform_init(&f->p,&f->config,&ops,f,&f->registry));
}
int main(void)
{
    struct fixture f;actual(&f);
    assert(run(&f)==A733_USB_PLATFORM_PREPARED && f.reset_seen);
    assert(f.p.token.id==f.before.id && f.p.token.exclusive==A733_CLOCK_USB0);
    assert(f.p.token.shared&A733_CLOCK_USB01_REF);
    assert(*reg(&f,C+0x1340)==0x80000000 && !*reg(&f,C+0x1308) && !*reg(&f,C+0x130c));
    assert(*reg(&f,U+0x40)==0x01400020 && (*reg(&f,U+0x400)&0x7f)==0x70);
    actual(&f);*reg(&f,U+0x400)=0x8000c07f;assert(run(&f)==A733_USB_PLATFORM_PREPARED);
    unsigned writes=f.writes;
    for(unsigned i=1;i<=writes;++i){actual(&f);f.fail_write=i;assert(run(&f)==A733_USB_PLATFORM_RETAINED);terminal(&f);}
    for(unsigned i=1;i<=writes;++i){actual(&f);f.stick_write=i;assert(run(&f)==A733_USB_PLATFORM_RETAINED);terminal(&f);}
    for(unsigned i=0;i<16;++i){actual(&f);*reg(&f,U+0x540+i*16)=0x80000000;assert(run(&f)==A733_USB_PLATFORM_RETAINED && !f.writes);terminal(&f);}
    for(unsigned i=0;i<3;++i){actual(&f);*reg(&f,U+0x40)|=1u<<(8+i);assert(run(&f)==A733_USB_PLATFORM_RETAINED && !f.writes);terminal(&f);}
    actual(&f);*reg(&f,C+0x1304)|=1;assert(run(&f)==A733_USB_PLATFORM_RETAINED && !f.writes);
    actual(&f);*reg(&f,C+0x1340)=0x81000000;assert(run(&f)==A733_USB_PLATFORM_RETAINED && !f.writes);
    actual(&f);f.config.deadline_us=f.p.config.deadline_us=150100;assert(run(&f)==A733_USB_PLATFORM_RETAINED);terminal(&f);
    actual(&f);
    while(!f.writes){assert(a733_usb_platform_poll(&f.p)==A733_USB_PLATFORM_PENDING);f.now+=100;}
    *reg(&f,U+0x540)=0x80000000;assert(run(&f)==A733_USB_PLATFORM_RETAINED);terminal(&f);
    puts("PASS ROM device takeover: actual clocks, preserved shared reference, ordered detach/reset, DMA/host refusal and retained uncertainty");
}
