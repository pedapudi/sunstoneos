#ifndef SUNSTONE_A733_ADC_NATIVE_H
#define SUNSTONE_A733_ADC_NATIVE_H
#include "a733_adc.h"
struct a733_platform_locks;
struct a733_adc_native {
 struct a733_adc_ops operations;struct a733_platform_locks *locks;
 void *owner_task,*admission_cookie;
 int (*admit)(void *,const struct a733_adc_config *,const struct a733_clock_token *);
 unsigned initialized;
};
/* Pure binding of actual privileged FreeRTOS owner and common register mutex.
 * Admit authenticates source/electrical/boot authority. Accesses require identity
 * Device-nGnRnE mappings. No reference/test/pad/DMA access is provided. */
int a733_adc_native_init(struct a733_adc_native *,struct a733_platform_locks *,void *,
 int (*)(void *,const struct a733_adc_config *,const struct a733_clock_token *),void *);
#endif
