// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
/* Copyright (C) 2019 frank@allwinnertech.com */
#include "a733_adc.h"
#include "a733_clock_reference.h"
#include <stddef.h>
#define BIT(n) (UINT32_C(1)<<(n))
#define ADC UINT32_C(0x02521000)
#define CCU UINT32_C(0x02002000)
#define MASK UINT32_C(15)
enum {ADMIT,START,ACTIVE,FAILED,STOPPED};
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y && (x+an<=y || y+bn<=x);
}
int a733_adc_init(struct a733_adc *p,const struct a733_adc_config *c,
 const struct a733_adc_ops *o,void *cookie,struct a733_clock_leases *registry,const struct a733_clock_token *token)
{
 if(!p||p->initialized||!c||!o||!registry||!token||!c->owner||!c->handoff||!c->startup_deadline_us||
 c->configure_adc_clock>1||(c->apb1&~0x0300001fu)||!o->now_us||!o->current||!o->authorize||
 !o->enter||!o->leave||!o->retain||!o->read||!o->write||!token->id||token->owner!=c->owner)return 0;
 const void *objects[]={p,c,o,registry,token};size_t sizes[]={sizeof(*p),sizeof(*c),sizeof(*o),sizeof(*registry),sizeof(*token)};
 for(unsigned i=0;i<5;++i)for(unsigned j=i+1;j<5;++j)if(!separate(objects[i],sizes[i],objects[j],sizes[j]))return 0;
 unsigned parent=c->apb1>>24,div=(c->apb1&31)+1;uint32_t hz=parent==0?24000000u:parent==3?600000000u:0;
 a733_clock_mask_t stable=A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_FABRIC|A733_CLOCK_GPADC24M;
 if(parent==3)stable|=A733_CLOCK_PERI0;
 if(!hz||hz%div||c->apb1_hz!=hz/div||((token->shared|token->exclusive)&stable)!=stable||
 !(token->exclusive&A733_CLOCK_GPADC)||(c->configure_adc_clock&&!(token->exclusive&A733_CLOCK_GPADC24M)))return 0;
 *p=(struct a733_adc){.config=*c,.ops=*o,.token=*token,.registry=registry,.cookie=cookie,.deadline=c->startup_deadline_us,.initialized=1};atomic_init(&p->busy,0);return 1;
}
static int timely(struct a733_adc *p)
{
    uint64_t now;
    if (p->ops.now_us(p->cookie,&now)!=1 || now<p->last_now || now>=p->deadline) return 0;
    p->last_now=now;return 1;
}
static int rd(struct a733_adc *p,uintptr_t a,uint32_t *v)
{return timely(p) && p->ops.read(p->cookie,a,v)==1 && timely(p);}
static int bits(struct a733_adc *p,uintptr_t a,uint32_t mask,uint32_t value)
{uint32_t v;return rd(p,a,&v) && (v&mask)==value;}
static int update(struct a733_adc *p,uintptr_t a,uint32_t mask,uint32_t value)
{
    uint32_t old,actual;
    if (!rd(p,a,&old)) return 0;
    uint32_t next=(old&~mask)|(value&mask);
    if (old==next) return 1;
    return timely(p) && p->ops.write(p->cookie,a,next)==1 && timely(p) && rd(p,a,&actual) && actual==next;
}
static int clocks(struct a733_adc *p,int enabled,uint32_t *cold_source)
{
    uint32_t v,ref,oscillator;
    if (!rd(p,UINT32_C(0x07090160),&oscillator) || !rd(p,CCU,&ref) ||
        !a733_clock_ref24_valid(oscillator,ref) ||
        !bits(p,CCU+0x518,0x0300001f,p->config.apb1)) return 0;
    unsigned direct24=a733_clock_dcxo_hz(oscillator)==24000000;
    if (cold_source) *cold_source=direct24?BIT(24):0;
    if (p->config.apb1>>24==3) {
        if (!rd(p,CCU+0xa0,&v) || (v&0xfe000000u)!=0xfe000000u || (v&BIT(1)) ||
            !bits(p,CCU+0xa8,BIT(31),0) || !bits(p,CCU+0xac,0x8e000000,0) ||
            !bits(p,CCU+0x1908,0x0a000a00,0x0a000a00) || !bits(p,CCU+0x1988,0x0a000000,0x0a000000)) return 0;
        unsigned n=((v>>8)&255)+1;
        if (n<53 || n>105 || 24u*n!=600u*(((v>>20)&7)+1)*2) return 0;
    }
    if (!enabled && p->config.configure_adc_clock) return bits(p,CCU+0xfc0,BIT(31),0);
    if (!rd(p,CCU+0xfc0,&v)) return 0;
    v&=UINT32_C(0x8700001f);
    return (v==UINT32_C(0x80000000) || (direct24&&v==UINT32_C(0x81000000))) &&
        (!enabled || bits(p,CCU+0xfc4,0x10001,0x10001));
}
static int masked(struct a733_adc *p)
{
 return bits(p,0x03400000,BIT(31),0)&&bits(p,0x03400108,BIT(6),0)&&
 bits(p,0x03400208,BIT(6),0)&&bits(p,0x03400308,BIT(6),0);
}
static int clear_observed(struct a733_adc *p,uintptr_t a,uint32_t value)
{return !value||(timely(p)&&p->ops.write(p->cookie,a,value)==1&&timely(p));}
static int startup(struct a733_adc *p)
{
 uint32_t status,cold_source;
 if(!clocks(p,0,&cold_source)||!masked(p))return 0;
 if(p->config.configure_adc_clock&&(!update(p,CCU+0xfc0,0x0700001f,cold_source)||!update(p,CCU+0xfc0,BIT(31),BIT(31))))return 0;
 if(!update(p,CCU+0xfc4,BIT(16),BIT(16))||!update(p,CCU+0xfc4,1,1))return 0;
 /* Adopt only an inactive, non-DMA controller. Never erase inherited activity. */
 if(!bits(p,ADC+4,BIT(16)|BIT(17),0)||!bits(p,ADC+4,BIT(23),BIT(23))||
 !bits(p,ADC+8,0x007f007f,0)||!bits(p,ADC+0x0c,0x00070010,0)||
 !bits(p,ADC+0x10,0x00037f00,0)||!bits(p,ADC+0x20,127,0)||
 !bits(p,ADC+0x24,127,0)||!bits(p,ADC+0x28,127,0)||
 !bits(p,ADC+0x30,127,0)||!bits(p,ADC+0x34,127,0)||
 !bits(p,ADC+0xdc,1,0)||!bits(p,ADC+0xfc,1,1))return 0;
 if(!rd(p,ADC+0x38,&status)||(status&~MASK)||!clear_observed(p,ADC+0x38,status))return 0;
 /* Manual TACQ>10RC with C6.4pF:100us exceeds64us at admitted1Mohm.
  * Pinned U-Boot uses the same1kHz FS divider; no busy-wait is retained. */
 if(!update(p,ADC,UINT32_MAX,0x5dbf095f)||!update(p,ADC+0xd0,1,1)||
 !update(p,ADC+4,0x000f0000,0x00080000)||!update(p,ADC+8,0x007f007f,MASK)||
 !update(p,ADC+0x28,127,MASK)||!update(p,ADC+4,BIT(16),BIT(16)))return 0;
 return clocks(p,1,NULL);
}
static enum a733_adc_result finish(struct a733_adc *p,enum a733_adc_result r)
{atomic_store_explicit(&p->busy,0,memory_order_release);return r;}
static enum a733_adc_result fault(struct a733_adc *p)
{
    p->fault=1;
    if (p->held) {p->ops.retain(p->cookie);p->held=0;}
    if (a733_clock_retain(p->registry,&p->token)==A733_CLOCK_BUSY) return finish(p,A733_ADC_BUSY);
    p->stage=FAILED;return finish(p,A733_ADC_RETAINED);
}
static int claim(struct a733_adc *p)
{
    unsigned expected=0;
    return atomic_compare_exchange_strong_explicit(&p->busy,&expected,1,memory_order_acquire,memory_order_relaxed);
}
static enum a733_adc_result lock(struct a733_adc *p)
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
static int unlock(struct a733_adc *p)
{
    if (!timely(p)) return 0;
    p->ops.leave(p->cookie);p->held=0;
    return timely(p);
}
enum a733_adc_result a733_adc_poll(struct a733_adc *p)
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
enum a733_adc_result a733_adc_read(struct a733_adc *p,uint64_t deadline,struct a733_adc_sample *out)
{
 if(!p||!p->initialized||!out||!separate(out,sizeof(*out),p,sizeof(*p))||
 !separate(out,sizeof(*out),p->registry,sizeof(*p->registry)))return A733_ADC_INVALID;
 if(!claim(p))return A733_ADC_BUSY;if(p->stage==STOPPED) return finish(p,A733_ADC_STOPPED);
 if(p->stage==FAILED)return finish(p,A733_ADC_RETAINED);if(p->fault)return fault(p);
 if(p->ops.current(p->cookie)!=1||p->stage!=ACTIVE||!deadline)return fault(p);p->deadline=deadline;
 enum a733_adc_result r=lock(p);if(r!=A733_ADC_READY)return r;
 uint32_t status;
 if(!clocks(p,1,NULL)||!bits(p,ADC,UINT32_MAX,0x5dbf095f)||!bits(p,ADC+0xd0,1,1)||!bits(p,ADC+4,0x008f0000,0x00890000)||!bits(p,ADC+8,0x007f007f,MASK)||
 !bits(p,ADC+0x28,127,MASK)||!bits(p,ADC+0x0c,0x00070010,0)||!bits(p,ADC+0x10,0x00037f00,0)||
 !bits(p,ADC+0x20,127,0)||!bits(p,ADC+0x24,127,0)||!bits(p,ADC+0x30,127,0)||!bits(p,ADC+0x34,127,0)||!bits(p,ADC+0xdc,1,0)||!bits(p,ADC+0xfc,1,1)||
 !rd(p,ADC+0x38,&status)||(status&~MASK))return fault(p);
 struct a733_adc_sample sample={.sequence=p->sequence+1,.valid=status};
 if(!sample.sequence)return fault(p);
 for(unsigned i=0;i<4;++i)if(status&BIT(i)){uint32_t code;if(!rd(p,ADC+0x80+4*i,&code)||code>4095)return fault(p);sample.code[i]=(uint16_t)code;sample.observed_us[i]=p->last_now;}
 if(!clear_observed(p,ADC+0x38,status)||!unlock(p))return fault(p);
 if(!status)return finish(p,A733_ADC_AGAIN);p->sequence=sample.sequence;*out=sample;return finish(p,A733_ADC_SAMPLE);
}

