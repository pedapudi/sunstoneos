#!/usr/bin/env python3
"""Compare the new RCQ register blocks with unchanged pinned Linux setters.

This checks command construction, not device pixels or alpha quantization.
"""
import argparse,hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
NAMES={'g2d_bld.c':['bld_in_set','bld_out_setting','bld_set_rop_ctrl','bld_cs_set','bld_porter_duff'],
       'g2d_ovl_u.c':['g2d_ovl_u_fc_set','g2d_uilayer_set','g2d_uilayer_overlay_set']}
def function(text,name):
 start=text.index('__s32 '+name+'(');body=text.index('{',start);depth=1;end=body+1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--reference',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--cc',default='/usr/bin/clang-21');a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
 manifest=json.loads((ROOT/'apps/g2d/sources.json').read_text());identities={};parts=[]
 for name,names in NAMES.items():
  f=a.reference/'g2d_rcq'/name;data=f.read_bytes();sha=hashlib.sha256(data).hexdigest();assert sha==manifest['reference_pins']['g2d_rcq/'+name]['sha256'];identities[name]=sha
  # Preserve original notices, then copy each reference function verbatim.
  text=data.decode();parts.append(text[:text.index('#include')]);parts.extend(function(text,n) for n in names)
 uapi=a.reference/'sunxi-g2d.h';assert hashlib.sha256(uapi.read_bytes()).hexdigest()==manifest['reference_pins']['sunxi-g2d.h']['sha256']
 prelude=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "sunxi-g2d.h"
#include "a733_g2d.h"
#include "g2d_mixer_type.h"
struct blender_submodule {struct g2d_mixer_bld_reg *(*get_reg)(struct blender_submodule *);void (*set_block_dirty)(struct blender_submodule *,unsigned,unsigned);};
struct ovl_u_submodule {struct g2d_mixer_ovl_u_reg *(*get_reg)(struct ovl_u_submodule *,unsigned);void (*set_block_dirty)(struct ovl_u_submodule *,unsigned,unsigned);};
static struct g2d_mixer_bld_reg expected_bld;
static struct g2d_mixer_ovl_u_reg expected_ui;
static struct g2d_mixer_bld_reg *breg(struct blender_submodule *s){(void)s;return &expected_bld;}
static struct g2d_mixer_ovl_u_reg *ureg(struct ovl_u_submodule *s,unsigned layer){(void)s;assert(layer==2);return &expected_ui;}
static void bdirty(struct blender_submodule *s,unsigned n,unsigned dirty){(void)s;assert(!n&&dirty==1);}
static void udirty(struct ovl_u_submodule *s,unsigned n,unsigned dirty){(void)s;assert(n==2&&dirty==1);}
static unsigned cal_align(unsigned n,unsigned a){assert(a==0);return n;}
static void g2d_byte_cal(g2d_fmt_enh format,__u32 *y,__u32 *u,__u32 *v){assert(format==G2D_FORMAT_ARGB8888);*y=4;*u=*v=0;}
'''
 test=r'''
int main(void)
{
 _Alignas(32) uint8_t commands[4096];
 struct a733_g2d_session s={.width=64,.height=64,.stride=272,.op=A733_G2D_SRCOVER_SOLID,.dx=9,.dy=17,.w=31,.h=23,.sx=5,.sy=7,.color=0xff315a93};
 assert(a733_g2d_commands(commands,sizeof(commands),&s));
 struct blender_submodule b={breg,bdirty};struct ovl_u_submodule u={ureg,udirty};
 g2d_image_enh background={.bbuff=0,.color=s.color,.format=G2D_FORMAT_ARGB8888,.width=s.stride/4,.height=s.height,.clip_rect={0,0,s.w,s.h},.alpha=255,.mode=G2D_PIXEL_ALPHA,.bpremul=0};
 g2d_coor zero={0,0};g2d_rect rect={0,0,s.w,s.h};g2d_image_enh output={.clip_rect=rect,.bpremul=0};
 assert(!g2d_uilayer_set(&u,2,&background));assert(!g2d_uilayer_overlay_set(&u,2,&zero,s.w,s.h));
 assert(!bld_in_set(&b,0,rect,0));assert(!bld_in_set(&b,1,rect,0));
 assert(!bld_set_rop_ctrl(&b,0xf0));assert(!bld_porter_duff(&b,G2D_BLD_SRCOVER));
 assert(!bld_cs_set(&b,G2D_FORMAT_ARGB8888));assert(!bld_out_setting(&b,&output));
 assert(!memcmp(commands+256,&expected_ui,sizeof(expected_ui)));
 assert(!memcmp(commands+384,&expected_bld,sizeof(expected_bld)));
 const struct g2d_mixer_ovl_v_reg *v=(const void *)(commands+128);
 assert(v->ovl_attr.dwval==0xff000001&&v->ovl_attr.bits.alpha_mode==G2D_PIXEL_ALPHA&&!v->ovl_attr.bits.lay_premul_ctl);
 assert(v->ovl_mem_pitch0==s.stride&&v->ovl_mem_low_addr0==A733_G2D_SOURCE_IOVA+s.sy*s.stride+s.sx*4);
 const uint32_t *headers=(const void *)commands;assert(headers[3*4]==A733_G2D_RCQ_IOVA+256&&headers[3*4+1]==32&&headers[3*4+2]==1&&headers[3*4+3]==G2D_UI2);
 assert(*(uint32_t *)(commands+288)==0&&headers[7*4+2]==0);
 s.color=0xfe315a93;assert(!a733_g2d_commands(commands,sizeof(commands),&s));
 puts("Pinned UI2 and blender setters exactly match source-over RCQ blocks");
}
'''
 source=a.output/'reference.c';source.write_text(prelude+'\n'.join(parts)+test)
 cmd=[a.cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(ROOT/'include'),'-I'+str(ROOT/'drivers/g2d'),'-I'+str(a.reference),str(source),str(ROOT/'drivers/g2d/a733_g2d.c'),'-o',str(a.output/'reference')]
 (a.output/'commands.json').write_text(json.dumps([cmd,[str(a.output/'reference')]],indent=2)+'\n')
 for args in (cmd,[str(a.output/'reference')]):
  run=subprocess.run(args,capture_output=True,text=True);(a.output/('build.log' if args==cmd else 'run.log')).write_text(run.stdout+run.stderr);run.check_returncode()
 (a.output/'result.json').write_text(json.dumps(dict(passed=True,scope=__doc__,reference_sha256=identities,physical_access=False),indent=2)+'\n')
if __name__=='__main__':main()
