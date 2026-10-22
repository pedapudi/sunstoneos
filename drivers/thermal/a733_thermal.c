// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
/* Copyright (C) 2019 frank@allwinnertech.com */
#include "a733_thermal.h"
#include "a733_clock_reference.h"
#include "a733_range.h"
#include <stddef.h>
#define BIT(n) (UINT32_C(1)<<(n))
#define THS UINT32_C(0x02522000)
#define CCU UINT32_C(0x02002000)
#define SID UINT32_C(0x03006244)
#define MASK UINT32_C(31)
enum { ADMIT, START, ACTIVE, FAILED, STOPPED };
int a733_thermal_temperature(uint16_t code,int32_t *out)
{
    if (!out || !code || code>4095) return 0;
    *out=code>1769 ? ((int32_t)code-2822)*-62 : ((int32_t)code-2835)*-59;
    return 1;
}
int a733_thermal_calibrate(const uint32_t words[3],struct a733_thermal_calibration *out)
{
    if (!a733_ranges_disjoint(words,3*sizeof(*words),out,sizeof(*out))) return 0;
    struct a733_thermal_calibration c={0};
    c.factory_tenth_c=words[0]&4095;
    if (!c.factory_tenth_c || c.factory_tenth_c==4095) return 0;
    for (unsigned i=0;i<5;++i) {
        unsigned bit=(i+1)*12,index=bit/32,shift=bit%32;
        uint32_t code=words[index]>>shift;
        if (shift>20) code|=words[index+1]<<(32-shift);
        c.factory_code[i]=(uint16_t)(code&4095);
        int32_t temperature;
        if (c.factory_code[i]==4095 || !a733_thermal_temperature(c.factory_code[i],&temperature)) return 0;
        /* C signed division truncates toward zero, matching the A733 source. */
        int32_t delta=((int32_t)c.factory_tenth_c*100-temperature)/-62;
        int32_t correction=2048-delta;
        if (correction<0 || correction>4095) return 0;
        c.correction[i]=(uint16_t)correction;
    }
    *out=c;return 1;
}
int a733_thermal_init(struct a733_thermal *p,const struct a733_thermal_config *c,
    const struct a733_thermal_ops *o,void *cookie,struct a733_clock_leases *registry,
    const struct a733_clock_token *token)
{
    if (!p || p->initialized || !c || !o || !registry || !token || !c->owner ||
        !c->handoff || !c->startup_deadline_us || c->configure_adc_clock>1 ||
        ((c->alarm_mask|c->critical_mask)&~MASK) || (c->apb1&~0x0300001fu) ||
        !o->now_us || !o->authorize || !o->enter || !o->leave || !o->retain || !o->read || !o->write ||
        !token->id || token->owner!=c->owner) return 0;
    const void *objects[]={p,c,o,registry,token};
    size_t sizes[]={sizeof(*p),sizeof(*c),sizeof(*o),sizeof(*registry),sizeof(*token)};
    for (unsigned i=0;i<5;++i) for (unsigned j=i+1;j<5;++j)
        if (!a733_ranges_disjoint(objects[i],sizes[i],objects[j],sizes[j])) return 0;
    unsigned parent=c->apb1>>24,div=(c->apb1&31)+1;
    uint32_t hz=parent==0 ? 24000000u : parent==3 ? 600000000u : 0;
    a733_clock_mask_t stable=A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_FABRIC|A733_CLOCK_GPADC24M;
    if (parent==3) stable|=A733_CLOCK_PERI0;
    if (!hz || hz%div || c->apb1_hz!=hz/div ||
        ((token->shared|token->exclusive)&stable)!=stable || !(token->exclusive&A733_CLOCK_THS) ||
        (c->configure_adc_clock && !(token->exclusive&A733_CLOCK_GPADC24M))) return 0;
    for (unsigned i=0;i<5;++i) {
        const struct a733_thermal_limits *l=&c->limits[i];
        if ((c->alarm_mask&BIT(i)) && (!l->hot || l->hot>=l->hysteresis || l->hysteresis>4095)) return 0;
        if ((c->critical_mask&BIT(i)) && (!l->critical || l->critical>4095 ||
            ((c->alarm_mask&BIT(i)) && l->critical>l->hot))) return 0;
    }
    *p=(struct a733_thermal){.config=*c,.ops=*o,.token=*token,.registry=registry,
        .cookie=cookie,.deadline=c->startup_deadline_us,.initialized=1};
    atomic_init(&p->busy,0);return 1;
}
static int timely(struct a733_thermal *p)
{
    uint64_t now;
    return p->ops.now_us(p->cookie,&now)==1 && a733_deadline_admit(now,&p->last_now,p->deadline);
}
static int rd(struct a733_thermal *p,uintptr_t a,uint32_t *v)
{return timely(p) && p->ops.read(p->cookie,a,v)==1 && timely(p);}
static int bits(struct a733_thermal *p,uintptr_t a,uint32_t mask,uint32_t value)
{uint32_t v;return rd(p,a,&v) && (v&mask)==value;}
static int update(struct a733_thermal *p,uintptr_t a,uint32_t mask,uint32_t value)
{
    uint32_t old,actual;
    if (!rd(p,a,&old)) return 0;
    uint32_t next=(old&~mask)|(value&mask);
    if (old==next) return 1;
    return timely(p) && p->ops.write(p->cookie,a,next)==1 && timely(p) && rd(p,a,&actual) && actual==next;
}
static int clear(struct a733_thermal *p,unsigned offset,uint32_t value)
{
    if (!value) return 1;
    return timely(p) && p->ops.write(p->cookie,THS+offset,value&MASK)==1 && timely(p);
}
static int clocks(struct a733_thermal *p,int enabled,uint32_t *cold_source)
{
    uint32_t v,ref,oscillator;
    if (!rd(p,UINT32_C(0x07090160),&oscillator) || !rd(p,CCU,&ref) ||
        !a733_clock_ref24_valid(oscillator,ref) ||
        !bits(p,CCU+0x518,0x0300001f,p->config.apb1)) return 0;
    /* SYS_CLK24M is normalized through PLL_REF. Raw DCXO is selectable only
     * when it also supplies 24 MHz (manual4.1.6.170, mux0 versus mux1). */
    unsigned direct24=a733_clock_dcxo_hz(oscillator)==24000000;
    if(cold_source)*cold_source=direct24?BIT(24):0;
    if (p->config.apb1>>24==3) {
        if (!rd(p,CCU+0xa0,&v) || !a733_clock_peri0_valid(v) ||
            !bits(p,CCU+0xa8,BIT(31),0) || !bits(p,CCU+0xac,0x8e000000,0) ||
            !bits(p,CCU+0x1908,0x0a000a00,0x0a000a00) || !bits(p,CCU+0x1988,0x0a000000,0x0a000000)) return 0;
    }
    if (!enabled && p->config.configure_adc_clock) return bits(p,CCU+0xfc0,BIT(31),0);
    if(!rd(p,CCU+0xfc0,&v))return 0;
    v&=UINT32_C(0x8700001f);
    return (v==UINT32_C(0x80000000) || (direct24&&v==UINT32_C(0x81000000))) &&
        (!enabled || bits(p,CCU+0xfe4,0x10001,0x10001));
}
static int startup(struct a733_thermal *p)
{
    uint32_t words[3],cold_source;
    if (!clocks(p,0,&cold_source)) return 0;
    for (unsigned i=0;i<3;++i) if (!rd(p,SID+4*i,&words[i])) return 0;
    if (!a733_thermal_calibrate(words,&p->calibration)) return 0;
    if (p->config.configure_adc_clock &&
        (!update(p,CCU+0xfc0,0x0700001f,cold_source) || !update(p,CCU+0xfc0,BIT(31),BIT(31)))) return 0;
    if (!update(p,CCU+0xfe4,BIT(16),BIT(16)) || !update(p,CCU+0xfe4,1,1)) return 0;
    /* Refuse an inherited thermal/protection service instead of clearing it. */
    if (!bits(p,THS+4,MASK,0) || !bits(p,THS+0x10,MASK,0) ||
        !bits(p,THS+0x14,MASK,0) || !bits(p,THS+0x18,MASK,0)) return 0;
    for (unsigned i=0;i<3;++i) {
        uint32_t mask=i==2 ? 4095u : 0x0fff0fffu;
        uint32_t value=p->calibration.correction[2*i];
        if (i<2) value|=(uint32_t)p->calibration.correction[2*i+1]<<16;
        if (!update(p,THS+0xa0+4*i,mask,value)) return 0;
    }
    /* A733-specific source writes these fixed acquisition/filter/period values. */
    if (!update(p,THS,UINT32_MAX,0x01df002f) || !update(p,THS+8,0xfffff000,28u<<12) ||
        !update(p,THS+0x30,7,5)) return 0;
    for (unsigned i=0;i<5;++i) {
        const struct a733_thermal_limits *l=&p->config.limits[i];
        if ((p->config.alarm_mask&BIT(i)) &&
            !update(p,THS+0x40+4*i,0x0fff0fff,((uint32_t)l->hot<<16)|l->hysteresis)) return 0;
        unsigned shift=(i%2)*16;
        if ((p->config.critical_mask&BIT(i)) &&
            !update(p,THS+0x80+4*(i/2),4095u<<shift,(uint32_t)l->critical<<shift)) return 0;
    }
    if (!clear(p,0x20,MASK) || !clear(p,0x24,MASK) || !clear(p,0x28,MASK) || !clear(p,0x2c,MASK) ||
        !update(p,THS+0x14,MASK,p->config.critical_mask) ||
        !update(p,THS+0x18,MASK,p->config.alarm_mask) || !update(p,THS+4,MASK,MASK)) return 0;
    return clocks(p,1,NULL);
}
static enum a733_thermal_result finish(struct a733_thermal *p,enum a733_thermal_result r)
{atomic_store_explicit(&p->busy,0,memory_order_release);return r;}
static enum a733_thermal_result fault(struct a733_thermal *p)
{
    p->fault=1;
    if (p->held) {p->ops.retain(p->cookie);p->held=0;}
    if (a733_clock_retain(p->registry,&p->token)==A733_CLOCK_BUSY) return finish(p,A733_THERMAL_BUSY);
    p->stage=FAILED;return finish(p,A733_THERMAL_RETAINED);
}
static int claim(struct a733_thermal *p)
{
    unsigned expected=0;
    return atomic_compare_exchange_strong_explicit(&p->busy,&expected,1,memory_order_acquire,memory_order_relaxed);
}
static enum a733_thermal_result lock(struct a733_thermal *p)
{
    if (!timely(p)) return fault(p);
    enum a733_clock_result r=a733_clock_token_status(p->registry,&p->token);
    if (r==A733_CLOCK_BUSY) return finish(p,A733_THERMAL_BUSY);
    if (r!=A733_CLOCK_OK) return fault(p);
    int entered=p->ops.enter(p->cookie,p->deadline);
    if (entered!=0) p->held=1;
    if (entered!=1) return entered==0 && timely(p) ? finish(p,A733_THERMAL_BUSY) : fault(p);
    if (!timely(p)) return fault(p);
    /* Waiting for the shared mutex can outlive the token's validity. Recheck
     * before any controller access; registry contention has made no changes. */
    r=a733_clock_token_status(p->registry,&p->token);
    if (r==A733_CLOCK_BUSY) {
        p->ops.leave(p->cookie);p->held=0;
        return timely(p) ? finish(p,A733_THERMAL_BUSY) : fault(p);
    }
    if (r!=A733_CLOCK_OK || !timely(p)) return fault(p);
    return A733_THERMAL_READY; /* Internal: caller still holds both locks. */
}
static int unlock(struct a733_thermal *p)
{
    if (!timely(p)) return 0;
    p->ops.leave(p->cookie);p->held=0;
    return timely(p);
}
enum a733_thermal_result a733_thermal_poll(struct a733_thermal *p)
{
    if (!p || !p->initialized) return A733_THERMAL_INVALID;
    if (!claim(p)) return A733_THERMAL_BUSY;
    if (p->stage==STOPPED) return finish(p,A733_THERMAL_STOPPED);
 if (p->stage==FAILED) return finish(p,A733_THERMAL_RETAINED);
    if (p->fault) return fault(p);
    if (p->stage==ACTIVE) return finish(p,A733_THERMAL_READY);
    if (p->stage==ADMIT) {
        if (!timely(p)) return fault(p);
        enum a733_clock_result r=a733_clock_token_status(p->registry,&p->token);
        if (r==A733_CLOCK_BUSY) return finish(p,A733_THERMAL_BUSY);
        if (r!=A733_CLOCK_OK) return fault(p);
        int ok=p->ops.authorize(p->cookie,&p->config,&p->token);
        if (!timely(p) || (ok!=0 && ok!=1)) return fault(p);
        if (ok==1) p->stage=START;
        return finish(p,A733_THERMAL_PENDING);
    }
    enum a733_thermal_result r=lock(p);
    if (r!=A733_THERMAL_READY) return r;
    if (!startup(p) || !unlock(p)) return fault(p);
    p->stage=ACTIVE;return finish(p,A733_THERMAL_READY);
}
enum a733_thermal_result a733_thermal_read(struct a733_thermal *p,uint64_t deadline,
    struct a733_thermal_sample *out)
{
    if (!p || !p->initialized || !a733_ranges_disjoint(p,sizeof(*p),out,sizeof(*out)) ||
        !a733_ranges_disjoint(p->registry,sizeof(*p->registry),out,sizeof(*out))) return A733_THERMAL_INVALID;
    if (!claim(p)) return A733_THERMAL_BUSY;
    if (p->stage==STOPPED) return finish(p,A733_THERMAL_STOPPED);
 if (p->stage==FAILED) return finish(p,A733_THERMAL_RETAINED);
    if (p->fault) return fault(p);
    if (p->stage!=ACTIVE || !deadline) return finish(p,A733_THERMAL_INVALID);
    p->deadline=deadline;
    enum a733_thermal_result r=lock(p);
    if (r!=A733_THERMAL_READY) return r;
    uint32_t flags,critical,alarm,off;
    struct a733_thermal_sample sample={0};
    if (!clocks(p,1,NULL) || !bits(p,THS+4,MASK,MASK) || !rd(p,THS+0x20,&flags) ||
        !rd(p,THS+0x24,&critical) || !rd(p,THS+0x28,&off) || !rd(p,THS+0x2c,&alarm)) return fault(p);
    flags&=MASK;critical&=MASK;off&=MASK;alarm&=MASK;
    for (unsigned i=0;i<5;++i) if (flags&BIT(i)) {
        uint32_t code;
        if (!rd(p,THS+0xc0+4*i,&code)) return fault(p);
        sample.code[i]=(uint16_t)(code&4095);
        if (a733_thermal_temperature(sample.code[i],&sample.millicelsius[i])) sample.valid|=(uint8_t)BIT(i);
    }
    sample.critical=(uint8_t)critical;sample.alarm=(uint8_t)alarm;sample.alarm_off=(uint8_t)off;
    if (!clear(p,0x20,flags) || !clear(p,0x24,critical) || !clear(p,0x28,off) || !clear(p,0x2c,alarm) || !unlock(p)) return fault(p);
    if (!(sample.valid|sample.critical|sample.alarm|sample.alarm_off)) return finish(p,A733_THERMAL_AGAIN);
    if (p->sequence==UINT32_MAX) return fault(p);
    sample.sequence=++p->sequence;sample.observed_us=p->last_now;*out=sample;
    return finish(p,A733_THERMAL_SAMPLE);
}

