// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (C) 2019 frank@allwinnertech.com
 * Register/channel/clock sequencing adapted from pinned sunxi-lradc.c.
 * A733-only raw observation and retained lifecycle use the existing ADC adapter.
 * See apps/adc/lradc-sources.json for source pins and manual differences. */
#include "a733_lradc.h"
#include "a733_clock_reference.h"
#include <stddef.h>
#define BIT(n) (UINT32_C(1)<<(n))
#define ADC UINT32_C(0x02524000)
#define CCU UINT32_C(0x02002000)
#define RTC UINT32_C(0x07090000)
enum {ADMIT,START,ACTIVE,FAILED,STOPPED};
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y && (x+an<=y || y+bn<=x);
}
int a733_lradc_init(struct a733_lradc *p,const struct a733_adc_config *c,
 const struct a733_adc_ops *o,void *cookie,struct a733_clock_leases *registry,const struct a733_clock_token *token)
{
 if(!p||p->initialized||!c||!o||!registry||!token||!c->owner||!c->handoff||!c->startup_deadline_us||
 c->configure_adc_clock||(c->apb1&~0x0300001fu)||!o->now_us||!o->current||!o->authorize||
 !o->enter||!o->leave||!o->retain||!o->read||!o->write||!token->id||token->owner!=c->owner)return 0;
 const void *objects[]={p,c,o,registry,token};size_t sizes[]={sizeof(*p),sizeof(*c),sizeof(*o),sizeof(*registry),sizeof(*token)};
 for(unsigned i=0;i<5;++i)for(unsigned j=i+1;j<5;++j)if(!separate(objects[i],sizes[i],objects[j],sizes[j]))return 0;
 unsigned parent=c->apb1>>24,div=(c->apb1&31)+1;uint32_t hz=parent==0?24000000u:parent==3?600000000u:0;
 a733_clock_mask_t stable=A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_FABRIC|A733_CLOCK_RTC32K|A733_CLOCK_RC16M;
 if(parent==3)stable|=A733_CLOCK_PERI0;
 if(!hz||hz%div||c->apb1_hz!=hz/div||((token->shared|token->exclusive)&stable)!=stable||
 !(token->exclusive&A733_CLOCK_LRADC))return 0;
 *p=(struct a733_lradc){.config=*c,.ops=*o,.token=*token,.registry=registry,.cookie=cookie,.deadline=c->startup_deadline_us,.initialized=1};atomic_init(&p->busy,0);return 1;
}
static int timely(struct a733_lradc *p)
{
    uint64_t now;
    if (p->ops.now_us(p->cookie,&now)!=1 || now<p->last_now || now>=p->deadline) return 0;
    p->last_now=now;return 1;
}
static int rd(struct a733_lradc *p,uintptr_t a,uint32_t *v)
{return timely(p) && p->ops.read(p->cookie,a,v)==1 && timely(p);}
static int bits(struct a733_lradc *p,uintptr_t a,uint32_t mask,uint32_t value)
{uint32_t v;return rd(p,a,&v) && (v&mask)==value;}
static int update(struct a733_lradc *p,uintptr_t a,uint32_t mask,uint32_t value)
{
    uint32_t old,actual;
    if (!rd(p,a,&old)) return 0;
    uint32_t next=(old&~mask)|(value&mask);
    if (old==next) return 1;
    return timely(p) && p->ops.write(p->cookie,a,next)==1 && timely(p) && rd(p,a,&actual) && actual==next;
}
static int clocks(struct a733_lradc *p,int enabled)
{
    uint32_t v,ref,oscillator;
    if (!rd(p,UINT32_C(0x07090160),&oscillator) || !rd(p,CCU,&ref) ||
        !a733_clock_ref24_valid(oscillator,ref) ||
        !bits(p,CCU+0x518,0x0300001f,p->config.apb1)) return 0;
    if (p->config.apb1>>24==3) {
        if (!rd(p,CCU+0xa0,&v) || !a733_clock_peri0_valid(v) ||
            !bits(p,CCU+0xa8,BIT(31),0) || !bits(p,CCU+0xac,0x8e000000,0) ||
            !bits(p,CCU+0x1908,0x0a000a00,0x0a000a00) || !bits(p,CCU+0x1988,0x0a000000,0x0a000000)) return 0;
    }
    if (enabled && !bits(p,CCU+0x1024,0x10001,0x10001))return 0;
    uint32_t control,status,prescaler=0,calibration=0,osc=0;
    if(!rd(p,RTC,&control)||!rd(p,RTC+4,&status))return 0;
    unsigned source=1;
    if((control&0xc193)!=0x11||(status&7)!=1){
     if((control&0xc183)||(status&7)||!rd(p,RTC+8,&prescaler)||
       !rd(p,RTC+0xc,&calibration)||(calibration&1)||!rd(p,RTC+0x160,&osc)||!(osc&1))return 0;
     source=2;
    }
    if(p->source && (p->source!=source||((p->rtc_control^control)&0xc01f)||
      ((p->rtc_prescaler^prescaler)&31)||((p->rtc_calibration^calibration)&1)||((p->rtc_oscillator^osc)&1)))return 0;
    p->source=source;p->rtc_control=control;p->rtc_prescaler=prescaler;
    p->rtc_calibration=calibration;p->rtc_oscillator=osc;return 1;
}
static int masked(struct a733_lradc *p,int initial)
{
 return bits(p,0x03400000,BIT(31),0)&&bits(p,0x03400108,BIT(5),0)&&
 bits(p,0x03400308,BIT(5),0)&&(!initial||bits(p,0x03400208,BIT(5),0));
}
static int acknowledge(struct a733_lradc *p)
{return timely(p)&&p->ops.write(p->cookie,ADC+8,1)==1&&timely(p);}
static int clear_data(struct a733_lradc *p,uint32_t status,unsigned running)
{
 uint32_t v;
 if(status&~1u)return 0;
 if(status&&!acknowledge(p))return 0;
 /* Never mistake the power-on DATA0=63 or a stale pending flag for a sample. */
 if(!rd(p,ADC+8,&v))return 0;
 /* While EN is set, the next conversion (tens of milliseconds apart) can
  * land between W1C and readback. Acknowledge one such DATA0 reassertion;
  * a second within microseconds means the W1C was ignored. */
 if(running&&(v&0x1f1f)==1&&acknowledge(p))return bits(p,ADC+8,0x1f1f,0);
 return !(v&0x1f1f);
}
static int startup(struct a733_lradc *p)
{
 uint32_t status;
 if(!clocks(p,0)||!masked(p,1))return 0;
 /* Manual18.13.6: require gate off, deassert reset, then open local gate.
  * Never gate an inherited running controller or assert reset. */
 if(!bits(p,CCU+0x1024,1,0)||!update(p,CCU+0x1024,BIT(16),BIT(16))||
    !update(p,CCU+0x1024,1,1))return 0;
 if(!bits(p,ADC,1,0)||!bits(p,ADC+4,0x1f1f,0)||!rd(p,ADC+8,&status)||
    !clear_data(p,status,0))return 0;
 if(!update(p,ADC,0xffcf3ffd,A733_LRADC_CONTROL&~1u)||
    !update(p,ADC+4,0x1f1f,1)||!update(p,ADC,1,1))return 0;
 return clocks(p,1)&&masked(p,0);
}
static enum a733_adc_result finish(struct a733_lradc *p,enum a733_adc_result r)
{atomic_store_explicit(&p->busy,0,memory_order_release);return r;}
static enum a733_adc_result fault(struct a733_lradc *p)
{
    p->fault=1;
    if (p->held) {p->ops.retain(p->cookie);p->held=0;}
    if (a733_clock_retain(p->registry,&p->token)==A733_CLOCK_BUSY) return finish(p,A733_ADC_BUSY);
    p->stage=FAILED;return finish(p,A733_ADC_RETAINED);
}
static int claim(struct a733_lradc *p)
{
    unsigned expected=0;
    return atomic_compare_exchange_strong_explicit(&p->busy,&expected,1,memory_order_acquire,memory_order_relaxed);
}
static enum a733_adc_result lock(struct a733_lradc *p)
{
    if (!timely(p)) return fault(p);
    enum a733_clock_result r=a733_clock_token_status(p->registry,&p->token);
    if (r==A733_CLOCK_BUSY) return finish(p,A733_ADC_BUSY);
    if (r!=A733_CLOCK_OK) return fault(p);
    int entered=p->ops.enter(p->cookie,p->deadline);
    if (entered==1) p->held=1;
    if (entered!=1) return entered==0 && timely(p) ? finish(p,A733_ADC_BUSY) : fault(p);
    if (!timely(p)) return fault(p);
    /* Waiting for the shared mutex can outlive the token's validity. Recheck
     * before any controller access; registry contention has made no changes. */
    r=a733_clock_token_status(p->registry,&p->token);
    if (r==A733_CLOCK_BUSY) {
        p->ops.leave(p->cookie);p->held=0;
        return timely(p) ? finish(p,A733_ADC_BUSY) : fault(p);
    }
    if (r!=A733_CLOCK_OK || !timely(p)) return fault(p);
    return A733_ADC_READY; /* Internal: caller still holds both locks. */
}
static int unlock(struct a733_lradc *p)
{
    if (!timely(p)) return 0;
    p->ops.leave(p->cookie);p->held=0;
    return timely(p);
}
enum a733_adc_result a733_lradc_poll(struct a733_lradc *p)
{
 if(!p||!p->initialized)return A733_ADC_INVALID;if(!claim(p))return A733_ADC_BUSY;
 if(p->stage==STOPPED) return finish(p,A733_ADC_STOPPED);
 if(p->stage==FAILED)return finish(p,A733_ADC_RETAINED);if(p->fault)return fault(p);
 if(p->ops.current(p->cookie)!=1)return fault(p);
 if(p->stage==ACTIVE)return finish(p,A733_ADC_READY);
 if(p->stage==ADMIT){
  if(!timely(p))return fault(p);enum a733_clock_result r=a733_clock_token_status(p->registry,&p->token);
  if(r==A733_CLOCK_BUSY)return finish(p,A733_ADC_BUSY);if(r!=A733_CLOCK_OK)return fault(p);
  int ok=p->ops.authorize(p->cookie,&p->config,&p->token);
  if(!timely(p)||(ok!=0&&ok!=1))return fault(p);if(ok==1)p->stage=START;return finish(p,A733_ADC_PENDING);
 }
 enum a733_adc_result r=lock(p);if(r!=A733_ADC_READY)return r;
 if(!startup(p)||!unlock(p))return fault(p);p->stage=ACTIVE;return finish(p,A733_ADC_READY);
}
enum a733_adc_result a733_lradc_read(struct a733_lradc *p,uint64_t deadline,struct a733_adc_sample *out)
{
 if(!p||!p->initialized||!out||!separate(out,sizeof(*out),p,sizeof(*p))||
 !separate(out,sizeof(*out),p->registry,sizeof(*p->registry)))return A733_ADC_INVALID;
 if(!claim(p))return A733_ADC_BUSY;if(p->stage==STOPPED)return finish(p,A733_ADC_STOPPED);
 if(p->stage==FAILED)return finish(p,A733_ADC_RETAINED);if(p->fault)return fault(p);
 if(p->stage!=ACTIVE||!deadline)return finish(p,A733_ADC_INVALID);
 if(p->ops.current(p->cookie)!=1)return fault(p);p->deadline=deadline;
 enum a733_adc_result r=lock(p);if(r!=A733_ADC_READY)return r;
 uint32_t status,code;
 if(!clocks(p,1)||!masked(p,0)||!bits(p,ADC,0xffcf3ffd,A733_LRADC_CONTROL)||
 !bits(p,ADC+4,0x1f1f,1)||!rd(p,ADC+8,&status)||(status&~1u))return fault(p);
 if(!status){if(!unlock(p))return fault(p);return finish(p,A733_ADC_AGAIN);}
 struct a733_adc_sample sample={.sequence=p->sequence+1,.valid=1};
 if(!sample.sequence||!rd(p,ADC+0xc,&code)||code>63)return fault(p);
 sample.code[0]=(uint16_t)code;sample.observed_us[0]=p->last_now;
 if(!clear_data(p,status,1)||!unlock(p))return fault(p);
 p->sequence=sample.sequence;*out=sample;return finish(p,A733_ADC_SAMPLE);
}
enum a733_adc_result a733_lradc_stop(struct a733_lradc *p,uint64_t deadline)
{
 if(!p||!p->initialized)return A733_ADC_INVALID;if(!claim(p))return A733_ADC_BUSY;
 if(p->stage==STOPPED)return finish(p,A733_ADC_STOPPED);
 if(p->stage==FAILED)return finish(p,A733_ADC_RETAINED);if(p->fault)return fault(p);
 if(p->stage!=ACTIVE||!deadline)return finish(p,A733_ADC_INVALID);
 if(p->ops.current(p->cookie)!=1)return fault(p);p->deadline=deadline;
 enum a733_adc_result r=lock(p);if(r!=A733_ADC_READY)return r;
 if(!clocks(p,1)||!masked(p,0)||!update(p,ADC,1,0)||!update(p,ADC+4,0x1f1f,0)||
 !bits(p,ADC,1,0)||!bits(p,ADC+4,0x1f1f,0)||!unlock(p))return fault(p);
 p->stage=STOPPED;return finish(p,A733_ADC_STOPPED);
}
