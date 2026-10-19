/* Captured device session through the actual provider; no physical access. */
#define A733_USB_ROM_HANDOFF_FIXTURE_ONLY
#include "a733-usb-rom-handoff.c"
int main(void)
{
    struct fixture f;
    actual(&f);*reg(&f,U+0x40)=0x00029940;
    assert(run(&f)==A733_USB_PLATFORM_PREPARED && f.reset_seen);
    assert(*reg(&f,U+0x40)==0x00029900); /* only soft-connect removed */
    assert(f.p.token.id==f.before.id && f.p.token.shared&A733_CLOCK_USB01_REF);
    unsigned total=f.writes;
    for(unsigned bit=9;bit<=10;++bit){
        actual(&f);*reg(&f,U+0x40)=0x00029940|(1u<<bit);
        assert(run(&f)==A733_USB_PLATFORM_RETAINED && !f.writes);terminal(&f);
    }
    actual(&f);*reg(&f,U+0x40)=0x00021940; /* no B-device authority */
    assert(run(&f)==A733_USB_PLATFORM_RETAINED && !f.writes);terminal(&f);
    for(unsigned i=0;i<16;++i){
        actual(&f);*reg(&f,U+0x40)=0x00029940;*reg(&f,U+0x540+i*16)=0x80000000;
        assert(run(&f)==A733_USB_PLATFORM_RETAINED && !f.writes);terminal(&f);
    }
    /* Later HOSTMODE must also be rejected by the detached-core predicate. */
    actual(&f);*reg(&f,U+0x40)=0x00029940;
    while(f.p.stage!=28){ /* ROM_CORE_CHECK, after reset release */
        assert(a733_usb_platform_poll(&f.p)==A733_USB_PLATFORM_PENDING);f.now+=100;
    }
    *reg(&f,U+0x40)|=1u<<10;unsigned before=f.writes;
    assert(run(&f)==A733_USB_PLATFORM_RETAINED && f.writes==before);
    assert(f.p.failure.address==U+0x40 && f.p.failure.mask==0x640);terminal(&f);
    for(unsigned i=1;i<=total;++i){
        actual(&f);*reg(&f,U+0x40)=0x00029940;f.fail_write=i;
        assert(run(&f)==A733_USB_PLATFORM_RETAINED);terminal(&f);
    }
    puts("PASS captured SESSION device handoff, host/B-device/DMA refusals, detached host-mode guard and retained write failures");
}
