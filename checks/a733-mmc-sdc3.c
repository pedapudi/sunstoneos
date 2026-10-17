/* Reuse provider/card model helpers; the previous main is never executed. */
#define main previous_mmc_platform_main
#include "a733-mmc-platform.c"
#undef main
static struct a733_mmc_platform second;
static void named_route(enum a733_mmc_emmc_host host)
{
    struct a733_pin_fields f;
    const unsigned pins[]={5,6,10};
    for(unsigned i=0;i<3;++i) {
        assert(a733_pin_fields_banked(A733_PIN_C,pins[i],&f));
        assert(((*pmic_model_word(&m.power,f.mux_address)>>f.mux_shift)&15)==3u+(unsigned)host);
    }
    assert(m.platform.pin.masks[A733_PIN_C]==0x1ef63);
}
int main(void)
{
    uint8_t data[512];struct sunxi_mmc_result result;
    for(unsigned h=0;h<2;++h) {
        storage_setup_selected(SUNXI_MMC_EMMC,(enum a733_mmc_emmc_host)h);
        unsigned index=h?3:2;
        assert(m.platform.base==0x04020000+index*0x1000);
        assert(m.platform.module==0x02002d00+index*0x10&&m.platform.bus==0x02002d0c+index*0x10);
        ready_storage();named_route((enum a733_mmc_emmc_host)h);
        assert(m.platform.clock.exclusive==(A733_CLOCK_SDC2|A733_CLOCK_SDC3));
        assert(a733_mmc_platform_read(&m.platform,0,1,data,sizeof(data),1000000)==A733_MMC_PENDING);
        enum a733_mmc_platform_result r=A733_MMC_PENDING;
        for(unsigned i=0;i<200&&r!=A733_MMC_COMPLETE;++i){r=step();assert(r!=A733_MMC_RETAINED);}
        assert(r==A733_MMC_COMPLETE);
        assert(a733_mmc_platform_take(&m.platform,&result,1000000)==A733_MMC_READY&&result.received==512);
        /* Opposite route cannot acquire either host or change a pad. */
        struct a733_mmc_platform_config c=m.platform.config;c.owner=43;c.emmc_host=h?A733_MMC_EMMC_SDC2:A733_MMC_EMMC_SDC3;
        for(unsigned i=0;i<12;++i)if(c.desired[i].function)c.desired[i].function=3+(unsigned)c.emmc_host;
        memset(&second,0,sizeof(second));
        assert(a733_mmc_platform_init(&second,&c,&m.platform.ops,&m,&m.power.clocks,&m.power.pins,&m.power.rails,&m.power.provider));
        unsigned a=m.accesses;
        assert(a733_mmc_platform_poll(&second)==A733_MMC_BUSY&&!second.clock.id&&m.accesses==a);
    }
    /* A separately owned real SDC2 pad route also blocks SDC3 before enable. */
    storage_setup_selected(SUNXI_MMC_EMMC,A733_MMC_EMMC_SDC3);
    struct a733_pin_request_banked q={A733_PIN_C,5,{0,0,0},{3,1,0},0,0,0};
    struct a733_pin_lease_banked pin={0};
    assert(a733_pins_acquire_banked(&m.power.pins,84,&q,1,&pin,1000000)==A733_PINS_OK);
    enum a733_mmc_platform_result r=A733_MMC_PENDING;
    for(unsigned i=0;i<1000;++i) {
        r=step();assert(r!=A733_MMC_RETAINED);
        if(r==A733_MMC_BUSY&&m.platform.rail_index==3)break;
    }
    assert(r==A733_MMC_BUSY&&!m.platform.pin.id&&!m.module_at&&!m.card.calls);
    assert(a733_pins_release_banked(&m.power.pins,&pin,1,1000000)==A733_PINS_OK);
    /* No second identification workload is needed to prove conflict refusal. */
    for(unsigned bad=0;bad<2;++bad) {
        storage_setup_selected(SUNXI_MMC_EMMC,A733_MMC_EMMC_SDC3);
        if(bad)m.card.reg[0x7c/4]=1;else m.card.reg[0x300/4]=0x040900;
        r=A733_MMC_PENDING;
        for(unsigned i=0;i<1000&&r!=A733_MMC_RETAINED;++i)r=step();
        assert(r==A733_MMC_RETAINED&&!m.card.command_count);retained();
    }
    storage_setup_selected(SUNXI_MMC_EMMC,A733_MMC_EMMC_SDC3);
    struct a733_mmc_platform_config c=m.platform.config;
    c.desired[2].function=2;memset(&second,0,sizeof(second));
    assert(!a733_mmc_platform_init(&second,&c,&m.platform.ops,&m,&m.power.clocks,&m.power.pins,&m.power.rails,&m.power.provider));
    assert(!m.accesses&&!second.initialized);
    for(unsigned h=0;h<2;++h) {
        enum a733_mmc_emmc_host host=(enum a733_mmc_emmc_host)h;
        uintptr_t own=h?0x04023000:0x04022000,other=h?0x04022000:0x04023000;
        assert(a733_mmc_platform_native_address_selected(SUNXI_MMC_EMMC,host,own,0));
        assert(!a733_mmc_platform_native_address_selected(SUNXI_MMC_EMMC,host,other,0));
        assert(!a733_mmc_platform_native_address_selected(SUNXI_MMC_EMMC,host,other,1));
        assert(!a733_mmc_platform_native_address_selected(SUNXI_MMC_EMMC,host,own+0x200,1));
    }
    assert(!a733_mmc_platform_native_address_selected(SUNXI_MMC_SD,A733_MMC_EMMC_SDC3,0x04020000,0));
    puts("PASS: corrected SDC2 and selected SDC3 routes, joint eMMC exclusion, actual PC ownership and retained version/DMA refusal");
}
