#ifndef SUNSTONE_THERMAL_PRIVATE_H
#define SUNSTONE_THERMAL_PRIVATE_H
#include "thermal_session.h"
#include "../board/application.h"
#include "a733_thermal.h"
#include "gicv3_spi.h"
struct thermal_application {
 struct thermal_session session;
 struct a733_thermal driver;
 struct a733_thermal_config config;
 struct a733_clock_token token;
 struct gicv3_spi irq;
 StaticTask_t task;StackType_t stack[2048];TaskHandle_t handle;
 atomic_uint result,failed,pending;
 unsigned active,retaining,masked,rearm_wait,sets;
 uint32_t last_sequence;
 uint64_t next_read;
 uint8_t payload[THERMAL_OUTPUT_BYTES],digest[32],session_digest[32];
 uint8_t working[96];
 struct a733_thermal_policy policy;
};
extern struct thermal_application thermal;
void thermal_task(void *);
void thermal_application_irq(void);
void thermal_cancel(void);
int thermal_output_ready(void);
int thermal_construct(void);
int thermal_step(void);
int thermal_collect(const struct a733_thermal_sample *);
#endif
