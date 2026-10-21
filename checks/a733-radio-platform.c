/* Actual PMIC, rail, pin, clock, SDIO and card composition on modeled MMIO. */
#define PMIC_MODEL_NO_MAIN
#include "a733-pmic.c"
#include "a733_radio_platform_native.h"
struct radio_model {
    struct pmic_model power;
    struct a733_radio_platform radio;
    uint32_t sdc[256];
    uint8_t card[3][0x20000];
    unsigned commands,ready_queries,accesses,register_retained,admitted,fail_write;
    uint64_t pins_at,enabled_at,module_at,first_host_at;
};
static struct radio_model r;
static int radio_now(void *v,uint64_t *out)
{*out=((struct radio_model *)v)->power.now;return 1;}
static int radio_current(void *v){return !((struct radio_model *)v)->power.worker;}
static int radio_admit(void *v,const struct a733_radio_platform_config *c,const struct a733_clock_token *t)
{
    struct radio_model *m=v;
    assert(c->handoff==200 && c->topology.identity==201 && t->owner==42);
    assert(a733_clock_token_status(&m->power.clocks,t)==A733_CLOCK_OK);
    ++m->admitted;return 1;
}
static int radio_enter(void *v,uint64_t deadline)
{struct radio_model *m=v;return m->register_retained?0:pmic_model_pin_enter(&m->power,deadline);}
static void radio_leave(void *v){pmic_model_pin_leave(&((struct radio_model *)v)->power);}
static void radio_retain(void *v)
{struct radio_model *m=v;assert(m->power.register_held);m->register_retained=1;}
static void host_accessible(struct radio_model *m)
{
    /* Only widening holds the register lock, for its module-clock change. */
    assert(m->admitted && m->radio.pin_lease.id);
    assert(!m->power.register_held || m->radio.host.state==SUNXI_SDIO_WIDENING);
    assert(a733_rail_status(&m->power.rails,&m->radio.rail)==A733_RAIL_READY);
    assert(m->enabled_at && m->power.now-m->enabled_at>=50000);
    assert((*pmic_model_word(&m->power,0x02002d1c)&0x10001)==0x10001);
    uint32_t module=*pmic_model_word(&m->power,0x02002d10)&0x87001f1f;
    assert(module==0x8000001d || (m->radio.four_bit && module==0x81000007));
    if(!m->first_host_at)m->first_host_at=m->power.now;
}
static int radio_read(void *v,uintptr_t a,uint32_t *out)
{
    struct radio_model *m=v;++m->accesses;
    assert(a733_radio_platform_native_address(a,0));
    if(a>=0x04021000 && a<=0x04021300) {
        host_accessible(m);unsigned off=a-0x04021000;
        if(!off)m->sdc[0]&=~7u;
        *out=m->sdc[off/4];return 1;
    }
    assert(m->power.register_held);*out=*pmic_model_word(&m->power,a);return 1;
}
static int radio_write(void *v,uintptr_t a,uint32_t value)
{
    struct radio_model *m=v;++m->accesses;
    assert(a733_radio_platform_native_address(a,1));
    if(a>=0x04021000 && a<=0x04021300) {
        host_accessible(m);unsigned off=a-0x04021000;
        if(off==0x38){assert(!(value&(1u<<16)));m->sdc[off/4]&=~value;return 1;}
        m->sdc[off/4]=value;if(off!=0x18)return 1;
        if(value&(1u<<21)){m->sdc[off/4]&=~(1u<<31);return 1;}
        m->sdc[off/4]&=~(1u<<31); /* The controller has loaded the command. */
        unsigned command=value&63;uint32_t arg=m->sdc[0x1c/4];
        assert(command==3 || command==5 || command==7 || command==52);
        ++m->commands;m->sdc[0x38/4]|=4;m->sdc[0x20/4]=0;
        if(command==5) {
            m->sdc[0x20/4]=(2u<<28)|0x00ff8000;
            if(arg && ++m->ready_queries>1)m->sdc[0x20/4]|=1u<<31;
        } else if(command==3)m->sdc[0x20/4]=0x12340000;
        else if(command==7)assert(arg==0x12340000);
        else {
            unsigned fn=(arg>>28)&7,address=(arg>>9)&0x1ffff;assert(fn<3);
            if(arg>>31)m->card[fn][address]=(uint8_t)arg;
            m->sdc[0x20/4]=m->card[fn][address];
        }
        return 1;
    }
    assert(m->power.register_held && m->admitted);
    if(a==0x03400198){assert(value==4);*pmic_model_word(&m->power,0x03400118)&=~4u;}
    else {
        assert(a==0x02002d10 || a==0x02002d1c);
        if(a==0x02002d10 && (value&(1u<<31))) {
            assert(m->enabled_at && m->power.now-m->enabled_at>=50000);m->module_at=m->power.now;
        }
        if(a==0x02002d1c && (value&(1u<<16)))assert(m->module_at && m->power.now-m->module_at>=10);
        *pmic_model_word(&m->power,a)=value;
        if(m->fail_write)return 0;
    }
    return 1;
}
static int radio_barrier(void *v){(void)v;return 1;}
static void radio_cis(struct radio_model *m,unsigned function)
{
    unsigned address=0x1000+function*0x100;
    for(unsigned i=0;i<3;++i)m->card[0][function*256+9+i]=(uint8_t)(address>>(8*i));
    m->card[0][function*256]=7;uint8_t *p=m->card[0]+address;
    *p++=0x20;*p++=4;*p++=0xa1;*p++=0xc8;*p++=0x82;*p++=1;
    *p++=0x22;
    if(function){*p++=42;p[0]=1;p[13]=2;p[28]=50;p+=42;}
    else {*p++=4;*p++=0;*p++=0;*p++=2;*p++=0x32;}
    *p=0xff;
}
static void radio_setup(void)
{
    /* Each setup represents a separate simulated board lifetime. */
    memset(&r,0,sizeof(r));pmic_model_setup(&r.power);
    *pmic_model_word(&r.power,0x02002000)=0xf8000000u|(99u<<16)|(99u<<8);
    *pmic_model_word(&r.power,0x02002580)=1u<<31;
    *pmic_model_word(&r.power,0x02002588)=1u<<31;
    *pmic_model_word(&r.power,0x020025c0)=1u<<24;
    *pmic_model_word(&r.power,0x020025e0)=1u<<30;
    *pmic_model_word(&r.power,0x0200259c)=0x30001;
    r.sdc[0x300/4]=0x03050530;r.sdc[0x5c/4]=0x81710000;
    for(unsigned i=0;i<3;++i)radio_cis(&r,i);
    r.card[0][0]=0x43;r.card[0][1]=3;r.card[0][8]=2;
    struct a733_radio_platform_config config={.owner=42,.handoff=200,
        .deadline_us=500000,.lifetime_deadline_us=10000000,
        .topology={201,A733_RADIO_DCDC1,A733_RADIO_DCDC8,A733_RADIO_DCDC8,A733_RADIO_DCDC8},
        .fabric={0,1u<<31,1u<<31},.fabric_hz={24000000,24000000,24000000},
        .card={.supported_ocr=0x00300000,.command_timeout=20000,.ready_poll_interval=100}};
    for(unsigned i=0;i<8;++i)config.desired[i].function=i<6?2:i==6?0:1;
    const struct a733_radio_platform_ops ops={radio_now,radio_current,radio_admit,radio_enter,
        radio_leave,radio_retain,radio_read,radio_write,radio_barrier};
    assert(a733_radio_platform_init(&r.radio,&config,&ops,&r,&r.power.clocks,
        &r.power.pins,&r.power.rails,&r.power.provider));
}
static enum a733_radio_platform_result radio_step(void)
{
    r.power.now+=100;
    assert(pmic_model_step(&r.power)!=A733_PMIC_RETAINED);
    unsigned was_enabled=*pmic_model_word(&r.power,0x07025040)&2;
    unsigned had_pins=r.radio.pin_lease.id!=0;
    enum a733_radio_platform_result result=a733_radio_platform_poll(&r.radio);
    if(!had_pins && r.radio.pin_lease.id)r.pins_at=r.power.now;
    unsigned enabled=*pmic_model_word(&r.power,0x07025040)&2;
    if(!was_enabled && enabled){assert(r.pins_at && r.power.now-r.pins_at>=10000);r.enabled_at=r.power.now;}
    assert(!(*pmic_model_word(&r.power,0x07025040)&4)); /* PM2 level preserved. */
    return result;
}
/* PERI0 at 2400 MHz with its 400 MHz branch (divider 6) enabled and gated on. */
static void radio_peri0(unsigned gated)
{
    *pmic_model_word(&r.power,0x020020a0)=0xfe000000u|(1u<<20)|(99u<<8);
    uint32_t branch=(1u<<11)|(1u<<2)|(1u<<1);
    *pmic_model_word(&r.power,0x02003908)=gated?branch|(branch<<16):0;
    *pmic_model_word(&r.power,0x02003988)=branch<<16;
}
static void retained_without_io(void)
{
    unsigned accesses=r.accesses,writes=r.power.writes;
    assert(a733_radio_platform_poll(&r.radio)==A733_RADIO_RETAINED);
    assert(r.accesses==accesses && r.power.writes==writes);
    assert(a733_clock_token_status(&r.power.clocks,&r.radio.clock)==A733_CLOCK_FAULT);
    assert(a733_rail_status(&r.power.rails,&r.radio.rail)==A733_RAIL_RETAINED);
}
int main(void)
{
    radio_setup();
    enum a733_radio_platform_result status=A733_RADIO_PENDING;
    for(unsigned i=0;i<4000 && status!=A733_RADIO_READY;++i){status=radio_step();assert(status!=A733_RADIO_RETAINED);}
    assert(status==A733_RADIO_READY && r.radio.card.function_count==2 && r.radio.host.card_hz==400000);
    assert(r.radio.rail_count==2 && r.radio.rail_index==2 && !r.radio.pmic_request.id);
    assert(r.radio.snapshots[0].programmed_uv==3300000 && r.radio.snapshots[1].programmed_uv==1800000);
    assert(r.first_host_at>=r.enabled_at+50000 && r.power.provider.pin.id && r.radio.pin_lease.id);
    struct a733_clock_token collision={0};
    assert(a733_clock_acquire(&r.power.clocks,99,0,A733_CLOCK_SDC1,1000000,&collision)==A733_CLOCK_BUSY);
    assert(a733_clock_acquire(&r.power.clocks,99,0,A733_CLOCK_RADIO_MODULE,1000000,&collision)==A733_CLOCK_BUSY);
    struct a733_rail_token rail_collision={0};
    assert(a733_rail_acquire(&r.power.rails,99,0,A733_RAIL_BIT(A733_RAIL_DCDC8),1000000,&rail_collision)==A733_RAIL_BUSY);
    struct a733_radio_borrow token;struct sdio_card *card=0;
    assert(!a733_radio_platform_borrow(&r.radio,80,(void *)&r.power.pins,&card));
    assert(a733_radio_platform_borrow(&r.radio,80,&token,&card) && card==&r.radio.card);
    assert(!a733_radio_platform_borrow(&r.radio,81,&token,&card));
    struct a733_radio_borrow forged=token;++forged.generation;
    assert(a733_radio_platform_live(&r.radio,&forged,r.power.now+1000)==A733_RADIO_INVALID);
    uint64_t deadline=r.power.now+1000;
    assert(a733_radio_platform_live(&r.radio,&token,deadline)==A733_RADIO_READY);
    struct sunxi_sdio_request request;struct sunxi_sdio_result result;
    assert(sdio_card_cmd52(card,1,0x40,1,1,0x7a,&request));
    assert(sunxi_sdio_submit(&r.radio.host,&request,deadline));
    assert(a733_radio_platform_live(&r.radio,&token,deadline+1)==A733_RADIO_INVALID);
    for(unsigned i=0;i<20 && r.radio.host.state!=SUNXI_SDIO_DONE;++i){++r.power.now;sunxi_sdio_poll(&r.radio.host);}
    assert(sunxi_sdio_take_result(&r.radio.host,&result) && !result.error && result.response==0x7a);
    r.power.now=deadline+1;deadline=r.power.now+1000;
    assert(a733_radio_platform_live(&r.radio,&token,deadline)==A733_RADIO_READY);
    *pmic_model_word(&r.power,0x07025040)|=1;unsigned level=9;
    assert(a733_radio_platform_wake(&r.radio,&token,deadline,&level)==A733_RADIO_READY && level==1);
    assert(a733_radio_platform_wake(&r.radio,&token,deadline,(void *)&r.power.rails)==A733_RADIO_INVALID);
    assert(sunxi_sdio_submit(&r.radio.host,&request,deadline));r.power.now=deadline;
    assert(a733_radio_platform_live(&r.radio,&token,deadline+1000)==A733_RADIO_RETAINED);
    retained_without_io();
    /* Wrong observed rail voltage stops before module or controller writes. */
    radio_setup();r.power.pmic[0x19]=99;
    status=A733_RADIO_PENDING;
    for(unsigned i=0;i<500 && status!=A733_RADIO_RETAINED;++i)status=radio_step();
    assert(status==A733_RADIO_RETAINED && !r.accesses && !r.radio.pin_lease.id);
    assert(r.radio.pmic_request.id && a733_pmic_reap(&r.power.provider,&r.radio.pmic_request)!=A733_PMIC_COMPLETE);
    retained_without_io();
    /* A clock write with uncertain completion retains the common lock. */
    radio_setup();r.fail_write=1;status=A733_RADIO_PENDING;
    for(unsigned i=0;i<1500 && status!=A733_RADIO_RETAINED;++i)status=radio_step();
    assert(status==A733_RADIO_RETAINED && r.register_retained && r.radio.pin_lease.id && !r.first_host_at);
    retained_without_io();
    /* Four-bit request: CCCR width with read-after-write, module switch under the
     * register lock, controller verification and a CCCR reread at 25 MHz. */
    radio_setup();radio_peri0(1);
    assert(a733_radio_platform_four_bit(&r.radio) && !a733_radio_platform_four_bit(&r.radio));
    status=A733_RADIO_PENDING;
    for(unsigned i=0;i<4000 && status!=A733_RADIO_READY;++i){status=radio_step();assert(status!=A733_RADIO_RETAINED);}
    assert(status==A733_RADIO_READY && r.radio.host.wide && r.radio.host.card_hz==25000000);
    assert((r.card[0][7]&3)==2 && r.sdc[0xc/4]==1 && r.sdc[0x4/4]==(1u<<16));
    assert(((r.sdc[0x140/4]>>16)&3)==3 && r.sdc[0x144/4]==(1u<<7) && !(r.sdc[0x5c/4]&0x80000000u));
    assert((*pmic_model_word(&r.power,0x02002d10)&0x87001f1f)==0x81000007 && !r.power.register_held);
    assert(r.radio.shared&A733_CLOCK_PERI0);
    assert(a733_radio_platform_borrow(&r.radio,80,&token,&card) && card==&r.radio.card);
    /* Too late after the first poll; a missing 400 MHz branch stops before any
     * module or controller write. */
    assert(!a733_radio_platform_four_bit(&r.radio));
    radio_setup();radio_peri0(0);assert(a733_radio_platform_four_bit(&r.radio));
    status=A733_RADIO_PENDING;
    for(unsigned i=0;i<1500 && status!=A733_RADIO_RETAINED;++i)status=radio_step();
    assert(status==A733_RADIO_RETAINED && !r.first_host_at && !r.radio.initial_module);
    retained_without_io();
    assert(!a733_radio_platform_native_address(0x04021078,1));
    assert(!a733_radio_platform_native_address(0x04021084,1));
    assert(!a733_radio_platform_native_address(0x04020000,1));
    assert(!a733_radio_platform_native_address(0x020025c0,1));
    assert(!a733_radio_platform_native_address(0x07025040,1));
    puts("PASS actual radio/PMIC/rail/pin/clock/card composition, power settling, exclusive lifetime, bounded borrowing, four-bit widening and retained faults");
    return 0;
}
