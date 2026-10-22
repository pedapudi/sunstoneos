/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Fixed offline configuration uses the pinned GPL DI301 implementation.
 * Its original notices remain in the generated compilation inputs.
 * Source and transformation pins: apps/deinterlace/sources.json and adapt.py. */
#include "a733_deinterlace.h"
#include "di301.c"
unsigned di_port_violation;
_Static_assert(offsetof(struct top_reg,status)==0x18,"DI301 status offset");
_Static_assert(offsetof(struct top_reg,func_en)==0x20,"DI301 function offset");
_Static_assert(offsetof(struct top_reg,in_f1_addr)==0xb0,"DI301 previous-frame input");
_Static_assert(offsetof(struct top_reg,out_dit1_addr)==0x110,"DI301 output1");
_Static_assert(offsetof(struct dit_reg,dit_cropv)==0x18,"DI301 DIT crop");
__attribute__((used,retain)) const uint64_t deinterlace_register_layout[]={
 sizeof(struct top_reg),sizeof(struct dit_reg),offsetof(struct top_reg,status),
 offsetof(struct top_reg,start),offsetof(struct top_reg,in_f1_addr),
 offsetof(struct top_reg,out_dit1_addr),sizeof(union di_status_reg)};
/* Explicit active-weave writable fields from di301_reg.h. Reserved bits are
 * preserved by native RMW. No reset/start/int/status/version/BIST word is here.
 * MD/FMD/TNR are disabled by func_en and all their DMA gates are zero. Their
 * upstream default generation remains CPU-only and is never submitted. */
static const struct a733_deinterlace_word words[]={
 {0x020,0x0000000f},{0x024,0x91111111},{0x028,0xffffffff},{0x02c,0xffffffff},
 {0x030,0x07ff07ff},{0x034,0x10033333},{0x038,0x00000001},
 {0x040,0xffffffff},{0x044,0xffffffff},{0x048,0xffffffff},
 {0x070,0x0000ffff},{0x074,0x0000ffff},{0x078,0x0000ffff},
 {0x0b0,0xffffffff},{0x0b4,0xffffffff},{0x0b8,0xffffffff},{0x0bc,0x00ffffff},
 {0x0c0,0xffffffff},{0x0c4,0xffffffff},{0x0c8,0xffffffff},{0x0cc,0x00ffffff},
 {0x110,0xffffffff},{0x114,0xffffffff},{0x118,0xffffffff},{0x11c,0x00ffffff},
 {0x2000,0x01110731},{0x2004,0x30ffff00},{0x2008,0x1fff010f},
 {0x200c,0x71f3ffff},{0x2010,0x33373337},{0x2014,0x07ff07ff},
 {0x2018,0x07ff07ff},{0x201c,0x07ff07ff},{0x2020,0x07ff07ff},
 /* MOTION-only additions. Confidence registers1004..100c deliberately retain
  * the source lifecycle's hardware reset defaults, never CPU-shadow zeros. */
 {0x050,0x0000ffff},{0x054,0x0000ffff},{0x058,0x0000ffff},{0x080,0x0000ffff},
 {0x0d0,0xffffffff},{0x0d4,0xffffffff},{0x0d8,0xffffffff},{0x0dc,0x00ffffff},
 {0x0e0,0xffffffff},{0x0e4,0xffffffff},{0x0e8,0xffffffff},{0x0ec,0x00ffffff},
 {0x120,0xffffffff},{0x124,0xffffffff},{0x128,0x0000ffff},
 {0x1000,0x7f3fffff},{0x1024,0x07ff07ff},{0x1028,0x07ff07ff}};
const struct a733_deinterlace_word *a733_deinterlace_words(size_t *n)
{if(!n)return NULL;*n=sizeof(words)/sizeof(*words);return words;}
__attribute__((used,retain)) void a733_deinterlace_bitfields(uint32_t out[6])
{
 union di_status_reg status={0};status.bits.finish_flag=1;status.bits.busy=1;
 union di_dma_ctl_reg dma={0};dma.bits.dma_p=1;dma.bits.diw1=1;dma.bits.mclk_gate=1;
 union di_2pitch_reg pitch={0};pitch.bits.f23=1280;
 union di_fmt_reg fmt={0};fmt.bits.in1fmt=2;fmt.bits.ditfmt=2;
 out[0]=status.dwval;out[1]=dma.dwval;out[2]=pitch.dwval;out[3]=fmt.dwval;
 out[4]=sizeof(struct top_reg);out[5]=sizeof(struct dit_reg);
}
/* TNR uses the existing previous/current inputs and MD flags. DIT/FMD are
 * disabled; only their top-level enable bits are published. Every TNR mask
 * follows the named nonreserved fields in pinned di301_reg.h. */
