/* Reuse existing mux/atomic DATA model; execute only the new retained resume. */
#define main pins_expanded_existing_main
#include "pins-expanded.c"
#undef main
static struct a733_pin_lease_banked stopped_fan(void)
{
 setup(0);struct a733_pin_request_banked r=req(A733_PIN_D,5,7);
 struct a733_pin_lease_banked t={0};
 assert(a733_pins_acquire_banked(&m,42,&r,1,&t,100)==A733_PINS_OK);
 latches[A733_PIN_D]=(1u<<5)|(1u<<20);
 assert(a733_pins_pwm0_fan_low(&m,&t,1,100)==A733_PINS_OK);
 return t;
}
int main(void)
{
 struct a733_pin_lease_banked t=stopped_fan(),wrong=t;wrong.owner++;
 unsigned before=writes;
 assert(a733_pins_pwm0_fan_resume(&m,&wrong,1,100)==A733_PINS_CONFLICT&&writes==before);
 assert(a733_pins_pwm0_fan_resume(&m,&t,0,100)==A733_PINS_INVALID&&writes==before);
 assert(a733_pins_pwm0_fan_resume(&m,&t,1,100)==A733_PINS_OK);
 assert(mux(A733_PIN_D,5)==7&&latches[A733_PIN_D]==(1u<<20)&&!m.failed&&m.pins[A733_PIN_D][5].id==t.id);
 assert(m.pins[A733_PIN_D][5].current.pull==0&&m.pins[A733_PIN_D][5].current.drive==1);
 before=writes;assert(a733_pins_pwm0_fan_resume(&m,&t,1,100)==A733_PINS_INVALID&&writes==before);
 t=stopped_fan();latches[A733_PIN_D]|=1u<<5;before=writes;
 assert(a733_pins_pwm0_fan_resume(&m,&t,1,100)==A733_PINS_RECOVERY&&writes==before);retained(&t);
 t=stopped_fan();fail_write=writes+1;
 assert(a733_pins_pwm0_fan_resume(&m,&t,1,100)==A733_PINS_RECOVERY);retained(&t);
 t=stopped_fan();expire_write=writes+1;
 assert(a733_pins_pwm0_fan_resume(&m,&t,1,100)==A733_PINS_RECOVERY);retained(&t);
 puts("PASS: exact retained PD5 resume preserves lease/neighbor/pull/drive and retains changed/uncertain state");
}
