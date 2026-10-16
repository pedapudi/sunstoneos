#!/usr/bin/env python3
"""Compare inactive A733 DE framebuffer programming with pinned source."""
import argparse, hashlib, json, pathlib, re, subprocess
p=argparse.ArgumentParser()
p.add_argument('--source-root',type=pathlib.Path,required=True)
p.add_argument('--evidence',type=pathlib.Path,required=True)
p.add_argument('--cc',default='clang-21')
a=p.parse_args(); a.evidence.mkdir(parents=True,exist_ok=True)
root=pathlib.Path(__file__).resolve().parents[1]
base='bsp/drivers/drm/sunxi_device/hardware/lowlevel_de/'
pins={'de_top.c': '19aacddfaa41e73127977580103244da76539e82d0fe4b949b9c859d622b895d', 'de_top.h': 'b2649f97c524db7533a33766d60cbb3a51f74df20b49ffcb2bd49dcf9cd8080c', 'de_channel.c': '7b5f9f2b0db3bf9632d0d1bf9456b5a0ffb65cbd81b32bddb6264b6244ab0636', 'de_channel.h': '9aaf1df124c1944ed0f3e7404d6154d8009e0f6df17b6e1efc8eed74f054d27a', 'de_backend.c': '925d6c0290b8a89f7722deaceb1660d4adfe0afd6d2355d8face509b6f22c4af', 'de_frontend.c': '2fa1cbc80fb383bde92e73b7867e9c11af5843cc20014cc0dea33bff3ee0a5c9', 'ovl/de_ovl.c': '2241dda24c6824442178a7536ea79ea47dfa24bf52dad645483c293ed0cf17a4', 'ovl/de_ovl_type.h': '2f14b553d3280e3fd83d87e4d57c3880449d76832168547a0c7ba724933c0fe3', 'ovl/de_ovl_platform.c': '56b91d7038e63fa7108e042097b72bf67166688de1d2c4c005b63f006bbc3d0b', 'ovl/de_ovl_platform.h': 'f8a0f0f2b8ab2eee90866d4e4928dd8d129fb92f7833a367b16cace91814fd75', 'scaler/de_scaler.c': '0194efea5796a8445ea6ad7463626959d64812ef3134109328eddc1853122ac0', 'scaler/de_scaler_platform.c': '1855f2fe555810be4089c9a27a1f2518c843e79157a2e61104acf55edb902b1e', 'scaler/de_scaler_type.h': 'c7158644c966a359c215af4522bdc0d70a6da54318a0474e6b45357048834b08', 'sunxi_de.c': '06053a177b4de847910678f080a11fa996971bdc2ac67b259a68f217a6195c04', 'sunxi_de.h': '24dc31058db034dbbb5162ddb124184099ceab98297eceb609f64387254ce855', 'bld/de_bld.c': '629e50040c58c0a1afe237fe35743715a3a2576b8790a25a4c2d66719ffb42e5', 'bld/de_bld_type.h': '570a532afe31c787f5fa56d0ca30b9d49d6856212c1fa86ef3e5742a83eda6d0', 'bld/de_bld_platform.c': 'd388c83f8712377e0f7fcadef95fac19da2df691bb444b2fd0fa3578ae12cad7', 'de_base.h': '7379f0913edc2f5704d714a67c7e46923000476d5b30ceead0c54dddd733e85b', 'de_frontend.h': '5f778785ef90417ad71cca67fbe118659277513c0b1e1a60bd0c0db452ff6f1e', 'de_backend.h': 'bb350767dc4dadf8d620c484d70ac897acde63da4881a762bbdc81de6f451688', 'csc/de_csc_platform.c': '9fcc9be3056127477f4a35854c2a8040bb9f17fc251e33114fdaae344ff7e2f4', 'csc/de_csc.c': '43852dc3ce323cf5e9b89c880932164d41c56d0f9d5b0a767347b3c152dd434f', 'csc/de_csc_type.h': '77d1563b3ccb4e5859a2832c6e8e67a76c2e03a8eaa92e0ce8a39dbd8dbd5a19', 'gamma/de_gamma_platform.c': '9794393fb92d048fecb76fe870f7e569228d4ecc0b02892b930dada143534045', 'gamma/de_gamma.c': 'e830a4d026cab4bb1903233ec5b8e796d2db4fe268f173a79b80fb008ec46c75', 'gamma/de_gamma_type.h': '0ce6f69a33a1f539e87e8efeffdd1a61ed4206918d0fa52ecd8959e0a5087008', 'dither/de_dither_platform.c': '2a660219aa5ae1c629b381e2d165220e1cca5e5f1eca658c28a3ba6c7ebc2781', 'dither/de_dither.c': '98dfe587dafcb7aa05b26a7d1875fede6f2d37bac0eb11acb972370357d14de7', 'crc/de_crc_platform.c': 'e859aaebe471bca1dce456f1424c993c228caf20b67cc36a33d385f69f16b134', 'cdc/de_cdc_platform.c': '5dc968d431813732b05d0127147b255b32760e8610d8e1d1a7ad24e52d85ded7', 'snr/de_snr_platform.c': '999e7c1d2193bb414efdef9c9904efd7ec16ff0101f0cf2898df16abb6260e5c', 'sharp/de_sharp_platform.c': '5d395140da7b8e34cd4cdb4cce43881ea3f27ef18abc8e1fd57322d775e9d716', 'fcm/de_fcm_platform.c': '21bd978273354fcb0b2385a594d3db66a59bc822e95d468080c1c292ec74dd80', 'dci/de_dci_platform.c': '77a3a687cfde07fef624041a9cbcfcb2da98295a316778f1e7ffa9293eff9165', 'dlc/de_dlc_platform.c': 'e0b0682450f6a892aba6049fe80c63e1e77b1b1d44f6172cedc1dae2276539c6', 'afbd/de_fbd_atw.c': '18984661fe61eb200458c0f00cf67c594c688220857651a024f2dfbf878baa2c', 'tfbd/de_tfbd.c': '329ab3fa516f202863f013ea4e43ac985c211dbc33a6339b26a9a742c23d69ec', 'deband/de_deband_platform.c': 'ff00357f20e3dda1ed49157059a00175ed87822c8e8ad2a73f95bc3779d09bd4', 'deband/de_deband.c': 'd055d84fa249c2da273e3695d65ff98e3b8615e263133c8aabad7687fdfd2c61', 'smbl/de_smbl.c': 'c2ded4b78dbc01bd7a414dabf2b55f27e4cd798e7896a16dbe4f2b9bbadf998c', 'smbl/de_smbl_platform.c': 'b63b97e91aa769fd0d9bd91bed0f93b7ea5a3374d6750050964c887c07c6295e', 'crc/de_crc.c': '11688c6767b47e0b9b0cd042b4e90231e16b60b946984ca6c3a6bd440f9e509d', 'crc/de_crc_type.h': '703587ae13cf5baf55b3b01ec236f91d61d29c86e7d0c2ff4a09ae9e24fb586f', 'ovl/de_ovl.h': '96ff543f9b328aa6915f2016eb85c8e9742cb6daf98969a15b704dc4988df8f6'}
src={}
for name,digest in pins.items():
 b=(a.source_root/base/name).read_bytes(); assert hashlib.sha256(b).hexdigest()==digest,name
 src[name]=b.decode()
