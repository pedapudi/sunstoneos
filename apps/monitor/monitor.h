#ifndef SUNSTONE_MONITOR_PRIVATE_H
#define SUNSTONE_MONITOR_PRIVATE_H
#include "monitor_session.h"
#include "service.h"
#include "../board/application.h"
#include "a733_adc_native.h"
#include "a733_thermal.h"
#include "a733_thermal_native.h"
#include "gicv3_spi.h"
enum monitor_state {MONITOR_BOOT,MONITOR_RUNNING,MONITOR_STOP_REQUEST,MONITOR_HALTED,MONITOR_FAULT};
struct monitor_owner {
 struct a733_adc_native native;
 struct a733_clock_token token;
 TaskHandle_t handle;StaticTask_t task;StackType_t stack[2048];
 struct gicv3_spi irq;atomic_uint pending;
 uint8_t alarm,alarm_off,critical;
 unsigned kind,stage,masked,rearm_wait,in_call,stopped,retained,valid,last_sequence;
 uint64_t next_read,times[5];uint16_t codes[5];int32_t temperatures[5];
};
struct monitor_application {
 struct monitor_session session;
 struct monitor_owner owners[2]; /* ADC, THS */
 struct a733_adc adc;struct a733_thermal thermal;
 struct a733_adc_config adc_config;struct a733_thermal_config thermal_config;
 struct a733_adc_ops adc_ops;struct a733_thermal_ops thermal_ops;
 struct a733_clock_token parent;
 struct a733_rail_token rail;
 struct a733_pmic_lifetime lifetime;struct a733_pmic_request request;struct a733_pmic_snapshot supply;
 atomic_uint state,parent_ready,active_mask;unsigned supply_stage,rail_retained,parent_retained,parent_configured;
 uint64_t stop_deadline;struct monitor_snapshot latest;uint8_t session_digest[32];
};
extern struct monitor_application monitor;
int monitor_live(void);int monitor_construct(void);void monitor_fail(void);
void monitor_task(void *);int monitor_step(unsigned);void monitor_application_irq(unsigned);
int monitor_parent_step(void);
int monitor_collect_adc(const struct a733_adc_sample *);
int monitor_collect_thermal(const struct a733_thermal_sample *);
int monitor_output_eligible(void);
#endif
