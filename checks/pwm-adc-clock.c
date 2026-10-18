/* Only new PWM/ADC scope and shared-parent conflicts; no old registry replay. */
#include "a733_clock_leases.h"
#include <assert.h>
#include <stdio.h>
static int now(void *v,uint64_t *out){(void)v;*out=1;return 1;}
int main(void)
{
 struct a733_clock_leases m={0};struct a733_clock_token pwm={0},adc={0},ths={0},change={0};
 assert(a733_clock_leases_init(&m,now,NULL));
 assert(a733_clock_acquire(&m,1,A733_CLOCK_PWM0,0,100,&pwm)==A733_CLOCK_INVALID);
 assert(a733_clock_acquire(&m,1,0,A733_CLOCK_PWM0,100,&pwm)==A733_CLOCK_OK);
 assert(pwm.exclusive==A733_CLOCK_BIT(67)&&pwm.shared==(A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_FABRIC));
 assert(a733_clock_acquire(&m,2,0,A733_CLOCK_PWM0,100,&change)==A733_CLOCK_BUSY);
 assert(a733_clock_acquire(&m,2,A733_CLOCK_GPADC,0,100,&adc)==A733_CLOCK_INVALID);
 assert(a733_clock_acquire(&m,2,0,A733_CLOCK_GPADC,100,&adc)==A733_CLOCK_OK);
 assert(adc.exclusive==A733_CLOCK_BIT(68)&&adc.shared==(A733_CLOCK_GPADC24M|A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC));
 assert(a733_clock_acquire(&m,3,0,A733_CLOCK_THS,100,&ths)==A733_CLOCK_OK);
 assert(a733_clock_acquire(&m,4,0,A733_CLOCK_GPADC24M,100,&change)==A733_CLOCK_BUSY);
 assert(a733_clock_acquire(&m,4,0,A733_CLOCK_GPADC,100,&change)==A733_CLOCK_BUSY);
 assert(a733_clock_release(&m,&adc,100)==A733_CLOCK_OK);
 assert(a733_clock_acquire(&m,4,0,A733_CLOCK_GPADC24M,100,&change)==A733_CLOCK_BUSY);
 assert(a733_clock_release(&m,&ths,100)==A733_CLOCK_OK);
 assert(a733_clock_acquire(&m,4,0,A733_CLOCK_GPADC|A733_CLOCK_GPADC24M,100,&change)==A733_CLOCK_OK);
 assert(change.exclusive==(A733_CLOCK_GPADC|A733_CLOCK_GPADC24M)&&!(change.shared&A733_CLOCK_GPADC24M));
 puts("PASS: exclusive PWM67/GPADC68, shared stability and THS/ADC parent-retune exclusion");
}