def function(text,name):
 m=re.search(r'^(?:static )?(?:inline )?[\w *]+\b'+name+r'\([^;{}]*\)\s*\{',text,re.M); assert m,name
 pos,depth=m.end(),1
 while depth:
  depth+=(text[pos]=='{')-(text[pos]=='}');pos+=1
 return text[m.start():pos]+'\n'
def declaration(text,name):
 m=re.search(r'(?:enum|struct) '+name+r' \{.*?^};',text,re.M|re.S);assert m,name
 return m.group()+'\n'
# Pin platform tables independently of the reduced execution shims.
assert '.version = 0x352' in src['sunxi_de.c']
assert '.name = "u3"' in src['bld/de_bld_platform.c']
assert '.version = 0x352' in src['ovl/de_ovl_platform.c']
assert 'true, true, false, true, true, false, false' in src['tfbd/de_tfbd.c']
assert 'FORCE_ATOMIC_FLUSH' in src['sunxi_de.c'] and 'de_update_ahb(hwde);' in src['sunxi_de.c']
shim=r'''
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Extracted source retains Copyright(c) 2020 - 2023 Allwinner Technology
 * Co.,Ltd. All rights reserved. Copyright (C) 2017 Allwinner. */
#include "a733_display_engine.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;typedef uint64_t u64;typedef uint8_t u8;
#define __iomem
#define DRM_ERROR(...) abort()
#define DRM_DEBUG_DRIVER(...) ((void)0)
#define LINUX_VERSION_CODE 0x060200
#define KERNEL_VERSION(a,b,c) (((a)<<16)|((b)<<8)|(c))
#define DRM_MODE_BLEND_PIXEL_NONE 0
#define fallthrough __attribute__((fallthrough))
#define SET_BITS(s,w,r,v) (((r)&~(((1u<<(w))-1)<<(s)))|((v)<<(s)))
#define GET_BITS(s,w,r) (((r)>>(s))&((1u<<(w))-1))
struct drm_gem_dma_object {u64 dma_addr;};
struct drm_format_info {u32 format;};
struct drm_framebuffer {u32 pitches[4],offsets[4];struct drm_format_info *format;struct drm_gem_dma_object gem;};
static struct drm_gem_dma_object *drm_fb_dma_get_gem_obj(struct drm_framebuffer *fb,unsigned p){assert(!p);return &fb->gem;}
static int drm_to_de_format(u32 f){assert(f==4);return 4;}
struct drm_plane_state {struct drm_framebuffer *fb;u32 src_x,src_y,src_w,src_h,pixel_blend_mode,alpha;};
struct display_channel_state {
 struct drm_plane_state base;bool fake_layer0;u32 color[4],src_x[3],src_y[3],src_w[3],src_h[3],pixel_blend_mode[3],alpha[3];
 struct drm_framebuffer *fb[3];
};
struct de_rect_s {u32 left,top,width,height;};
struct de_ovl_cfg {u8 layer_en[4],lay_premul[4];struct de_rect_s lay_win[4],ovl_win,ovl_out_win;};
'''
shim+=declaration(src['de_channel.h'],'de_pixel_format')
for n in ['ovl/de_ovl_type.h','bld/de_bld_type.h']:
 shim+=re.sub(r'^#include.*\n','',src[n],flags=re.M)
