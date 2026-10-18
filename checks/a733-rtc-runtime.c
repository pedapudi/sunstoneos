#include "a733_rtc_application.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct model {uint32_t reg[32];uint64_t now,busy8,busy7;unsigned held,reads,writes,pauses,cancel,cancel_at_write,late,fail_after_write,loss,stuck,midnight,cold_source,lose_after_pause,report_roll,day_reads;};
static int now(void *c,uint64_t *n){struct model *m=c;m->now+=100;*n=m->now;return !m->loss;}
static int rd(void *c,uint32_t o,uint32_t *v)
{
 struct model *m=c;assert(!m->held);m->held=1;++m->reads;assert(o<=0x50&&!(o&3));
 if(o==0){m->reg[0]&=~0x180u;if(m->now<m->busy8)m->reg[0]|=256;if(m->now<m->busy7)m->reg[0]|=128;}
 *v=m->reg[o/4];
 if(o==0x10&&++m->day_reads==5&&m->report_roll){++m->reg[4];m->reg[5]=0;}
 m->held=0;return !m->loss;
}
static int wr(void *c,uint32_t o,uint32_t v)
{
 struct model *m=c;assert(!m->held);m->held=1;
 if(o==0||o==4){assert(m->cold_source);if(o==0){assert(v>>16==0x16aa);m->reg[0]=v&65535;}else {assert(v==2);m->reg[1]&=~2u;}m->held=0;return 1;}
 assert(o==0x10||o==0x14);
 if(!m->writes&&m->cancel_at_write){m->cancel=1;m->held=0;return 2;}
 ++m->writes;m->reg[o/4]=v;
 if(o==0x14){m->busy8=m->now+600000;if(m->late)m->cancel=1;}
 else {m->busy7=m->now+(m->stuck?2000000:600000);if(m->midnight)m->reg[5]=0;}
 m->held=0;return !m->fail_after_write;
}
static int pause_until(void *c,uint64_t end)
{struct model *m=c;assert(!m->held);++m->pauses;m->now=end>m->now?end:m->now+1000;if(m->lose_after_pause&&m->writes)m->loss=1;return !m->loss;}
static int cancelled(void *c){return ((struct model *)c)->cancel;}
static struct model base(void)
{struct model m={0};m.reg[0]=0x401d;m.reg[1]=1;m.reg[8]=0xabcd;return m;}
static struct a733_rtc_calendar_result run(struct model *m,uint32_t hms,uint64_t deadline)
{
 struct a733_rtc_ops ops={now,rd,wr};struct a733_rtc_setting s={4,20744,hms,0};
 struct a733_rtc_calendar_result out={0};assert(a733_rtc_set_calendar(&s,&ops,m,deadline,pause_until,cancelled,&out));
 assert(!m->held&&m->reg[8]==0xabcd);return out;
}
int main(void)
{
 struct model m=base();struct a733_rtc_calendar_result r=run(&m,0x123438,10000000);
 assert(r.outcome==A733_RTC_CALENDAR_APPLIED&&r.operation.stage==6&&r.operation.writes==2&&m.writes==2&&m.pauses);
 assert(m.reg[4]==20744&&m.reg[5]==0x123438&&m.reg[0]==0x401d);
 m=base();m.report_roll=1;r=run(&m,0x173b3b,10000000);
 assert(r.outcome==A733_RTC_CALENDAR_APPLIED&&r.operation.after_day==20744&&r.operation.after_hms==0x173b3b&&m.reg[4]==20745&&!m.reg[5]);
 m=base();m.cancel=1;r=run(&m,1,10000000);assert(r.outcome==A733_RTC_CALENDAR_CANCELLED&&!r.operation.writes&&!m.writes);
 m=base();m.cancel_at_write=1;r=run(&m,1,10000000);assert(r.outcome==A733_RTC_CALENDAR_CANCELLED&&!r.operation.writes&&!m.writes);
 m=base();m.late=1;r=run(&m,1,10000000);assert(r.outcome==A733_RTC_CALENDAR_APPLIED_LATE_CANCEL&&r.operation.writes==2&&m.writes==2&&r.cancel_seen);
 m=base();m.fail_after_write=1;m.late=1;r=run(&m,1,10000000);assert(r.outcome==A733_RTC_CALENDAR_RETAINED&&r.operation.writes==1&&m.writes==1);
 m=base();m.lose_after_pause=1;r=run(&m,1,10000000);assert(r.outcome==A733_RTC_CALENDAR_RETAINED&&m.writes==1);
 m=base();m.midnight=1;r=run(&m,0x173b3b,10000000);assert(r.outcome==A733_RTC_CALENDAR_RETAINED&&m.writes==2);
 m=base();m.stuck=1;r=run(&m,1,10000000);assert(r.outcome==A733_RTC_CALENDAR_RETAINED&&r.operation.stage==4&&m.writes==2);
 m=base();r=run(&m,1,500000);assert(r.outcome==A733_RTC_CALENDAR_RETAINED&&m.writes==1);
 for(unsigned offset=0x28;offset<=0x30;offset+=4){m=base();m.reg[offset/4]=1;r=run(&m,1,10000000);assert(r.outcome==A733_RTC_CALENDAR_RETAINED&&!m.writes);}
 /* New boot branch verifies source while retaining an invalid calendar for task repair. */
 m=base();struct a733_rtc_ops ops={now,rd,wr};struct a733_rtc_initialization boot={0};struct a733_rtc_setting setting={4,20744,1,0};
 assert(a733_rtc_initialize(&setting,&ops,&m,6000000,&boot)&&boot.stage==6&&!m.writes&&!boot.after_day);
 m=base();m.report_roll=1;boot=(struct a733_rtc_initialization){0};setting=(struct a733_rtc_setting){2,20744,0x173b3b,0};
 assert(a733_rtc_initialize(&setting,&ops,&m,6000000,&boot)&&boot.after_day==20744&&boot.after_hms==0x173b3b&&m.reg[4]==20745);
 m=base();setting=(struct a733_rtc_setting){1,0,0,0};m.cold_source=1;boot=(struct a733_rtc_initialization){0};assert(!a733_rtc_initialize(&setting,&ops,&m,6000000,&boot)&&boot.stage==5);
 puts("PASS runtime RTC yielding writes/cancellation/partial retention and invalid-calendar boot handoff");
}