static const struct a733_deinterlace_word tnr_words[]={
 {0x020,0x0000000f},{0x024,0x91111111},{0x028,0xffffffff},{0x02c,0xffffffff},
 {0x030,0x07ff07ff},{0x034,0x10033333},{0x038,0x00000001},
 {0x040,0xffffffff},{0x044,0xffffffff},{0x048,0xffffffff},
 {0x050,0x0000ffff},{0x054,0x0000ffff},{0x058,0x0000ffff},
 {0x060,0x0000ffff},{0x064,0x0000ffff},{0x068,0x0000ffff},{0x080,0x0000ffff},
 {0x0b0,0xffffffff},{0x0b4,0xffffffff},{0x0b8,0xffffffff},{0x0bc,0x00ffffff},
 {0x0c0,0xffffffff},{0x0c4,0xffffffff},{0x0c8,0xffffffff},{0x0cc,0x00ffffff},
 {0x0d0,0xffffffff},{0x0d4,0xffffffff},{0x0d8,0xffffffff},{0x0dc,0x00ffffff},
 {0x0e0,0xffffffff},{0x0e4,0xffffffff},{0x0e8,0xffffffff},{0x0ec,0x00ffffff},
 {0x0f0,0xffffffff},{0x0f4,0xffffffff},{0x0f8,0xffffffff},{0x0fc,0x00ffffff},
 {0x120,0xffffffff},{0x124,0xffffffff},{0x128,0x0000ffff},
 {0x1000,0x7f3fffff},{0x1024,0x07ff07ff},{0x1028,0x07ff07ff},
 {0x4000,0x00ffffff},{0x4004,0x003f3f3f},{0x4008,0x00070707},
 {0x400c,0xffffffff},{0x4010,0xffffffff},{0x4014,0xffffffff},
 {0x4018,0x00ff0707},{0x401c,0x007f000f},{0x4020,0xffffffff},
 {0x4024,0x00ff0707},{0x4028,0x03330333},
 {0x402c,0xffffffff},{0x4030,0xffffffff},{0x4034,0xffffffff},
 {0x4038,0xffffffff},{0x403c,0xffffffff},{0x4040,0xffffffff},
 {0x4044,0xffffffff},{0x4048,0xffffffff},{0x404c,0xffffffff},
 {0x4050,0x000f0f0f},{0x4054,0x0f0f1f1f},{0x4058,0xffffffff},
 {0x405c,0x00010001},{0x4060,0x07ff07ff},{0x4064,0x07ff07ff},
 {0x4068,0x07ff07ff},{0x406c,0x07ff07ff}};
const struct a733_deinterlace_word *a733_deinterlace_tnr_words(size_t *n)
{if(!n)return NULL;*n=sizeof(tnr_words)/sizeof(*tnr_words);return tnr_words;}
/* Statistics that pinned di_dev_get_proc_result reads after each job:
 * FMD/FOD/ITD counters and the TNR accumulators. They are only read. */
static const struct a733_deinterlace_word statistics[]={
 {0x300c,0x3fffffff},{0x3010,0x3fffffff},{0x3014,0x3fffffff},{0x3018,0x3fffffff},
 {0x301c,0x3fffffff},{0x3020,0x3fffffff},{0x3024,0x3fffffff},{0x3028,0x3fffffff},
 {0x3048,0xffffffff},{0x304c,0xffffffff},{0x3054,0xffffffff},{0x3058,0xffffffff},
 {0x3064,0xffffffff},{0x3068,0xffffffff},{0x306c,0xffffffff},
 {0x402c,0xffffffff},{0x4030,0xffffffff},{0x4034,0xffffffff},
 {0x4038,0xffffffff},{0x403c,0xffffffff},{0x4040,0xffffffff},
 {0x4044,0xffffffff},{0x4048,0xffffffff},{0x404c,0xffffffff}};
const struct a733_deinterlace_word *a733_deinterlace_statistics(size_t *n)
{if(!n)return NULL;*n=sizeof(statistics)/sizeof(*statistics);return statistics;}
/* Source fixed strength, then the pinned adaptive initial gains for the
 * LOW, MIDDLE and HIGH levels, indexed by session strength. */
