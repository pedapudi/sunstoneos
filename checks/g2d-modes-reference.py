#!/usr/bin/env python3
"""Compare general-scaling, image-over-image and format-conversion RCQ blocks with pinned setters.

Unchanged pinned Linux scaler, overlay, blender and writeback setters run under
ASan/UBSan beside the native command builder. The previous native builder from
Git compares every earlier operation byte for byte. Host preparation must agree
with the native geometry predicate on every nominal and refusal case. This
checks command construction only, not device pixels, interpolation or rounding.
"""
import argparse,hashlib,importlib.util,json,re,subprocess
from pathlib import Path
if not __debug__:raise SystemExit('checks require assertions; unset PYTHONOPTIMIZE and do not pass -O')
ROOT=Path(__file__).resolve().parents[1]
NAMES={'g2d_scal.c':['g2d_vsu_calc_fir_coef','g2d_vsu_para_set'],
       'g2d_ovl_v.c':['g2d_ovl_v_calc_coarse','g2d_ovl_v_fc_set','g2d_vlayer_set','g2d_vlayer_overlay_set'],
       'g2d_ovl_u.c':['g2d_ovl_u_fc_set','g2d_uilayer_set','g2d_uilayer_overlay_set'],
       'g2d_bld.c':['bld_in_set','bld_out_setting','bld_set_rop_ctrl','bld_cs_set','bld_porter_duff'],
       'g2d_wb.c':['g2d_wb_set']}
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
def function(text,name):
    match=re.search(r'(?:static )?__(?:s32|u32) '+name+r'\(',text);assert match,name
    start=match.start();body=text.index('{',start);depth=1;end=body+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