enum a733_thermal_result a733_thermal_stop(struct a733_thermal *p,uint64_t deadline)
{
 if (!p || !p->initialized) return A733_THERMAL_INVALID;
 if (!claim(p)) return A733_THERMAL_BUSY;
 if (p->stage==STOPPED) return finish(p,A733_THERMAL_STOPPED);
 if (p->stage==FAILED) return finish(p,A733_THERMAL_RETAINED);
 if (p->fault) return fault(p);
 if (p->stage!=ACTIVE || !deadline) return finish(p,A733_THERMAL_INVALID);

 p->deadline=deadline;
 enum a733_thermal_result r=lock(p);
 if (r!=A733_THERMAL_READY) return r;
 if (!clocks(p,1,NULL) || !bits(p,THS+0x10,MASK,0) ||
     !update(p,THS+0x14,MASK,0) || !update(p,THS+0x18,MASK,0) ||
     !update(p,THS+4,MASK,0) || !bits(p,THS+4,MASK,0) ||
     !bits(p,THS+0x14,MASK,0) || !bits(p,THS+0x18,MASK,0) ||
     !unlock(p)) return fault(p);
 p->stage=STOPPED;return finish(p,A733_THERMAL_STOPPED);
}

/* sun60iw2p1.dtsi thermal-zones: the second passive trip of each CPU/GPU zone
 * is the one bound to a CPU or GPU cooling device; the first only starts fans
 * and power accounting. NPU and DDR zones have critical trips only. */
