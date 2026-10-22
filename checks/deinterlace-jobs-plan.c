#include "a733_deinterlace.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
/* Generates every job of each WEAVE, MOTION and TNR sequence with the pinned
 * DI301 code. Statistics come from a fixed generator and are recorded with
 * each plan, so the caller can recompute the TNR gain independently. */
static struct a733_deinterlace_plan plan,single,previous,copy;
static uint32_t state=0x2545f491;
static uint32_t next_random(void){state=state*1664525u+1013904223u;return state;}
static size_t submitted(const struct a733_deinterlace_session *s,const struct a733_deinterlace_word **w)
{
 size_t n;*w=s->mode==A733_DEINTERLACE_TNR?a733_deinterlace_tnr_words(&n):a733_deinterlace_words(&n);
 return s->mode==A733_DEINTERLACE_WEAVE?A733_DEINTERLACE_WEAVE_WORDS:n;
}
/* Submitted words that may change between jobs: input and flag addresses,
 * the TNR strength and the TNR accumulators that every job clears. */
static int may_change(uint32_t offset)
{
 return (offset>=0xb0&&offset<0xf0)||offset==0x120||offset==0x124||offset==0x4000||
  (offset>=0x402c&&offset<=0x404c);
}
static void statistics(uint32_t *words,unsigned kind,unsigned job)
{
 size_t n;const struct a733_deinterlace_word *w=a733_deinterlace_statistics(&n);
 for(size_t i=0;i<n;++i){
  uint32_t v=next_random()&w[i].mask;
  /* Field-order counts stay within what the source's s32 products hold.
   * Kind 1 leaves every counter zero, so the adaptive gain falls. Kind 2
   * starts that way and then reports noise, so the gain rises again. */
  if(w[i].offset>=0x3014&&w[i].offset<=0x3020)v&=0x07ffffff;
  if(kind==1||(kind==2&&!job))v=0;
  words[w[i].offset/4]=v;
 }
}
int main(int argc,char **argv)
{
 assert(argc==3);FILE *out=fopen(argv[1],"wb"),*log=fopen(argv[2],"w");assert(out&&log);
 struct a733_deinterlace_session s={.width=640,.height=480,.y_stride=640,.uv_stride=640,
  .format=A733_DEINTERLACE_FORMAT_NV12,.top_field_first=1};
 unsigned cases=0,jobs=0;
 for(unsigned mode=A733_DEINTERLACE_WEAVE;mode<=A733_DEINTERLACE_TNR;++mode)
 for(unsigned strength=0;strength<(mode==A733_DEINTERLACE_TNR?4u:1u);++strength)
 for(unsigned repeats=1;repeats<=A733_DEINTERLACE_MAX_REPEATS;++repeats)
 for(unsigned kind=0;kind<3;++kind){
  s.mode=mode;s.strength=(uint16_t)strength;s.repeats=0;
  memset(&single,0,sizeof(single));assert(a733_deinterlace_commands(&single,&s));
  s.repeats=(uint16_t)repeats;memset(&plan,0,sizeof(plan));
  assert(a733_deinterlace_geometry_valid(&s)&&a733_deinterlace_commands(&plan,&s)&&plan.job==0);
  /* The first job of a sequence differs from a single job only where the
   * source's enabled algorithm writes DIT words. */
  for(size_t i=0;i<sizeof(plan.words)/4;++i)if(plan.words[i]!=single.words[i])
   assert(mode!=A733_DEINTERLACE_WEAVE&&!(mode==A733_DEINTERLACE_TNR&&strength)&&
    (i*4==0x2010||(mode==A733_DEINTERLACE_TNR&&i*4==0x2000)));
  ++cases;
  for(unsigned job=0;;++job){
   assert(a733_deinterlace_plan_valid(&plan)&&plan.job==job);
   const uint32_t *w=plan.words;
   uint32_t previous_iova=job&1?A733_DEINTERLACE_CURRENT_IOVA:A733_DEINTERLACE_SOURCE_IOVA;
   if(mode==A733_DEINTERLACE_WEAVE)assert(w[0xb0/4]==A733_DEINTERLACE_SOURCE_IOVA);
   else{
    uint32_t out_flag=mode==A733_DEINTERLACE_TNR?A733_DEINTERLACE_TNR_FLAG_OUT_IOVA:A733_DEINTERLACE_FLAG_OUT_IOVA;
    assert(w[0xb0/4]==previous_iova&&w[0xd0/4]==(job&1?A733_DEINTERLACE_SOURCE_IOVA:A733_DEINTERLACE_CURRENT_IOVA));
    assert(w[0x120/4]==(job&1?out_flag:A733_DEINTERLACE_FLAG_IN_IOVA)&&w[0x124/4]==(job&1?A733_DEINTERLACE_FLAG_IN_IOVA:out_flag));
   }
   if(job){
    const struct a733_deinterlace_word *list;size_t n=submitted(&s,&list);
    for(size_t i=0;i<n;++i)if(!may_change(list[i].offset))
     assert(!((w[list[i].offset/4]^previous.words[list[i].offset/4])&list[i].mask));
    if(mode==A733_DEINTERLACE_WEAVE)for(size_t i=0;i<n;++i)
     assert(!((w[list[i].offset/4]^single.words[list[i].offset/4])&list[i].mask));
   }
   fprintf(log,"plan %u %u %u %u %u %#x\n",mode,strength,repeats,kind,job,w[0x4000/4]);
   assert(fwrite(plan.words,1,sizeof(plan.words),out)==sizeof(plan.words));++jobs;
   if(job==repeats)break;
   previous=plan;previous.self=(uintptr_t)&previous;
   statistics(plan.words,kind,job);
   fprintf(log,"stats");
   size_t n;const struct a733_deinterlace_word *list=a733_deinterlace_statistics(&n);
   for(size_t i=0;i<n;++i)fprintf(log," %#x=%u",list[i].offset,plan.words[list[i].offset/4]);
   fputc('\n',log);
   /* A copied plan, another session and a missing session are refused
    * without changing the original. */
   copy=plan;copy.self=(uintptr_t)&copy;assert(!a733_deinterlace_next(&copy,&s));
   s.repeats=(uint16_t)(repeats-1);assert(!a733_deinterlace_next(&plan,&s));s.repeats=(uint16_t)repeats;
   assert(!a733_deinterlace_next(&plan,NULL)&&a733_deinterlace_plan_valid(&plan));
   assert(a733_deinterlace_next(&plan,&s));
  }
  /* No job follows the last one. */
  assert(!a733_deinterlace_next(&plan,&s)&&a733_deinterlace_plan_valid(&plan));
 }
 assert(!fclose(out));
 /* A single-job session has no next job. */
 s.mode=A733_DEINTERLACE_TNR;s.strength=2;s.repeats=0;memset(&plan,0,sizeof(plan));
 assert(a733_deinterlace_commands(&plan,&s)&&!a733_deinterlace_next(&plan,&s));
 /* Statistics whose 64-bit divisor has zero low 32 bits make the source's
  * do_div divide by zero; the port refuses and leaves the plan unusable. */
 s.repeats=1;memset(&plan,0,sizeof(plan));assert(a733_deinterlace_commands(&plan,&s));
 memset(&plan.words[0x402c/4],0,9*4);plan.words[0x4038/4]=1;plan.words[0x404c/4]=8192;plan.words[0x4044/4]=1;
 assert(!a733_deinterlace_next(&plan,&s)&&!a733_deinterlace_plan_valid(&plan));
 fprintf(log,"refused-divisor 1 8192 1\n");
 /* A field-order count whose s32 product would overflow is refused. */
 s.mode=A733_DEINTERLACE_MOTION;s.strength=0;memset(&plan,0,sizeof(plan));assert(a733_deinterlace_commands(&plan,&s));
 plan.words[0x301c/4]=INT32_MAX/11+1;assert(!a733_deinterlace_next(&plan,&s)&&!a733_deinterlace_plan_valid(&plan));
 memset(&plan,0,sizeof(plan));assert(a733_deinterlace_commands(&plan,&s));
 plan.words[0x301c/4]=INT32_MAX/11;assert(a733_deinterlace_next(&plan,&s)&&a733_deinterlace_plan_valid(&plan));
 /* Session limits: at most three repeats, in every mode. */
 for(unsigned mode=A733_DEINTERLACE_WEAVE;mode<=A733_DEINTERLACE_TNR;++mode){
  s.mode=mode;s.strength=0;s.repeats=A733_DEINTERLACE_MAX_REPEATS+1;memset(&plan,0,sizeof(plan));
  assert(!a733_deinterlace_geometry_valid(&s)&&!a733_deinterlace_commands(&plan,&s));
  s.repeats=0xffff;assert(!a733_deinterlace_geometry_valid(&s));
  s.repeats=A733_DEINTERLACE_MAX_REPEATS;assert(a733_deinterlace_input_bytes(&s)==
   A733_DEINTERLACE_FRAME_BYTES*(mode==A733_DEINTERLACE_WEAVE?4u:5u)&&a733_deinterlace_jobs(&s)==4);
 }
 /* A plan whose adaptive gain left the selected level's bounds is refused. */
 s.mode=A733_DEINTERLACE_TNR;s.strength=3;s.repeats=1;memset(&plan,0,sizeof(plan));
 assert(a733_deinterlace_commands(&plan,&s));
 memset(&plan.words[0x402c/4],0,9*4);
 assert(a733_deinterlace_next(&plan,&s)&&plan.words[0x4000/4]==0x3c3c3c);
 plan.words[0x4000/4]=0x333333;assert(a733_deinterlace_plan_valid(&plan));
 plan.words[0x4000/4]=0x3f3f3f;assert(!a733_deinterlace_plan_valid(&plan));
 plan.words[0x4000/4]=0x3c3c3d;assert(!a733_deinterlace_plan_valid(&plan));
 assert(!fclose(log));
 printf("PASS pinned DI301 sequences: %u sessions, %u job plans, swapped inputs and flags, bounded gains, refusals\n",cases,jobs);
}
