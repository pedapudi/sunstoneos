/* The private sequencer helper is exercised with an independent 24MHz clock.
 * Its MMIO read can consume time; its pause stops exactly at a floor-us target. */
#include "../drivers/rtc/a733_rtc_initialize.c"
#include <assert.h>
#include <stdio.h>
struct model {uint64_t ticks,clear_ticks;unsigned cost,freeze,pauses,reads;};
static int clock_now(void *v,uint64_t *out)
{struct model *m=v;*out=m->ticks/24;return 1;}
static int control_read(void *v,unsigned offset,uint32_t *out)
{struct model *m=v;assert(!offset);++m->reads;m->ticks+=m->cost*24;m->clear_ticks=m->ticks;*out=0;return 1;}
static int pause_at(void *v,uint64_t target)
{struct model *m=v;++m->pauses;if(!m->freeze)m->ticks=target*24;return 1;}
int main(void)
{
 const struct a733_rtc_ops ops={.now_us=clock_now,.read=control_read};
 for(unsigned cost=0;cost<2;++cost){
  struct model m={.ticks=23,.cost=cost?17:0};struct a733_rtc_initialization out={0};
  struct initialization c={.ops=&ops,.context=&m,.deadline=10000,.out=&out,.pause=pause_at};
  assert(wait_clear(&c,256));
  assert(m.ticks-m.clear_ticks>=2000*24&&m.reads==1&&m.pauses==1);
 }
 struct model m={.ticks=23,.freeze=1};struct a733_rtc_initialization out={0};
 struct initialization c={.ops=&ops,.context=&m,.deadline=10000,.out=&out,.pause=pause_at};
 assert(!wait_clear(&c,256)&&c.stalled==4096&&m.pauses<4096&&m.reads==1);
 m=(struct model){.ticks=23};c=(struct initialization){.ops=&ops,.context=&m,.deadline=2001,.out=&out,.pause=pause_at};
 assert(!wait_clear(&c,256)&&!m.pauses);
 puts("PASS RTC minimum settle: fractional origin, costly status read, stopped clock and deadline");
}