static const struct { int32_t passive,hysteresis,critical; } trips[A733_THERMAL_SENSORS]={
    {90000,2000,110000},{0,0,110000},{0,0,110000},{90000,2000,110000},{90000,0,110000}};
#define STALE_US UINT64_C(1000000) /* The zones' 1000 ms polling delay. */
#define LOST_US UINT64_C(5000000)
int a733_thermal_policy_init(struct a733_thermal_policy *p,uint64_t now,
    int (*notify)(void *,enum a733_thermal_level),void *cookie)
{
    if (!p || p->initialized || !notify) return 0;
    *p=(struct a733_thermal_policy){.notify=notify,.cookie=cookie,.start_us=now,.last_us=now,.initialized=1};
    atomic_init(&p->level,A733_THERMAL_NORMAL);atomic_init(&p->published_seen,0);return 1;
}
unsigned a733_thermal_policy_seen(const struct a733_thermal_policy *p)
{
    if (!p || !p->initialized) return 0;
    return atomic_load_explicit(&p->published_seen,memory_order_acquire);
}
enum a733_thermal_level a733_thermal_policy_level(const struct a733_thermal_policy *p)
{
    if (!p || !p->initialized) return A733_THERMAL_CRITICAL;
    return (enum a733_thermal_level)atomic_load_explicit(&p->level,memory_order_acquire);
}
enum a733_thermal_level a733_thermal_policy_update(struct a733_thermal_policy *p,uint64_t now,
    enum a733_thermal_result r,const struct a733_thermal_sample *s)
{
    if (!p || !p->initialized) return A733_THERMAL_CRITICAL;
    unsigned old=atomic_load_explicit(&p->level,memory_order_relaxed);
    if (old!=A733_THERMAL_CRITICAL) {
        if (now<p->last_us || (r==A733_THERMAL_SAMPLE ? !s || (s->valid&~MASK) ||
            s->sequence<=p->sequence || s->observed_us<p->last_us || s->observed_us>now :
            r!=A733_THERMAL_AGAIN && r!=A733_THERMAL_BUSY)) p->sensor=1;
        else {
            p->last_us=now;
            if (r==A733_THERMAL_SAMPLE) {
                p->sequence=s->sequence;
                for (unsigned i=0;i<A733_THERMAL_SENSORS;++i) if (s->valid&BIT(i)) {
                    p->millicelsius[i]=s->millicelsius[i];p->seen_us[i]=s->observed_us;p->seen|=(uint8_t)BIT(i);
                }
            }
            for (unsigned i=0;i<A733_THERMAL_SENSORS;++i) {
                uint8_t bit=(uint8_t)BIT(i);
                uint64_t age=now-((p->seen&bit) ? p->seen_us[i] : p->start_us);
                int32_t t=p->millicelsius[i];
                p->stale=(uint8_t)(age>STALE_US ? p->stale|bit : p->stale&~bit);
                if (age>LOST_US || ((p->seen&bit) && t>=trips[i].critical)) p->critical|=bit;
                if (!trips[i].passive || !(p->seen&bit)) continue;
                if (t>=trips[i].passive) p->passive|=bit;
                else if (t<trips[i].passive-trips[i].hysteresis) p->passive&=(uint8_t)~bit;
            }
        }
        unsigned level=p->sensor || p->critical ? A733_THERMAL_CRITICAL :
            p->passive || p->stale ? A733_THERMAL_THROTTLE : A733_THERMAL_NORMAL;
        /* Channels publish after the level, so a reader that sees a channel
         * also sees the level that its reading produced. */
        if (level!=old) atomic_store_explicit(&p->level,level,memory_order_release);
        atomic_store_explicit(&p->published_seen,p->seen,memory_order_release);
        if (level==old) return (enum a733_thermal_level)level;
        if (level!=A733_THERMAL_CRITICAL) {
            (void)p->notify(p->cookie,(enum a733_thermal_level)level);return (enum a733_thermal_level)level;
        }
        p->request=1;
    }
    if (p->request==1) {
        int ok=p->notify(p->cookie,A733_THERMAL_CRITICAL);
        p->request=ok==1 ? 2 : ok==0 ? 1 : 3;
    }
    return A733_THERMAL_CRITICAL;
}
