/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (c) 2020, Martin <wuyan@allwinnertech.com>
 * Calendar ordering/settling adapted from pinned rtc-sunxi.c:settime.
 * Source mux/gate fields follow ccu-sun60iw2-rtc.c and manual9.1.7 tables. */
#include "a733_rtc_application.h"
#include "a733_range.h"
#include <stddef.h>
struct initialization {const struct a733_rtc_ops *ops;void *context;uint64_t deadline,last,start;struct a733_rtc_initialization *out;
 int (*pause)(void *,uint64_t);int (*cancelled)(void *);uint32_t cancel_seen;};
static int now(struct initialization *c,uint64_t *n)
{
 if(c->ops->now_us(c->context,n)!=1||!a733_deadline_admit(*n,&c->last,c->deadline))return 0;
 if(c->cancelled&&c->cancelled(c->context)){
  c->cancel_seen=1;if(!c->out->writes)return 0;
 }
 return 1;
}
static int read32(struct initialization *c,unsigned offset,uint32_t *v)
{uint64_t n;return now(c,&n)&&c->ops->read(c->context,offset,v)==1&&now(c,&n);}
static int write32(struct initialization *c,unsigned offset,uint32_t v)
{
 uint64_t n;if(!now(c,&n))return 0;++c->out->writes;
 int result=c->ops->write(c->context,offset,v);
 if(result==2&&c->cancelled&&c->out->writes==1&&offset==0x14){
  c->out->writes=0;c->cancel_seen=1;return 0;
 }
 return result==1&&now(c,&n);
}
static int delay(struct initialization *c,uint64_t us)
{
 uint64_t n;if(!now(c,&n)||us>=c->deadline-n)return 0;uint64_t end=n+us;
 do {if(c->pause&&c->pause(c->context,end)!=1)return 0;if(!now(c,&n))return 0;}while(n<end);
 return 1;
}
static int wait_clear(struct initialization *c,uint32_t mask)
{
 uint64_t n;if(!now(c,&n))return 0;uint64_t end=c->deadline-n>1000000?n+1000000:c->deadline;
 do {uint32_t v;if(!read32(c,0,&v))return 0;
  if(!(v&mask))return delay(c,2000);
  if(c->pause&&c->pause(c->context,n+1000)!=1)return 0;
  if(!now(c,&n))return 0;
 }while(n<end);
 return 0;
}
static int quiet(struct initialization *c)
{
 const unsigned offsets[]={0x28,0x2c,0x30,0x50};uint32_t v;
 for(unsigned i=0;i<4;++i)if(!read32(c,offsets[i],&v)||(v&1))return 0;
 return 1;
}
static int control(struct initialization *c,uint32_t mask,uint32_t value)
{
 uint32_t v,check;
 if(!read32(c,0,&v))return 0;
 uint32_t low=((v&65535)&~mask)|value;
 if(!write32(c,0,UINT32_C(0x16aa0000)|low)||!read32(c,0,&check))return 0;
 return (check&UINT32_C(0xc01f))==(low&UINT32_C(0xc01f));
}
static int source(struct initialization *c)
{
 /* Manual9.1.7.1/.2: select osc32k via bit1=0, external via bit0=1.
  * Vendor probe polls statusbit4; manual defines healthbit2 (clear=healthy).
  * Do not copy its unrelated DCXO enable or fanout mux writes. */
 if(!control(c,UINT32_C(0xc000),UINT32_C(0x4000))||!control(c,16,16))return 0;
 uint64_t n;if(!now(c,&n))return 0;uint64_t end=c->deadline-n>1000000?n+1000000:c->deadline;
 unsigned healthy=0;
 do {uint32_t status;if(!read32(c,4,&status))return 0;
  healthy=(status&4)?0:healthy+1;
  if(healthy==4)break;
  if(!delay(c,3)||!now(c,&n))return 0;
 }while(n<end);
 if(healthy!=4||!control(c,3,1))return 0;
 /* Clear only the source-switch pending bit under explicit source ownership. */
 uint32_t status;
 if(!read32(c,4,&status))return 0;
 if((status&2)&&!write32(c,4,2))return 0;
 if(!now(c,&n)||n>=end)return 0;
 do {uint32_t v;
  if(!read32(c,0,&v)||!read32(c,4,&status))return 0;
  if((v&0x193)==0x11&&(status&7)==1)return 1;
  if(!now(c,&n))return 0;
 }while(n<end);
 return 0;
}
static uint64_t absolute(uint32_t day,uint32_t hms)
{return (uint64_t)day*86400+((hms>>16)&31)*3600+((hms>>8)&63)*60+(hms&63);}
int a733_rtc_setting_valid(const struct a733_rtc_setting *s)
{
 if(!s||!s->flags||(s->flags&~7u)||s->reserved)return 0;
 if((s->flags&6)==6)return 0;
 if(!(s->flags&(A733_RTC_SET_CALENDAR|A733_RTC_RUNTIME_CALENDAR)))return !s->day&&!s->hms;
 return s->day&&s->day<=65535&&!(s->hms&~UINT32_C(0x1f3f3f))&&
        ((s->hms>>16)&31)<24&&((s->hms>>8)&63)<60&&(s->hms&63)<60;
}
static int complete(struct initialization *c,int result,uint32_t *cancel_seen)
{
 if(c->last>=c->start)c->out->elapsed_us=(uint32_t)(c->last-c->start);
 if(cancel_seen)*cancel_seen=c->cancel_seen;return result;
}
static int initialize(const struct a733_rtc_setting *s,const struct a733_rtc_ops *ops,
 void *context,uint64_t deadline,struct a733_rtc_initialization *out,
 int (*pause)(void *,uint64_t),int (*cancelled)(void *),uint32_t *cancel_seen)
{
 if(!a733_rtc_setting_valid(s)||!ops||!ops->now_us||!ops->read||!ops->write||!out||out->stage)return 0;
 struct initialization c={.ops=ops,.context=context,.deadline=deadline,.out=out,.pause=pause,.cancelled=cancelled};
 out->stage=1;out->requested=s->flags;
 if(!now(&c,&c.start)||!quiet(&c)||!read32(&c,0,&out->before_control)||
    !read32(&c,4,&out->before_status)||!read32(&c,0x10,&out->before_day)||
    !read32(&c,0x14,&out->before_hms))return complete(&c,0,cancel_seen);
 out->stage=2;
 if((s->flags&A733_RTC_INIT_SOURCE)&&!source(&c))return complete(&c,0,cancel_seen);
 uint64_t written=0;
 if(s->flags&A733_RTC_SET_CALENDAR){
  /* Manual allows1s refresh; pinned driver requires2ms after each clear.
   * Preserve reserved fields and never rewrite the requested date on rollover. */
  uint32_t v,status;
  if(!read32(&c,0,&v)||!read32(&c,4,&status)||(v&0x13)!=0x11||(status&7)!=1||!quiet(&c))return complete(&c,0,cancel_seen);
  out->stage=3;
  if(!wait_clear(&c,256)||!read32(&c,0x14,&v)||!now(&c,&written)||
     !write32(&c,0x14,(v&~UINT32_C(0x1f3f3f))|s->hms)||!wait_clear(&c,256))return complete(&c,0,cancel_seen);
  out->stage=4;
  if(!wait_clear(&c,128)||!read32(&c,0x10,&v)||
     !write32(&c,0x10,(v&~UINT32_C(65535))|s->day)||!wait_clear(&c,128))return complete(&c,0,cancel_seen);
 }
 out->stage=5;
 struct a733_rtc adopted={0};
 if((!(s->flags&A733_RTC_RUNTIME_CALENDAR)&&!a733_rtc_open(&adopted,ops,context,deadline))||!quiet(&c)||
    !read32(&c,0,&out->after_control)||!read32(&c,4,&out->after_status)||
    !read32(&c,0x10,&out->after_day)||!read32(&c,0x14,&out->after_hms))return complete(&c,0,cancel_seen);
 if((out->after_control&0x13)!=0x11||(out->after_status&7)!=1)return complete(&c,0,cancel_seen);
 uint64_t n;if(!now(&c,&n))return complete(&c,0,cancel_seen);
 if(s->flags&A733_RTC_SET_CALENDAR){
  struct a733_rtc_time *t=&adopted.snapshot.initial;
  uint32_t hms=(t->hour<<16)|(t->minute<<8)|t->second;
  uint64_t actual=absolute(t->day,hms),requested=absolute(s->day,s->hms);
  /* Lost midnight carry yields a timestamp before requested and is refused. */
  if(actual<requested||actual-requested>(n-written+999999)/1000000)return complete(&c,0,cancel_seen);
  /* Publish the verified coherent pair, not the later diagnostic reads which
   * can straddle midnight. Reserved fields remain preserved separately. */
  out->after_day=(out->after_day&~UINT32_C(65535))|t->day;
  out->after_hms=(out->after_hms&~UINT32_C(0x1f3f3f))|hms;
 }
 if((out->before_control^out->after_control)&(cancelled?UINT32_C(0xc01f):12))return complete(&c,0,cancel_seen);
 out->elapsed_us=(uint32_t)(n-c.start);out->stage=6;return complete(&c,1,cancel_seen);
}

