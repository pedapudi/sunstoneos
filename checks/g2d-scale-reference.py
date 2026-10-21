#!/usr/bin/env python3
"""Compare fixed-ratio G2D RCQ blocks with pinned RGB scaler/overlay setters.

This checks emitted register images, not interpolation pixels or edge behavior.
"""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def function(text,name):
 match=re.search(r'(?:static )?__(?:s32|u32) '+name+r'\(',text);assert match,name
 start=match.start();body=text.index('{',start);depth=1;end=body+1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--reference',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--cc',default='/usr/bin/clang-21');a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
 manifest=json.loads((ROOT/'apps/g2d/sources.json').read_text());identities={};parts=[]
 names={'g2d_scal.c':['g2d_vsu_calc_fir_coef','g2d_vsu_para_set'],'g2d_ovl_v.c':['g2d_ovl_v_calc_coarse','g2d_ovl_v_fc_set','g2d_vlayer_set'],'g2d_wb.c':['g2d_wb_set']}
 for name,functions in names.items():
  f=a.reference/'g2d_rcq'/name;data=f.read_bytes();sha=hashlib.sha256(data).hexdigest();assert sha==manifest['reference_pins']['g2d_rcq/'+name]['sha256'];identities[name]=sha;text=data.decode();parts.append(text[:text.index('#include')])
  if name=='g2d_scal.c':parts.append(text[text.index('static __s32 linearcoefftab32'):text.index('static int scal_rcq_setup')])
  parts.extend(function(text,n) for n in functions)
 for name in ('sunxi-g2d.h','g2d_rcq/g2d_bsp.h'):
  assert hashlib.sha256((a.reference/name).read_bytes()).hexdigest()==manifest['reference_pins'][name]['sha256']
 prelude=r'''
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
static unsigned cal_align(unsigned n,unsigned a){assert(a==0);return n;}
static void g2d_byte_cal(g2d_fmt_enh format,__u32 *y,__u32 *u,__u32 *v){assert(format==G2D_FORMAT_ARGB8888);*y=4;*u=*v=0;}
'''
 test=r'''
static void compare(unsigned width,unsigned height,unsigned stride,unsigned ow,unsigned oh,unsigned sx,unsigned sy,unsigned dx,unsigned dy,const char *dump)
{
 _Alignas(32) uint8_t commands[4096];
 struct a733_g2d_session s={.width=width,.height=height,.stride=stride,.op=A733_G2D_SCALE2X,.dx=dx,.dy=dy,.w=ow,.h=oh,.sx=sx,.sy=sy};
 assert(a733_g2d_commands(commands,sizeof(commands),&s));
 if(dump){FILE *f=fopen(dump,"wb");assert(f&&fwrite(commands,1,sizeof(commands),f)==sizeof(commands)&&!fclose(f));}
 memset(&expected_scaler_submodule,0,sizeof(expected_scaler_submodule));memset(&expected_ovl_v_submodule,0,sizeof(expected_ovl_v_submodule));memset(&expected_wb_submodule,0,sizeof(expected_wb_submodule));
 struct scaler_submodule vs={reg_scaler_submodule,dirty_scaler_submodule};struct ovl_v_submodule v={reg_ovl_v_submodule,dirty_ovl_v_submodule};struct wb_submodule wb={reg_wb_submodule,dirty_wb_submodule};
 g2d_image_enh src={.bbuff=1,.format=G2D_FORMAT_ARGB8888,.width=s.stride/4,.height=s.height,.clip_rect={sx,sy,ow/2,oh/2},.alpha=255,.mode=G2D_PIXEL_ALPHA,.bpremul=0,.laddr={A733_G2D_SOURCE_IOVA,0,0}};
 g2d_image_enh dst={.bbuff=1,.format=G2D_FORMAT_ARGB8888,.width=s.stride/4,.height=s.height,.clip_rect={dx,dy,ow,oh},.laddr={A733_G2D_OUTPUT_IOVA,0,0}};
 unsigned midw=0,midh=0;assert(!g2d_vlayer_set(&v,0,&src));assert(!g2d_ovl_v_calc_coarse(&v,src.format,ow/2,oh/2,ow,oh,&midw,&midh));assert(midw==ow/2&&midh==oh/2);assert(!g2d_vsu_para_set(&vs,src.format,midw,midh,ow,oh,255));
 (void)g2d_wb_set(&wb,&dst); /* Pinned setter returns its unchanged -1 after writes. */
 assert(!memcmp(commands+128,&expected_ovl_v_submodule,sizeof(expected_ovl_v_submodule)));
 assert(!memcmp(commands+1024,&expected_scaler_submodule,sizeof(expected_scaler_submodule)));
 assert(!memcmp(commands+832,&expected_wb_submodule,sizeof(expected_wb_submodule)));
 const struct g2d_mixer_bld_reg *b=(const void *)(commands+384);unsigned size=(ow-1)|((oh-1)<<16);
 assert(b->bld_en_ctrl.dwval==0x101&&b->mem_size[0].dwval==size&&b->out_size.dwval==size&&b->bld_ctrl.dwval==0x10001);
 const uint32_t *headers=(const void *)commands;assert(headers[16]==A733_G2D_RCQ_IOVA+1024&&headers[17]==sizeof(expected_scaler_submodule)&&headers[18]==1&&headers[19]==G2D_VSU&&headers[30]==0);
}
int main(int argc,char **argv)
{
 assert(argc==2);
 compare(1280,720,5120,2,2,1279,719,1278,718,NULL);compare(60,64,256,32,24,40,42,9,17,argv[1]);compare(1280,720,5120,1280,720,640,360,0,0,NULL);
 struct a733_g2d_session s={.width=60,.height=64,.stride=256,.op=A733_G2D_SCALE2X,.dx=9,.dy=17,.w=32,.h=24,.sx=40,.sy=42};assert(a733_g2d_geometry_valid(&s));
 s.w=31;assert(!a733_g2d_geometry_valid(&s));s.w=32;s.h=23;assert(!a733_g2d_geometry_valid(&s));s.h=24;s.sx=45;assert(!a733_g2d_geometry_valid(&s));s.sx=40;s.sy=53;assert(!a733_g2d_geometry_valid(&s));s.sy=42;s.color=1;assert(!a733_g2d_geometry_valid(&s));
 puts("Pinned RGB scaler, overlay and writeback setters match scale2x RCQ images");
}
'''
 source=a.output/'reference.c';source.write_text(prelude+'\n'.join(parts)+test)
 cmd=[a.cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-but-set-variable','-fsanitize=address,undefined','-I'+str(ROOT/'include'),'-I'+str(ROOT/'drivers/g2d'),'-I'+str(a.reference),str(source),str(ROOT/'drivers/g2d/a733_g2d.c'),'-o',str(a.output/'reference')]
 (a.output/'commands.json').write_text(json.dumps([cmd,[str(a.output/'reference'),str(a.output/'commands.bin')]],indent=2)+'\n')
 for args in (cmd,[str(a.output/'reference'),str(a.output/'commands.bin')]):
  run=subprocess.run(args,capture_output=True,text=True);(a.output/('build.log' if args==cmd else 'run.log')).write_text(run.stdout+run.stderr);run.check_returncode()
 (a.output/'result.json').write_text(json.dumps(dict(passed=True,scope=__doc__,reference_sha256=identities,register_geometries=3,geometry_refusals=5,physical_access=False),indent=2)+'\n')
if __name__=='__main__':main()
