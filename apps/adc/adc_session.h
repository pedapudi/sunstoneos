#ifndef SUNSTONE_ADC_SESSION_H
#define SUNSTONE_ADC_SESSION_H
#include "a733_board_application.h"
#define A733_ADC_SESSION_MAGIC UINT64_C(0x4153373341444131)
#define A733_ADC_CONTRACT_MAGIC UINT64_C(0x4153373341444331)
/* Version1 observes GPADC0..3; version2 observes only LRADC0. Both retain
 * the same typed extent; LRADC requires configure_adc_clock=0. Version3 adds
 * a common GPADC raw-code compare window: reserved[0]=channel mask1..15,
 * reserved[1]=0, reserved[2:4]=LE16 low, reserved[4:6]=LE16 high. */
#define ADC_OUTPUT_BYTES 160u
struct adc_session {
 struct a733_board_session board;uint64_t magic,version,bytes,owner,authority;
 uint8_t initial[32],electrical[32],clocks[32],supervision[32];
 uint32_t apb1,apb1_hz;
 uint8_t configure_adc_clock,cldo3_selector,reserved[6];
 uint32_t cldo3_uv,source_impedance_ohms;
};
_Static_assert(sizeof(struct adc_session)==784,"ADC session geometry");
static inline unsigned adc_window_low(const struct adc_session *s)
{return s->reserved[2]|(unsigned)s->reserved[3]<<8;}
static inline unsigned adc_window_high(const struct adc_session *s)
{return s->reserved[4]|(unsigned)s->reserved[5]<<8;}
int adc_session_valid(const struct adc_session *);
#endif
