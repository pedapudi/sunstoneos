#include "a733_rtc.h"
#include "a733_rtc_boot.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct model {uint32_t regs[32];uint64_t now;unsigned writes,acks,reads,roll,late;};
static int now(void *p,uint64_t *n){struct model *m=p;*n=++m->now;return 1;}
static int rd(void *p,uint32_t o,uint32_t *v)
{
 struct model *m=p;assert(o<=0x50&&!(o&3));++m->reads;
 if(m->roll&&o==0x14){m->regs[4]=2;m->regs[5]=0;m->roll=0;}
 /* late 1: one second passes during the first target write; 2: it keeps passing. */
 if(m->late==1&&o==0x14&&m->writes>=2)m->regs[5]=2;
 if(m->late==2&&o==0x14)m->regs[5]=m->writes;
 *v=m->regs[o/4];return 1;
}
static int wr(void *p,uint32_t o,uint32_t v)
{
 struct model *m=p;assert(o==0x20||o==0x24||o==0x28||o==0x2c||o==0x30);++m->writes;
 if(o==0x30){assert(v==1);++m->acks;m->regs[o/4]&=~v;}else m->regs[o/4]=v;
 return 1;
}
static struct model base(void)
{struct model m={0};m.regs[0]=0x4011;m.regs[1]=1;m.regs[4]=1;m.regs[8]=0xa5000020;m.regs[9]=0x80000000;return m;}
static void opened(struct model *m,struct a733_rtc *r)
{struct a733_rtc_ops ops={now,rd,wr};assert(a733_rtc_open(r,&ops,m,1000000));}
struct bootmodel {uint32_t gic[0x6800/4],rtc[0x54/4],bus;unsigned writes,fail;};
static uint32_t *word(struct bootmodel *m,uintptr_t p)
{
 if(p==0x0701020c)return &m->bus;
 if(p>=0x03400000&&p<0x03406800)return &m->gic[(p-0x03400000)/4];
 assert(p>=0x07090000&&p<=0x07090050);return &m->rtc[(p-0x07090000)/4];
}
static int brd(void *p,uintptr_t a,uint32_t *v){*v=*word(p,a);return 1;}
static int bwr(void *p,uintptr_t a,uint32_t v)
{struct bootmodel *m=p;++m->writes;if(m->fail&&m->writes==m->fail)return 0;*word(m,a)=v;return 1;}
static int route(void *p,uintptr_t a,uint64_t v)
{return bwr(p,a,(uint32_t)v)&&bwr(p,a+4,(uint32_t)(v>>32));}
int main(void)
{
 struct model m=base();struct a733_rtc r={0};opened(&m,&r);
 assert(r.snapshot.initial.year==1970&&r.snapshot.initial.month==1&&r.snapshot.initial.date==1);
 assert(a733_rtc_arm(&r,2));m.regs[5]=2;m.regs[12]=1;
 assert(a733_rtc_irq(&r)&&m.acks==1&&a733_rtc_finish(&r,0));
 assert(r.state==A733_RTC_FIRED&&r.snapshot.alarm_seen&&!r.snapshot.cancelled);
 assert(m.regs[8]==0xa5000020&&m.regs[9]==0x80000000&&m.regs[0]==0x4011);
 m=base();r=(struct a733_rtc){0};m.regs[4]=1;m.regs[5]=0x173b3b;m.roll=1;opened(&m,&r);
 assert(r.snapshot.initial.day==2&&!r.snapshot.initial.hour);
 m=base();r=(struct a733_rtc){0};m.regs[4]=11017;opened(&m,&r);
 assert(r.snapshot.initial.year==2000&&r.snapshot.initial.month==2&&r.snapshot.initial.date==29);
 m=base();r=(struct a733_rtc){0};m.regs[4]=65535;m.regs[5]=0x173b3b;opened(&m,&r);
 assert(!a733_rtc_arm(&r,1)&&r.state==A733_RTC_RETAINED&&!m.writes);
 const unsigned inherited[]={0x28,0x2c,0x30,0x50};
 for(unsigned i=0;i<4;++i){m=base();r=(struct a733_rtc){0};m.regs[inherited[i]/4]=1;
  struct a733_rtc_ops ops={now,rd,wr};assert(!a733_rtc_open(&r,&ops,&m,1000000)&&!m.writes);}
 m=base();r=(struct a733_rtc){0};m.regs[4]=0;struct a733_rtc_ops ops={now,rd,wr};assert(!a733_rtc_open(&r,&ops,&m,1000000));
 m=base();r=(struct a733_rtc){0};opened(&m,&r);m.late=1;assert(a733_rtc_arm(&r,1)&&r.state==A733_RTC_ARMED&&m.writes==6&&
  r.snapshot.target_day==1&&r.snapshot.target_hms==3&&(m.regs[9]&0x001f3f3f)==3&&(m.regs[10]&1));
 m=base();r=(struct a733_rtc){0};opened(&m,&r);m.late=2;assert(!a733_rtc_arm(&r,1)&&r.state==A733_RTC_RETAINED&&m.writes==4&&!(m.regs[10]&1));
 m=base();r=(struct a733_rtc){0};opened(&m,&r);assert(a733_rtc_arm(&r,2));assert(!a733_rtc_irq(&r)&&!m.acks);
 m=base();r=(struct a733_rtc){0};opened(&m,&r);assert(a733_rtc_arm(&r,2));assert(a733_rtc_finish(&r,1)&&r.state==A733_RTC_CANCELLED&&!m.acks);
 m=base();r=(struct a733_rtc){0};opened(&m,&r);assert(a733_rtc_arm(&r,2));m.regs[5]=2;m.regs[12]=1;
 assert(a733_rtc_finish(&r,1)&&r.state==A733_RTC_FIRED&&m.acks==1);
 m=base();r=(struct a733_rtc){0};opened(&m,&r);assert(a733_rtc_arm(&r,2));m.regs[12]=1;
 assert(a733_rtc_irq(&r)&&!a733_rtc_finish(&r,0)); /* early event is not completion */
 m=base();r=(struct a733_rtc){0};opened(&m,&r);m.regs[0]^=4;assert(!a733_rtc_arm(&r,2)&&!m.writes);
 struct a733_rtc_boot_ops bops={brd,bwr,route};
 struct bootmodel b={0};struct a733_rtc_boot boot={0};b.bus=0xa5000000;
 assert(a733_rtc_boot_bus(&boot,&bops,&b)&&b.bus==0xa5010001);b.gic[0]=0x33;
 assert(a733_rtc_boot_route(&boot,0)&&boot.state==2&&b.gic[0x9c/4]==16&&b.gic[0x4e4/4]==0xe0);
 for(unsigned off=0x11c;off<=0x31c;off+=0x100){memset(&b,0,sizeof(b));boot=(struct a733_rtc_boot){0};b.gic[off/4]=16;
  assert(!a733_rtc_boot_bus(&boot,&bops,&b)&&!b.writes);}
 memset(&b,0,sizeof(b));boot=(struct a733_rtc_boot){0};b.rtc[0x30/4]=1;
 assert(!a733_rtc_boot_bus(&boot,&bops,&b)&&b.writes==1&&b.rtc[0x30/4]==1&&b.bus==0x10001);
 memset(&b,0,sizeof(b));boot=(struct a733_rtc_boot){0};b.fail=1;
 assert(!a733_rtc_boot_bus(&boot,&bops,&b)&&boot.state==3&&b.writes==1);
 memset(&b,0,sizeof(b));boot=(struct a733_rtc_boot){0};assert(a733_rtc_boot_bus(&boot,&bops,&b));b.gic[0]=0x33;b.fail=3;
 assert(!a733_rtc_boot_route(&boot,0)&&boot.state==3&&b.bus==0x10001);
 puts("PASS: RTC TypeA read/alarm/retained failure and primary boot ownership");return 0;
}
