/*
 * PWM register layout and finite cycle encoding adapted from Allwinner's
 * drivers/pwm/sunxi_pwm.{c,h}, pinned Orange Pi U-Boot b791be842935b27268ae3d00e943a9075495f30a.
 * (C) Copyright 2012 tyle@allwinnertech.com
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2, or (at your option) any later version.
 * This program is distributed without any warranty; without even the implied
 * warranty of merchantability or fitness for a particular purpose.
 * The FDT/global synchronous lifecycle is replaced by permanent FreeRTOS
 * resource ownership and bounded state transitions. Register semantics are
 * cross-checked against A733 manual18.7; no Linux/U-Boot runtime is required.
 */
#include "a733_pwm.h"
#include <string.h>
#define BIT(n) (UINT32_C(1)<<(n))
#define PWM A733_PWM_BASE
#define BGR UINT32_C(0x02002784)
#define PCGR (PWM+0x40)
#define PCR (PWM+0x240)
#define PPR (PWM+0x244)
#define PER (PWM+0x80)
#define PIER PWM
#define PISR (PWM+4)
#define GIC UINT32_C(0x03400000)
enum { CLOCK,RAIL,RAIL_WAIT,IDENTITY,SUBMIT,SNAPSHOT,REAP,ADMIT,ENABLE,RELEASE,CONFIGURE,PINS,ARMED,RUN,STOP,PIN_LOW,DONE };
int a733_pwm_plan(const struct a733_pwm_config *c,struct a733_pwm_plan *p,a733_clock_mask_t *shared)
{
 if(!c||!p||!shared||!c->owner||!c->handoff||!c->deadline_us||!c->period_ns||
 c->duty_ns>c->period_ns||!c->pulses||c->pulses>65536||c->active_high>1||
 (c->apb0&~UINT32_C(0x0300001f))||
 (c->expected.function!=0&&c->expected.function!=15)||c->desired.function!=7||
 !a733_pin_config_valid_banked(A733_PIN_D,5,c->expected)||
 !a733_pin_config_valid_banked(A733_PIN_D,5,c->desired))return 0;
 uint64_t ticks=(uint64_t)c->period_ns*24,active=(uint64_t)c->duty_ns*24;
 if(ticks%1000||active%1000||!(ticks/=1000)||ticks>65536||(active/=1000)>65535)return 0;
 unsigned source=c->apb0>>24,div=(c->apb0&31)+1,hz=source==0?24000000:source==3?600000000:0;
 if(!hz||hz%div||c->apb0_hz!=hz/div)return 0;
 uint64_t duration=((uint64_t)c->period_ns*c->pulses+999)/1000;
 if(duration>1000000)return 0; /* This finite owner never requests over1 second. */
 *p=(struct a733_pwm_plan){((c->pulses-1)<<16)|BIT(9)|(c->active_high?BIT(8):0),
 ((uint32_t)(ticks-1)<<16)|(uint32_t)active,(uint32_t)ticks,(uint32_t)active,duration};
 *shared=A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC|(source==3?A733_CLOCK_PERI0:0);return 1;
}
static int separate(const void *a,size_t an,const void *b,size_t bn)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return a&&b&&an<=UINTPTR_MAX-x&&bn<=UINTPTR_MAX-y&&(x+an<=y||y+bn<=x);}
static enum a733_pwm_result fail(struct a733_pwm *p,unsigned error)
{
 p->fault=1;if(!p->report.error)p->report.error=error;
 if(p->held){p->ops.retain(p->cookie);p->held=0;}
 if(p->clock.id&&!p->clock_retained&&a733_clock_retain(p->clocks,&p->clock)!=A733_CLOCK_BUSY)p->clock_retained=1;
 if(p->rail.id&&!p->rail_retained&&a733_rail_retain(p->rails,&p->rail)!=A733_RAIL_BUSY)p->rail_retained=1;
 return A733_PWM_RETAINED;
}
static int timely(struct a733_pwm *p)
{uint64_t n;if(p->ops.now_us(p->cookie,&n)!=1||n<p->last_now)return 0;p->last_now=n;return n<p->config.deadline_us;}
static enum a733_pwm_result finish(struct a733_pwm *p,enum a733_pwm_result r)
{if(p->held){p->ops.leave(p->cookie);p->held=0;}atomic_store_explicit(&p->busy,0,memory_order_release);return r;}
static int api(struct a733_pwm *p)
{unsigned zero=0;return p&&p->initialized&&atomic_compare_exchange_strong_explicit(&p->busy,&zero,1,memory_order_acquire,memory_order_relaxed);}
static int rd(struct a733_pwm *p,uintptr_t a,uint32_t *v)
{return timely(p)&&p->ops.read(p->cookie,a,v)==1&&timely(p);}
static int wr(struct a733_pwm *p,uintptr_t a,uint32_t v)
{return timely(p)&&p->ops.write(p->cookie,a,v)==1&&timely(p);}
static int exact(struct a733_pwm *p,uintptr_t a,uint32_t v)
{uint32_t x;return wr(p,a,v)&&rd(p,a,&x)&&x==v;}
static int health(struct a733_pwm *p)
{
 if(p->clock.id){enum a733_clock_result c=a733_clock_token_status(p->clocks,&p->clock);if(c==A733_CLOCK_BUSY)return 0;if(c!=A733_CLOCK_OK)return -1;}
 if(p->rail.id&&p->report.phase>RAIL_WAIT){enum a733_rail_result r=a733_rail_status(p->rails,&p->rail);if(r==A733_RAIL_BUSY||r==A733_RAIL_PENDING)return 0;if(r!=A733_RAIL_READY)return -1;}
 if(p->power.identity){enum a733_pmic_status s=a733_pmic_live(p->pmic,&p->power);if(s==A733_PMIC_BUSY||s==A733_PMIC_PENDING)return 0;if(s!=A733_PMIC_READY)return -1;}
 return timely(p)?1:-1;
}
static int profile(struct a733_pwm *p)
{
 uint32_t v,ref;
 if(!rd(p,0x07090160,&v)||(v&(BIT(28)|BIT(1)))!=(BIT(28)|BIT(1))||
 (((v>>14)&3)!=0&&((v>>14)&3)!=3)||!rd(p,0x02002000,&ref)||(ref&0xf8000000)!=0xf8000000)return 0;
 unsigned n=((ref>>8)&255)+1,m=((ref>>16)&127)+1;
 if(n<80||n>116||n!=m||!rd(p,0x02002510,&v)||(v&0x0300001f)!=p->config.apb0)return 0;
 p->report.apb0=v;
 if(p->shared&A733_CLOCK_PERI0){uint32_t pll,a,b,gate,status;
  if(!rd(p,0x020020a0,&pll)||(pll&0xfe000000)!=0xfe000000||(pll&(BIT(24)|BIT(1)))||
  !rd(p,0x020020a8,&a)||(a&BIT(31))||!rd(p,0x020020ac,&b)||(b&BIT(27))||
  !rd(p,0x02003908,&gate)||!rd(p,0x02003988,&status))return 0;
  n=((pll>>8)&255)+1;unsigned div=(((pll>>20)&7)+1)*2,branches=BIT(11)|BIT(9),enabled=branches<<16;
  if(n<53||n>105||24*n!=600*div||(gate&(branches|enabled))!=(branches|enabled)||(status&enabled)!=enabled)return 0;
 }
 if(!rd(p,0x02000040,&p->report.voltage_mode)||!rd(p,0x02000048,&p->report.voltage_value))return 0;
 /* PD uses generic two-bit encoding: fixed/adaptive1.8V and detected1.8V.
  * Actual rail voltage and physical topology are authenticated independently. */
 unsigned mode=(p->report.voltage_mode>>6)&3,value=(p->report.voltage_value>>6)&3;
 return (mode==0||mode==2)&&value==0;
}
static int zero(struct a733_pwm *p,uintptr_t a)
{uint32_t v;return rd(p,a,&v)&&v==0;}
int a733_pwm_init(struct a733_pwm *p,const struct a733_pwm_config *c,const struct a733_pwm_ops *o,void *cookie,
 struct a733_clock_leases *clocks,struct a733_pins *pins,struct a733_rail_leases *rails,struct a733_pmic *pmic)
{
 struct a733_pwm_plan plan;a733_clock_mask_t shared;
 if(!p||p->initialized||!a733_pwm_plan(c,&plan,&shared)||!o||!o->now_us||!o->current||!o->admit||!o->enter||
 !o->leave||!o->retain||!o->read||!o->write||!o->outside||!clocks||!clocks->initialized||!pins||!pins->initialized||
 !rails||rails->self!=rails||rails->binding.clocks!=clocks||!rails->binding.gpu||!pmic||pmic->clocks!=clocks||pmic->pins!=pins||
 pmic->rails!=rails||!pmic->ops.outside||pmic->ops.outside(pmic->cookie,p,sizeof(*p))!=1||o->outside(cookie,p,sizeof(*p))!=1||
 !separate(p,sizeof(*p),c,sizeof(*c))||!separate(p,sizeof(*p),o,sizeof(*o))||!separate(p,sizeof(*p),clocks,sizeof(*clocks))||
 !separate(p,sizeof(*p),pins,sizeof(*pins))||!separate(p,sizeof(*p),rails,sizeof(*rails))||!separate(p,sizeof(*p),pmic,sizeof(*pmic))||
 !separate(p,sizeof(*p),rails->binding.gpu,sizeof(*rails->binding.gpu)))return 0;
 *p=(struct a733_pwm){.config=*c,.plan=plan,.ops=*o,.cookie=cookie,.clocks=clocks,.pins=pins,.rails=rails,.pmic=pmic,.shared=shared};
 atomic_init(&p->busy,0);p->initialized=1;return 1;
}
static enum a733_pwm_result metadata(struct a733_pwm *p)
{
 switch(p->report.phase){
 case CLOCK:{enum a733_clock_result r=a733_clock_acquire(p->clocks,p->config.owner,p->shared,A733_CLOCK_PWM0,p->config.deadline_us,&p->clock);
  if(r==A733_CLOCK_BUSY)return A733_PWM_BUSY;if(r!=A733_CLOCK_OK)return fail(p,2);break;}
 case RAIL:{enum a733_rail_result r=a733_rail_acquire(p->rails,p->config.owner,A733_RAIL_BIT(A733_RAIL_BLDO1),0,p->config.deadline_us,&p->rail);
  if(r==A733_RAIL_BUSY)return A733_PWM_BUSY;if(r!=A733_RAIL_PENDING)return fail(p,3);break;}
 case RAIL_WAIT:{enum a733_rail_result r=a733_rail_poll(p->rails,&p->rail);
  if(r==A733_RAIL_BUSY)return A733_PWM_BUSY;if(r==A733_RAIL_PENDING)return A733_PWM_PENDING;if(r!=A733_RAIL_READY)return fail(p,3);break;}
 case IDENTITY:{enum a733_pmic_status r=a733_pmic_identity(p->pmic,&p->power);
  if(r==A733_PMIC_BUSY||r==A733_PMIC_PENDING)return A733_PWM_BUSY;if(r!=A733_PMIC_READY)return fail(p,4);
  break;}
 case SUBMIT:{enum a733_pmic_status r=a733_pmic_submit(p->pmic,p->config.owner,&p->rail,A733_RAIL_BLDO1,p->config.deadline_us,&p->request);
  if(r==A733_PMIC_BUSY)return A733_PWM_BUSY;if(r!=A733_PMIC_PENDING||!p->request.id)return fail(p,4);
  break;}
 case SNAPSHOT:{enum a733_pmic_status r=a733_pmic_result(p->pmic,&p->request,&p->supply);
  if(r==A733_PMIC_BUSY||r==A733_PMIC_PENDING)return A733_PWM_BUSY;
  if(r!=A733_PMIC_COMPLETE||p->supply.rail!=A733_RAIL_BLDO1||p->supply.page_before||p->supply.page_after||
  p->supply.chip!=3||p->supply.revision||p->supply.enabled!=1||p->supply.decoded!=1||p->supply.programmed_uv!=1800000)return fail(p,4);
  break;}
 case REAP:{enum a733_pmic_status r=a733_pmic_reap(p->pmic,&p->request);if(r==A733_PMIC_BUSY)return A733_PWM_BUSY;
  if(r!=A733_PMIC_COMPLETE)return fail(p,4);p->request=(struct a733_pmic_request){0};break;}
 case ADMIT:if(p->ops.admit(p->cookie,&p->config,&p->clock)!=1)return fail(p,5);break;
 case PINS:{struct a733_pin_request_banked r={A733_PIN_D,5,p->config.expected,p->config.desired,0,0,0};
  enum a733_pins_result v=a733_pins_acquire_banked(p->pins,p->config.owner,&r,1,&p->pin,p->config.deadline_us);
  if(v==A733_PINS_BUSY||v==A733_PINS_CONFLICT)return A733_PWM_BUSY;if(v!=A733_PINS_OK)return fail(p,6);break;}
 case PIN_LOW:{enum a733_pins_result r=a733_pins_pwm0_fan_low(p->pins,&p->pin,1,p->config.deadline_us);
  if(r==A733_PINS_BUSY)return A733_PWM_BUSY;if(r!=A733_PINS_OK)return fail(p,14);
  p->report.gpio_low=1;break;}
 default:return fail(p,7);
 }
 if(!timely(p))return fail(p,1);
 if(p->report.phase==PIN_LOW)p->report.completed_us=p->last_now;
 ++p->report.phase;return p->report.phase==ARMED?A733_PWM_ARMED:p->report.phase==DONE?A733_PWM_COMPLETE:A733_PWM_PENDING;
}
static enum a733_pwm_result hardware(struct a733_pwm *p,unsigned notice)
{
 int lock=p->ops.enter(p->cookie,p->config.deadline_us);
 if(!lock)return A733_PWM_BUSY;if(lock!=1)return fail(p,8);p->held=1;
 if(!timely(p)||!profile(p))return fail(p,9);
 uint32_t v;
 switch(p->report.phase){
 case ENABLE:
  if(!rd(p,BGR,&p->initial_bgr)||(p->initial_bgr&0x10001)||
  !rd(p,GIC,&v)||(v&BIT(31))||!rd(p,GIC+0x108,&v)||(v&BIT(3))||
  !rd(p,GIC+0x208,&v)||(v&BIT(3))||!rd(p,GIC+0x308,&v)||(v&BIT(3))||
  !rd(p,GIC+0x88,&v)||!(v&BIT(3))||!rd(p,GIC+0xd08,&v)||(v&BIT(3))||
  !rd(p,GIC+0xc10,&v)||(v&(3u<<6))||!rd(p,GIC+0x440,&v)||(v&0xff000000)!=0xd8000000||
  !zero(p,GIC+0x6218)||!zero(p,GIC+0x621c)||
  !exact(p,BGR,p->initial_bgr|1))return fail(p,10);
  break;
 case RELEASE:if(!exact(p,BGR,p->initial_bgr|0x10001))return fail(p,10);
  break;
 case CONFIGURE:
  if(!zero(p,PIER)||!zero(p,PISR)||!zero(p,PWM+0x10)||!zero(p,PWM+0x14)||!zero(p,PER)||!zero(p,PCGR)||
  !zero(p,PWM+0xc0)||!zero(p,PWM+0x68)||!zero(p,PWM+0x90)||!zero(p,PWM+0x94)||!zero(p,PWM+0x98)||!zero(p,PWM+0x9c)||
  !zero(p,PWM+0x28)||!rd(p,PCR,&v)||(v&BIT(11))||v||!exact(p,PCR,p->plan.pcr)||
  !exact(p,PCGR,BIT(5))||!rd(p,PCR,&v)||(v&BIT(11))||!exact(p,PPR,p->plan.ppr)||!exact(p,PWM+0x248,0))return fail(p,11);
  break;
 case RUN:
  if(!rd(p,PCR,&p->report.pcr)||!rd(p,PISR,&p->report.pisr)||
  (p->report.pisr&~BIT(5)))return fail(p,12);
  if(notice){if((!p->report.irq_seen&&!(p->report.pisr&BIT(5)))||!exact(p,PIER,0)||!wr(p,PISR,BIT(5)))return fail(p,12);p->report.irq_seen=1;}
  if(p->report.pcr&BIT(10))return A733_PWM_RUNNING;
  if(!p->report.irq_seen)return A733_PWM_RUNNING;
  if(!rd(p,PPR,&p->report.ppr)||!rd(p,PWM+0x248,&p->report.counter)||!rd(p,PWM+0x24c,&p->report.pulse_counter)||
  p->report.ppr!=p->plan.ppr||(p->report.pcr&~BIT(10))!=p->plan.pcr)return fail(p,12);
  break;
 case STOP:
  if(!exact(p,PIER,0)||!exact(p,PER,0)||!exact(p,PCGR,0)||!wr(p,PISR,BIT(5))||!rd(p,BGR,&p->report.bgr))return fail(p,13);
  p->report.waveform_stopped=1;break;
 default:return fail(p,7);
 }
 ++p->report.phase;return A733_PWM_PENDING;
}
enum a733_pwm_result a733_pwm_poll(struct a733_pwm *p,unsigned notice)
{
 if(!p||!p->initialized||notice>1)return A733_PWM_INVALID;if(!api(p))return A733_PWM_BUSY;
 if(p->ops.current(p->cookie)!=1)return finish(p,A733_PWM_INVALID);
 if(p->fault)return finish(p,fail(p,p->report.error));
 if(!timely(p))return finish(p,fail(p,1));
 int ok=health(p);if(ok<0)return finish(p,fail(p,9));if(!ok)return finish(p,A733_PWM_BUSY);
 if(notice&&p->report.phase!=RUN)return finish(p,fail(p,12));
 if(p->report.phase==ARMED)return finish(p,A733_PWM_ARMED);
 if(p->report.phase==DONE)return finish(p,A733_PWM_COMPLETE);
 enum a733_pwm_result r=(p->report.phase<=ADMIT||p->report.phase==PINS||p->report.phase==PIN_LOW)?metadata(p):hardware(p,notice);
 return finish(p,r);
}
enum a733_pwm_result a733_pwm_start(struct a733_pwm *p)
{
 if(!p||!p->initialized)return A733_PWM_INVALID;if(!api(p))return A733_PWM_BUSY;
 if(p->ops.current(p->cookie)!=1||p->report.phase!=ARMED)return finish(p,A733_PWM_INVALID);
 if(p->fault||!timely(p))return finish(p,fail(p,1));
 int ok=health(p);if(ok<0)return finish(p,fail(p,9));if(!ok)return finish(p,A733_PWM_BUSY);
 int lock=p->ops.enter(p->cookie,p->config.deadline_us);if(!lock)return finish(p,A733_PWM_BUSY);if(lock!=1)return finish(p,fail(p,8));p->held=1;
 uint32_t v;
 if(p->config.deadline_us-p->last_now<=p->plan.duration_us+20000||!profile(p)||!rd(p,PCR,&v)||v!=p->plan.pcr||!rd(p,PPR,&v)||v!=p->plan.ppr||
 !rd(p,PCGR,&v)||v!=BIT(5)||!zero(p,PER)||!zero(p,PISR)||!rd(p,GIC+0x108,&v)||!(v&BIT(3))||
 !exact(p,PIER,BIT(5))||!exact(p,PER,BIT(5)))return finish(p,fail(p,15));
 p->report.started=1;p->report.started_us=p->last_now;
 if(!wr(p,PCR,p->plan.pcr|BIT(10)))return finish(p,fail(p,15));
 p->report.phase=RUN;return finish(p,A733_PWM_RUNNING);
}
enum a733_pwm_result a733_pwm_retain(struct a733_pwm *p)
{
 if(!p||!p->initialized)return A733_PWM_INVALID;if(!api(p))return A733_PWM_BUSY;
 if(p->ops.current(p->cookie)!=1)return finish(p,A733_PWM_INVALID);
 (void)fail(p,16);
 return finish(p,((p->clock.id&&!p->clock_retained)||(p->rail.id&&!p->rail_retained))?A733_PWM_BUSY:A733_PWM_RETAINED);
}
