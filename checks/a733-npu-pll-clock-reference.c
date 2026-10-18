/* Reuse established callbacks; execute only changed clock cases below. */
#define main preserved_pll_main
#include "npu-pll.c"
#undef main
static void actual(struct model *m,unsigned variant)
{
 setup(m,A733_NPU_PLL_ENABLED_RETUNE);m->osc=variant?0x10004002:0x983fb0f7;
 m->regs[0]=variant?0xf84f6300:0xf8675f00;
}
int main(void)
{
 struct model m;
 for(unsigned v=0;v<2;++v){actual(&m,v);assert(run(&m)==A733_NPU_PLL_READY&&!m.retained);}
 for(unsigned loss=0;loss<4;++loss){
  actual(&m,0);
  if(loss==0)m.regs[0]^=0x100;
  if(loss==1)m.regs[0]&=~0x10000000u;
  if(loss==2)m.osc&=~2u;
  if(loss==3)m.osc&=~0x10000000u;
  assert(run(&m)==A733_NPU_PLL_RETAINED&&!m.writes);terminal(&m);
 }
 /* Preserve the existing final output/lock readback failure boundary. */
 actual(&m,0);
 while(!m.p.settle_until){assert(a733_npu_pll_poll(&m.p)==A733_NPU_PLL_PENDING);m.now+=3;}
 m.regs[0x2a0/4]^=BIT(8);
 assert(run(&m)==A733_NPU_PLL_RETAINED&&!(m.regs[0x2a0/4]&BIT(27)));terminal(&m);
 puts("PASS NPU PLL normalized26/19.2 and retained ratio/lock/enable/stability/final-control loss");
}
