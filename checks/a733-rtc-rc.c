/* Host model of the RTC running from the inherited internal RC oscillator (control 0x401c, source status 0,
 * prescaler 0x0f, calibration clear, RC enabled in +0x160): the RC-mode open, the calendar set, and one alarm.
 * The model records every write offset: the clock-source registers (+0x00, +0x04, +0x08, +0x0c, +0x160) must
 * never be written. */
#include "a733_rtc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct model {uint32_t regs[0x180/4];uint64_t now;unsigned writes[0x180/4],order[16],norder,busy,dyn,crystal,sick;};
static void reset(struct model *m)
{
 memset(m,0,sizeof(*m));
 m->regs[0]=0x401c;m->regs[2]=0x0f;m->regs[0x10/4]=1;m->regs[0x14/4]=0x012d10;m->regs[0x160/4]=1;
}
static int now(void *p,uint64_t *n){struct model *m=p;*n=(m->now+=50);return 1;}
static int rd(void *p,uint32_t o,uint32_t *v)
{
 struct model *m=p;assert(o<0x180&&!(o&3));
 *v=m->regs[o/4];
 if(m->dyn&&o==4){
  /* Health: bit 2 set while the crystal is not oscillating; the source reads external once selected. */
  *v=(m->crystal?0:4)|((m->regs[0]&1)&&m->crystal?1:0);
  if(m->sick&&m->writes[0]>=2&&m->sick--)*v|=4;
 }
 if(o==0&&m->busy){*v|=256|128;--m->busy;}
 return 1;
}
static int wr(void *p,uint32_t o,uint32_t v)
{
 struct model *m=p;assert(o<0x180&&!(o&3));++m->writes[o/4];
 if(m->norder<16)m->order[m->norder++]=o;
 if(o==0x30)m->regs[o/4]&=~v;else if(o==0)m->regs[0]=v&0xffff;else m->regs[o/4]=v;
 if(o==0x14||o==0x10)m->busy=3;
 return 1;
}
static int pause(void *p,uint64_t until){struct model *m=p;if(m->now<until)m->now=until;return 1;}
static void no_source_writes(const struct model *m)
{
 assert(!m->writes[0]&&!m->writes[1]&&!m->writes[2]&&!m->writes[3]&&!m->writes[0x160/4]);
}
int main(void)
{
 struct model m;struct a733_rtc r={0};
 struct a733_rtc_ops ops={now,rd,wr};
 /* The external-source open still refuses the RC state; the RC-mode open accepts it. */
 reset(&m);
 assert(!a733_rtc_open(&r,&ops,&m,1000000000));
 memset(&r,0,sizeof(r));reset(&m);
 assert(a733_rtc_open_rc(&r,&ops,&m,1000000000)&&r.state==A733_RTC_READY&&r.read_source==A733_RTC_READ_RC_NORMAL);
 /* An uncalibrated, wrong or half-selected source is refused. */
 for(unsigned bad=0;bad<4;++bad){
  memset(&r,0,sizeof(r));reset(&m);
  if(bad==0)m.regs[0]=0x4011;            /* the external source selected: not the RC state */
  if(bad==1)m.regs[3]=1;                 /* calibration enabled */
  if(bad==2)m.regs[0x160/4]=0;           /* RC disabled */
  if(bad==3)m.regs[1]=1;                 /* source status not RC */
  if(bad==0){assert(a733_rtc_open_rc(&r,&ops,&m,1000000000)==0||r.read_source==A733_RTC_READ_EXTERNAL);}
  else assert(!a733_rtc_open_rc(&r,&ops,&m,1000000000));
 }
 /* Calendar set: HMS before DAY, with the busy waits, and no clock-source write. */
 memset(&r,0,sizeof(r));reset(&m);
 assert(a733_rtc_open_rc(&r,&ops,&m,1000000000));
 const uint32_t day=20769,hms=(4u<<16)|(5u<<8)|6u;
 assert(a733_rtc_set_time(&r,day,hms,pause));
 assert(m.regs[0x10/4]==day&&(m.regs[0x14/4]&0x1f3f3f)==hms);
 assert(m.norder==2&&m.order[0]==0x14&&m.order[1]==0x10);
 assert(r.snapshot.initial.day==day&&r.snapshot.initial.hour==4&&r.snapshot.initial.minute==5&&r.snapshot.initial.second==6);
 no_source_writes(&m);
 /* A clock-source change after the write is caught by the next read. */
 /* Invalid requests change nothing. */
 unsigned writes=m.norder;
 assert(!a733_rtc_set_time(&r,0,hms,pause)&&!a733_rtc_set_time(&r,day,(24u<<16),pause)&&!a733_rtc_set_time(&r,day,60,pause));
 assert(m.norder==writes);
 /* One alarm three seconds ahead: arm, interrupt, finish; the saved alarm words return. */
 assert(a733_rtc_arm(&r,3)&&r.state==A733_RTC_ARMED);
 assert(m.regs[0x28/4]&1&&m.regs[0x2c/4]&1&&m.regs[0x24/4]==((4u<<16)|(5u<<8)|9u));
 m.regs[0x30/4]=1;                       /* the alarm fires */
 assert(a733_rtc_irq(&r)&&!(m.regs[0x30/4]&1));
 m.regs[0x14/4]=(4u<<16)|(5u<<8)|10u;    /* a coherent read after the target */
 assert(a733_rtc_finish(&r,0)&&r.state==A733_RTC_FIRED&&r.snapshot.alarm_seen);
 assert(!(m.regs[0x28/4]&1)&&!(m.regs[0x2c/4]&1)&&m.regs[0x20/4]==0&&m.regs[0x24/4]==0);
 /* The calendar stays readable after the alarm has finished (the board run refused this read). */
 struct a733_rtc_time after;
 assert(a733_rtc_read(&r,&after)&&after.second==10);
 no_source_writes(&m);
 /* The source moving to the external oscillator while open retains the instance. */
 memset(&r,0,sizeof(r));reset(&m);
 assert(a733_rtc_open_rc(&r,&ops,&m,1000000000));
 m.regs[0]=0x4011;m.regs[1]=1;
 struct a733_rtc_time t;
 assert(!a733_rtc_read(&r,&t)&&r.state==A733_RTC_RETAINED);
 /* Crystal selection: continuity, the control word as the only source write, and adoption of the external state. */
 memset(&r,0,sizeof(r));reset(&m);m.dyn=1;m.crystal=1;
 assert(a733_rtc_open_rc(&r,&ops,&m,1000000000));
 struct a733_rtc_switch sw;
 assert(a733_rtc_select_crystal(&r,pause,&sw)&&sw.result==A733_RTC_SWITCH_DONE&&sw.healthy_run==4);
 assert((m.regs[0]&0x193)==0x11&&sw.control_before==0x401c&&sw.after.day==sw.before.day);
 assert(m.writes[0]&&!m.writes[2]&&!m.writes[3]&&!m.writes[0x160/4]&&!m.writes[0x10/4]&&!m.writes[0x14/4]);
 assert(r.read_source==A733_RTC_READ_EXTERNAL&&a733_rtc_read(&r,&t));   /* the instance accepts the new source */
 assert(a733_rtc_arm(&r,2)&&r.state==A733_RTC_ARMED);
 assert(!a733_rtc_select_crystal(&r,pause,&sw));                         /* not in the READY state */
 /* A sick crystal: the control word returns to its entry value and the RC stays selected. */
 memset(&r,0,sizeof(r));reset(&m);m.dyn=1;m.crystal=1;
 assert(a733_rtc_open_rc(&r,&ops,&m,1000000000));
 m.sick=1000000;   /* the crystal stays unhealthy once the sequence starts */
 assert(a733_rtc_select_crystal(&r,pause,&sw)&&sw.result==A733_RTC_SWITCH_FALLBACK&&sw.healthy_run<4&&sw.samples>100);
m.sick=0;   /* the entry control word is back, so the status reads as on entry */
 assert(m.regs[0]==0x401c&&r.read_source==A733_RTC_READ_RC_NORMAL&&a733_rtc_read(&r,&t));
 /* An instance not opened in RC mode cannot set the calendar. */
 memset(&r,0,sizeof(r));reset(&m);m.regs[0]=0x4011;m.regs[1]=1;
 assert(a733_rtc_open(&r,&ops,&m,1000000000)&&!a733_rtc_set_time(&r,day,hms,pause)&&!m.norder);
 puts("PASS rtc rc mode: open, calendar set, alarm, source refusals, no clock-source write");
 return 0;
}