static const uint32_t strengths[]={0x003a3a3a,0x00262626,0x00303030,0x003e3e3e};
/* di_alg_fixed_para gain bounds for LOW, MIDDLE and HIGH. */
static const uint32_t gain_low[]={0,16,28,52},gain_high[]={0,38,48,62};
static int tnr_strength(uint32_t word,unsigned job)
{
 for(unsigned i=0;i<sizeof(strengths)/sizeof(*strengths);++i)if(word==strengths[i])return 1;
 /* Later adaptive jobs program the fed-back gain to all three channels. */
 uint32_t gain=word&255;
 return job&&word==gain*0x010101u&&gain>=gain_low[1]&&gain<=gain_high[3];
}
/* Job k reads frame k as previous and frame k+1 as current. The two input
 * slots and the two MD flag buffers therefore swap roles on every job. */
struct inputs {uint32_t previous,current,flag_in,flag_out;};
static struct inputs inputs(unsigned job,uint32_t flag_out)
{
 struct inputs r={A733_DEINTERLACE_SOURCE_IOVA,A733_DEINTERLACE_CURRENT_IOVA,A733_DEINTERLACE_FLAG_IN_IOVA,flag_out};
 if(job&1)r=(struct inputs){r.current,r.previous,r.flag_out,r.flag_in};
 return r;
}
static int tnr_valid(const uint32_t *w,unsigned job)
{
 struct inputs in=inputs(job,A733_DEINTERLACE_TNR_FLAG_OUT_IOVA);
 return w[0x24/4]==0x91001011&&w[0x34/4]==0x00000110&&
  w[0x38/4]==0&&w[0x30/4]==((479u<<16)|639u)&&
  w[0x40/4]==(1280u<<16)&&w[0x44/4]==(1280u<<16)&&w[0x48/4]==(1280u<<16)&&
  w[0x50/4]==1280&&w[0x54/4]==1280&&w[0x58/4]==1280&&w[0x80/4]==160&&
  w[0xb0/4]==in.previous&&w[0xb4/4]==in.previous+307200&&
  !w[0xb8/4]&&!w[0xbc/4]&&w[0xc0/4]==in.previous+640&&
  w[0xc4/4]==in.previous+307840&&!w[0xc8/4]&&!w[0xcc/4]&&
  w[0xd0/4]==in.current&&w[0xd4/4]==in.current+307200&&
  !w[0xd8/4]&&!w[0xdc/4]&&w[0xe0/4]==in.current+640&&
  w[0xe4/4]==in.current+307840&&!w[0xe8/4]&&!w[0xec/4]&&
  w[0xf0/4]==A733_DEINTERLACE_OUTPUT_IOVA&&w[0xf4/4]==A733_DEINTERLACE_OUTPUT_IOVA+307200&&
  w[0xf8/4]==A733_DEINTERLACE_OUTPUT_IOVA+384000&&!w[0xfc/4]&&
  w[0x60/4]==640&&w[0x64/4]==320&&w[0x68/4]==320&&
  w[0x120/4]==in.flag_in&&w[0x124/4]==in.flag_out&&
  !w[0x128/4]&&w[0x1000/4]==0x21360c04&&w[0x1024/4]==(639u<<16)&&w[0x1028/4]==(479u<<16)&&
  tnr_strength(w[0x4000/4],job)&&w[0x405c/4]==0&&
  w[0x4060/4]==(639u<<16)&&w[0x4064/4]==(479u<<16)&&
  w[0x4068/4]==(639u<<16)&&w[0x406c/4]==(479u<<16);
}
int a733_deinterlace_plan_valid(const struct a733_deinterlace_plan *p)
{
 if(!p||p->self!=(uintptr_t)p||p->ready!=1||p->job>A733_DEINTERLACE_MAX_REPEATS)return 0;
 const uint32_t *w=p->words;
 if(w[0x20/4]==6)return tnr_valid(w,p->job);
 unsigned motion=w[0x20/4]==3;
 struct inputs in=inputs(motion?p->job:0,A733_DEINTERLACE_FLAG_OUT_IOVA);
 uint32_t source=in.previous,uv=source+640*480;
 /* Field weave needs film detection, which these modes leave disabled. */
 if(motion && !(w[0x24/4]==0x90101011&&w[0x34/4]==0x00010110&&
    (w[0x2000/4]&0x01110011)==0x01010011&&!(w[0x2010/4]&0x00010001)&&
    w[0x50/4]==1280&&w[0x54/4]==1280&&w[0x58/4]==1280&&w[0x80/4]==160&&
    w[0xd0/4]==in.current&&w[0xd4/4]==in.current+640*480&&
    !w[0xd8/4]&&!w[0xdc/4]&&w[0xe0/4]==in.current+640&&
    w[0xe4/4]==in.current+640*480+640&&!w[0xe8/4]&&!w[0xec/4]&&
    w[0x120/4]==in.flag_in&&w[0x124/4]==in.flag_out&&
    !w[0x128/4]&&w[0x1000/4]==0x21360c04&&w[0x1024/4]==(639u<<16)&&w[0x1028/4]==(479u<<16)))return 0;
 if(!motion && !(w[0x20/4]==1&&w[0x24/4]==0x80100010&&w[0x34/4]==0x00010010&&
    (w[0x2000/4]&0x00110011)==0))return 0;
 return w[0x38/4]==0&&
  w[0x30/4]==((479u<<16)|639u)&&(w[0x2000/4]&0x01000000)&&
  w[0x40/4]==(1280u<<16)&&w[0x44/4]==(1280u<<16)&&w[0x48/4]==(1280u<<16)&&
  w[0xb0/4]==source&&w[0xb4/4]==uv&&!w[0xb8/4]&&!w[0xbc/4]&&
  w[0xc0/4]==source+640&&w[0xc4/4]==uv+640&&!w[0xc8/4]&&!w[0xcc/4]&&
  w[0x110/4]==A733_DEINTERLACE_OUTPUT_IOVA&&
  w[0x114/4]==A733_DEINTERLACE_OUTPUT_IOVA+640*480&&!w[0x118/4]&&!w[0x11c/4]&&
  w[0x70/4]==640&&w[0x74/4]==640&&w[0x78/4]==640;
}
/* The pinned client, its algorithm history and framebuffer descriptions
 * persist in the plan between the jobs of one session. */
