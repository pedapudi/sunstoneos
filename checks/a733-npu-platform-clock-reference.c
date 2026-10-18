/* Reuse established callbacks; execute only changed clock cases below. */
#define main preserved_platform_main
#include "npu-platform.c"
#undef main
static void actual(struct model *m,unsigned variant)
{
 setup(m,4);*reg(m,0x07090160)=variant?0x10004002:0x983fb0f7;
 *reg(m,CCU)=variant?0xf84f6300:0xf8675f00;
 *reg(m,CCU+0xa0)|=0x01000000;
}
int main(void)
{
 struct model m;
 for(unsigned v=0;v<2;++v){actual(&m,v);assert(run(&m)==READY&&m.p.parent_hz==492000000&&m.p.divisor==1);}
 for(unsigned loss=0;loss<4;++loss){
  actual(&m,0);
  if(loss==0)*reg(&m,CCU)^=0x100;
  if(loss==1)*reg(&m,CCU)&=~0x10000000u;
  if(loss==2)*reg(&m,0x07090160)&=~2u;
  if(loss==3)*reg(&m,0x07090160)&=~0x10000000u;
  assert(run(&m)==RETAINED&&!m.nwrite);retained(&m);
 }
 puts("PASS NPU platform normalized26/19.2, reservedPERI24 and retained ratio/lock/enable/stability loss");
}