enum a733_adc_result a733_adc_stop(struct a733_adc *p,uint64_t deadline)
{
 if (!p || !p->initialized) return A733_ADC_INVALID;
 if (!claim(p)) return A733_ADC_BUSY;
 if (p->stage==STOPPED) return finish(p,A733_ADC_STOPPED);
 if (p->stage==FAILED) return finish(p,A733_ADC_RETAINED);
 if (p->fault) return fault(p);
 if (p->stage!=ACTIVE || !deadline) return finish(p,A733_ADC_INVALID);
 if (p->ops.current(p->cookie)!=1) return fault(p);
 p->deadline=deadline;
 enum a733_adc_result r=lock(p);
 if (r!=A733_ADC_READY) return r;
 if (!clocks(p,1,NULL) || !bits(p,ADC+0x0c,0x00070010,0) ||
     !bits(p,ADC+0x10,0x00037f00,0) ||
     !update(p,ADC+0x28,127,0) || !update(p,ADC+4,BIT(16),0) ||
     !update(p,ADC+8,0x007f007f,0) ||
     !bits(p,ADC+0x28,127,0) || !bits(p,ADC+4,BIT(16),0) ||
     !bits(p,ADC+8,0x007f007f,0) || !unlock(p)) return fault(p);
 p->stage=STOPPED;return finish(p,A733_ADC_STOPPED);
}