int a733_rtc_initialize(const struct a733_rtc_setting *s,const struct a733_rtc_ops *ops,
 void *context,uint64_t deadline,struct a733_rtc_initialization *out)
{return initialize(s,ops,context,deadline,out,NULL,NULL,NULL);}
int a733_rtc_set_calendar(const struct a733_rtc_setting *s,const struct a733_rtc_ops *ops,
 void *context,uint64_t deadline,int (*pause)(void *,uint64_t),int (*cancelled)(void *),
 struct a733_rtc_calendar_result *out)
{
 if(!s||!(s->flags&A733_RTC_RUNTIME_CALENDAR)||!a733_rtc_setting_valid(s)||
    !pause||!cancelled||!out||out->outcome||out->operation.stage)return 0;
 struct a733_rtc_setting target=*s;target.flags=A733_RTC_SET_CALENDAR;
 int result=initialize(&target,ops,context,deadline,&out->operation,pause,cancelled,&out->cancel_seen);
 out->outcome=result?(out->cancel_seen?A733_RTC_CALENDAR_APPLIED_LATE_CANCEL:A733_RTC_CALENDAR_APPLIED):
     (out->cancel_seen&&!out->operation.writes?A733_RTC_CALENDAR_CANCELLED:A733_RTC_CALENDAR_RETAINED);
 return 1;
}
