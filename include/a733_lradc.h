#ifndef SUNSTONE_A733_LRADC_H
#define SUNSTONE_A733_LRADC_H
#include "a733_adc.h"
#define A733_LRADC_INTID 69u /* Effective A733 DTS SPI37. */
#define A733_LRADC_CONTROL UINT32_C(0x0100004d) /* Delay1, channel0, normal, selector3. */
/* Fixed passive ADC application adapter: uses its existing callback/config and
 * sample types, but never accesses GPADC. configure_adc_clock must be zero.
 * authorize additionally establishes safe LRADC0 input despite the board's
 * nominal1.8V pull-up exceeding documented1.35V conversion range, qualified
 * AVCC/reference, no legacy RTC source/calibration/oscillator writers, inactive
 * controller and INTID69 owned/masked. Linux key maps grant no such authority.
 * enter returns1 only with acquired ownership,0 for busy and negative for
 * denial without ownership. RTC32K and FABRIC are stable; selected RC16M or OSC remains reserved. */
struct a733_lradc {
 struct a733_adc_config config;struct a733_adc_ops ops;struct a733_clock_token token;
 struct a733_clock_leases *registry;void *cookie;atomic_uint busy;
 uint64_t deadline,last_now;uint32_t sequence,rtc_control,rtc_prescaler,rtc_calibration,rtc_oscillator;
 unsigned initialized,stage,fault,held,source;
};
int a733_lradc_init(struct a733_lradc *,const struct a733_adc_config *,const struct a733_adc_ops *,void *,struct a733_clock_leases *,const struct a733_clock_token *);
enum a733_adc_result a733_lradc_poll(struct a733_lradc *);
/* DATA0 is copied only after fresh DATA_PENDING. W1C is followed by a required
 * clear boundary; one DATA0 reassertion by the next conversion is acknowledged
 * once more before faulting. Calls before READY or with a zero deadline return
 * INVALID. Output remains unchanged on AGAIN/BUSY/fault. These are latest
 * observations, not lossless conversion counts or simultaneous GPADC samples. */
enum a733_adc_result a733_lradc_read(struct a733_lradc *,uint64_t,struct a733_adc_sample *);
/* Configured EN/local IRQ disable only; no analog drain, reset, release/restart.
 * All leases/rail/configuration remain retained. Terminal calls perform no I/O. */
enum a733_adc_result a733_lradc_stop(struct a733_lradc *,uint64_t);
#endif
