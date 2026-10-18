/* Reuse the existing atomic DATA/mux model; execute only the new transition. */
#define main pins_expanded_existing_main
#include "pins-expanded.c"
#undef main
static struct a733_pin_lease_banked fan(void)
{
 setup(0);struct a733_pin_request_banked r=req(A733_PIN_D,5,7);
 struct a733_pin_lease_banked t={0};
 assert(a733_pins_acquire_banked(&m,42,&r,1,&t,100)==A733_PINS_OK);
 latches[A733_PIN_D]=(1u<<5)|(1u<<20);return t;
}
int main(void)
{
 struct a733_pin_lease_banked t=fan(),wrong=t;wrong.owner++;
 unsigned before=writes;
 assert(a733_pins_pwm0_fan_low(&m,&wrong,1,100)==A733_PINS_CONFLICT&&writes==before);
 assert(a733_pins_pwm0_fan_low(&m,&t,0,100)==A733_PINS_INVALID&&writes==before);
 assert(a733_pins_pwm0_fan_low(&m,&t,1,100)==A733_PINS_OK);
 assert(mux(A733_PIN_D,5)==1&&latches[A733_PIN_D]==(1u<<20)&&!m.failed);
 assert(m.pins[A733_PIN_D][5].id==t.id&&m.pins[A733_PIN_D][5].current.function==1);
 before=writes;assert(a733_pins_pwm0_fan_low(&m,&t,1,100)==A733_PINS_OK&&writes==before);
 latches[A733_PIN_D]|=1u<<5;
 assert(a733_pins_pwm0_fan_low(&m,&t,1,100)==A733_PINS_RECOVERY);retained(&t);
 t=fan();fail_write=writes+1;
 assert(a733_pins_pwm0_fan_low(&m,&t,1,100)==A733_PINS_RECOVERY);retained(&t);
 t=fan();expire_write=writes+1;
 assert(a733_pins_pwm0_fan_low(&m,&t,1,100)==A733_PINS_RECOVERY);retained(&t);
 puts("PASS: fixed PWM pin low preserves neighbors/token; verifies idempotence; retains uncertain transitions");
 return 0;
}
