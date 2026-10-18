#include "a733_rtc_application.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct model {uint32_t reg[0x320/4];uint64_t now,busy8,busy7;unsigned writes,source_writes,calendar_writes,fail,stuck,roll;};
static int now(void *p,uint64_t *v){struct model *m=p;m->now+=100;*v=m->now;return 1;}
static int read_reg(void *p,uint32_t o,uint32_t *v)
{
 struct model *m=p;assert(o<=0x50&&!(o&3));
 if(o==0){m->reg[0]&=~0x180u;if(m->now<m->busy8)m->reg[0]|=256;if(m->now<m->busy7)m->reg[0]|=128;}
 *v=m->reg[o/4];return 1;
}
static int write_reg(void *p,uint32_t o,uint32_t v)
{
 struct model *m=p;assert(o==0||o==4||o==0x10||o==0x14);++m->writes;
 if(m->fail&&m->writes==m->fail)return 0;
 if(o==0){assert((v>>16)==0x16aa);++m->source_writes;m->reg[0]=v&65535;
  m->reg[1]=(m->reg[1]&6)|((v&0x13)==0x11&&!m->stuck?1:0);return 1;}
 if(o==4){assert(v==2);m->reg[1]&=~2u;return 1;}
 ++m->calendar_writes;m->reg[o/4]=v;
 if(o==0x14)m->busy8=m->now+600000; /* Valid hardware slower than old50ms. */
 if(o==0x10){m->busy7=m->now+600000;
  if(m->roll){m->reg[5]=0;++m->reg[4];m->reg[4]=v;}}
 return 1;
}
static struct model base(void)
{struct model m={0};m.reg[0]=0x800c;m.reg[1]=2;m.reg[0x100/4]=0xaabbccdd;m.reg[0x160/4]=0x12343878;m.reg[0x20/4]=0xaaaa;return m;}
static int initialize(struct model *m,struct a733_rtc_setting *setting,struct a733_rtc_initialization *out)
{struct a733_rtc_ops ops={now,read_reg,write_reg};return a733_rtc_initialize(setting,&ops,m,6000000,out);}
int main(void)
{
 struct a733_rtc_setting s={3,20744,0x123438,0};struct a733_rtc_initialization out={0};struct model m=base();
 assert(initialize(&m,&s,&out)&&out.stage==6&&m.calendar_writes==2&&m.source_writes==3);
 assert((m.reg[0]&0xc01f)==0x401d&&m.reg[4]==s.day&&m.reg[5]==s.hms);
 assert(m.reg[0x100/4]==0xaabbccdd&&m.reg[0x160/4]==0x12343878&&m.reg[8]==0xaaaa);
 assert(out.elapsed_us>1200000&&out.elapsed_us<6000000&&out.before_day==0);
 m=base();out=(struct a733_rtc_initialization){0};m.reg[0]=0x4011;m.reg[1]=1;s=(struct a733_rtc_setting){2,11017,0x08090a,0};
 assert(initialize(&m,&s,&out)&&!m.source_writes&&m.calendar_writes==2);
 m=base();out=(struct a733_rtc_initialization){0};m.reg[4]=11017;m.reg[5]=0x08090a;s=(struct a733_rtc_setting){1,0,0,0};
 assert(initialize(&m,&s,&out)&&m.source_writes==3&&!m.calendar_writes);
 m=base();out=(struct a733_rtc_initialization){0};s=(struct a733_rtc_setting){3,20744,0x173b3b,0};m.roll=1;
 assert(!initialize(&m,&s,&out)&&out.stage==5&&m.calendar_writes==2); /* Lost midnight carry is retained. */
 for(unsigned flag=0;flag<4;++flag){
  m=base();out=(struct a733_rtc_initialization){0};s=(struct a733_rtc_setting){3,20744,1,0};m.reg[(0x28+flag*4)/4]=1;
  if(flag==3){m.reg[0x34/4]=0;m.reg[0x50/4]=1;}
  assert(!initialize(&m,&s,&out)&&!m.writes);
 }
 m=base();out=(struct a733_rtc_initialization){0};s=(struct a733_rtc_setting){3,20744,1,0};m.stuck=1;m.reg[1]=4;
 assert(!initialize(&m,&s,&out)&&out.stage==2&&!m.calendar_writes&&m.now>=1000000);
 m=base();out=(struct a733_rtc_initialization){0};m.fail=5;
 assert(!initialize(&m,&s,&out)&&out.stage==3&&m.calendar_writes==0); /* Source pending W1C is fourth write. */
 m=base();out=(struct a733_rtc_initialization){0};m.reg[0]=0x4011;m.reg[1]=1;m.busy8=2000000;s=(struct a733_rtc_setting){2,20744,1,0};
 assert(!initialize(&m,&s,&out)&&!m.calendar_writes&&out.stage==3);
 m=base();out=(struct a733_rtc_initialization){0};s=(struct a733_rtc_setting){3,0,1,0};assert(!initialize(&m,&s,&out)&&!m.writes);
 puts("PASS new RTC source/calendar initialization and retained refusal");
}
