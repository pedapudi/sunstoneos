/* Audio sequencing with the real HDMI I/O adapter. Display admission and
 * authority are modeled prerequisites; this fixture does not establish them. */
#include "a733_hdmi_audio.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct model {uint64_t ticks,fifo_tick,cts_tick;unsigned cost,cts_writes,mc_writes,following,retained;uint8_t registers[0x5000];};
static int clock_now(void *v,uint64_t *out){*out=((struct model *)v)->ticks/24;return 1;}
static int validate(void *v,const struct a733_hdmi_request *q,const struct a733_hdmi_reservation *r,uint64_t deadline)
{(void)v;(void)q;(void)r;return deadline!=0;}
static int read8(void *v,uintptr_t a,uint8_t *out)
{struct model *m=v;assert(a>=A733_HDMI_BASE&&a<A733_HDMI_BASE+sizeof(m->registers));*out=m->registers[a-A733_HDMI_BASE];return 1;}
static int write8(void *v,uintptr_t a,uint8_t value)
{
 struct model *m=v;unsigned o=a-A733_HDMI_BASE;assert(o<sizeof(m->registers));
 if(o==0x3100&&!m->fifo_tick&&(value&0x80)){m->ticks+=m->cost*24;m->fifo_tick=m->ticks;}
 if(o==0x4002){assert(m->ticks-m->fifo_tick>=5*24);++m->mc_writes;}
 if(o==0x3205&&++m->cts_writes==2){m->ticks+=m->cost*24;m->cts_tick=m->ticks;}
 else if(m->cts_tick){assert(m->ticks-m->cts_tick>=5000*24);++m->following;}
 m->registers[o]=value;return 1;
}
static int barrier(void *v){(void)v;return 1;}
static int retain(void *v,const struct a733_hdmi_reservation *r){(void)r;++((struct model *)v)->retained;return 1;}
static void setup(struct model *m,struct a733_hdmi *h,struct a733_hdmi_audio *a,unsigned cost)
{
 memset(m,0,sizeof(*m));memset(h,0,sizeof(*h));memset(a,0,sizeof(*a));m->ticks=23;m->cost=cost;
 h->self=h;h->state=A733_HDMI_ACTIVE;h->io.self=&h->io;h->io.context=m;h->io.admitted=1;h->io.deadline=100000;
 h->io.ops=(struct a733_hdmi_ops){.now_us=clock_now,.validate=validate,.read8=read8,.write8=write8,.barrier=barrier,.retain=retain};
 h->edid_size=256;h->edid[126]=1;h->edid[127]=255;h->edid[128]=2;h->edid[130]=4;h->edid[131]=0x40;h->edid[255]=186;
 assert(a733_hdmi_audio_init(a,h));
}
int main(void)
{
 for(unsigned costly=0;costly<2;++costly){
  struct model m;struct a733_hdmi h;struct a733_hdmi_audio a;setup(&m,&h,&a,costly?17:0);
  assert(!a733_hdmi_audio_prepare(&a)&&a.stage==1);uint64_t anchor=h.io.now;
  m.ticks=(anchor+5)*24;assert(!a733_hdmi_audio_prepare(&a)&&!m.mc_writes);
  m.ticks+=24;assert(!a733_hdmi_audio_prepare(&a)&&m.mc_writes==1);
  m.ticks+=23;
  for(unsigned i=0;i<6&&a.stage!=3;++i)assert(!a733_hdmi_audio_prepare(&a));
  assert(a.stage==3&&m.cts_tick);anchor=h.io.now;
  m.ticks=(anchor+5000)*24;assert(!a733_hdmi_audio_prepare(&a)&&!m.following);
  m.ticks+=24;assert(!a733_hdmi_audio_prepare(&a)&&m.following==1);
  int result=0;for(unsigned i=0;i<30&&!result;++i)result=a733_hdmi_audio_prepare(&a);
  assert(result==1&&a.ready);
 }
 struct model m;struct a733_hdmi h;struct a733_hdmi_audio a;setup(&m,&h,&a,0);h.io.deadline=6;
 assert(a733_hdmi_audio_prepare(&a)==-1&&a.failed&&!m.mc_writes);
 unsigned following=m.following;assert(a733_hdmi_audio_prepare(&a)==-1&&m.following==following);
 setup(&m,&h,&a,0);assert(!a733_hdmi_audio_prepare(&a));m.ticks=6*24;
 assert(!a733_hdmi_audio_prepare(&a)&&a.stage==2);h.io.deadline=h.io.now+5001;
 for(unsigned i=0;i<4;++i)assert(!a733_hdmi_audio_prepare(&a));
 assert(a733_hdmi_audio_prepare(&a)==-1&&a.failed&&m.cts_tick&&!m.following);
 assert(a733_hdmi_audio_prepare(&a)==-1&&!m.following);
 puts("PASS HDMI audio minima: fractional origin, costly trigger writes and deadline refusal");
}
