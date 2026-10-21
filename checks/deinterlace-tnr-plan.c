#include "a733_deinterlace.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct a733_deinterlace_plan plan;
int main(void)
{
 struct a733_deinterlace_session s={.width=640,.height=480,.y_stride=640,.uv_stride=640,
 .format=A733_DEINTERLACE_FORMAT_NV12,.mode=A733_DEINTERLACE_TNR,.top_field_first=1};
 assert(a733_deinterlace_commands(&plan,&s));assert(a733_deinterlace_input_bytes(&s)==921600);
 size_t n;const struct a733_deinterlace_word *w=a733_deinterlace_tnr_words(&n);
 for(size_t i=0;i<n;++i){
  assert(w[i].offset>=0x20&&w[i].offset<=0x406c&&!(w[i].offset&3)&&w[i].mask);
  assert(w[i].offset<0x1004||w[i].offset>0x100c);
  assert(w[i].offset<0x2000||w[i].offset>=0x4000);
  for(size_t j=0;j<i;++j)assert(w[i].offset!=w[j].offset);
 }
 assert(plan.words[0x20/4]==6&&plan.words[0x4000/4]==0x3a3a3a);
 assert(plan.words[0xf8/4]==A733_DEINTERLACE_OUTPUT_IOVA+384000);
 assert(plan.words[0x124/4]==A733_DEINTERLACE_TNR_FLAG_OUT_IOVA);
 plan.words[0xf8/4]+=4096;assert(!a733_deinterlace_plan_valid(&plan));
 memset(&plan,0,sizeof(plan));s.mode=4;assert(!a733_deinterlace_commands(&plan,&s));
 puts("PASS pinned DI301 fixed-strength TNR generation, planar output and mode-specific flag mapping; no pixel arithmetic executed");
}
