#ifndef SUNSTONE_PWM_PRIVATE_H
#define SUNSTONE_PWM_PRIVATE_H
#include "pwm_session.h"
#include "a733_pwm_native.h"
#include "gicv3_spi.h"
#include "../board/application.h"
struct pwm_application {
 struct pwm_session session;
 struct a733_pwm driver;
 struct a733_pwm_native native;
 struct a733_pwm_config config;
 struct gicv3_spi irq;
 StaticSemaphore_t mutex_storage;
 SemaphoreHandle_t mutex;
 StaticTask_t task;
 StackType_t stack[2048];
 TaskHandle_t handle;
 atomic_uint result,cancel,pending;
 unsigned constructed,active,masked,retaining,rearm_wait,error;
 uint64_t deadline;
 uint8_t session_digest[32];
 struct pwm_request accepted;
 uint64_t rejected_sequence,command_deadline,completion_before,command_completed_us;
 unsigned ready,boot_stopping,command_pending,command_started,ack_state,rejection;

};
extern struct pwm_application pwm;
void pwm_task(void *);
void pwm_application_irq(void);
void pwm_cancel(void);
int pwm_output_ready(void);
int pwm_copy(uint8_t *);
int pwm_submit(const uint8_t request[80]);
#endif