shim+=r'''
struct ovl_desc {unsigned type,layer_cnt;};
struct de_ovl_private {struct ovl_desc *dsc;struct ovl_u_reg reg;struct ovl_v_reg unused;struct {u32 w[4],h[4];} debug;};
struct de_ovl_handle {struct de_ovl_private *private;};
#define OVL_V_REG_BLK_LAY_0_1 0
#define OVL_V_REG_BLK_LAY_2_3 1
#define OVL_U_REG_BLK_PARA 4
#define OVL_U_REG_BLK_DS 5
static struct ovl_u_reg *get_ovl_u_reg(struct de_ovl_private *p){return &p->reg;}
static struct ovl_v_reg *get_ovl_v_reg(struct de_ovl_private *p){return &p->unused;}
static void ovl_set_block_dirty(struct de_ovl_private *p,unsigned b,unsigned d){(void)p;(void)b;assert(d==1);}
struct drm_rect {u32 x1,y1,x2,y2;};
static u32 drm_rect_width(const struct drm_rect *r){return r->x2-r->x1;}
static u32 drm_rect_height(const struct drm_rect *r){return r->y2-r->y1;}
struct de_bld_private {struct bld_reg reg;struct {bool enable,is_premult;unsigned port_id;struct drm_rect crtc_pos;} debug[6];};
struct de_bld_handle {struct de_bld_private *private;};
#define BLD_REG_BLK_ATTR 0
#define BLD_REG_BLK_CTL 1
#define BLD_REG_BLK_CK 2
#define DE_FORMAT_SPACE_RGB 0
static struct bld_reg *get_bld_reg(struct de_bld_private *p){return &p->reg;}
static struct bld_reg *get_bld_hw_reg(struct de_bld_private *p){return &p->reg;}
static void bld_set_block_dirty(struct de_bld_private *p,unsigned b,unsigned d){(void)p;(void)b;assert(d==1);}
'''
for n in ['de_ovl_convert_fmt','de_ovl_disable_lay','de_ovl_u_set_coarse','de_ovl_u_set_lay_layout','de_ovl_u_apply_lay']:
 shim+=function(src['ovl/de_ovl.c'],n)
