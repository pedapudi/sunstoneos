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
    *reg(f,U+0x40)=0x01408060; /* EP index, B-device and high-speed/soft-connect. */
    *reg(f,U+0x48)=0x00040003;*reg(f,U+0x50)=255;*reg(f,U+0x500)=65535;
    *reg(f,U+0x400)=0x8000007f;f->device_enabled=0;
    assert(a733_usb_platform_init(&f->p,&f->config,&ops,f,&f->registry));
}
#ifndef A733_USB_ROM_HANDOFF_FIXTURE_ONLY
int main(void)
{
    struct fixture f;actual(&f);
    assert(run(&f)==A733_USB_PLATFORM_PREPARED && f.reset_seen);
    assert(f.p.token.id==f.before.id && f.p.token.exclusive==A733_CLOCK_USB0);
    assert(f.p.token.shared&A733_CLOCK_USB01_REF);
    assert(*reg(&f,C+0x1340)==0x80000000 && !*reg(&f,C+0x1308) && !*reg(&f,C+0x130c));
    assert(*reg(&f,U+0x40)==0x01408020 && (*reg(&f,U+0x400)&0x7f)==0x70);
    actual(&f);*reg(&f,U+0x400)=0x8000c07f;assert(run(&f)==A733_USB_PLATFORM_PREPARED);
    unsigned writes=f.writes;
    for(unsigned i=1;i<=writes;++i){actual(&f);f.fail_write=i;assert(run(&f)==A733_USB_PLATFORM_RETAINED);terminal(&f);}
    for(unsigned i=1;i<=writes;++i){actual(&f);f.stick_write=i;assert(run(&f)==A733_USB_PLATFORM_RETAINED);terminal(&f);}
    for(unsigned i=0;i<16;++i){actual(&f);*reg(&f,U+0x540+i*16)=0x80000000;assert(run(&f)==A733_USB_PLATFORM_RETAINED && !f.writes);terminal(&f);}
    for(unsigned i=0;i<2;++i){actual(&f);*reg(&f,U+0x40)|=1u<<(9+i);assert(run(&f)==A733_USB_PLATFORM_RETAINED && !f.writes);terminal(&f);}
    actual(&f);*reg(&f,C+0x1304)|=1;assert(run(&f)==A733_USB_PLATFORM_RETAINED && !f.writes);
    actual(&f);*reg(&f,C+0x1340)=0x81000000;assert(run(&f)==A733_USB_PLATFORM_RETAINED && !f.writes);
    actual(&f);f.config.deadline_us=f.p.config.deadline_us=150100;assert(run(&f)==A733_USB_PLATFORM_RETAINED);terminal(&f);
    actual(&f);
    while(!f.writes){assert(a733_usb_platform_poll(&f.p)==A733_USB_PLATFORM_PENDING);f.now+=100;}
    *reg(&f,U+0x540)=0x80000000;assert(run(&f)==A733_USB_PLATFORM_RETAINED);terminal(&f);
    /* Words observed on 2026-10-23 before the refused disconnect: packed
     * PCTL/DEVCTL/EPIND/VEND0 0x00029940 (SoftConn; SESSION, VBUS valid and
     * B-device; endpoint index 2) and the matched ROM's interrupt enables. */
    const uint32_t observed=0x00029940;
    actual(&f);*reg(&f,U+0x40)=observed;*reg(&f,U+0x48)=1;*reg(&f,U+0x50)=7;
    *reg(&f,U+0x500)=0;*reg(&f,U+0x400)=0x8000c000;
    assert(run(&f)==A733_USB_PLATFORM_PREPARED && f.reset_seen && *reg(&f,U+0x40)==0x00029900);
    /* SoftConn that reads back clear only 100 ms after its write still passes. */
    actual(&f);*reg(&f,U+0x40)=observed;f.pctl_delay=100000;
    assert(run(&f)==A733_USB_PLATFORM_PREPARED && f.reset_seen && *reg(&f,U+0x40)==0x00029900);
    /* SoftConn that never clears: the provider re-reads without writing
     * until the deadline and refuses before the reset write. */
    actual(&f);*reg(&f,U+0x40)=observed;f.pctl_stuck=1;
    enum a733_usb_platform_result r=A733_USB_PLATFORM_PENDING;unsigned polls=0;
    while(r==A733_USB_PLATFORM_PENDING){assert(++polls<2000);r=a733_usb_platform_poll(&f.p);f.now+=1000;}
    assert(r==A733_USB_PLATFORM_RETAINED && !f.reset_seen && f.writes==5);
    assert(f.p.failure.kind==A733_USB_FAILURE_TIME && f.p.failure.stage==29 &&
        f.p.failure.address==U+0x40 && f.p.failure.observed==observed);
    assert(*reg(&f,C+0x1304)==0x01000100);terminal(&f);
    /* The 150 ms interval starts at the first read with SoftConn clear. */
    actual(&f);*reg(&f,U+0x40)=observed;f.pctl_delay=30000;
    while(f.p.stage!=23){assert(a733_usb_platform_poll(&f.p)==A733_USB_PLATFORM_PENDING);f.now+=100;}
    assert(f.p.wait_until>=f.pctl_at+150000 && f.p.wait_until<f.pctl_at+150000+200);
    /* Inherited bits: other PCTL bits, SESSION clear, LSDEV and FSDEV,
     * VEND0 bus selection and endpoint index 0. */
    const uint32_t inherited[]={observed|0xbf,observed&~0x100u,observed|1u<<13,observed|1u<<14,
        observed|0x01000000,observed&~0x00ff0000u};
    for(unsigned i=0;i<6;++i){
        actual(&f);*reg(&f,U+0x40)=inherited[i];
        assert(run(&f)==A733_USB_PLATFORM_PREPARED && f.reset_seen);
        assert(*reg(&f,U+0x40)==(inherited[i]&~64u));
    }
    /* Refused bits: HOSTREQ, HOSTMODE, VEND0 DMA request selection, and a
     * missing B-device role; no write before refusal. */
    const uint32_t refused[]={observed|1u<<9,observed|1u<<10,
        observed&~0x8000u,observed|1u<<25,observed|1u<<26,observed|1u<<27,observed|1u<<28,
        observed|1u<<29,observed|1u<<30,observed|1u<<31};
    for(unsigned i=0;i<sizeof(refused)/sizeof(*refused);++i){
        actual(&f);*reg(&f,U+0x40)=refused[i];
        assert(run(&f)==A733_USB_PLATFORM_RETAINED && !f.writes);
        assert(f.p.failure.stage==20 && f.p.failure.address==U+0x40 && f.p.failure.observed==refused[i]);
        terminal(&f);
    }
    puts("PASS ROM device takeover: actual clocks, preserved shared reference, ordered detach/reset, DMA/host refusal, observed 2026-10-23 words, detach interval from the first clear SoftConn read and retained uncertainty");
}

#endif
