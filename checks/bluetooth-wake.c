/* Run only new optional wake-pin acquisition over the actual shared pin
 * manager. Existing radio/card/PMIC protocol cases are never called. */
#define main bluetooth_archived_platform_main
#include "a733-radio-platform.c"
#undef main
#include "../drivers/net/a733_radio_platform.c"
int main(void)
{
    radio_setup();
    struct a733_radio_bluetooth_pins wake={.authority=8,.desired={{1,0,0},{0,0,0}}};
    assert(a733_radio_platform_bluetooth(&r.radio,&wake));
    assert(!a733_radio_platform_bluetooth(&r.radio,&wake));
    struct a733_pin_fields pm2;assert(a733_pin_fields_banked(A733_PIN_M,2,&pm2));
    *pmic_model_word(&r.power,pm2.mux_address)|=15u<<pm2.mux_shift;
    uint32_t pm2_before=*pmic_model_word(&r.power,pm2.mux_address)&(15u<<pm2.mux_shift);
    /* Earlier admission/resource proofs supply this single new metadata
     * boundary; no card enumeration or firmware operation is replayed. */
    assert(metadata(&r.radio)==A733_RADIO_PENDING&&r.radio.clock.id);
    assert(r.radio.clock.owner==r.radio.config.owner&&
        a733_clock_token_status(&r.power.clocks,&r.radio.clock)==A733_CLOCK_OK&&
        r.radio.deadline>r.power.now&&r.power.pins.initialized&&radio_current(&r));
    r.radio.stage=PINS;
    assert(metadata(&r.radio)==A733_RADIO_PENDING);
    assert(r.radio.pin_lease.masks[A733_PIN_G]==0x3f&&r.radio.pin_lease.masks[A733_PIN_M]==0x1b);
    assert((*pmic_model_word(&r.power,0x07025040)&0x3f)==8);
    assert((*pmic_model_word(&r.power,pm2.mux_address)&(15u<<pm2.mux_shift))==pm2_before);
    assert(!r.commands&&!r.accesses);
    puts("PASS Bluetooth wake: actual shared pin lease drives only PM3 high, owns PM4 input and preserves PM2");
    return 0;
}
