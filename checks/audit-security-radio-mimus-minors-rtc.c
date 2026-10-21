/* Host check for the RTC arm retarget rule: only a one-second request may be
 * retargeted once; a longer request whose target is reached while the target
 * is written retains the RTC with the alarm still disabled.
 * Build: clang -std=c11 -Iinclude -fsanitize=address,undefined this.c
 *        drivers/rtc/a733_rtc.c */
#include "a733_rtc.h"
#include <assert.h>
#include <stdio.h>
struct model {uint32_t regs[32];uint64_t now;unsigned writes,late;};
static int now(void *p,uint64_t *n){struct model *m=p;*n=++m->now;return 1;}
static int rd(void *p,uint32_t o,uint32_t *v)
{
 struct model *m=p;assert(o<=0x50&&!(o&3));
 /* Once the first target is written, the calendar shows m->late seconds. */
 if(m->late&&o==0x14&&m->writes>=2)m->regs[5]=m->late;
 *v=m->regs[o/4];return 1;
}
static int wr(void *p,uint32_t o,uint32_t v)
{struct model *m=p;assert(o==0x20||o==0x24||o==0x28||o==0x2c);++m->writes;m->regs[o/4]=v;return 1;}
static void arm(unsigned delay,unsigned late,unsigned target)
{
 struct model m={0};m.regs[0]=0x4011;m.regs[1]=1;m.regs[4]=1;m.regs[8]=0xa5000020;m.regs[9]=0x80000000;m.late=late;
 struct a733_rtc r={0};struct a733_rtc_ops ops={now,rd,wr};assert(a733_rtc_open(&r,&ops,&m,1000000));
 if(target){
  /* Two writes per target, two enable writes. */
  assert(a733_rtc_arm(&r,delay)&&r.state==A733_RTC_ARMED&&(m.regs[10]&1)&&m.writes==(target==delay?4u:6u));
  assert(r.snapshot.target_hms==target&&(m.regs[9]&0x001f3f3f)==target);
 }else{
  assert(!a733_rtc_arm(&r,delay)&&r.state==A733_RTC_RETAINED&&m.writes==2&&!(m.regs[10]&1)&&!(m.regs[11]&1));
 }
}
int main(void)
{
 arm(1,1,2);arm(1,2,3);arm(2,1,2);arm(10,9,10);
 arm(2,2,0);arm(3,5,0);arm(10,10,0);
 puts("rtc retarget: PASS");return 0;
}