shim+=declaration(src['bld/de_bld.c'],'de_blend_mode')
for n in ['de_bld_set_pipe_fcolor','de_bld_output_set_attr','de_bld_set_premul','de_bld_en_pipe','de_bld_set_route','de_bld_set_blend_mode','de_bld_pipe_set_attr']:
 shim+=function(src['bld/de_bld.c'],n)
shim+=src['de_top.c'][src['de_top.c'].index('#define DE_REG_OFFSET'):src['de_top.c'].index('struct de_top_desc {')]
shim+=r'''
struct de_top_desc {u32 version,glb_ctl_offset,auto_clk_offset,auto_clk_disp_size,auto_clk_disp_shift,support_rcq_gate,support_pixel_mode,
 de2tcon_mux_offset,port2chn_mux_offset,out_size_offset,rcq_header_laddr_offset,support_mbus_urgency;};
struct de_top_private {struct de_top_desc *dsc;};
struct de_top_handle {struct {u8 *de_reg_base;} cinfo;struct de_top_private *private;};
static u32 readl(u8 *p){u32 v;memcpy(&v,p,4);return v;}
static void writel(u32 v,u8 *p){memcpy(p,&v,4);}
static unsigned sunxi_get_soc_ver(void){return 2;}
'''
for n in ['de_top_set_rtmx_enable','de_top_set_mbus_urgency_depth','de_top_set_pixel_mode','de_top_set_out_size','de_top_set_de2tcon_mux','de_top_set_chn2core_mux_v2','de_top_set_port2chn_mux','de_top_set_rcq_head']:
 shim+=function(src['de_top.c'],n)