PRELUDE=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "sunxi-g2d.h"
#include "a733_g2d.h"
#include "g2d_mixer_type.h"
#define VSU_PHASE_FRAC_BITWIDTH 19
#define VSU_PHASE_NUM 32
#define VSU_ZOOM0_SIZE 1
#define VSU_ZOOM1_SIZE 8
#define VSU_ZOOM2_SIZE 4
#define VSU_ZOOM3_SIZE 1
#define VSU_ZOOM4_SIZE 1
#define do_div(n,d) ((n)/=(d))
enum {VSU_FORMAT_YUV422,VSU_FORMAT_YUV420,VSU_FORMAT_YUV411,VSU_FORMAT_RGB};
#define SUB(name,type) struct name {struct type *(*get_reg)(struct name *);void (*set_block_dirty)(struct name *,unsigned,unsigned);};static struct type expected_##name;static struct type *reg_##name(struct name *s){(void)s;return &expected_##name;}static void dirty_##name(struct name *s,unsigned n,unsigned dirty){(void)s;assert(!n&&dirty==1);}
SUB(scaler_submodule,g2d_mixer_video_scaler_reg)
SUB(ovl_v_submodule,g2d_mixer_ovl_v_reg)
SUB(wb_submodule,g2d_mixer_write_back_reg)
SUB(blender_submodule,g2d_mixer_bld_reg)
struct ovl_u_submodule {struct g2d_mixer_ovl_u_reg *(*get_reg)(struct ovl_u_submodule *,unsigned);void (*set_block_dirty)(struct ovl_u_submodule *,unsigned,unsigned);};
static struct g2d_mixer_ovl_u_reg expected_ui;
static struct g2d_mixer_ovl_u_reg *ureg(struct ovl_u_submodule *s,unsigned layer){(void)s;assert(layer==2);return &expected_ui;}
static void udirty(struct ovl_u_submodule *s,unsigned n,unsigned dirty){(void)s;assert(n==2&&dirty==1);}
static unsigned cal_align(unsigned n,unsigned a){assert(a==0);return n;}
/* Every admitted format is one of the eight 32-bit RGB orders. */
static void g2d_byte_cal(g2d_fmt_enh format,__u32 *y,__u32 *u,__u32 *v){assert(format<=G2D_FORMAT_BGRX8888);*y=4;*u=*v=0;}
'''
TEST=r'''
int old_commands(void *,size_t,const void *);
static struct scaler_submodule vs={reg_scaler_submodule,dirty_scaler_submodule};
static struct ovl_v_submodule ov={reg_ovl_v_submodule,dirty_ovl_v_submodule};
static struct wb_submodule wb={reg_wb_submodule,dirty_wb_submodule};
static struct blender_submodule bld={reg_blender_submodule,dirty_blender_submodule};
static struct ovl_u_submodule ou={ureg,udirty};
static void reset(void)
{
 memset(&expected_scaler_submodule,0,sizeof(expected_scaler_submodule));memset(&expected_ovl_v_submodule,0,sizeof(expected_ovl_v_submodule));
 memset(&expected_wb_submodule,0,sizeof(expected_wb_submodule));memset(&expected_blender_submodule,0,sizeof(expected_blender_submodule));
 memset(&expected_ui,0,sizeof(expected_ui));
}
static const uint32_t *headers(const uint8_t *c){return (const void *)c;}
static void header(const uint8_t *c,unsigned n,unsigned at,unsigned size,unsigned offset)
{const uint32_t *h=headers(c)+4*n;assert(h[0]==A733_G2D_RCQ_IOVA+at&&h[1]==size&&h[2]==1&&h[3]==offset);}
static void common_headers(const uint8_t *c,unsigned blend,unsigned scale)
{
 header(c,0,128,sizeof(struct g2d_mixer_ovl_v_reg),G2D_V0);header(c,1,192,4,G2D_UI0);header(c,2,224,4,G2D_UI1);
 header(c,3,256,blend?32:4,G2D_UI2);header(c,4,scale?1024:288,scale?sizeof(struct g2d_mixer_video_scaler_reg):4,G2D_VSU);
 header(c,5,384,sizeof(struct g2d_mixer_bld_reg),G2D_BLD);header(c,6,832,sizeof(struct g2d_mixer_write_back_reg),G2D_WB);
 const uint32_t *h=headers(c)+28;assert(!h[0]&&!h[1]&&!h[2]&&!h[3]);
}
/* Bitblt route of g2d_bsp_bitblt with blender COPY, as the earlier operations. */
static void copy_blender(unsigned w,unsigned h)
{
 struct g2d_mixer_bld_reg *b=&expected_blender_submodule;
 b->bld_en_ctrl.dwval=0x101;b->mem_size[0].dwval=(w-1)|((h-1)<<16);b->out_size.dwval=b->mem_size[0].dwval;
 b->bld_ctrl.dwval=0x00010001;b->rop_ctrl.dwval=0xf0;b->ch3_index0.dwval=0x41000;
}
static void compare_blocks(const uint8_t *c,unsigned blend,unsigned scale)
{
 assert(!memcmp(c+128,&expected_ovl_v_submodule,sizeof(expected_ovl_v_submodule)));
 assert(!memcmp(c+384,&expected_blender_submodule,sizeof(expected_blender_submodule)));
 assert(!memcmp(c+832,&expected_wb_submodule,sizeof(expected_wb_submodule)));
 if(blend)assert(!memcmp(c+256,&expected_ui,sizeof(expected_ui)));
 else assert(!*(const uint32_t *)(c+256));
 if(scale)assert(!memcmp(c+1024,&expected_scaler_submodule,sizeof(expected_scaler_submodule)));
 else assert(!*(const uint32_t *)(c+288));
 common_headers(c,blend,scale);
}
static void destination(g2d_image_enh *dst,const struct a733_g2d_session *s)
{
 *dst=(g2d_image_enh){.bbuff=1,.format=s->output_format,.width=s->stride/4,.height=s->height,
  .clip_rect={s->dx,s->dy,s->w,s->h},.laddr={A733_G2D_OUTPUT_IOVA,0,0}};
}
static unsigned scales,converts,blends,coarse_h,coarse_v,reductions,enlargements;
static void scale(unsigned width,unsigned height,unsigned stride,unsigned sx,unsigned sy,unsigned sw,unsigned sh,
  unsigned dx,unsigned dy,unsigned w,unsigned h,unsigned in,unsigned out)
{
 _Alignas(32) uint8_t c[4096];reset();
 struct a733_g2d_session s={.width=width,.height=height,.stride=stride,.format=in,.output_format=out,.op=A733_G2D_SCALE,
  .dx=dx,.dy=dy,.w=w,.h=h,.sx=sx,.sy=sy,.sw=sw,.sh=sh};
 assert(a733_g2d_geometry_valid(&s)&&a733_g2d_commands(c,sizeof(c),&s));
 g2d_image_enh src={.bbuff=1,.format=in,.width=stride/4,.height=height,.clip_rect={sx,sy,sw,sh},.alpha=255,
  .mode=G2D_PIXEL_ALPHA,.bpremul=0,.laddr={A733_G2D_SOURCE_IOVA,0,0}},dst;destination(&dst,&s);
 unsigned midw=0,midh=0;assert(!g2d_vlayer_set(&ov,0,&src));
 assert(!g2d_ovl_v_calc_coarse(&ov,src.format,sw,sh,w,h,&midw,&midh));
 assert(!g2d_vsu_para_set(&vs,src.format,midw,midh,w,h,0xff));
 (void)g2d_wb_set(&wb,&dst); /* Pinned setter returns its unchanged -1 after writes. */
 copy_blender(w,h);compare_blocks(c,0,1);
 ++scales;coarse_h+=midw!=sw;coarse_v+=midh!=sh;reductions+=sw>w||sh>h;enlargements+=sw<w||sh<h;
}
static void convert(unsigned in,unsigned out)
{
 _Alignas(32) uint8_t c[4096];reset();
 struct a733_g2d_session s={.width=64,.height=48,.stride=272,.format=in,.output_format=out,.op=A733_G2D_BLIT,
  .dx=11,.dy=5,.w=37,.h=29,.sx=3,.sy=9};
 assert(a733_g2d_geometry_valid(&s)&&a733_g2d_commands(c,sizeof(c),&s));
 g2d_image_enh src={.bbuff=1,.format=in,.width=s.stride/4,.height=s.height,.clip_rect={s.sx,s.sy,s.w,s.h},.alpha=255,
  .mode=G2D_PIXEL_ALPHA,.bpremul=0,.laddr={A733_G2D_SOURCE_IOVA,0,0}},dst;destination(&dst,&s);
 assert(!g2d_vlayer_set(&ov,0,&src));(void)g2d_wb_set(&wb,&dst);
 copy_blender(s.w,s.h);compare_blocks(c,0,0);++converts;
}
static void blend(unsigned width,unsigned height,unsigned stride,unsigned sx,unsigned sy,unsigned bx,unsigned by,
  unsigned dx,unsigned dy,unsigned w,unsigned h,unsigned alpha)
{
 _Alignas(32) uint8_t c[4096];reset();
 struct a733_g2d_session s={.width=width,.height=height,.stride=stride,.op=A733_G2D_SRCOVER_IMAGE,
  .dx=dx,.dy=dy,.w=w,.h=h,.sx=sx,.sy=sy,.bx=bx,.by=by,.alpha=alpha};
 assert(a733_g2d_geometry_valid(&s)&&a733_g2d_commands(c,sizeof(c),&s));
 /* g2d_bsp_bld with V0 foreground in pipe0 and the UI2 background in pipe1. */
 g2d_image_enh src={.bbuff=1,.format=G2D_FORMAT_ARGB8888,.width=stride/4,.height=height,.clip_rect={sx,sy,w,h},
  .alpha=alpha,.mode=G2D_MIXER_ALPHA,.bpremul=0,.laddr={A733_G2D_SOURCE_IOVA,0,0}};
 g2d_image_enh src2={.bbuff=1,.format=G2D_FORMAT_ARGB8888,.width=stride/4,.height=height,.clip_rect={bx,by,w,h},
  .alpha=255,.mode=G2D_PIXEL_ALPHA,.bpremul=0,.laddr={A733_G2D_SOURCE_IOVA,0,0}},dst;destination(&dst,&s);
 g2d_coor zero={0,0};g2d_rect rect={0,0,w,h};g2d_image_enh output={.clip_rect=rect,.bpremul=0};
 assert(!g2d_vlayer_set(&ov,0,&src));assert(!g2d_vlayer_overlay_set(&ov,0,&zero,w,h));
 assert(!g2d_uilayer_set(&ou,2,&src2));assert(!g2d_uilayer_overlay_set(&ou,2,&zero,w,h));
 assert(!bld_set_rop_ctrl(&bld,0xf0));assert(!bld_in_set(&bld,0,rect,0));assert(!bld_in_set(&bld,1,rect,0));
 assert(!bld_porter_duff(&bld,G2D_BLD_SRCOVER));assert(!bld_cs_set(&bld,G2D_FORMAT_BGRA1010102));
 assert(!bld_out_setting(&bld,&output));(void)g2d_wb_set(&wb,&dst);
 compare_blocks(c,1,0);++blends;
}
/* Every earlier operation must emit the same bytes as the previous builder. */
static unsigned unchanged;
static void same(struct a733_g2d_session s)
{
 _Alignas(32) uint8_t now[4096],before[4096];
 assert(a733_g2d_commands(now,sizeof(now),&s)&&old_commands(before,sizeof(before),&s));
 assert(!memcmp(now,before,sizeof(now)));++unchanged;
}
int main(int argc,char **argv)
{
 assert(argc==3);FILE *cases=fopen(argv[1],"w"),*emitted=fopen(argv[2],"wb");assert(cases&&emitted);
 /* Enlargement, reduction, mixed axes, coarse decimation and the extremes. */
 scale(1280,720,5120,0,0,1,1,0,0,1280,720,0,0);scale(1280,720,5120,0,0,1280,720,0,0,1,1,0,0);
 scale(1280,720,5120,0,0,1280,720,7,3,160,180,0,0);scale(1280,720,5120,0,0,1279,719,0,0,159,179,0,0);
 scale(1280,720,5120,3,1,1277,717,0,0,1280,1,0,0);scale(1280,720,5120,0,0,1,720,0,0,1280,1,0,0);
 scale(60,64,256,40,42,16,12,9,17,32,24,0,0);scale(60,64,256,0,0,60,64,1,2,37,23,0,0);
 scale(60,64,256,5,6,13,17,3,4,51,29,0,0);scale(640,480,2600,10,20,600,440,4,8,317,93,0,0);
 for(unsigned w=1;w<=40;++w)for(unsigned sw=1;sw<=48;sw+=(sw<16?1:7))
  if(sw!=w)scale(64,64,256,0,0,sw,7,0,0,w,7,(w+sw)&7,(w*3+sw)&7);
 for(unsigned h=1;h<=24;++h)for(unsigned sh=1;sh<=64;sh+=(sh<12?1:5))
  if(sh!=h)scale(32,64,128,0,0,9,sh,0,0,9,h,0,0);
 for(unsigned in=0;in<8;++in)for(unsigned out=0;out<8;++out){convert(in,out);scale(80,60,320,2,3,40,30,1,1,77,57,in,out);}
 blend(64,64,272,5,7,31,3,9,17,31,23,255);blend(64,64,272,0,0,0,0,0,0,64,64,1);
 blend(1280,720,5120,0,0,640,360,0,0,640,360,128);blend(60,64,256,3,4,3,4,59,63,1,1,77);
 /* Scale2x is the general route at exactly half the destination size. */
 for(unsigned i=0;i<3;++i){
  static const unsigned g[3][9]={{1280,720,5120,1279,719,1278,718,2,2},{60,64,256,40,42,9,17,32,24},{1280,720,5120,640,360,0,0,1280,720}};
  _Alignas(32) uint8_t two[4096],general[4096];
  struct a733_g2d_session s={.width=g[i][0],.height=g[i][1],.stride=g[i][2],.op=A733_G2D_SCALE2X,.sx=g[i][3],.sy=g[i][4],.dx=g[i][5],.dy=g[i][6],.w=g[i][7],.h=g[i][8]};
  assert(a733_g2d_commands(two,sizeof(two),&s));same(s);
  s.op=A733_G2D_SCALE;s.sw=s.w/2;s.sh=s.h/2;assert(a733_g2d_commands(general,sizeof(general),&s));
  assert(!memcmp(two,general,sizeof(two)));
 }
 same((struct a733_g2d_session){.width=64,.height=64,.stride=256,.op=A733_G2D_FILL,.dx=3,.dy=4,.w=17,.h=9,.color=0x80402010});
 same((struct a733_g2d_session){.width=1280,.height=720,.stride=5120,.op=A733_G2D_FILL,.w=1280,.h=720,.color=0xffffffff});
 same((struct a733_g2d_session){.width=64,.height=64,.stride=272,.op=A733_G2D_BLIT,.dx=9,.dy=17,.w=31,.h=23,.sx=5,.sy=7});
 same((struct a733_g2d_session){.width=64,.height=64,.stride=272,.op=A733_G2D_SRCOVER_SOLID,.dx=9,.dy=17,.w=31,.h=23,.sx=5,.sy=7,.color=0xff315a93});
 /* Every geometry case is exported for the host predicate comparison. */
 const struct {const char *name;struct a733_g2d_session s;int valid;} trials[]={
  {"fill",{.width=60,.height=64,.stride=256,.op=1,.dx=3,.dy=4,.w=7,.h=5,.color=0x80402010},1},
  {"copy",{.width=60,.height=64,.stride=256,.op=2,.dx=3,.dy=4,.w=7,.h=5,.sx=9,.sy=8},1},
  {"source-over-solid",{.width=60,.height=64,.stride=256,.op=3,.dx=3,.dy=4,.w=7,.h=5,.sx=9,.sy=8,.color=0xff102030},1},
  {"scale2x",{.width=60,.height=64,.stride=256,.op=4,.dx=9,.dy=17,.w=32,.h=24,.sx=40,.sy=42},1},
  {"scale-reduce",{.width=60,.height=64,.stride=256,.op=5,.w=7,.h=5,.sw=60,.sh=64},1},
  {"scale-enlarge-one-pixel",{.width=60,.height=64,.stride=256,.op=5,.w=60,.h=64,.sw=1,.sh=1},1},
  {"scale-convert",{.width=60,.height=64,.stride=256,.op=5,.w=30,.h=20,.sw=50,.sh=40,.format=3,.output_format=6},1},
  {"copy-convert",{.width=60,.height=64,.stride=256,.op=2,.w=30,.h=20,.format=7,.output_format=0},1},
  {"blend-image",{.width=60,.height=64,.stride=256,.op=6,.w=30,.h=20,.sx=1,.sy=2,.bx=30,.by=44,.alpha=255},1},
  {"blend-image-translucent",{.width=60,.height=64,.stride=256,.op=6,.w=30,.h=20,.bx=30,.by=44,.alpha=1},1},
  {"scale-identity",{.width=60,.height=64,.stride=256,.op=5,.w=30,.h=20,.sw=30,.sh=20},0},
  {"scale-zero-source-width",{.width=60,.height=64,.stride=256,.op=5,.w=30,.h=20,.sw=0,.sh=20},0},
  {"scale-zero-source-height",{.width=60,.height=64,.stride=256,.op=5,.w=30,.h=20,.sw=20,.sh=0},0},
  {"scale-source-width-overflow",{.width=60,.height=64,.stride=256,.op=5,.w=30,.h=20,.sx=11,.sw=50,.sh=20},0},
  {"scale-source-height-overflow",{.width=60,.height=64,.stride=256,.op=5,.w=30,.h=20,.sy=25,.sw=50,.sh=40},0},
  {"scale-source-origin-wrap",{.width=60,.height=64,.stride=256,.op=5,.w=30,.h=20,.sx=0xffffffff,.sw=2,.sh=2},0},
  {"scale-color",{.width=60,.height=64,.stride=256,.op=5,.w=30,.h=20,.sw=10,.sh=10,.color=1},0},
  {"scale-background",{.width=60,.height=64,.stride=256,.op=5,.w=30,.h=20,.sw=10,.sh=10,.bx=1},0},
  {"copy-source-size",{.width=60,.height=64,.stride=256,.op=2,.w=30,.h=20,.sw=30,.sh=20},0},
  {"scale2x-source-size",{.width=60,.height=64,.stride=256,.op=4,.w=30,.h=20,.sw=15,.sh=10},0},
  {"scale2x-convert",{.width=60,.height=64,.stride=256,.op=4,.w=30,.h=20,.output_format=1},0},
  {"fill-convert",{.width=60,.height=64,.stride=256,.op=1,.w=30,.h=20,.format=1},0},
  {"solid-convert",{.width=60,.height=64,.stride=256,.op=3,.w=30,.h=20,.color=0xff000000,.output_format=2},0},
  {"blend-convert",{.width=60,.height=64,.stride=256,.op=6,.w=30,.h=20,.alpha=9,.format=1},0},
  {"source-format-range",{.width=60,.height=64,.stride=256,.op=2,.w=30,.h=20,.format=8},0},
  {"output-format-range",{.width=60,.height=64,.stride=256,.op=5,.w=30,.h=20,.sw=9,.sh=9,.output_format=0x14},0},
  {"reserved-first",{.width=60,.height=64,.stride=256,.op=2,.w=30,.h=20,.reserved={1,0}},0},
  {"reserved-second",{.width=60,.height=64,.stride=256,.op=2,.w=30,.h=20,.reserved={0,1}},0},
  {"blend-zero-alpha",{.width=60,.height=64,.stride=256,.op=6,.w=30,.h=20,.alpha=0},0},
  {"blend-alpha-range",{.width=60,.height=64,.stride=256,.op=6,.w=30,.h=20,.alpha=256},0},
  {"blend-background-x-edge",{.width=60,.height=64,.stride=256,.op=6,.w=30,.h=20,.alpha=9,.bx=60},0},
  {"blend-background-width-overflow",{.width=60,.height=64,.stride=256,.op=6,.w=30,.h=20,.alpha=9,.bx=31},0},
  {"blend-background-height-overflow",{.width=60,.height=64,.stride=256,.op=6,.w=30,.h=20,.alpha=9,.by=45},0},
  {"blend-foreground-overflow",{.width=60,.height=64,.stride=256,.op=6,.w=30,.h=20,.alpha=9,.sx=31},0},
  {"blend-color",{.width=60,.height=64,.stride=256,.op=6,.w=30,.h=20,.alpha=9,.color=0xff000000},0},
  {"blend-source-size",{.width=60,.height=64,.stride=256,.op=6,.w=30,.h=20,.alpha=9,.sw=1},0},
  {"copy-alpha",{.width=60,.height=64,.stride=256,.op=2,.w=30,.h=20,.alpha=255},0},
  {"solid-translucent",{.width=60,.height=64,.stride=256,.op=3,.w=30,.h=20,.color=0xfe000000},0},
  {"unknown-operation",{.width=60,.height=64,.stride=256,.op=7,.w=30,.h=20},0}};
 unsigned refusals=0;
 for(size_t i=0;i<sizeof(trials)/sizeof(*trials);++i){
  _Alignas(32) uint8_t c[4096];const struct a733_g2d_session *s=&trials[i].s;
  int valid=a733_g2d_geometry_valid(s);assert(valid==trials[i].valid);
  assert(a733_g2d_commands(c,sizeof(c),s)==valid);refusals+=!valid;
  /* Admitted commands are kept for comparison with the AArch64 image. */
  if(valid)assert(fwrite(c,1,sizeof(c),emitted)==sizeof(c));
  fprintf(cases,"%s %d %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u\n",trials[i].name,valid,
   s->width,s->height,s->stride,s->format,s->op,s->dx,s->dy,s->w,s->h,s->sx,s->sy,s->color,
   s->sw,s->sh,s->bx,s->by,s->alpha,s->output_format,s->reserved[0],s->reserved[1]);
 }
 assert(!fclose(cases)&&!fclose(emitted));
 assert(coarse_h&&coarse_v&&reductions&&enlargements);
 printf("PASS %u scale %u convert %u blend register images match pinned setters; %u earlier commands unchanged; %u refusals; coarse %u/%u\n",
  scales,converts,blends,unchanged,refusals,coarse_h,coarse_v);
}
'''
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--reference',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--cc',default='/usr/bin/clang-21');p.add_argument('--previous',default='HEAD',help='Git revision holding the previous native builder')
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    manifest=json.loads((ROOT/'apps/g2d/sources.json').read_text());identities={};parts=[]
    for name,functions in NAMES.items():
        f=a.reference/'g2d_rcq'/name;data=f.read_bytes();sha=hashlib.sha256(data).hexdigest()
        assert sha==manifest['reference_pins']['g2d_rcq/'+name]['sha256'],name;identities[name]=sha;text=data.decode()
        # Preserve original notices, then copy each reference function verbatim.
        parts.append(text[:text.index('#include')])
        if name=='g2d_scal.c':parts.append(text[text.index('static __s32 linearcoefftab32'):text.index('static int scal_rcq_setup')])
        parts.extend(function(text,n) for n in functions)
    for name in ('sunxi-g2d.h','g2d_rcq/g2d_bsp.h'):
        assert hashlib.sha256((a.reference/name).read_bytes()).hexdigest()==manifest['reference_pins'][name]['sha256'],name
    # The previous builder compiles against its own headers in a private directory.
    previous=a.output/'previous';previous.mkdir()
    for name in ('drivers/g2d/a733_g2d.c','include/a733_g2d.h','include/a733_g2d_application.h'):
        data=subprocess.run(['git','-C',str(ROOT),'show',a.previous+':'+name],capture_output=True,check=True).stdout
        (previous/Path(name).name).write_bytes(data)
    (previous/'wrapper.c').write_text('#define a733_g2d_commands old_builder\n#include "a733_g2d.c"\n'
        '#undef a733_g2d_commands\nint old_commands(void *m,size_t n,const void *s){return old_builder(m,n,s);}\n')
    source=a.output/'reference.c';source.write_text(PRELUDE+'\n'.join(parts)+TEST)
    flags=['-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-but-set-variable','-fsanitize=address,undefined','-fno-sanitize-recover=all']
    commands=[[a.cc,*flags,'-c','-I'+str(previous),'-I'+str(ROOT/'include'),'-I'+str(ROOT/'drivers/g2d'),str(previous/'wrapper.c'),'-o',str(a.output/'previous.o')],
        [a.cc,*flags,'-I'+str(ROOT/'include'),'-I'+str(ROOT/'drivers/g2d'),'-I'+str(a.reference),str(source),str(ROOT/'drivers/g2d/a733_g2d.c'),str(a.output/'previous.o'),'-o',str(a.output/'reference')],
        [str(a.output/'reference'),str(a.output/'cases.txt'),str(a.output/'commands.bin')]]
    (a.output/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    for index,args in enumerate(commands):
        run=subprocess.run(args,capture_output=True,text=True);(a.output/f'step{index}.log').write_text(run.stdout+run.stderr);run.check_returncode()
    summary=(a.output/'step2.log').read_text().strip()
    # Host preparation mirrors the native predicate for every exported case.
    prep=module('g2d_modes_preparation',ROOT/'apps/g2d/prepare.py');agreed=[]
    for line in (a.output/'cases.txt').read_text().splitlines():
        name,valid,*words=line.split();words=list(map(int,words));geometry,extension=words[:12],words[12:]
        try:size=prep.geometry_check(geometry,extension)
        except ValueError:accepted=False
        else:accepted=True;assert size==geometry[1]*geometry[2]
        assert accepted==(valid=='1'),name;agreed.append(name)
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,scope=__doc__,summary=summary,previous_revision=a.previous,
        reference_sha256=identities,host_predicate_cases=agreed,physical_access=False),indent=2)+'\n')
    print(summary)
if __name__=='__main__':main()
