/* The full radio platform graph verifies that IRQ retirement gates both
 * ordinary shutdown and failure abort without changing the power policy. */
#define main radio_reference_main
#include "a733-radio-platform.c"
#undef main
static int retirement;
static unsigned retire_calls;
static int interrupt_retire(void *context)
{assert(context==&r);++retire_calls;return retirement;}
static void ready(void)
{
    radio_setup();r.power.pmic[0x21]&=(uint8_t)~4u;r.power.pmic[0x2e]=0x3a;
    enum a733_radio_platform_result result=A733_RADIO_PENDING;
    for(unsigned i=0;i<4000&&result!=A733_RADIO_READY;++i){result=radio_step();assert(result!=A733_RADIO_RETAINED);}
    assert(result==A733_RADIO_READY);
    r.radio.interrupt_retire=interrupt_retire;r.radio.interrupt_context=&r;
    retirement=0;retire_calls=0;
}
static void clocks_live(void)
{assert((*pmic_model_word(&r.power,0x02002d10)&0x80000000)&&(*pmic_model_word(&r.power,0x02002d1c)&0x10001)==0x10001&&(r.power.pmic[0x21]&4));}
int main(void)
{
    ready();
    struct a733_radio_borrow token={0};struct sdio_card *card;
    assert(a733_radio_platform_borrow(&r.radio,80,&token,&card));
    assert(a733_radio_platform_shutdown(&r.radio,&token,r.power.now+400000));
    for(unsigned i=0;i<100&&r.radio.stage!=RSTAGE_SHUTDOWN_CLOCKS;++i)assert(radio_step()!=A733_RADIO_RETAINED);
    assert(r.radio.stage==RSTAGE_SHUTDOWN_CLOCKS);
    for(unsigned i=0;i<3;++i){assert(radio_step()==A733_RADIO_PENDING);clocks_live();}
    assert(retire_calls==3);retirement=1;
    enum a733_radio_platform_result result=A733_RADIO_PENDING;
    for(unsigned i=0;i<100&&result!=A733_RADIO_DOWN;++i)result=radio_step();
    assert(result==A733_RADIO_DOWN);
    /* A failed retirement in abort retains clocks and the fitted rail. */
    ready();assert(a733_radio_platform_retain(&r.radio)==A733_RADIO_PENDING);
    for(unsigned i=0;i<100&&r.radio.stage!=RSTAGE_ABORT_CLOCKS;++i)assert(radio_step()!=A733_RADIO_RETAINED);
    for(unsigned i=0;i<3;++i){assert(radio_step()==A733_RADIO_PENDING);clocks_live();}
    retirement=-1;result=radio_step();
    assert(result==A733_RADIO_RETAINED&&r.radio.fault);clocks_live();
    puts("PASS full radio normal/abort clock-off requires interrupt retirement");
}
