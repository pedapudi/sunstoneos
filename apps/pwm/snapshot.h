#ifndef SUNSTONE_PWM_SNAPSHOT_H
#define SUNSTONE_PWM_SNAPSHOT_H
#include "pwm_session.h"
struct a733_smp_contract;
/* Primary EL3 calls once with its already validated private contract. */
int pwm_session_prepare(const struct a733_smp_contract *);
const struct pwm_session *pwm_admitted_session(void);
#endif
