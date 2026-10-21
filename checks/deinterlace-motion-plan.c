#include "a733_deinterlace.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct {uint64_t before;struct a733_deinterlace_plan plan;uint64_t after;} storage;
int main(void)
{
 struct a733_deinterlace_session s={.width=640,.height=480,.y_stride=640,.uv_stride=640,
 .format=A733_DEINTERLACE_FORMAT_NV12,.mode=A733_DEINTERLACE_MOTION,.top_field_first=1};
 storage.before=0x7123456789abcdef;storage.after=~storage.before;
 assert(a733_deinterlace_commands(&storage.plan,&s));
 assert(storage.before==0x7123456789abcdef&&storage.after==~storage.before);
 assert(a733_deinterlace_plan_valid(&storage.plan));
 size_t n;const struct a733_deinterlace_word *w=a733_deinterlace_words(&n);assert(n==52);
 for(size_t i=0;i<n;i++){
  assert(w[i].offset>=0x20&&w[i].offset<=0x2020&&!(w[i].offset&3)&&w[i].mask);
  assert(w[i].offset<0x1004||w[i].offset>0x100c);
  for(size_t j=0;j<i;j++)assert(w[i].offset!=w[j].offset);
 }
 assert(a733_deinterlace_input_bytes(&s)==921600);
 storage.plan.words[0x120/4]+=4096;assert(!a733_deinterlace_plan_valid(&storage.plan));
 memset(&storage.plan,0,sizeof(storage.plan));s.mode=4;assert(!a733_deinterlace_commands(&storage.plan,&s));
 memset(&storage.plan,0,sizeof(storage.plan));s.mode=2;s.top_field_first=0;assert(!a733_deinterlace_commands(&storage.plan,&s));
 puts("PASS pinned DI301 initial MOTION generation, MD masks/addresses and refusals; no pixel arithmetic executed");
}
