/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (c) 2020, Martin <wuyan@allwinnertech.com>
 * TypeA calendar/read/alarm operations adapted from pinned rtc-sunxi.c.
 * Native ownership/deadline integration is described in apps/rtc/sources.json. */
#include "a733_rtc.h"
#include <stddef.h>
#define DAY 0x10u
#define HMS 0x14u
#define ALDAY 0x20u
#define ALHMS 0x24u
#define ENABLE 0x28u
#define IRQEN 0x2cu
#define STATUS 0x30u
#define HMS_MASK UINT32_C(0x001f3f3f)
static int timely(struct a733_rtc *r)
{uint64_t n;return r->ops.now_us(r->context,&n)==1&&n>=r->last&&n<r->deadline?(r->last=n,1):0;}
static int read_reg(struct a733_rtc *r,uint32_t o,uint32_t *v)
{return timely(r)&&r->ops.read(r->context,o,v)==1&&timely(r);}
static int write_reg(struct a733_rtc *r,uint32_t o,uint32_t v)
{return !r->read_only&&r->ops.write&&timely(r)&&r->ops.write(r->context,o,v)==1&&timely(r);}
static int fail(struct a733_rtc *r){r->state=A733_RTC_RETAINED;return 0;}
/* Manual9.1.7.1 pp391-393: busy8/7, source1/0, crystal gate4;
 * stable auto-switch15/14 and oscillator gain3/2.9.1.7.2 status0=external. */
