#ifndef SUNSTONE_A733_ADC_NATIVE_H
#define SUNSTONE_A733_ADC_NATIVE_H
#include "a733_adc.h"
struct a733_platform_locks;
struct a733_adc_native {
 struct a733_adc_ops operations;struct a733_platform_locks *locks;
 void *owner_task,*admission_cookie;
 int (*admit)(void *,const struct a733_adc_config *,const struct a733_clock_token *);
 unsigned initialized,lradc;struct a733_adc_thresholds thresholds;
};
/* Pure binding of actual privileged FreeRTOS owner and common register mutex.
 * Admit authenticates source/electrical/boot authority. Accesses require identity
 * Device-nGnRnE mappings. No reference/test/pad/DMA access is provided. Compare
 * writes require a one-time owner selection before the first controller I/O. */
int a733_adc_native_init(struct a733_adc_native *,struct a733_platform_locks *,void *,
 int (*)(void *,const struct a733_adc_config *,const struct a733_clock_token *),void *);
/* Owner task only, once before controller I/O. Narrows compare writes to these
 * exact selected0..3 windows and interrupt masks. No MMIO or allocation. */
int a733_adc_native_thresholds(struct a733_adc_native *,const struct a733_adc_thresholds *);
/* LRADC binding narrows I/O to its local controller, RTC source readbacks and
 * owned CCU1024 fields. It never admits GPADC writes. */
int a733_lradc_native_init(struct a733_adc_native *,struct a733_platform_locks *,void *,
 int (*)(void *,const struct a733_adc_config *,const struct a733_clock_token *),void *);
#endif
