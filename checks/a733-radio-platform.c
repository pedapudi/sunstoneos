/* Actual PMIC, rail, pin, clock, SDIO and card composition on modeled MMIO. */
#define PMIC_MODEL_NO_MAIN
#include "a733-pmic.c"
#include <stdlib.h>
#include <string.h>
#include "a733_radio_platform_native.h"
#include "a733_radio_result.h"
struct radio_model {
    struct pmic_model power;
    struct a733_radio_platform radio;
    uint32_t sdc[256];
    uint8_t card[3][0x20000];
    unsigned commands,ready_queries,accesses,register_retained,admitted,fail_write,fail_command,fail_pm1;
    uint64_t pins_at,enabled_at,module_at,first_host_at;
    uint64_t pins_ticks,enabled_ticks;
    unsigned fraction,gpio_cost;
};
static struct radio_model r;
/* Every PMIC register data write, with the PM1 level and time at which it happened. */
static struct {uint8_t reg,value;unsigned pm1;uint64_t at;} pmic_log[16];
static unsigned pmic_log_count,stuck_enable,guard_config;
static void pmic_logged(void *v,uint8_t reg,uint8_t value)
{
    struct radio_model *m=v;assert(pmic_log_count<16);
    pmic_log[pmic_log_count].reg=reg;pmic_log[pmic_log_count].value=value;
    pmic_log[pmic_log_count].pm1=!!(*pmic_model_word(&m->power,0x07025040)&2);
    pmic_log[pmic_log_count++].at=m->power.now;
    if(stuck_enable&&reg==0x21)m->power.pmic[0x21]&=(uint8_t)~4u; /* The output refuses to turn on. */
}
/* One line per record for the host decoder check: RESULT name hex. */
static void *report_allocate(void *context,size_t bytes){++*(unsigned *)context;return malloc(bytes);}
static void report_release(void *context,void *memory){(void)context;free(memory);}
static void emit_result(const char *name,const struct a733_radio_platform *p,unsigned scan)
{
    static unsigned allocations;
    struct a733_radio_report report;
    const struct a733_radio_allocator memory={report_allocate,report_release,&allocations};
    assert(a733_radio_report_init(&report,&memory));
    a733_radio_report_platform(&report,p);
    if(scan){
        static const uint8_t bssid_a[6]={0x02,0x11,0x22,0x33,0x44,0x55},bssid_b[6]={0x02,0xaa,0xbb,0xcc,0xdd,0xee};
        a733_radio_report_firmware(&report);a733_radio_report_identity(&report,0x00080001,0x20);
        a733_radio_report_ap(&report,bssid_a,(const uint8_t *)"lab",3,2437,-61);
        a733_radio_report_ap(&report,bssid_b,(const uint8_t *)"guest",5,5180,-48);
        a733_radio_report_ap(&report,bssid_a,(const uint8_t *)"lab",3,2437,-55);
        a733_radio_report_scan_done(&report,3);
    }
    if(scan==2){ /* Far more access points than any fixed list: 300, then the record is longer than its head. */
        for(unsigned i=0;i<300;++i){
            uint8_t bssid[6]={2,0,0,0,(uint8_t)(i>>8),(uint8_t)i};char ssid[8];ssid[0]='n';ssid[1]=(char)('0'+i%10);ssid[2]=0;
            a733_radio_report_ap(&report,bssid,(const uint8_t *)ssid,2,(uint16_t)(2412+5*(i%11)),(int8_t)(-30-(int)(i%60)));
        }
        a733_radio_report_scan_done(&report,303);
    }
    size_t bytes=a733_radio_report_bytes(&report);
    uint8_t *record=malloc(bytes);
    assert(!a733_radio_report_encode(&report,record,bytes-1)); /* Too small a buffer writes nothing. */
    assert(a733_radio_report_encode(&report,record,bytes)==bytes);
    printf("RESULT %s ",name);
    for(size_t i=0;i<bytes;++i)printf("%02x",record[i]);
    printf("\n");
    free(record);a733_radio_report_release(&report);
}
static int radio_pin_write(void *v,uintptr_t a,uint32_t value)
{
    unsigned rising=a==0x07025040&&(value&2)&&!(*pmic_model_word(v,a)&2);
    if(rising&&((struct radio_model *)v)->fail_pm1)return 0; /* The pin manager's write fails. */
    if(rising)r.power.now+=r.gpio_cost;
    int result=pmic_model_pin_write(v,a,value);
    if(rising){r.enabled_ticks=r.power.now*24+r.fraction;
        assert(r.enabled_ticks-r.pins_ticks>=10000*24);r.enabled_at=r.power.now;}
    return result;
}
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
        if(command==m->fail_command){m->sdc[0x38/4]=(m->sdc[0x38/4]&~4u)|0x100;return 1;} /* No response. */
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
            assert(m->power.now*24+m->fraction-m->enabled_ticks>=50000*24);
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
static int radio_try_setup(enum a733_radio_rail supply)
{
    /* Each setup represents a separate simulated board lifetime. */
    memset(&r,0,sizeof(r));pmic_model_setup(&r.power);
    r.power.pins.operations.write=radio_pin_write;
    *pmic_model_word(&r.power,0x02002000)=0xf8000000u|(99u<<16)|(99u<<8);
    *pmic_model_word(&r.power,0x02002580)=1u<<31;
    *pmic_model_word(&r.power,0x02002588)=1u<<31;
    *pmic_model_word(&r.power,0x020025c0)=1u<<24;
    *pmic_model_word(&r.power,0x020025e0)=1u<<30;
    *pmic_model_word(&r.power,0x0200259c)=0x30001;
    r.sdc[0x300/4]=0x03050530;r.sdc[0x5c/4]=0x81710000;
    pmic_log_count=0;
    r.power.pmic_written=pmic_logged;r.power.observer=&r;
    for(unsigned i=0;i<3;++i)radio_cis(&r,i);
    r.card[0][0]=0x43;r.card[0][1]=3;r.card[0][8]=2;
    /* The vendor wlan and bt power helpers enable BLDO5 (VCC-PG) and CLDO1
     * (VCC-PM) at 1.8V from VBAT's always-on DCDC1; DCDC8 is not in that path. */
    struct a733_radio_platform_config config={.owner=42,.handoff=200,
        .deadline_us=500000,.lifetime_deadline_us=10000000,
        .topology={201,A733_RADIO_DCDC1,supply,A733_RADIO_CLDO1,supply},
        .fabric={0,1u<<31,1u<<31},.fabric_hz={24000000,24000000,24000000},
        .card={.supported_ocr=0x00300000,.command_timeout=20000,.ready_poll_interval=100}};
    for(unsigned i=0;i<8;++i)config.desired[i].function=i<6?2:i==6?0:1;
    const struct a733_radio_platform_ops ops={radio_now,radio_current,radio_admit,radio_enter,
        radio_leave,radio_retain,radio_read,radio_write,radio_barrier};
    int ok=a733_radio_platform_init(&r.radio,&config,&ops,&r,&r.power.clocks,
        &r.power.pins,&r.power.rails,&r.power.provider);
    if(ok&&guard_config)assert(a733_radio_platform_first_run_guard(&r.radio)&&!a733_radio_platform_first_run_guard(&r.radio));
    return ok;
}
static void radio_setup(void){assert(radio_try_setup(A733_RADIO_BLDO5));}
static enum a733_radio_platform_result radio_step(void)
{
    r.power.now+=100;
    assert(pmic_model_step(&r.power)!=A733_PMIC_RETAINED);
    unsigned was_enabled=*pmic_model_word(&r.power,0x07025040)&2;
    unsigned had_pins=r.radio.pin_lease.id!=0;
    enum a733_radio_platform_result result=a733_radio_platform_poll(&r.radio);
    if(!had_pins && r.radio.pin_lease.id){r.pins_at=r.power.now;r.pins_ticks=r.power.now*24+r.fraction;}
    unsigned enabled=*pmic_model_word(&r.power,0x07025040)&2;
    if(!was_enabled && enabled){assert(r.pins_at && r.power.now-r.pins_at>=10000);r.enabled_at=r.power.now;}
    assert(!(*pmic_model_word(&r.power,0x07025040)&4)); /* PM2 level preserved. */
    return result;
}
/* PERI0 at 2400 MHz with its 400 MHz branch (divider 6) enabled and gated on. */
static void radio_peri0(unsigned gated)
{
    *pmic_model_word(&r.power,0x020020a0)=0xff000000u|(1u<<20)|(99u<<8);
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
static void minimum_power_intervals(void)
{
    for(unsigned costly=0;costly<2;++costly){
        radio_setup();r.fraction=23;r.gpio_cost=costly?17:0;
        for(unsigned i=0;i<2000&&r.radio.stage!=12;++i)assert(radio_step()!=A733_RADIO_RETAINED);
        assert(r.radio.stage==12&&r.radio.pin_lease.id);
        r.power.now=r.radio.wait_since+10000;r.fraction=0;
        assert(a733_radio_platform_poll(&r.radio)==A733_RADIO_PENDING&&r.radio.stage==12&&!r.enabled_at);
        ++r.power.now;assert(a733_radio_platform_poll(&r.radio)==A733_RADIO_PENDING&&r.radio.stage==13);
        r.fraction=23;assert(a733_radio_platform_poll(&r.radio)==A733_RADIO_PENDING&&r.radio.stage==14);
        assert(r.enabled_at&&r.radio.wait_since==r.enabled_at);
        r.power.now=r.radio.wait_since+50000;r.fraction=0;
        assert(a733_radio_platform_poll(&r.radio)==A733_RADIO_PENDING&&r.radio.stage==14&&!r.module_at);
        ++r.power.now;assert(a733_radio_platform_poll(&r.radio)==A733_RADIO_PENDING&&r.radio.stage==15);
        r.power.now=r.radio.config.deadline_us;
        assert(a733_radio_platform_poll(&r.radio)==A733_RADIO_RETAINED&&!r.module_at);
    }
}
int main(void)
{
    minimum_power_intervals();
    radio_setup();
    enum a733_radio_platform_result status=A733_RADIO_PENDING;
    for(unsigned i=0;i<4000 && status!=A733_RADIO_READY;++i){status=radio_step();assert(status!=A733_RADIO_RETAINED);}
    assert(status==A733_RADIO_READY && r.radio.card.function_count==2 && r.radio.host.card_hz==400000);
    emit_result("ready",&r.radio,0);emit_result("ready-scanned",&r.radio,1);emit_result("ready-many",&r.radio,2);
    assert(r.radio.rail_count==3 && r.radio.rail_index==3 && !r.radio.pmic_request.id && !r.radio.power_on);
    assert(r.radio.snapshots[0].programmed_uv==3300000 && r.radio.snapshots[1].programmed_uv==1800000 &&
        r.radio.snapshots[2].programmed_uv==1800000);
    assert(!pmic_log_count); /* Supplies already on at 1.8V are never written. */
    assert(r.first_host_at>=r.enabled_at+50000 && r.power.provider.pin.id && r.radio.pin_lease.id);
    struct a733_clock_token collision={0};
    assert(a733_clock_acquire(&r.power.clocks,99,0,A733_CLOCK_SDC1,1000000,&collision)==A733_CLOCK_BUSY);
    assert(a733_clock_acquire(&r.power.clocks,99,0,A733_CLOCK_RADIO_MODULE,1000000,&collision)==A733_CLOCK_BUSY);
    struct a733_rail_token rail_collision={0};
    assert(a733_rail_acquire(&r.power.rails,99,0,A733_RAIL_BIT(A733_RAIL_BLDO5),1000000,&rail_collision)==A733_RAIL_BUSY);
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
    /* DCDC8 is not in the vendor radio power path: it never becomes a radio supply. */
    assert(!radio_try_setup(A733_RADIO_DCDC8));
    assert(!radio_try_setup(A733_RADIO_CLDO1)); /* PG is BLDO5. */
    /* Vendor order with BLDO5 off at handoff and a different selector: PM1 low,
     * selector then enable written under an exclusive lease, every supply read
     * back strictly, 10ms before PM1 rises, 50ms before the module clock. */
    radio_setup();r.power.pmic[0x21]&=(uint8_t)~4u;r.power.pmic[0x2e]=0x3a; /* off, code 26, bits 7:5 = 1 */
    status=A733_RADIO_PENDING;
    for(unsigned i=0;i<4000 && status!=A733_RADIO_READY;++i){status=radio_step();assert(status!=A733_RADIO_RETAINED);}
    assert(status==A733_RADIO_READY && pmic_log_count==2);
    assert(pmic_log[0].reg==0x2e && pmic_log[0].value==0x20+13 && !pmic_log[0].pm1);
    assert(pmic_log[1].reg==0x21 && pmic_log[1].value==0xff && !pmic_log[1].pm1);
    assert(r.power.pmic[0x2e]==0x2d && (r.power.pmic[0x21]&4) && r.power.pmic[0x2f]==13);
    assert(r.radio.power_on && r.radio.rail_verify && r.enabled_at>=pmic_log[1].at+10000);
    assert(r.first_host_at>=r.enabled_at+50000);
    assert(r.radio.snapshots[1].rail==A733_RAIL_BLDO5 && r.radio.snapshots[1].enabled && r.radio.snapshots[1].programmed_uv==1800000);
    /* Shutdown with a readback at every step, in order: PM1 low, controller quiesce, module and bus
     * clocks off, BLDO5 off. Only the borrow holder may ask, and nothing is usable afterwards. */
    {
        struct a733_radio_borrow down_token={0},none={0};struct sdio_card *down_card=0;
        assert(!a733_radio_platform_shutdown(&r.radio,&down_token,r.power.now+400000)); /* Not borrowed yet. */
        assert(a733_radio_platform_borrow(&r.radio,80,&down_token,&down_card));
        struct a733_radio_borrow forged=down_token;++forged.generation;
        assert(!a733_radio_platform_shutdown(&r.radio,&none,r.power.now+400000));
        assert(!a733_radio_platform_shutdown(&r.radio,&forged,r.power.now+400000));
        assert(!a733_radio_platform_shutdown(&r.radio,&down_token,r.power.now)); /* Deadline in the past. */
        assert(a733_radio_platform_shutdown(&r.radio,&down_token,r.power.now+400000));
        assert(!a733_radio_platform_shutdown(&r.radio,&down_token,r.power.now+400000));
        unsigned order[16],n=0,last=r.radio.stage;enum a733_radio_platform_result down=A733_RADIO_PENDING;order[n++]=last;
        for(unsigned i=0;i<400&&down!=A733_RADIO_DOWN;++i){
            down=radio_step();assert(down!=A733_RADIO_RETAINED);
            if(r.radio.stage!=last&&n<16){order[n++]=last=r.radio.stage;}
        }
        assert(down==A733_RADIO_DOWN&&n==7);
        assert(order[0]==RSTAGE_SHUTDOWN_PM1);
        assert(order[n-1]==RSTAGE_DOWN&&r.radio.stage==RSTAGE_DOWN);
        for(unsigned i=1;i<n;++i)assert(order[i]>order[i-1]); /* Steps never run out of order. */
        assert(!(*pmic_model_word(&r.power,0x07025040)&2));                    /* PM1 low. */
        assert(r.sdc[0x04/4]==0&&r.sdc[0x30/4]==0&&r.sdc[0x8c/4]==0);           /* Card clock stopped, interrupts masked. */
        assert(!(*pmic_model_word(&r.power,0x02002d10)&(1u<<31)));              /* Module clock gate off. */
        assert(!(*pmic_model_word(&r.power,0x02002d1c)&0x10001));               /* Bus gate off, reset asserted. */
        assert(pmic_log_count==3&&pmic_log[2].reg==0x21&&pmic_log[2].value==0xfb&&!pmic_log[2].pm1); /* BLDO5 off after PM1 low. */
        assert(r.power.pmic[0x2e]==0x2d&&!(r.power.pmic[0x21]&4)&&r.power.pmic[0x2f]==13);
        assert(r.radio.host.state==SUNXI_SDIO_QUIESCED&&!r.radio.pmic_request.id);
        assert(a733_radio_platform_live(&r.radio,&down_token,r.power.now+1000)==A733_RADIO_INVALID);
        assert(a733_radio_platform_poll(&r.radio)==A733_RADIO_DOWN);
    }
    /* First-run guard: observes DCDC8 and the pad power words before any write and
     * proceeds only if BLDO5 is off, PG is unsupplied, CLDO1 is 1.8V and DCDC1 is code 23. */
    guard_config=1;
    radio_setup();r.power.pmic[0x21]&=(uint8_t)~4u;r.power.pmic[0x2e]=0x3a;
    *pmic_model_word(&r.power,0x02000048)=1u<<12;*pmic_model_word(&r.power,0x07025348)=3u<<2;
    status=A733_RADIO_PENDING;
    for(unsigned i=0;i<4000 && status!=A733_RADIO_READY;++i){status=radio_step();assert(status!=A733_RADIO_RETAINED);}
    assert(pmic_log_count==2 && r.radio.rail_count==4 && r.radio.guard_done);
    emit_result("guard-ready",&r.radio,0);
    assert(((r.radio.pad_power[1]>>12)&3)==1 && ((r.radio.pad_power[3]>>2)&3)==3);
    {unsigned seen=0;for(unsigned i=0;i<4;++i)if(r.radio.snapshots[i].rail==A733_RAIL_DCDC8){
        seen=1;assert(r.radio.snapshots[i].selector==100 && !r.radio.snapshots[i].decoded);}
     assert(seen);}
    for(unsigned mismatch=0;mismatch<4;++mismatch) {
        radio_setup();r.power.pmic[0x21]&=(uint8_t)~4u;r.power.pmic[0x2e]=0x3a;
        if(mismatch==0)*pmic_model_word(&r.power,0x02000048)=2u<<12; /* PG supplied at 3.3V */
        if(mismatch==1){r.power.pmic[0x21]|=4;r.power.pmic[0x2e]=13;} /* BLDO5 already on, 1.8V */
        if(mismatch==2)r.power.pmic[0x2f]=14;                        /* CLDO1 at 1.9V */
        if(mismatch==3)r.power.pmic[0x12]=22;                        /* DCDC1 at code 22 */
        status=A733_RADIO_PENDING;
        for(unsigned i=0;i<2000 && status!=A733_RADIO_RETAINED;++i)status=radio_step();
        /* Every observation is kept and nothing is written, PM1 stays low. */
        assert(status==A733_RADIO_RETAINED && !pmic_log_count && !r.first_host_at && !r.radio.pin_lease.id);
        assert(!(*pmic_model_word(&r.power,0x07025040)&2));
        if(mismatch==0)emit_result("guard-pg-supplied",&r.radio,0);
        if(mismatch==0){assert(r.radio.guard_done && ((r.radio.pad_power[1]>>12)&3)==2);}
        if(mismatch==1)assert(r.radio.guard_done && !r.radio.power_on);
        if(mismatch==3)assert(!r.radio.guard_done); /* The earlier DCDC1 check refuses first. */
        if(mismatch==2)assert(!r.radio.guard_done);
    }
    /* Failure after the power-on writes: PM1 low, then the SDC1 clocks, then BLDO5 off, each read back, and only then
     * the retention. No response to CMD5, an error on CMD3 or CMD7, and a failing PM1 write after power-on. */
    for(unsigned failure=0;failure<4;++failure) {
        radio_setup();r.power.pmic[0x21]&=(uint8_t)~4u;r.power.pmic[0x2e]=0x3a;
        *pmic_model_word(&r.power,0x02000048)=1u<<12;*pmic_model_word(&r.power,0x07025348)=3u<<2;
        if(failure==0)r.fail_command=5;
        if(failure==1)r.fail_command=3;
        if(failure==2)r.fail_command=7;
        if(failure==3)r.fail_pm1=1;
        status=A733_RADIO_PENDING;unsigned aborted_first=0;
        for(unsigned i=0;i<4000&&status!=A733_RADIO_RETAINED;++i) {
            r.power.now+=100;(void)pmic_model_step(&r.power);status=a733_radio_platform_poll(&r.radio);
            if(r.radio.aborting&&!aborted_first){aborted_first=1;assert(!r.radio.fault);} /* Not retained yet. */
        }
        assert(status==A733_RADIO_RETAINED&&aborted_first&&r.radio.abort_done&&r.radio.fault);
        assert(!(*pmic_model_word(&r.power,0x07025040)&2));                       /* PM1 low. */
        assert(!(r.power.pmic[0x21]&4)&&r.power.pmic[0x2e]==0x2d);                 /* BLDO5 off, selector kept. */
        assert(pmic_log_count==3&&pmic_log[2].reg==0x21&&pmic_log[2].value==0xfb);
        if(failure!=3)assert(!pmic_log[2].pm1);                                    /* PM1 low before BLDO5 off. */
        if(failure!=3){
            assert((r.radio.abort_flags&16u)&&(r.radio.abort_flags&14u)==14u);     /* Every readback matched. */
            assert(!(*pmic_model_word(&r.power,0x02002d10)&(1u<<31)));            /* Clocks off. */
            assert(!(*pmic_model_word(&r.power,0x02002d1c)&0x10001));
        } else assert(!(r.radio.abort_flags&16u)&&(r.radio.abort_flags&8u));       /* PM1 failed; BLDO5 still went off. */
        assert(!r.first_host_at||failure!=3);
        emit_result("failure-shutdown",&r.radio,0);
    }
    /* A reset left BLDO5 on at 1.8V and PG reads 1.8V: the guard accepts, the writes are skipped, the waits stay,
     * and the shutdown still turns BLDO5 off. */
    radio_setup();*pmic_model_word(&r.power,0x02000048)=3u<<12;*pmic_model_word(&r.power,0x07025348)=3u<<2;
    status=A733_RADIO_PENDING;
    for(unsigned i=0;i<4000 && status!=A733_RADIO_READY;++i){status=radio_step();assert(status!=A733_RADIO_RETAINED);}
    assert(r.radio.already_on&&!r.radio.power_on&&!pmic_log_count&&r.first_host_at>=r.enabled_at+50000);
    {
        struct a733_radio_borrow token={0};struct sdio_card *card=0;
        assert(a733_radio_platform_borrow(&r.radio,80,&token,&card));
        assert(a733_radio_platform_shutdown(&r.radio,&token,r.power.now+400000));
        enum a733_radio_platform_result down=A733_RADIO_PENDING;
        for(unsigned i=0;i<400&&down!=A733_RADIO_DOWN;++i){down=radio_step();assert(down!=A733_RADIO_RETAINED);}
        assert(down==A733_RADIO_DOWN&&pmic_log_count==1&&pmic_log[0].reg==0x21&&pmic_log[0].value==0xfb&&!pmic_log[0].pm1);
    }
    emit_result("already-on",&r.radio,0);
    guard_config=0;
    /* A rail that refuses to turn on stops before PM1 rises; no host access follows. */
    radio_setup();r.power.pmic[0x21]&=(uint8_t)~4u;stuck_enable=1;
    status=A733_RADIO_PENDING;
    for(unsigned i=0;i<2000 && status!=A733_RADIO_RETAINED;++i) {
        r.power.now+=100;(void)pmic_model_step(&r.power);status=a733_radio_platform_poll(&r.radio);
    }
    assert(status==A733_RADIO_RETAINED && r.power.provider.failed && !r.first_host_at);
    assert(!(*pmic_model_word(&r.power,0x07025040)&2));
    assert(pmic_log_count==2 && r.radio.pmic_request.id);
    stuck_enable=0;
    /* Only BLDO5 can be powered on, only to 1.8V, only under an exclusive lease. */
    radio_setup();{
        struct a733_rail_token t={0};struct a733_pmic_request q={0};
        assert(a733_rail_acquire(&r.power.rails,7,A733_RAIL_BIT(A733_RAIL_BLDO5),0,1000000,&t)==A733_RAIL_PENDING);
        assert(a733_pmic_submit_power_on(&r.power.provider,7,&t,A733_RAIL_BLDO5,13,1000000,&q)==A733_PMIC_INVALID);
        assert(a733_rail_release(&r.power.rails,&t,1000000)==A733_RAIL_READY);
        assert(a733_rail_acquire(&r.power.rails,7,0,A733_RAIL_BIT(A733_RAIL_CLDO1)|A733_RAIL_BIT(A733_RAIL_BLDO5),
            1000000,&t)==A733_RAIL_PENDING);
        assert(a733_pmic_submit_power_on(&r.power.provider,7,&t,A733_RAIL_CLDO1,13,1000000,&q)==A733_PMIC_INVALID);
        assert(a733_pmic_submit_power_on(&r.power.provider,7,&t,A733_RAIL_BLDO5,14,1000000,&q)==A733_PMIC_INVALID);
        assert(a733_pmic_submit_power_on(&r.power.provider,7,&t,A733_RAIL_BLDO5,12,1000000,&q)==A733_PMIC_INVALID);
        assert(!pmic_log_count && !q.id);
    }
    /* Wrong observed rail voltage stops before module or controller writes. */
    radio_setup();r.power.pmic[0x2e]=12;
    status=A733_RADIO_PENDING;
    for(unsigned i=0;i<500 && status!=A733_RADIO_RETAINED;++i)status=radio_step();
    assert(status==A733_RADIO_RETAINED && !r.accesses && !r.radio.pin_lease.id && !pmic_log_count);
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
