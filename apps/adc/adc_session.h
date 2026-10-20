#ifndef SUNSTONE_ADC_SESSION_H
#define SUNSTONE_ADC_SESSION_H
#include "a733_board_application.h"
#define A733_ADC_SESSION_MAGIC UINT64_C(0x4153373341444131)
#define A733_ADC_CONTRACT_MAGIC UINT64_C(0x4153373341444331)
/* Version1 observes GPADC0..3; version2 observes only LRADC0. Both retain
 * the same typed extent; LRADC requires configure_adc_clock=0. */
#define ADC_OUTPUT_BYTES 160u
struct adc_session {
 struct a733_board_session board;uint64_t magic,version,bytes,owner,authority;
 uint8_t initial[32],electrical[32],clocks[32],supervision[32];
 uint32_t apb1,apb1_hz;
 uint8_t configure_adc_clock,cldo3_selector,reserved[6];
 uint32_t cldo3_uv,source_impedance_ohms;
};
_Static_assert(sizeof(struct adc_session)==784,"ADC session geometry");
int adc_session_valid(const struct adc_session *);
#endif
