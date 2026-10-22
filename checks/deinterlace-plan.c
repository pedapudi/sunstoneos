#include "a733_deinterlace.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct {uint64_t before;struct a733_deinterlace_plan plan;uint64_t after;} storage;
int main(void)
{
 struct a733_deinterlace_session s={.width=640,.height=480,.y_stride=640,.uv_stride=640,
 .format=A733_DEINTERLACE_FORMAT_NV12,.mode=A733_DEINTERLACE_WEAVE,.top_field_first=1};
 storage.before=0x7123456789abcdef;storage.after=~storage.before;
 assert(a733_deinterlace_commands(&storage.plan,&s));
 assert(storage.before==0x7123456789abcdef&&storage.after==~storage.before);
 assert(a733_deinterlace_plan_valid(&storage.plan));
 uint32_t layout[6];a733_deinterlace_bitfields(layout);
 assert(layout[0]==0x101&&layout[1]==0x80100010&&layout[2]==(1280u<<16)&&layout[3]==0x00020020);
 size_t n;const struct a733_deinterlace_word *w=a733_deinterlace_words(&n);
 /* WEAVE submits the first 34 rows; motion rows follow in the same list. */
 assert(n>A733_DEINTERLACE_WEAVE_WORDS&&A733_DEINTERLACE_WEAVE_WORDS==34&&w[33].offset==0x2020);for(size_t i=0;i<n;i++){
  assert(w[i].offset>=0x20&&w[i].offset<=0x2020&&!(w[i].offset&3)&&w[i].mask);
  for(size_t j=0;j<i;j++)assert(w[i].offset!=w[j].offset);
 }
 assert(!a733_deinterlace_commands(&storage.plan,&s));
 struct a733_deinterlace_plan copied=storage.plan;assert(!a733_deinterlace_plan_valid(&copied));
 storage.plan.words[0xb0/4]+=4096;assert(!a733_deinterlace_plan_valid(&storage.plan));
 memset(&storage.plan,0,sizeof(storage.plan));s.y_stride=1280;assert(!a733_deinterlace_commands(&storage.plan,&s));
 puts("PASS pinned DI301 fixed-weave generation, masks/layout, bounds and immutable identity");
}
