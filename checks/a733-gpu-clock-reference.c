/* Reuse established callbacks; execute only changed clock cases below. */
#define main preserved_gpu_main
#include "a733-gpu-resources.c"
#undef main
static void actual(unsigned variant)
{
 fresh();*reg(&model,0x07090160)=variant?0x10004002:0x983fb0f7;
 *reg(&model,0x02002000)=variant?0xf84f6300:0xf8675f00;
 *reg(&model,0x020020a0)|=0x01000000; /* Reserved bit24 is not a selector. */
}
int main(void)
{
 uint64_t token=0;
 for(unsigned v=0;v<2;++v){actual(v);assert(acquire(0,A733_GPU_RESOURCE_PARENT,&token)==A733_GPU_RESOURCE_OK&&!model.updates);}
 for(unsigned loss=0;loss<4;++loss){
  actual(0);
  if(loss==0)*reg(&model,0x02002000)^=0x100;
  if(loss==1)*reg(&model,0x02002000)&=~0x10000000u;
  if(loss==2)*reg(&model,0x07090160)&=~2u;
  if(loss==3)*reg(&model,0x07090160)&=~0x10000000u;
  assert(acquire(0,A733_GPU_RESOURCE_PARENT,&token)==A733_GPU_RESOURCE_FAULT&&!model.updates);
  assert(manager.leases[0].mode==A733_GPU_RESOURCE_RETAINED);terminal();
 }
 actual(0);*reg(&model,0x02002000)=0;
 assert(acquire(0,A733_GPU_RESOURCE_OSC,&token)==A733_GPU_RESOURCE_OK&&!model.updates);
 puts("PASS GPU normalized26/19.2 parent, reserved24, enabled/stable OSC-only, retained ratio/lock/enable/stability loss");
}
