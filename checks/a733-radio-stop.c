/* Actual platform stop on the existing PMIC/pin/clock/SDIO composition. */
#define main existing_radio_platform_main
#include "a733-radio-platform.c"
#undef main

static enum a733_radio_platform_result stop_step(void)
{
    r.power.now+=100;
    assert(pmic_model_step(&r.power)!=A733_PMIC_RETAINED);
    unsigned pins=r.radio.pin_lease.id!=0,enabled=*pmic_model_word(&r.power,0x07025040)&2;
    enum a733_radio_platform_result status=a733_radio_platform_poll(&r.radio);
    if(!pins&&r.radio.pin_lease.id){r.pins_at=r.power.now;r.pins_ticks=r.power.now*24;}
    if(!enabled&&(*pmic_model_word(&r.power,0x07025040)&2))r.enabled_at=r.power.now;
    return status;
}
static void startup(unsigned target,unsigned off)
{
    guard_config=1;radio_setup();radio_peri0(1);
    assert(a733_radio_platform_four_bit(&r.radio));
    if(off){r.power.pmic[0x21]&=(uint8_t)~4u;r.power.pmic[0x2e]=0x3a;}
    for(unsigned i=0;r.radio.stage!=target&&i<10000;++i)
        assert(stop_step()!=A733_RADIO_RETAINED);
    assert(r.radio.stage==target);
}
static void stopped(unsigned commands,unsigned extra,uint64_t deadline)
{
    enum a733_radio_platform_result status=A733_RADIO_PENDING;
    unsigned passes=0;
    for(;status!=A733_RADIO_DOWN&&passes<10000;++passes){status=stop_step();assert(status!=A733_RADIO_RETAINED);}
    assert(status==A733_RADIO_DOWN&&!r.radio.fault&&!r.radio.held&&!r.radio.pmic_request.id);
    assert(r.commands==commands+extra&&r.power.now<deadline&&r.radio.deadline==deadline);
    assert(!(*pmic_model_word(&r.power,0x07025040)&2));
    assert(!(*pmic_model_word(&r.power,0x02002d10)&(1u<<31)));
    assert(!(*pmic_model_word(&r.power,0x02002d1c)&0x10001));
    assert(!(r.power.pmic[0x21]&4));
    assert(r.radio.host.state==SUNXI_SDIO_UNPREPARED||r.radio.host.state==SUNXI_SDIO_QUIESCED);
    unsigned accesses=r.accesses,writes=r.power.writes;
    assert(a733_radio_platform_poll(&r.radio)==A733_RADIO_DOWN);
    assert(accesses==r.accesses&&writes==r.power.writes);
    printf("stopped passes=%u elapsed_us=%llu commands=%u\n",passes,(unsigned long long)r.power.now,r.commands);
}
static struct a733_radio_bluetooth_stop bluetooth_setup(void)
{
    startup(RSTAGE_ACQUIRE,1);
    *pmic_model_word(&r.power,0x07025030)|=1u<<8;
    *pmic_model_word(&r.power,0x07025040)|=4;
    r.power.pins.handoff.latch[1].value|=4;r.power.pins.banks[A733_PIN_M].latch|=4;
    struct a733_radio_bluetooth_pins wake={.authority=11,.desired={{1,0,0},{0,0,0}}};
    assert(a733_radio_platform_bluetooth(&r.radio,&wake));
    struct a733_radio_bluetooth_stop b={.authority=12,.topology=201,
        .wiring_evidence={1},.expected={1,0,0},.inherited_level=1};
    return b;
}
static int pm2_refuse(void *v,uintptr_t a,uint32_t value)
{
    if(a==0x07025040&&!(value&4))value|=4;
    return radio_pin_write(v,a,value);
}
static void bluetooth_cases(void)
{
    for(unsigned bad=0;bad<7;++bad){
        struct a733_radio_bluetooth_stop b=bluetooth_setup();
        if(bad==0)b.authority=0;
        if(bad==1)b.topology++;
        if(bad==2)b.wiring_evidence[0]=0;
        if(bad==3)b.expected.function=0;
        if(bad==4)b.inherited_level=0;
        if(bad==5)b.reserved=1;
        if(bad==6)r.power.pins.handoff.authorized[A733_PIN_M]&=~4u;
        assert(!a733_radio_platform_bluetooth_stop(&r.radio,&b)&&!r.radio.bluetooth_stop.authority);
    }
    for(unsigned contract=0;contract<2;++contract){
        struct a733_radio_bluetooth_stop b=bluetooth_setup();
        if(contract)assert(a733_radio_platform_bluetooth_stop(&r.radio,&b));
        while(r.radio.stage!=RSTAGE_READY)assert(stop_step()!=A733_RADIO_RETAINED);
        struct a733_radio_borrow token={0};struct sdio_card *card;
        assert(a733_radio_platform_borrow(&r.radio,80,&token,&card));
        struct sunxi_sdio_request request;
        assert(sdio_card_cmd52(card,1,0x40,1,1,0x7a,&request));
        assert(sunxi_sdio_submit(&r.radio.host,&request,r.power.now+20000));
        unsigned commands=r.commands;uint64_t deadline=r.power.now+3000000;
        assert(a733_radio_platform_stop(&r.radio,&token,deadline));
        assert(a733_radio_platform_live(&r.radio,&token,deadline)==A733_RADIO_INVALID);
        for(unsigned i=0;i<500;++i)assert(stop_step()==A733_RADIO_PENDING);
        assert(r.radio.stop_phase==1&&r.radio.stop_completed&&r.radio.stop_result.response==0x7a);
        assert(r.commands==commands+1&&r.radio.stage==RSTAGE_READY&&!r.radio.peer_stopped);
        assert((*pmic_model_word(&r.power,0x07025040)&6)==6&&(r.power.pmic[0x21]&4));
        assert(a733_radio_platform_stop_held(&r.radio,&token)==A733_RADIO_READY);
        struct a733_radio_borrow forged=token;forged.generation++;
        assert(!a733_radio_platform_stop_release(&r.radio,&forged));
        assert(a733_radio_platform_stop_release(&r.radio,&token));
        assert(a733_radio_platform_stop_held(&r.radio,&token)==A733_RADIO_INVALID);
        stopped(commands,1,deadline);
        assert(r.radio.peer_stopped==contract);
        assert(!!(*pmic_model_word(&r.power,0x07025040)&4)==!contract);
    }
    struct a733_radio_bluetooth_stop b=bluetooth_setup();
    assert(a733_radio_platform_bluetooth_stop(&r.radio,&b));
    while(r.radio.stage!=RSTAGE_READY)assert(stop_step()!=A733_RADIO_RETAINED);
    struct a733_radio_borrow token={0};struct sdio_card *card;
    assert(a733_radio_platform_borrow(&r.radio,80,&token,&card));
    assert(a733_radio_platform_stop(&r.radio,&token,r.power.now+3000000));
    assert(a733_radio_platform_stop_release(&r.radio,&token));
    r.power.pins.operations.write=pm2_refuse;
    enum a733_radio_platform_result status=A733_RADIO_PENDING;
    for(unsigned i=0;status!=A733_RADIO_RETAINED&&i<20;++i)status=stop_step();
    assert(status==A733_RADIO_RETAINED&&!r.radio.peer_stopped&&r.radio.bluetooth_stop_lease.id);
    assert((*pmic_model_word(&r.power,0x07025040)&6)==6);
    puts("PASS explicit Bluetooth reset authority, concurrent drain, one-way release and refused readback");
}
int main(void)
{
    bluetooth_cases();
    guard_config=0;radio_setup();
    struct a733_radio_borrow early={0};
    assert(a733_radio_platform_stop(&r.radio,&early,r.power.now+3000000));
    struct a733_radio_bluetooth_pins wake={.authority=11,.desired={{1,0,0},{0,0,0}}};
    assert(!a733_radio_platform_four_bit(&r.radio));
    assert(!a733_radio_platform_first_run_guard(&r.radio));
    assert(!a733_radio_platform_bluetooth(&r.radio,&wake));
    for(unsigned off=0;off<2;++off)for(unsigned stage=RSTAGE_ACQUIRE;stage<=RSTAGE_READY;++stage){
        if(!off&&stage>=RSTAGE_POWER_ON&&stage<=RSTAGE_VERIFY_RAILS)continue;
        startup(stage,off);
        unsigned commands=r.commands,enabled=!!(r.power.pmic[0x21]&4);
        struct a733_radio_borrow token={0};uint64_t deadline=r.power.now+3000000;
        assert(a733_radio_platform_stop(&r.radio,&token,deadline));
        assert(a733_radio_platform_stop(&r.radio,&token,deadline+1000)&&r.radio.stop_deadline==deadline);
        stopped(commands,0,deadline);
        if(!enabled&&stage<=RSTAGE_POWER_ON){
            assert(!pmic_log_count);
            struct a733_radio_report report={0};a733_radio_report_platform(&report,&r.radio);
            assert(report.head.stage<A733_RADIO_STAGE_POWERED&&(report.head.flags&8));
        }
        printf("PASS partial startup stage=%u initial_off=%u\n",stage,off);
    }
    /* Stop between host admission, issue and completion; never admit another card command. */
    for(unsigned phase=SUNXI_SDIO_QUEUED;phase<=SUNXI_SDIO_DONE;++phase){
        startup(RSTAGE_READY,1);
        struct a733_radio_borrow token={0};struct sdio_card *card;
        assert(a733_radio_platform_borrow(&r.radio,80,&token,&card));
        struct sunxi_sdio_request submitted;
        assert(sdio_card_cmd52(card,1,0x40,1,1,0x7a,&submitted));
        assert(sunxi_sdio_submit(&r.radio.host,&submitted,r.power.now+20000));
        for(unsigned i=0;r.radio.host.state!=(enum sunxi_sdio_state)phase&&i<100;++i)
            sunxi_sdio_poll(&r.radio.host);
        assert(r.radio.host.state==(enum sunxi_sdio_state)phase);
        struct sunxi_sdio_request request=r.radio.host.request;
        uint64_t accepted_deadline=r.radio.host.deadline,deadline=r.power.now+3000000;
        unsigned commands=r.commands;
        assert(a733_radio_platform_stop(&r.radio,&token,deadline));
        assert(r.radio.host.deadline==accepted_deadline&&!memcmp(&request,&r.radio.host.request,sizeof(request)));
        stopped(commands,phase==SUNXI_SDIO_QUEUED,deadline);
        assert(r.radio.stop_completed&&!r.radio.stop_result.error&&r.radio.stop_result.response==0x7a);
    }
    /* An expired accepted operation cannot use the normal abort's extra two seconds. */
    startup(RSTAGE_ENUMERATING,1);
    while(r.radio.host.state!=SUNXI_SDIO_QUEUED)assert(stop_step()!=A733_RADIO_RETAINED);
    struct a733_radio_borrow token={0};uint64_t deadline=r.power.now+3000000;
    assert(a733_radio_platform_stop(&r.radio,&token,deadline));
    r.power.now=r.radio.host.deadline;unsigned accesses=r.accesses,writes=r.power.writes;
    assert(a733_radio_platform_poll(&r.radio)==A733_RADIO_RETAINED);
    assert(!r.radio.aborting&&r.radio.deadline==deadline&&r.accesses==accesses&&r.power.writes==writes);
    assert(a733_radio_platform_poll(&r.radio)==A733_RADIO_RETAINED&&r.accesses==accesses&&r.power.writes==writes);
    puts("PASS radio partial stop, accepted-command completion and deadline retention");
    return 0;
}