shim+=r'''
void de_reference(const struct a733_de_config *c,const u32 *initial,const u32 *actual,unsigned phase)
{
 struct ovl_desc od={OVL_TYPE_UI,4};struct de_ovl_private op={.dsc=&od};struct de_ovl_handle oh={&op};
 struct drm_format_info format={4};struct drm_framebuffer fb={.pitches={c->surface.pitch},.format=&format,.gem={c->surface.device_address}};
 struct display_channel_state state={.base={.fb=&fb,.src_w=(u32)c->timing.dsi.width<<16,.src_h=(u32)c->timing.dsi.height<<16,.alpha=65535}};
 struct de_ovl_cfg cfg={.layer_en={1},.ovl_win={0,0,c->timing.dsi.width,c->timing.dsi.height},.ovl_out_win={0,0,c->timing.dsi.width,c->timing.dsi.height}};
 assert(!de_ovl_u_apply_lay(&oh,&state,&cfg));
 struct de_bld_private bp={0};struct de_bld_handle bh={&bp};
 struct drm_rect rect={0,0,c->timing.dsi.width,c->timing.dsi.height};
 assert(!de_bld_output_set_attr(&bh,rect.x2,rect.y2,0,0,false));
 assert(!de_bld_pipe_set_attr(&bh,0,0,&rect,false));
 assert(sizeof(op.reg)==256 && sizeof(bp.reg)==256);
 assert(!memcmp(&op.reg,actual+0x221000/4,sizeof(op.reg)));
 assert(!memcmp(&bp.reg,actual+0x2a1000/4,sizeof(bp.reg)));
 u32 expected[0x8200/4];memcpy(expected,initial,sizeof(expected));
 struct de_top_desc td={.version=0x352,.glb_ctl_offset=0x8100,.auto_clk_offset=0x810c,.auto_clk_disp_size=0x40,
  .support_pixel_mode=1,.de2tcon_mux_offset=0x8010,.port2chn_mux_offset=0x8030,.out_size_offset=0x8108,
  .rcq_header_laddr_offset=0x8114,.support_mbus_urgency=1};
 struct de_top_private tp={&td};struct de_top_handle th={{(u8 *)expected},&tp};
 assert(!de_top_set_mbus_urgency_depth(&th,2048));
 assert(!de_top_set_rtmx_enable(&th,1,1));
 assert(!de_top_set_out_size(&th,1,rect.x2,rect.y2));
 assert(!de_top_set_pixel_mode(&th,1,1));
 assert(!de_top_set_de2tcon_mux(&th,1,1));
 assert(!de_top_set_rcq_head(&th,1,0,0));
 assert(!de_top_set_port2chn_mux(&th,1,0,3,false));
 assert(!de_top_set_chn2core_mux_v2(&th,1,3,false));
 if(phase==0){ /* Explicit deferred producer enable and auto-clock control. */
  assert((expected[0x8140/4]&1) && (expected[0x814c/4]&1));
  expected[0x8140/4]&=~1u;expected[0x814c/4]&=~1u;
 }else if(phase==2){
  assert(!de_top_set_rtmx_enable(&th,1,0));
  assert(!(expected[0x814c/4]&1));expected[0x814c/4]|=1; /* Retain clocks pending external drain. */
 }
 for(unsigned i=0;i<sizeof(expected)/4;i++) if(actual[i]!=expected[i]){
  fprintf(stderr,"top %x actual %08x expected %08x\n",4*i,actual[i],expected[i]);abort();
 }
}
'''
reference=a.evidence/'reference.c';reference.write_text(shim)
commands=[]
def run(cmd):
 r=subprocess.run(cmd,capture_output=True,text=True);commands.append(dict(command=cmd,returncode=r.returncode,stdout=r.stdout,stderr=r.stderr))
 (a.evidence/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');assert r.returncode==0,commands[-1]
 return r.stdout
binary=a.evidence/'check'
compiled=['drivers/display/a733_display_engine.c','drivers/display/a733_tcon_lcd.c','drivers/display/a733_dsi_video.c','drivers/display/a733_dsi_phy.c']
run([a.cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(root/'include'),str(root/'checks/a733-display-engine.c'),*[str(root/n) for n in compiled],str(reference),'-o',str(binary)])
result=run([str(binary)])
obj=a.evidence/'a733_display_engine.o'
run([a.cc,'--target=aarch64-none-elf','-std=c11','-O2','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-mgeneral-regs-only','-mstrict-align','-mno-outline-atomics','-fstack-usage','-I'+str(root/'include'),'-I'+str(root/'runtime/include'),'-c',str(root/compiled[0]),'-o',str(obj)])
inputs=compiled+['include/a733_display_engine.h','include/a733_tcon_lcd.h','include/a733_dsi_video.h','include/a733_dsi_phy.h','runtime/include/string.h','checks/a733-display-engine.c','checks/a733-display-engine.py']
record=dict(result=result,sources=pins,inputs={n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in inputs},reference_sha256=hashlib.sha256(reference.read_bytes()).hexdigest(),binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),object_sha256=hashlib.sha256(obj.read_bytes()).hexdigest(),size=run(['/usr/lib/llvm-21/bin/llvm-size',str(obj)]),undefined=run(['/usr/lib/llvm-21/bin/llvm-nm','-u',str(obj)]),stack=obj.with_suffix('.su').read_text(),scope='Host source/model and native compilation; no physical execution')
(a.evidence/'results.json').write_text(json.dumps(record,indent=2)+'\n');print(result,end='')