static int clock_ok(struct a733_rtc *r)
{
    uint32_t control,status,prescaler=0,calibration=0,oscillator=0;
    if(!read_reg(r,0,&control)||!read_reg(r,4,&status))return 0;
    unsigned source=A733_RTC_READ_EXTERNAL;
    if((control&0x193)!=0x11||(status&7)!=1){
        /* Manual9.1.7.1-.4: RTC_SRC=LOSC, LOSC_SRC=RC. Normal RC
         * divides by32 and (PRESCAL[4:0]+1); do not assert a 32768Hz rate.
         * +0x160 bit0 is RC enable. These registers share the admitted RTC
         * aperture; the caller must establish bus accessibility first. */
        if(!r->read_only||(control&0x183)||(status&7)||
           !read_reg(r,0x0c,&calibration)||(calibration&1)||
           !read_reg(r,0x08,&prescaler)||!read_reg(r,0x160,&oscillator)||!(oscillator&1))return 0;
        source=A733_RTC_READ_RC_NORMAL;
    }
    if((r->state!=A733_RTC_EMPTY||(r->read_only&&r->read_source))&&
       (((control^r->snapshot.clock_control)&0xc01f)||
        (r->read_only&&(source!=r->read_source||
         ((prescaler^r->prescaler)&31)||((calibration^r->calibration)&1)||
         ((oscillator^r->oscillator)&1)))))return 0;
    r->snapshot.clock_control=control;r->snapshot.clock_status=status;
    r->read_source=source;r->prescaler=prescaler;r->calibration=calibration;r->oscillator=oscillator;
    return 1;
}
static unsigned leap(unsigned y){return y%4==0&&(y%100!=0||y%400==0);}
static int decode(uint32_t day,uint32_t hms,struct a733_rtc_time *out)
{
    static const unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned h=(hms>>16)&31,m=(hms>>8)&63,s=hms&63;
    if(!day||day>65535||h>23||m>59||s>59)return 0;
    *out=(struct a733_rtc_time){.day=day,.hour=h,.minute=m,.second=s};
    unsigned remaining=day,y=1970,month=1;
    while(remaining>365+leap(y)){remaining-=365+leap(y);++y;}
    while(remaining>days[month-1]+(month==2&&leap(y))){remaining-=days[month-1]+(month==2&&leap(y));++month;}
    out->year=y;out->month=month;out->date=remaining;return 1;
}
static uint64_t seconds(const struct a733_rtc_time *t)
{return (uint64_t)t->day*86400+t->hour*3600+t->minute*60+t->second;}
static int coherent(struct a733_rtc *r,struct a733_rtc_time *out)
{
    for(unsigned attempt=0;attempt<8;++attempt){
        uint32_t d,h,d2,h2;
        if(!clock_ok(r)||!read_reg(r,DAY,&d)||!read_reg(r,HMS,&h)||
           !read_reg(r,DAY,&d2)||!read_reg(r,HMS,&h2))return 0;
        if(d==d2&&h==h2)return decode(d&65535,h,out)&&clock_ok(r);
    }
    return 0;
}
static int open_rtc(struct a733_rtc *r,const struct a733_rtc_ops *ops,void *context,uint64_t deadline,unsigned read_only)
{
    if(!r||r->state!=A733_RTC_EMPTY||!ops||!ops->now_us||!ops->read||(!read_only&&!ops->write)||!deadline)return 0;
    r->ops=*ops;r->read_only=read_only;if(read_only)r->ops.write=NULL;
    r->read_source=0;r->prescaler=0;r->calibration=0;r->oscillator=0;
    r->context=context;r->deadline=deadline;
    atomic_init(&r->owned,0);atomic_init(&r->irq_seen,0);atomic_init(&r->irq_fault,0);
    uint32_t pending,output;
    if(!clock_ok(r)||!read_reg(r,ENABLE,&r->saved_enable)||!read_reg(r,IRQEN,&r->saved_irq_enable)||
       !read_reg(r,STATUS,&pending)||!read_reg(r,0x50,&output)||
       ((r->saved_enable|r->saved_irq_enable|pending|output)&1)||
       !read_reg(r,ALDAY,&r->snapshot.saved_day)||!read_reg(r,ALHMS,&r->snapshot.saved_hms)||
       !coherent(r,&r->snapshot.initial))return fail(r);
    r->state=A733_RTC_READY;return 1;
}
int a733_rtc_open(struct a733_rtc *r,const struct a733_rtc_ops *ops,void *context,uint64_t deadline)
{return open_rtc(r,ops,context,deadline,0);}
int a733_rtc_open_readonly(struct a733_rtc *r,const struct a733_rtc_ops *ops,void *context,uint64_t deadline)
{return open_rtc(r,ops,context,deadline,1);}
int a733_rtc_read(struct a733_rtc *r,struct a733_rtc_time *out)
{
    if(!r||!out||(r->state!=A733_RTC_READY&&r->state!=A733_RTC_ARMED))return 0;
    return coherent(r,out)?1:fail(r);
}
int a733_rtc_arm(struct a733_rtc *r,unsigned delay)
{
    if(!r||r->read_only||r->state!=A733_RTC_READY||delay<1||delay>10)return 0;
    struct a733_rtc_time now,latest;
    if(!coherent(r,&now))return fail(r);
    uint32_t rd,rh,pending,en,irq,output;
    /* A one-second delay can elapse while the target is written. The alarm is
     * still disabled and its registers hold only this owner's values, so
     * retarget once from the later read. A longer delay reached during this
     * short sequence, or a second miss, means the sequence stalled; the RTC
     * is then retained. */
    for(unsigned attempt=0;;++attempt){
        uint64_t target=seconds(&now)+delay;
        if(target/86400>65535)return fail(r);
        uint32_t d=(uint32_t)(target/86400),s=(uint32_t)(target%86400);
        uint32_t h=((s/3600)<<16)|(((s/60)%60)<<8)|(s%60);
        r->snapshot.target_day=d;r->snapshot.target_hms=h;
        if(!write_reg(r,ALDAY,(r->snapshot.saved_day&~UINT32_C(65535))|d)||
           !write_reg(r,ALHMS,(r->snapshot.saved_hms&~HMS_MASK)|h)||
           !read_reg(r,ALDAY,&rd)||!read_reg(r,ALHMS,&rh)||
           (rd&65535)!=d||(rh&HMS_MASK)!=h||!coherent(r,&latest)||
           !read_reg(r,STATUS,&pending)||!read_reg(r,ENABLE,&en)||
           !read_reg(r,IRQEN,&irq)||!read_reg(r,0x50,&output)||((pending|en|irq|output)&1)||
           !coherent(r,&latest))return fail(r);
        if(seconds(&latest)<target)break;
        if(attempt||delay!=1)return fail(r);
        now=latest;
    }
    atomic_store_explicit(&r->owned,1,memory_order_release);
    if(!write_reg(r,ENABLE,r->saved_enable|1)||!write_reg(r,IRQEN,r->saved_irq_enable|1)||
       !read_reg(r,ENABLE,&en)||!read_reg(r,IRQEN,&irq)||!(en&irq&1))return fail(r);
    r->state=A733_RTC_ARMED;return 1;
}
int a733_rtc_irq(struct a733_rtc *r)
{
    uint32_t pending;
    if(!r||r->read_only||!atomic_load_explicit(&r->owned,memory_order_acquire))return 0;
    if(!read_reg(r,STATUS,&pending)||!(pending&1)||
       !write_reg(r,IRQEN,r->saved_irq_enable&~UINT32_C(1))||!write_reg(r,STATUS,1)){
        atomic_store_explicit(&r->irq_fault,1,memory_order_release);return 0;
    }
    atomic_store_explicit(&r->irq_seen,1,memory_order_release);return 1;
}
int a733_rtc_finish(struct a733_rtc *r,int cancel)
{
    if(!r||r->read_only)return 0;
    if(r->state!=A733_RTC_ARMED||atomic_load_explicit(&r->irq_fault,memory_order_acquire))return fail(r);
    unsigned seen=atomic_load_explicit(&r->irq_seen,memory_order_acquire);
    if(!cancel&&!seen)return 0;
    uint32_t pending,en,irq,d,h;
    if(!write_reg(r,IRQEN,r->saved_irq_enable&~UINT32_C(1))||
       !write_reg(r,ENABLE,r->saved_enable&~UINT32_C(1))||!read_reg(r,STATUS,&pending))return fail(r);
    if(pending&1){if(!write_reg(r,STATUS,1))return fail(r);seen=1;}
    if(!read_reg(r,STATUS,&pending)||!read_reg(r,ENABLE,&en)||!read_reg(r,IRQEN,&irq)||
       ((pending|en|irq)&1)||!coherent(r,&r->snapshot.terminal)||
       seconds(&r->snapshot.terminal)<seconds(&r->snapshot.initial)||
       (seen&&seconds(&r->snapshot.terminal)<(uint64_t)r->snapshot.target_day*86400+
        ((r->snapshot.target_hms>>16)&31)*3600+((r->snapshot.target_hms>>8)&63)*60+
        (r->snapshot.target_hms&63)))return fail(r);
    if(!write_reg(r,ALDAY,r->snapshot.saved_day)||!write_reg(r,ALHMS,r->snapshot.saved_hms)||
       !read_reg(r,ALDAY,&d)||!read_reg(r,ALHMS,&h)||d!=r->snapshot.saved_day||h!=r->snapshot.saved_hms)return fail(r);
    r->snapshot.alarm_seen=seen;r->snapshot.cancelled=!seen;
    r->state=seen?A733_RTC_FIRED:A733_RTC_CANCELLED;return 1;
}
