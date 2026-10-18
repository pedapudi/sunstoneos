/* Real PWM, PMIC, rail, pin and clock cores; model only MMIO, time and scheduling. */
#define PMIC_MODEL_NO_MAIN
#include "a733-pmic.c"
#include "a733_pwm.h"
struct pwm_model {struct pmic_model power;struct a733_pwm pwm;unsigned retained,admitted,writes,fail_start,contended;};
static struct pwm_model m;
static int pwm_now(void *v,uint64_t *out){*out=((struct pwm_model *)v)->power.now;return 1;}
static int pwm_current(void *v){return !((struct pwm_model *)v)->power.worker;}
static int pwm_admit(void *v,const struct a733_pwm_config *c,const struct a733_clock_token *t)
{struct pwm_model *s=v;assert(c->owner==42&&c->handoff==200&&t->exclusive==A733_CLOCK_PWM0);++s->admitted;return 1;}
static int pwm_enter(void *v,uint64_t d)
{struct pwm_model *s=v;if(s->contended){s->contended=0;return 0;}return s->retained?0:pmic_model_pin_enter(&s->power,d);}
static void pwm_leave(void *v){pmic_model_pin_leave(&((struct pwm_model *)v)->power);}
static void pwm_retain(void *v){struct pwm_model *s=v;assert(s->power.register_held);s->retained=1;}
static int pwm_outside(void *v,const void *p,size_t n)
{struct pwm_model *s=v;return pmic_model_separate(p,n,&s->retained,sizeof(*s)-(size_t)((char *)&s->retained-(char *)s));}
static int pwm_read(void *v,uintptr_t a,uint32_t *out)
{struct pwm_model *s=v;assert(s->power.register_held&&!s->retained);*out=*pmic_model_word(&s->power,a);return 1;}
static int pwm_write(void *v,uintptr_t a,uint32_t value)
{
 struct pwm_model *s=v;assert(s->power.register_held&&!s->retained&&s->admitted);++s->writes;
 if(a==A733_PWM_BASE+0x240&&(value&(1u<<10))){
  assert(s->pwm.report.started&&*pmic_model_word(&s->power,A733_PWM_BASE)==(1u<<5));
  assert(*pmic_model_word(&s->power,A733_PWM_BASE+0x80)==(1u<<5));
  assert(*pmic_model_word(&s->power,A733_PWM_BASE+0x244)==0x0257012c);
  *pmic_model_word(&s->power,a)=value;return !s->fail_start;
 }
 if(a==A733_PWM_BASE+4){assert(value==(1u<<5));*pmic_model_word(&s->power,a)&=~value;}
 else *pmic_model_word(&s->power,a)=value;
 return 1;
}
static int pin_write(void *v,uintptr_t a,uint32_t value)
{
 struct pmic_model *p=v;uintptr_t b=A733_PINS_BASE+0x80+A733_PIN_D*0x80;
 if(a==b+0x18){assert(p->register_held&&value==(1u<<5));*pmic_model_word(p,b+0x10)&=~value;return 1;}
 return pmic_model_pin_write(v,a,value);
}
static void setup_pwm(void)
{
 memset(&m,0,sizeof(m));pmic_model_setup(&m.power);
 m.power.pins.operations.write=pin_write;
 m.power.pmic[0x20]=1u<<6;m.power.pmic[0x2a]=13;
 *pmic_model_word(&m.power,0x02002000)=0xf8000000u|(99u<<16)|(99u<<8);
 *pmic_model_word(&m.power,0x03400088)=1u<<3;
 *pmic_model_word(&m.power,0x03400440)=0xd8000000;
 struct a733_pwm_config c={.owner=42,.handoff=200,.deadline_us=900000,.apb0=0,.apb0_hz=24000000,
  .period_ns=25000,.duty_ns=12500,.pulses=4000,.active_high=1,.expected={0,0,0},.desired={7,0,1}};
 const struct a733_pwm_ops ops={pwm_now,pwm_current,pwm_admit,pwm_enter,pwm_leave,pwm_retain,pwm_read,pwm_write,pwm_outside};
 assert(a733_pwm_init(&m.pwm,&c,&ops,&m,&m.power.clocks,&m.power.pins,&m.power.rails,&m.power.provider));
 assert(m.pwm.plan.pcr==0x0f9f0300&&m.pwm.plan.ppr==0x0257012c&&m.pwm.plan.duration_us==100000&&!m.writes);
}
static enum a733_pwm_result step_pwm(unsigned notice)
{m.power.now+=100;assert(pmic_model_step(&m.power)!=A733_PMIC_RETAINED);return a733_pwm_poll(&m.pwm,notice);}
static void armed(void)
{
 enum a733_pwm_result r=A733_PWM_PENDING;
 for(unsigned i=0;i<2000&&r!=A733_PWM_ARMED;++i){r=step_pwm(0);assert(r!=A733_PWM_RETAINED);}
 assert(r==A733_PWM_ARMED&&m.pwm.supply.programmed_uv==1800000&&m.pwm.pin.id&&m.admitted==1);
 *pmic_model_word(&m.power,0x03400108)=1u<<3;
}
int main(void)
{
 setup_pwm();armed();assert(a733_pwm_start(&m.pwm)==A733_PWM_RUNNING);
 *pmic_model_word(&m.power,A733_PWM_BASE+4)=1u<<5;
 m.contended=1;assert(step_pwm(1)==A733_PWM_BUSY&&!m.pwm.report.irq_seen);
 assert(step_pwm(1)==A733_PWM_RUNNING&&m.pwm.report.irq_seen);
 assert(!*pmic_model_word(&m.power,A733_PWM_BASE)&&!*pmic_model_word(&m.power,A733_PWM_BASE+4));
 m.power.now+=100000;*pmic_model_word(&m.power,A733_PWM_BASE+0x240)&=~(1u<<10);
 enum a733_pwm_result r=A733_PWM_PENDING;
 for(unsigned i=0;i<10&&r!=A733_PWM_COMPLETE;++i)r=step_pwm(0);
 assert(r==A733_PWM_COMPLETE&&m.pwm.report.waveform_stopped&&m.pwm.report.gpio_low);
 assert(!*pmic_model_word(&m.power,A733_PWM_BASE+0x80)&&!*pmic_model_word(&m.power,A733_PWM_BASE+0x40));
 assert(m.power.pins.pins[A733_PIN_D][5].current.function==1&&m.pwm.clock.id&&m.pwm.rail.id&&m.pwm.pin.id);
 setup_pwm();armed();m.fail_start=1;assert(a733_pwm_start(&m.pwm)==A733_PWM_RETAINED&&m.retained);
 unsigned n=m.writes;assert(a733_pwm_poll(&m.pwm,0)==A733_PWM_RETAINED&&m.writes==n);
 setup_pwm();armed();assert(a733_pwm_start(&m.pwm)==A733_PWM_RUNNING);m.power.now=m.pwm.config.deadline_us;
 n=m.writes;assert(a733_pwm_poll(&m.pwm,0)==A733_PWM_RETAINED&&m.writes==n);
 assert(m.pwm.clock_retained&&m.pwm.rail_retained);
 puts("PASS: actual PWM/provider composition, exact finite plan, first-notice contention, stop/low and retained start/deadline");return 0;
}
