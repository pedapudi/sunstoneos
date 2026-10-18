#ifndef SUNSTONE_A733_PWM_NATIVE_H
#define SUNSTONE_A733_PWM_NATIVE_H
#include "a733_pwm.h"
struct a733_platform_locks;
struct a733_pwm_native {
 struct a733_pwm_ops operations;
 struct a733_platform_locks *locks;
 void *task,*admission_cookie;
 int (*admit)(void *,const struct a733_pwm_config *,const struct a733_clock_token *);
 struct a733_pwm_plan plan;
 unsigned initialized;
};
int a733_pwm_native_init(struct a733_pwm_native *,struct a733_platform_locks *,void *task,
 const struct a733_pwm_config *,int (*)(void *,const struct a733_pwm_config *,const struct a733_clock_token *),void *);
#endif
