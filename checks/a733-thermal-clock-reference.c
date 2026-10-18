/* Reuse accepted callbacks; execute only changed source/clock startup cases. */
#define main preserved_thermal_main
#include "a733-thermal.c"
#undef main
#undef CCU
#undef THS
#undef SID
#include "../drivers/thermal/a733_thermal.c"
static void actual(struct model *m,unsigned cold,unsigned peri,unsigned oscillator)
{
 setup(m,cold,peri);
 m->osc=oscillator==0?0x10000002:oscillator==1?0x983fb0f7:0x10004002;
 m->ccu[0]=oscillator==0?(0xf8000000u|(99u<<8)|(99u<<16)):
     oscillator==1?0xf8675f00:0xf84f6300;
 if(peri)m->ccu[0xa0/4]=0xff126310; /* Actual PERI0: reserved bit24 stays set. */
}
int main(void)
{
 struct model m;
 for(unsigned oscillator=0;oscillator<3;++oscillator){
  actual(&m,1,0,oscillator);assert(start(&m)==A733_THERMAL_READY);
  assert((m.ccu[0xfc0/4]&0x8700001f)==(oscillator?0x80000000u:0x81000000u));
  assert((m.ccu[0xfc0/4]&~0x8700001fu)==0x40); /* Preserve unrelated fields. */
  for(unsigned i=0;i<m.writes;++i)assert(m.addresses[i]!=CCU&&m.addresses[i]!=CCU+0xa0&&m.addresses[i]!=CCU+0x518);
  actual(&m,0,0,oscillator);m.ccu[0xfc0/4]=0x80000040;
  assert(start(&m)==A733_THERMAL_READY);
  for(unsigned i=0;i<m.writes;++i)assert(m.addresses[i]!=CCU+0xfc0);
 }
 actual(&m,0,0,0);assert(start(&m)==A733_THERMAL_READY); /* Existing direct24 adoption. */
 for(unsigned oscillator=1;oscillator<3;++oscillator){
  actual(&m,0,0,oscillator);assert(start(&m)==A733_THERMAL_RETAINED&&!m.writes);inert(&m);
 }
 actual(&m,1,1,1);assert(start(&m)==A733_THERMAL_READY); /* PERI0/APB1 uses REF24. */
 for(unsigned loss=0;loss<5;++loss){
  actual(&m,1,0,1);
  if(loss==0)m.ccu[0]^=0x100;
  if(loss==1)m.ccu[0]&=~0x10000000u;
  if(loss==2)m.osc&=~2u;
  if(loss==3)m.osc&=~0x10000000u;
  if(loss==4)m.ccu[0x518/4]^=1;
  assert(start(&m)==A733_THERMAL_RETAINED&&!m.writes);inert(&m);
 }
 puts("PASS thermal normalized24 source for26/19.2 DCXO; direct24 preserved; PERI0 reserved24; cold/adopt ownership and ratio/lock/stability/source refusals");
}
