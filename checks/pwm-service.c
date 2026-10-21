/* Real PWM, PMIC, rail, pin and clock cores; model only MMIO, time and scheduling. */
#define PMIC_MODEL_NO_MAIN
#include "a733-pmic.c"
#include "a733_pwm.h"
struct pwm_model {struct pmic_model power;struct a733_pwm pwm;unsigned retained,admitted,writes,fail_start,contended,fail_led;};
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
 if(a==A733_PWM_BASE+0x240)assert(!(value&(1u<<10))&&value==s->pwm.plan.pcr);
 if(a==0x03400188){assert(value==8);*pmic_model_word(&s->power,0x03400108)&=~value;return 1;}
 if(a==0x03400288){assert(value==8);*pmic_model_word(&s->power,0x03400208)&=~value;return 1;}
 if(a==A733_PWM_BASE+0x80&&value){
  assert(*pmic_model_word(&s->power,A733_PWM_BASE+0x240)==s->pwm.plan.pcr);
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
 int r=pmic_model_pin_write(v,a,value);
 if(a==0x07025010&&m.fail_led)return 0;
 return r;
}
static struct a733_pwm_config profile(uint32_t period_ns,uint32_t duty_ns,uint32_t pulses,uint32_t active_high)
{
 return (struct a733_pwm_config){.owner=42,.handoff=200,.deadline_us=900000,.apb0=0,.apb0_hz=24000000,
  .period_ns=period_ns,.duty_ns=duty_ns,.pulses=pulses,.active_high=active_high,.expected={0,0,0},.desired={7,0,1},
  .led_enabled=1,.led_expected={0,0,0},.apbs0=0,.apbs0_hz=24000000};
}
static void setup_profile(uint32_t period_ns,uint32_t duty_ns,uint32_t active_high)
{
 memset(&m,0,sizeof(m));pmic_model_setup(&m.power);
 struct a733_pins_operations po=m.power.pins.operations;po.write=pin_write;
 struct a733_pin_handoff handoff=m.power.pins.handoff;handoff.latch[0].value=1u<<6;
 *pmic_model_word(&m.power,0x07025000)|=1u<<24;*pmic_model_word(&m.power,0x07025010)=1u<<6;
 memset(&m.power.pins,0,sizeof(m.power.pins));assert(a733_pins_init_banks(&m.power.pins,&po,&m.power,&handoff));
 m.power.pmic[0x20]=1u<<6;m.power.pmic[0x2a]=13;
 *pmic_model_word(&m.power,0x02002000)=0xf8000000u|(99u<<16)|(99u<<8);
 *pmic_model_word(&m.power,0x03400088)=1u<<3;
 *pmic_model_word(&m.power,0x03400440)=0xd8000000;
 struct a733_pwm_config c=profile(period_ns,duty_ns,0,active_high);
 const struct a733_pwm_ops ops={pwm_now,pwm_current,pwm_admit,pwm_enter,pwm_leave,pwm_retain,pwm_read,pwm_write,pwm_outside};
 assert(a733_pwm_init(&m.pwm,&c,&ops,&m,&m.power.clocks,&m.power.pins,&m.power.rails,&m.power.provider));
 assert(m.pwm.plan.duration_us==0&&!m.writes);
}
static void setup_pwm(void)
{setup_profile(25000,12500,1);assert(m.pwm.plan.pcr==0x100&&m.pwm.plan.ppr==0x0257012c);}
static enum a733_pwm_result step_pwm(unsigned notice)
{m.power.now+=100;assert(pmic_model_step(&m.power)!=A733_PMIC_RETAINED);return a733_pwm_poll(&m.pwm,notice);}
static void armed(void)
{
 enum a733_pwm_result r=A733_PWM_PENDING;
 for(unsigned i=0;i<2000&&r!=A733_PWM_ARMED;++i){r=step_pwm(0);assert(r!=A733_PWM_RETAINED);}
 assert(r==A733_PWM_ARMED&&m.pwm.supply.programmed_uv==1800000&&m.pwm.pin.id&&m.admitted==1);
 assert(m.pwm.led_supply.programmed_uv==3300000&&(m.pwm.clock.shared&A733_CLOCK_APBS0));
}
static void command(unsigned ticks,unsigned run)
{
 uint64_t before=m.pwm.service_completed;
 assert(a733_pwm_control(&m.pwm,ticks,run,m.power.now+50000)==A733_PWM_PENDING);
 for(unsigned i=0;i<20&&m.pwm.service_completed==before;++i){
  unsigned notice=m.pwm.service_step==6;
  if(notice)*pmic_model_word(&m.power,A733_PWM_BASE+4)=32;
  enum a733_pwm_result r=step_pwm(notice);assert(r!=A733_PWM_RETAINED);
 }
 assert(m.pwm.service_completed==before+1&&!m.pwm.service_step);
 if(run)assert(m.pwm.report.phase==13&&m.pwm.report.irq_seen&&!m.pwm.report.gpio_low);
 else assert(m.pwm.report.phase==16&&m.pwm.report.gpio_low&&m.pwm.report.waveform_stopped);
}
int main(void)
{
 setup_pwm();armed();uint64_t pin=m.pwm.pin.id,clock=m.pwm.clock.id,rail=m.pwm.rail.id;
 command(300,0);command(300,1);command(599,1);
 assert(m.pwm.plan.active==599&&m.pwm.plan.ppr==(599u<<16|599));
 assert(a733_pwm_led(&m.pwm,1,m.power.now+50000)==A733_PWM_COMPLETE);
 assert(*pmic_model_word(&m.power,0x07025010)==((1u<<6)|(1u<<7)));
 assert(a733_pwm_led(&m.pwm,0,m.power.now+50000)==A733_PWM_COMPLETE);
 assert(*pmic_model_word(&m.power,0x07025010)==(1u<<6));
 command(300,0);command(1,1);command(300,0);
 assert(m.pwm.pin.id==pin&&m.pwm.clock.id==clock&&m.pwm.rail.id==rail&&!m.pwm.fault);
 assert(a733_pwm_control(&m.pwm,601,1,m.power.now+50000)==A733_PWM_INVALID);
 command(300,1);m.fail_led=1;assert(a733_pwm_led(&m.pwm,1,m.power.now+50000)==A733_PWM_RETAINED);
 assert(!m.pwm.report.gpio_low&&*pmic_model_word(&m.power,A733_PWM_BASE+0x80)==32&&m.pwm.pin.id==pin);
 setup_pwm();armed();command(300,0);
 assert(a733_pwm_control(&m.pwm,300,1,m.power.now+50000)==A733_PWM_PENDING);
 while(m.pwm.service_step!=6)assert(step_pwm(0)!=A733_PWM_RETAINED);
 m.power.now=m.pwm.service_deadline;unsigned n=m.writes;
 assert(a733_pwm_poll(&m.pwm,0)==A733_PWM_RETAINED&&n==m.writes&&m.pwm.clock_retained&&m.pwm.rail_retained);
 setup_pwm();armed();command(300,0);m.fail_start=1;
 assert(a733_pwm_control(&m.pwm,300,1,m.power.now+50000)==A733_PWM_PENDING);
 enum a733_pwm_result r=A733_PWM_PENDING;
 for(unsigned i=0;i<10&&r!=A733_PWM_RETAINED;++i)r=step_pwm(0);
 assert(r==A733_PWM_RETAINED&&m.retained&&!m.pwm.report.gpio_low);
 /* Constant endpoints at the fan rate: zero holds inactive, 600 holds active. */
 setup_pwm();armed();command(0,1);assert(m.pwm.plan.ppr==(599u<<16));
 command(600,1);assert(m.pwm.plan.ppr==(599u<<16|600)&&*pmic_model_word(&m.power,A733_PWM_BASE+0x244)==m.pwm.plan.ppr);
 assert(a733_pwm_control(&m.pwm,601,1,m.power.now+50000)==A733_PWM_INVALID);command(300,0);
 /* A 100Hz active-low profile needs prescale4: 60000 ticks, PCR K field3, ACT_STA clear. */
 setup_profile(10000000,5000000,0);assert(m.pwm.plan.pcr==3&&m.pwm.plan.ticks==60000&&m.pwm.plan.ppr==(59999u<<16|30000));
 armed();command(30000,1);assert(*pmic_model_word(&m.power,A733_PWM_BASE+0x240)==3);
 command(0,1);command(60000,1);assert(m.pwm.plan.ppr==(59999u<<16|60000));
 assert(a733_pwm_control(&m.pwm,60001,1,m.power.now+50000)==A733_PWM_INVALID);command(1,0);
 assert(m.pwm.report.gpio_low&&!m.pwm.fault);
 /* A 1kHz active-low profile stays at prescale1 with 24000 ticks. */
 setup_profile(1000000,250000,0);assert(m.pwm.plan.pcr==0&&m.pwm.plan.ppr==(23999u<<16|6000));
 armed();command(6000,1);command(24000,1);command(0,0);
 /* Planning refusals: periods above10ms in cycle mode, duty inexact at the chosen
  * prescale and periods needing more than256x prescale. Finite pulses may use
  * the longer period inside their one-second bound. */
 struct a733_pwm_plan plan;a733_clock_mask_t shared;struct a733_pwm_config c=profile(20000000,10000000,0,1);
 assert(!a733_pwm_plan(&c,&plan,&shared));c.pulses=10;
 assert(a733_pwm_plan(&c,&plan,&shared)&&plan.pcr==((9u<<16)|(1u<<9)|(1u<<8)|7)&&plan.ticks==60000&&plan.duration_us==200000);
 c=profile(10000000,125,0,1);assert(!a733_pwm_plan(&c,&plan,&shared));
 c=profile(1000000000,0,1,1);assert(!a733_pwm_plan(&c,&plan,&shared));
 c=profile(25000,12500,0,2);assert(!a733_pwm_plan(&c,&plan,&shared));
 puts("PASS: actual reusable cycle/LED providers, stop-duty-restart with permanent leases, LED neighbor preservation and retained failures; constant endpoints, prescaled rates and active-low polarity");
}
