#include "a733_deinterlace.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
/* Generates the TNR register image for each strength with the pinned DI301
 * configuration code. Selected strengths may change only the TNR strength
 * word among submitted registers; DIT words differ only in the CPU image. */
static struct a733_deinterlace_plan plans[4],plan;
int main(int argc,char **argv)
{
 assert(argc==2);FILE *out=fopen(argv[1],"wb");assert(out);
 static const uint32_t expected[4]={0x3a3a3a,0x262626,0x303030,0x3e3e3e};
 struct a733_deinterlace_session s={.width=640,.height=480,.y_stride=640,.uv_stride=640,
  .format=A733_DEINTERLACE_FORMAT_NV12,.mode=A733_DEINTERLACE_TNR,.top_field_first=1};
 size_t n;const struct a733_deinterlace_word *w=a733_deinterlace_tnr_words(&n);
 for(unsigned strength=0;strength<4;++strength){
  s.strength=strength;assert(a733_deinterlace_geometry_valid(&s)&&a733_deinterlace_commands(&plans[strength],&s));
  assert(a733_deinterlace_plan_valid(&plans[strength])&&plans[strength].words[0x4000/4]==expected[strength]);
  for(size_t i=0;i<n;++i)if(w[i].offset!=0x4000)
   assert(!((plans[strength].words[w[i].offset/4]^plans[0].words[w[i].offset/4])&w[i].mask));
  for(size_t i=0;i<sizeof(plan.words)/4;++i)if(plans[strength].words[i]!=plans[0].words[i])
   assert(strength&&(i*4==0x4000||i*4==0x2000||i*4==0x2010));
  assert(fwrite(plans[strength].words,1,sizeof(plan.words),out)==sizeof(plan.words));
 }
 assert(!fclose(out));
 /* A plan whose strength word differs from every source value is refused. */
 plan=plans[2];plan.self=(uintptr_t)&plan;plan.words[0x4000/4]=0x313131;assert(!a733_deinterlace_plan_valid(&plan));
 /* Out-of-range strength and strength outside TNR are refused before generation. */
 s.strength=4;memset(&plan,0,sizeof(plan));assert(!a733_deinterlace_geometry_valid(&s)&&!a733_deinterlace_commands(&plan,&s));
 s.strength=0xffffffff;assert(!a733_deinterlace_geometry_valid(&s));
 for(unsigned mode=A733_DEINTERLACE_WEAVE;mode<=A733_DEINTERLACE_MOTION;++mode){
  s.mode=mode;s.strength=1;memset(&plan,0,sizeof(plan));
  assert(!a733_deinterlace_geometry_valid(&s)&&!a733_deinterlace_commands(&plan,&s));
  s.strength=0;memset(&plan,0,sizeof(plan));assert(a733_deinterlace_commands(&plan,&s));
 }
 puts("PASS pinned DI301 TNR strength generation: four strengths, only the submitted strength word differs, five refusals");
}
