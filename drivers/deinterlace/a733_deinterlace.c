/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Fixed-weave configuration uses the pinned GPL DI301 implementation.
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
 {0x2018,0x07ff07ff},{0x201c,0x07ff07ff},{0x2020,0x07ff07ff}};
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
int a733_deinterlace_plan_valid(const struct a733_deinterlace_plan *p)
{
 if(!p||p->self!=(uintptr_t)p||p->ready!=1)return 0;
 const uint32_t *w=p->words;uint32_t source=A733_DEINTERLACE_SOURCE_IOVA,uv=source+640*480;
 return w[0x20/4]==1&&w[0x24/4]==0x80100010&&w[0x38/4]==0&&
  w[0x30/4]==((479u<<16)|639u)&&w[0x34/4]==0x00010010&&
  (w[0x2000/4]&0x00110011)==0&&(w[0x2000/4]&0x01000000)&&
  w[0x40/4]==(1280u<<16)&&w[0x44/4]==(1280u<<16)&&w[0x48/4]==(1280u<<16)&&
  w[0xb0/4]==source&&w[0xb4/4]==uv&&!w[0xb8/4]&&!w[0xbc/4]&&
  w[0xc0/4]==source+640&&w[0xc4/4]==uv+640&&!w[0xc8/4]&&!w[0xcc/4]&&
  w[0x110/4]==A733_DEINTERLACE_OUTPUT_IOVA&&
  w[0x114/4]==A733_DEINTERLACE_OUTPUT_IOVA+640*480&&!w[0x118/4]&&!w[0x11c/4]&&
  w[0x70/4]==640&&w[0x74/4]==640&&w[0x78/4]==640;
}
int a733_deinterlace_commands(struct a733_deinterlace_plan *p,const struct a733_deinterlace_session *s)
{
 if(!p||p->self||!a733_deinterlace_geometry_valid(s))return 0;
 memset(p,0,sizeof(*p));di_port_violation=0;
 struct di_client c={0};struct di_dev_cdata data={0};
 struct di_dma_fb input={0},output={0};struct di_fb in={0},out={0};
 c.dev_cdata=(uintptr_t)&data;c.video_size=(struct di_size){640,480};
 c.mode=DI_MODE_WEAVE;c.dit_mode=(struct di_dit_mode){DI_DIT_INTP_MODE_WEAVE,DI_DIT_OUT_1FRAME};
 c.fb_arg.is_interlace=1;c.fb_arg.top_field_first=1;
 c.md_out_crop=c.dit_out_crop=c.tnr_out_crop=c.fmd_out_crop=(struct di_rect){0,0,640,480};
 c.dit_demo_crop=c.tnr_demo_crop=(struct di_rect){0,0,640,480};
 in.format=out.format=DRM_FORMAT_NV12;in.dma_buf_fd=out.dma_buf_fd=-1;
 in.size=out.size=(struct di_size){640,480};
 in.buf=(struct di_buf){A733_DEINTERLACE_SOURCE_IOVA,A733_DEINTERLACE_SOURCE_IOVA+640*480,0,640,640};
 out.buf=(struct di_buf){A733_DEINTERLACE_OUTPUT_IOVA,A733_DEINTERLACE_OUTPUT_IOVA+640*480,0,640,640};
 in.field_type=DI_FIELD_TYPE_COMBINE_FIELD;
 input.fb=&in;output.fb=&out;c.in_fb0=c.dma_p=&input;c.out_dit_fb0=c.di_w1=&output;
 di_dev_set_reg_base(p->words);
 if(di_dev_apply_fixed_para(&c)||di_port_violation)return 0;
 /* Upstream enables adaptive per-frame feedback even for its WEAVE client.
  * Fixed offline weave uses the existing alg_en==0 path in set_dit_para:
  * preserve configured weave modes instead of replacing them with ITD output. */
 data.proc_rst.di_para.alg_para.alg_en=0;
 if(di_dev_apply_para(&c)||di_port_violation)return 0;
 p->self=(uintptr_t)p;p->ready=1;
 return a733_deinterlace_plan_valid(p);
}
