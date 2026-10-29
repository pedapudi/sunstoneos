#ifndef SUNSTONE_ADC_PRIVATE_H
#define SUNSTONE_ADC_PRIVATE_H
#include "adc_session.h"
#include "../board/application.h"
#include "a733_adc_native.h"
#include "a733_lradc.h"
#include "gicv3_spi.h"
struct adc_application {
 struct adc_session session;struct a733_adc driver;struct a733_lradc lradc;struct a733_adc_native native;
 struct a733_adc_config config;struct a733_adc_ops operations;
 struct a733_clock_token token;struct a733_rail_token rail;
 struct a733_pmic_lifetime lifetime;struct a733_pmic_request request;struct a733_pmic_snapshot supply;
 StaticTask_t task;StackType_t stack[2048];TaskHandle_t handle;struct gicv3_spi irq;
 atomic_uint result,failed,pending;unsigned stage,rail_retained,clock_retained,masked,rearm_wait,sets,valid,last_sequence;
 uint64_t next_read,last_times[4];uint32_t low_seen,high_seen;
 uint8_t payload[ADC_OUTPUT_BYTES],digest[32],session_digest[32];
};
extern struct adc_application adc;
int adc_construct(void);int adc_step(void);void adc_task(void *);void adc_cancel(void);
int adc_output_ready(void);int adc_live(void);int adc_publish(void);
int adc_collect(const struct a733_adc_sample *);void adc_application_irq(void);
#endif
