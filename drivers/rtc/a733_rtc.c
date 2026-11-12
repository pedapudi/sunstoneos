/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (c) 2020, Martin <wuyan@allwinnertech.com>
 * TypeA calendar/read/alarm operations adapted from pinned rtc-sunxi.c.
 * Native ownership/deadline integration is described in apps/rtc/sources.json. */
#include "a733_rtc.h"
#include "a733_range.h"
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
{uint64_t n;return r->ops.now_us(r->context,&n)==1&&a733_deadline_admit(n,&r->last,r->deadline);}
static int timely_now(struct a733_rtc *r,uint64_t *n)
{return r->ops.now_us(r->context,n)==1&&a733_deadline_admit(*n,&r->last,r->deadline);}
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
        if(!(r->read_only||r->rc_mode)||(control&0x183)||(status&7)||
           !read_reg(r,0x0c,&calibration)||(calibration&1)||
           !read_reg(r,0x08,&prescaler)||!read_reg(r,0x160,&oscillator)||!(oscillator&1))return 0;
        source=A733_RTC_READ_RC_NORMAL;
    }
    const unsigned adopted=r->read_only||r->rc_mode;
    if((r->state!=A733_RTC_EMPTY||(adopted&&r->read_source))&&
       (((control^r->snapshot.clock_control)&0xc01f)||
        (adopted&&(source!=r->read_source||
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
static int open_rtc(struct a733_rtc *r,const struct a733_rtc_ops *ops,void *context,uint64_t deadline,unsigned read_only,unsigned rc)
{
    if(!r||r->state!=A733_RTC_EMPTY||!ops||!ops->now_us||!ops->read||(!read_only&&!ops->write)||!deadline)return 0;
    r->ops=*ops;r->read_only=read_only;r->rc_mode=rc;if(read_only)r->ops.write=NULL;
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
{return open_rtc(r,ops,context,deadline,0,0);}
int a733_rtc_open_rc(struct a733_rtc *r,const struct a733_rtc_ops *ops,void *context,uint64_t deadline)
{return open_rtc(r,ops,context,deadline,0,1);}
int a733_rtc_open_readonly(struct a733_rtc *r,const struct a733_rtc_ops *ops,void *context,uint64_t deadline)
{return open_rtc(r,ops,context,deadline,1,0);}
static int waiting_clear(struct a733_rtc *r,uint32_t mask,int (*pause)(void *,uint64_t))
{
    uint64_t n;
    if(!timely_now(r,&n))return 0;
    const uint64_t end=n+1000000;
    for(;;){
        uint32_t v;
        if(!read_reg(r,0,&v))return 0;
        if(!(v&mask)){
            if(!timely_now(r,&n)||!pause(r->context,n+2001))return 0;
            return 1;
        }
        if(!timely_now(r,&n)||n>=end||!pause(r->context,n+1000))return 0;
    }
}
int a733_rtc_set_time(struct a733_rtc *r,uint32_t day,uint32_t hms,int (*pause)(void *,uint64_t))
{
    if(!r||!pause||!r->rc_mode||r->state!=A733_RTC_READY||!day||day>65535||(hms&~UINT32_C(0x1f3f3f))||
       ((hms>>16)&31)>23||((hms>>8)&63)>59||(hms&63)>59)return 0;
    uint32_t v;uint64_t written,done;
    if(!waiting_clear(r,256,pause)||!read_reg(r,HMS,&v)||!timely_now(r,&written)||
       !write_reg(r,HMS,(v&~UINT32_C(0x1f3f3f))|hms)||!waiting_clear(r,256,pause)||
       !waiting_clear(r,128,pause)||!read_reg(r,DAY,&v)||
       !write_reg(r,DAY,(v&~UINT32_C(65535))|day)||!waiting_clear(r,128,pause))return fail(r);
    struct a733_rtc_time t;
    if(!coherent(r,&t)||!timely_now(r,&done))return fail(r);
    const uint64_t requested=(uint64_t)day*86400+((hms>>16)&31)*3600+((hms>>8)&63)*60+(hms&63);
    const uint64_t actual=seconds(&t);
    /* The RC runs slower than real time (about 1.1 s per RTC second), so the new calendar can be no
     * later than the request plus the elapsed time, and a lost carry would leave it before the request. */
    if(actual<requested||actual-requested>(done-written+999999)/1000000)return fail(r);
    r->snapshot.initial=t;
    return 1;
}
int a733_rtc_read(struct a733_rtc *r,struct a733_rtc_time *out)
{
    if(!r||!out||(r->state!=A733_RTC_READY&&r->state!=A733_RTC_ARMED&&r->state!=A733_RTC_FIRED&&r->state!=A733_RTC_CANCELLED))return 0;
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
    /* Deferred notifications can outlive the alarm that generated them. A
     * completed or retained instance must never acknowledge a later event.
     * All entry points run in the same owner task with the GIC line masked. */
    if(!r||r->read_only||r->state!=A733_RTC_ARMED||
       !atomic_load_explicit(&r->owned,memory_order_acquire)||
       atomic_load_explicit(&r->irq_seen,memory_order_acquire)||
       atomic_load_explicit(&r->irq_fault,memory_order_acquire))return 0;
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
    atomic_store_explicit(&r->owned,0,memory_order_release);
    r->state=seen?A733_RTC_FIRED:A733_RTC_CANCELLED;return 1;
}

/* --- external crystal selection for an RC-mode instance ------------------------------------------------------------ */
static int control_word(struct a733_rtc *r,uint32_t mask,uint32_t value,struct a733_rtc_switch *o)
{
    uint32_t v,check;
    if(!read_reg(r,0,&v))return 0;
    const uint32_t low=((v&65535)&~mask)|value;
    ++o->writes;
    if(!write_reg(r,0,UINT32_C(0x16aa0000)|low)||!read_reg(r,0,&check))return 0;
    return (check&UINT32_C(0xc01f))==(low&UINT32_C(0xc01f));
}
static int restore_control(struct a733_rtc *r,struct a733_rtc_switch *o)
{
    uint32_t v;
    return control_word(r,UINT32_C(0xffff),o->control_before&65535,o)&&read_reg(r,0,&v);
}
int a733_rtc_select_crystal(struct a733_rtc *r,int (*pause)(void *,uint64_t),struct a733_rtc_switch *o)
{
    if(!r||!pause||!o||!r->rc_mode||r->state!=A733_RTC_READY||r->read_source!=A733_RTC_READ_RC_NORMAL)return 0;
    *o=(struct a733_rtc_switch){0};
    uint64_t n;uint32_t c,s;
    if(!timely_now(r,&o->before_us)||!coherent(r,&o->before)||!read_reg(r,0,&o->control_before)||
       !read_reg(r,4,&o->status_before))return fail(r);
    /* The gate and the stable auto-switch field, as the manual's source sequence orders them. */
    if(!control_word(r,UINT32_C(0xc000),UINT32_C(0x4000),o)||!control_word(r,16,16,o))return fail(r);
    if(!timely_now(r,&n))return fail(r);
    const uint64_t end=n+1000000;
    unsigned run=0;
    do{
        if(!read_reg(r,4,&s))return fail(r);
        ++o->samples;
        run=(s&4)?0:run+1;
        if(run==4)break;
        if(!timely_now(r,&n)||!pause(r->context,n+3)||!timely_now(r,&n))return fail(r);
    }while(n<end);
    o->healthy_run=run;
    int switched=0;
    if(run==4&&control_word(r,3,1,o)){
        if(!read_reg(r,4,&s))return fail(r);
        if((s&2)&&(++o->writes,!write_reg(r,4,2)))return fail(r);
        if(!timely_now(r,&n))return fail(r);
        const uint64_t settle_end=n+1000000;
        do{
            if(!read_reg(r,0,&c)||!read_reg(r,4,&s))return fail(r);
            if((c&0x193)==0x11&&(s&7)==1){switched=1;break;}
            if(!timely_now(r,&n)||!pause(r->context,n+1000)||!timely_now(r,&n))return fail(r);
        }while(n<settle_end);
    }
    if(!switched){
        /* Fall back: the control word returns to its entry value, so the RC stays selected. */
        if(!restore_control(r,o)||!read_reg(r,0,&o->control_after)||!read_reg(r,4,&o->status_after)||
           ((o->control_after^o->control_before)&0xc01f))return fail(r);
        o->result=A733_RTC_SWITCH_FALLBACK;
        return 1;
    }
    if(!read_reg(r,0,&o->control_after)||!read_reg(r,4,&o->status_after))return fail(r);
    /* Adopt the external source as this instance's stable state. */
    r->snapshot.clock_control=o->control_after;r->snapshot.clock_status=o->status_after;
    r->read_source=A733_RTC_READ_EXTERNAL;r->prescaler=r->calibration=r->oscillator=0;
    if(!coherent(r,&o->after)||!timely_now(r,&o->after_us))return fail(r);
    r->snapshot.initial=o->after;
    o->result=A733_RTC_SWITCH_DONE;
    return 1;
}