struct sequence {
 struct di_client c;struct di_dev_cdata data;
 struct di_dma_fb input,output,current;struct di_fb in,out,cur;
 struct di_mapped_buf flags[2];
 uint32_t mode,strength,repeats;
};
_Static_assert(sizeof(struct sequence)<=A733_DEINTERLACE_CLIENT_BYTES,"DI301 client state");
static int gain_valid(const struct a733_deinterlace_plan *p,const struct a733_deinterlace_session *s)
{
 if(s->mode!=A733_DEINTERLACE_TNR)return 1;
 uint32_t word=p->words[0x4000/4],gain=word&255;
 if(!p->job||!s->strength)return word==strengths[s->strength];
 return word==gain*0x010101u&&gain>=gain_low[s->strength]&&gain<=gain_high[s->strength];
}
int a733_deinterlace_commands(struct a733_deinterlace_plan *p,const struct a733_deinterlace_session *s)
{
 if(!p||p->self||!a733_deinterlace_geometry_valid(s))return 0;
 memset(p,0,sizeof(*p));di_port_violation=0;
 struct sequence *q=(struct sequence *)p->client;
 struct di_client *c=&q->c;
 q->mode=s->mode;q->strength=s->strength;q->repeats=s->repeats;
 unsigned tnr=s->mode==A733_DEINTERLACE_TNR;
 unsigned motion=s->mode!=A733_DEINTERLACE_WEAVE;
 c->dev_cdata=(uintptr_t)&q->data;c->video_size=(struct di_size){640,480};
 c->mode=tnr?DI_MODE_TNR:motion?DI_MODE_30HZ:DI_MODE_WEAVE;
 c->dit_mode=(struct di_dit_mode){motion?DI_DIT_INTP_MODE_MOTION:DI_DIT_INTP_MODE_WEAVE,DI_DIT_OUT_1FRAME};
 if(tnr)c->dit_mode=(struct di_dit_mode){DI_DIT_INTP_MODE_INVALID,DI_DIT_OUT_0FRAME};
 c->md_en=motion;c->tnr_en=tnr;
 if(tnr)c->tnr_mode=(struct di_tnr_mode){DI_TNR_MODE_FIX,DI_TNR_LEVEL_MIDDLE};
 /* A selected strength uses the source's adaptive mode, whose initial gain
  * comes from the level. Later jobs of a sequence feed the gain back. */
 static const u32 levels[]={DI_TNR_LEVEL_MIDDLE,DI_TNR_LEVEL_LOW,DI_TNR_LEVEL_MIDDLE,DI_TNR_LEVEL_HIGH};
 if(tnr&&s->strength)c->tnr_mode=(struct di_tnr_mode){DI_TNR_MODE_ADAPTIVE,levels[s->strength]};
 c->fb_arg.is_interlace=1;c->fb_arg.top_field_first=1;
 c->md_out_crop=c->dit_out_crop=c->tnr_out_crop=c->fmd_out_crop=(struct di_rect){0,0,640,480};
 c->dit_demo_crop=c->tnr_demo_crop=(struct di_rect){0,0,640,480};
 struct di_fb *in=&q->in,*out=&q->out;
 in->format=out->format=DRM_FORMAT_NV12;in->dma_buf_fd=out->dma_buf_fd=-1;
 in->size=out->size=(struct di_size){640,480};
 in->buf=(struct di_buf){A733_DEINTERLACE_SOURCE_IOVA,A733_DEINTERLACE_SOURCE_IOVA+640*480,0,640,640};
 out->buf=(struct di_buf){A733_DEINTERLACE_OUTPUT_IOVA,A733_DEINTERLACE_OUTPUT_IOVA+640*480,0,640,640};
 in->field_type=DI_FIELD_TYPE_COMBINE_FIELD;
 if(tnr){out->format=DRM_FORMAT_YUV420;out->buf.cr_addr=A733_DEINTERLACE_OUTPUT_IOVA+384000;out->buf.cstride=320;}
 q->input.fb=in;q->output.fb=out;c->in_fb0=c->dma_p=&q->input;c->out_dit_fb0=c->di_w1=&q->output;
 if(tnr){c->out_dit_fb0=c->di_w1=NULL;c->out_tnr_fb0=c->tnr_w=&q->output;}
 if(motion){
  q->cur=*in;q->cur.buf.y_addr=A733_DEINTERLACE_CURRENT_IOVA;q->cur.buf.cb_addr=A733_DEINTERLACE_CURRENT_IOVA+640*480;
  q->current.fb=&q->cur;c->in_fb1=c->dma_c=&q->current;
  q->flags[0].dma_addr=A733_DEINTERLACE_FLAG_IN_IOVA;q->flags[1].dma_addr=tnr?A733_DEINTERLACE_TNR_FLAG_OUT_IOVA:A733_DEINTERLACE_FLAG_OUT_IOVA;
  c->md_buf.mbuf[0]=&q->flags[0];c->md_buf.mbuf[1]=&q->flags[1];c->md_buf.dir=0;
  c->md_buf.w_bit=1280;c->md_buf.w_stride=160;c->md_buf.h=480;
 }
 di_dev_set_reg_base(p->words);
 if(di_dev_apply_fixed_para(c)||di_port_violation)return 0;
 /* A single job retains the explicitly selected WEAVE, MOTION or TNR
  * hardware mode. alg_en==0 preserves fixed hardware settings; it does not
  * disable MD. A selected TNR strength keeps alg_en==1 so the source writes
  * the initial gain; sequence zero runs no algorithm, and the only other
  * writes are DIT fields that the TNR register list never submits.
  * A MOTION or TNR sequence keeps the source's alg_en==1, so every later job
  * runs its ITD, field-order and TNR gain feedback. WEAVE sequences keep
  * alg_en==0: the ITD branch would otherwise select motion interpolation. */
 q->data.proc_rst.di_para.alg_para.alg_en=(tnr&&s->strength)||(motion&&s->repeats);
 if(di_dev_apply_para(c)||di_port_violation)return 0;
 p->self=(uintptr_t)p;p->ready=1;
 return a733_deinterlace_plan_valid(p)&&gain_valid(p,s);
}
int a733_deinterlace_next(struct a733_deinterlace_plan *p,const struct a733_deinterlace_session *s)
{
 struct sequence *q=p?(struct sequence *)p->client:NULL;
 if(!a733_deinterlace_plan_valid(p)||!a733_deinterlace_geometry_valid(s)||
    q->mode!=s->mode||q->strength!=s->strength||q->repeats!=s->repeats||p->job>=s->repeats||
    q->c.dev_cdata!=(uintptr_t)&q->data)return 0;
 p->ready=0;di_port_violation=0;di_dev_set_reg_base(p->words);
 /* Field-order detection multiplies these counts as s32 by up to 11. Linux
  * builds wrap on overflow and C does not, so refuse counts that overflow. */
 for(uint32_t off=0x3014;off<=0x3020;off+=4)if((p->words[off/4]&0x3fffffff)>INT32_MAX/11)return 0;
 /* Equivalent of the finish interrupt: collect the statistics the caller
  * copied from hardware, then let the source compute the next job. */
 di_dev_get_proc_result(&q->c);++q->c.proc_fb_seqno;
 unsigned job=p->job+1;
 if(s->mode!=A733_DEINTERLACE_WEAVE){
  struct inputs in=inputs(job,0);
  q->in.buf.y_addr=in.previous;q->in.buf.cb_addr=in.previous+640*480;
  q->cur.buf.y_addr=in.current;q->cur.buf.cb_addr=in.current+640*480;
 }
 if(di_dev_apply_para(&q->c)||di_port_violation)return 0;
 p->job=job;p->ready=1;
 if(a733_deinterlace_plan_valid(p)&&gain_valid(p,s))return 1;
 p->ready=0;return 0;
}
