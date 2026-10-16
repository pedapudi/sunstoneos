#!/usr/bin/env python3
"""Compare camera route, VIPP parameters and SCAP with pinned source functions."""
import argparse,ast,hashlib,json,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--sources',type=Path,required=True,help='Pinned Orange Pi bsp/drivers/vin')
p.add_argument('--vipp',type=Path,required=True,help='Pinned Radxa drivers/vin/vin-vipp/vipp200')
p.add_argument('--evidence',type=Path,required=True)
p.add_argument('--cc',default='clang-21')
a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=True);inputs={}
def read(root,path,h):
 f=root/path;b=f.read_bytes();assert hashlib.sha256(b).hexdigest()==h,path;inputs[str(f)]=h
 return re.sub(r'^#include[^\n]*\n','',b.decode(),flags=re.M)
def fn(source,name):
 m=re.search(r'^(?:static )?(?:void|int) '+name+r'\(',source,re.M);assert m,name
 start=m.start();i=source.index('{',m.end())+1;depth=1
 while depth:depth+=(source[i]=='{')-(source[i]=='}');i+=1
 return source[start:i]
vh=read(a.vipp,'vipp200_reg.h','6879ce32d094148d0636c2cd5c9c2ffead2f7037798afd34f3c85ab4b01cb16e')
vi=read(a.vipp,'vipp200_reg_i.h','37c0b5ca6866e58b05fbf6680d3969dce9107b3ad11aff6139a0ae90d9730e15')
vc=read(a.vipp,'vipp200_reg.c','0738084ab4fc72ce670db5158741c9e49d54d4ca21e996064e8f97ac90de95ab')
tc=read(a.sources,'top_reg.c','810e2b59b85096e1a25ec29bea636a9c7b93b727033707f8b3e43d57e963511b')
ti=read(a.sources,'top_reg_i.h','a596f73382656b8814089480f609b76d622c80f6f426a30334b94ca7f35c72cb')
sc=read(a.sources,'vin-vipp/sunxi_scaler.c','17bebbdd8f8e290a9533f0270d218cedbb718021b80337298ab555863eb0e13d')
preamble=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
typedef uint64_t vin_dma_addr_t;
#define __iomem
#define IS_ENABLED(x) x
#define CONFIG_ARCH_SUN60IW2 1
#define VIPP_200
#define VIN_MAX_SCALER 18
#define MAX_CSIC_TOP_NUM 1
static uint32_t regs[2][256];static uint8_t params[1024];
static volatile void *csic_top_base[1];
static uint32_t vin_reg_readl(volatile void *p){return *(volatile uint32_t *)p;}
static void vin_reg_writel(volatile void *p,uint32_t v){
 if(p==&regs[1][0x30/4])*(volatile uint32_t *)p&=~v;else *(volatile uint32_t *)p=v;
}
static void vin_reg_clr_set(volatile void *p,uint32_t mask,uint32_t v){vin_reg_writel(p,(vin_reg_readl(p)&~mask)|(v&mask));}
'''
names=['vipp_set_base_addr','vipp_top_clk_en','vipp_cap_enable','vipp_cap_disable','vipp_work_mode',
'__vipp_chn_cap_enable','vipp_chn_cap_enable','__vipp_set_para_ready','vipp_set_para_ready',
'__vipp_chn_cap_disable_para_notready','vipp_chn_cap_disable_para_notready',
'__vipp_chn_bypass_mode','vipp_chn_bypass_mode','__vipp_set_reg_load_addr','vipp_set_reg_load_addr',
'vipp_map_reg_load_addr','vipp_scaler_en','vipp_scaler_cfg','vipp_scaler_output_size','vipp_output_fmt_cfg',
'vipp_set_crop','vipp_clear_status']
main=r'''
int main(int argc,char **argv){assert(argc==2);FILE *f=fopen(argv[1],"wb");assert(f);
 for(unsigned parser=0;parser<2;parser++){
  memset(regs,0,sizeof(regs));memset(params,0,sizeof(params));
  regs[0][0x40/4]=4;regs[1][0]=0x80;regs[1][0x30/4]=0xffffff03;
  csic_top_base[0]=regs[0];vipp_set_base_addr(0,(unsigned long)regs[1]);
  vipp_map_reg_load_addr(0,(unsigned long)params);
  struct vipp_scaler_config cfg={.sc_out_fmt=YUV422,.sc_x_ratio=256,.sc_y_ratio=256,
   .sc_w_shift=__scaler_w_shift(256,256)};
  struct vipp_scaler_size size={640,480};struct vipp_crop crop={.width=640,.height=480};
  vipp_scaler_en(0,1);vipp_scaler_cfg(0,&cfg);vipp_scaler_output_size(0,&size);
  vipp_output_fmt_cfg(0,YUV422);vipp_set_crop(0,&crop);
  csic_isp_input_select(0,1,0,parser,0);csic_vipp_input_select(0,0,1,0);
  vipp_work_mode(0,0);vipp_cap_disable(0);vipp_top_clk_en(0,1);
  vipp_clear_status(0,0xffffff03);vipp_chn_cap_disable_para_notready(0);vipp_chn_bypass_mode(0,0);
  vipp_set_reg_load_addr(0,0x48000000);vipp_cap_enable(0);vipp_set_para_ready(0,HAS_READY);vipp_chn_cap_enable(0);
  assert(fwrite(params,1,sizeof(params),f)==sizeof(params));assert(fwrite(regs,1,sizeof(regs),f)==sizeof(regs));
 }assert(!fclose(f));return 0;
}
'''
route=preamble+vi+vh+ti+vc[:vc.index('int vipp_set_base_addr')]+tc[:tc.index('#if IS_ENABLED(CONFIG_ARCH_SUN50IW9)')]
route+='\n'.join(fn(vc,n) for n in names)+fn(tc,'csic_isp_input_select')+fn(tc,'csic_vipp_input_select')+fn(sc,'__scaler_w_shift')+main
(a.evidence/'route-reference.c').write_text(route)
# Reuse the existing reference harness declarations, not its executable checks.
# The new harness runs only both capture modes and compares complete images.
old=Path('checks/a733-csi-receiver-reference.py');tree=ast.parse(old.read_text());values={}
inputs[str(old)]=hashlib.sha256(old.read_bytes()).hexdigest()
for node in tree.body:
 if isinstance(node,ast.Assign) and len(node.targets)==1 and isinstance(node.targets[0],ast.Name):
  name=node.targets[0].id
  if name in ['pins','paths','preamble','main']:values[name]=ast.literal_eval(node.value)
parts=[]
for path in values['paths']:
 t=read(a.sources,path,values['pins'][path])
 if path=='vin-csi/parser_reg.c':t=t[:t.index('void csic_prs_sync_en_cfg')]
 parts.append(t)
m=values['main'].replace('for(unsigned port=0;port<2;port++){','for(unsigned port=0;port<2;port++)for(unsigned image=0;image<2;image++){')
m=m.replace('regs[0][1]=0xdeadbeef;','').replace('regs[1][0]=0x2000;regs[2][0]=0x100000;regs[3][0]=0x20;','')
m=m.replace('port?255:42','42').replace('port?7:2','2').replace('.mode=VCAP','.mode=image?SCAP:VCAP')
(a.evidence/'receiver-reference.c').write_text(values['preamble']+'\n'.join(parts)+m)
flags=['-std=c11','-O1','-g','-Wall','-Wextra','-Werror']
commands=[]
for stem in ['route-reference','receiver-reference']:
 commands.append([a.cc,*flags,str(a.evidence/(stem+'.c')),'-o',str(a.evidence/stem)])
commands += [[a.cc,*flags,'-fsanitize=address,undefined','-fno-omit-frame-pointer','-Iinclude',
 'checks/a733-camera-route.c','drivers/camera/a733_camera_route.c','drivers/camera/a733_csi_receiver.c','-o',str(a.evidence/'check')],
 [str(a.evidence/'route-reference'),str(a.evidence/'route-reference.bin')],
 [str(a.evidence/'receiver-reference'),str(a.evidence/'receiver-reference.bin')],
 [str(a.evidence/'check'),str(a.evidence/'native.bin')]]
logs=[]
for command in commands:
 r=subprocess.run(command,capture_output=True,text=True);logs.append({'command':command,'returncode':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
 (a.evidence/'commands.json').write_text(json.dumps(logs,indent=2)+'\n')
 if r.returncode:print(r.stdout+r.stderr);raise SystemExit(r.returncode)
x=(a.evidence/'route-reference.bin').read_bytes()+(a.evidence/'receiver-reference.bin').read_bytes()
y=(a.evidence/'native.bin').read_bytes();assert len(x)==len(y)==22528
assert x==y,[(hex(i),u,v) for i,(u,v) in enumerate(zip(x,y)) if u!=v]
for name in ['include/a733_camera_route.h','drivers/camera/a733_camera_route.c','include/a733_csi_receiver.h',
'drivers/camera/a733_csi_receiver.c','checks/a733-camera-route.c','checks/a733-camera-route-reference.py',
str(a.evidence/'route-reference.c'),str(a.evidence/'receiver-reference.c')]:
 inputs[name]=hashlib.sha256(Path(name).read_bytes()).hexdigest()
(a.evidence/'results.json').write_text(json.dumps({'result':'PASS','inputs':inputs,'native_sanitizers':['address','undefined'],
 'comparison':'Complete route and parameter images for two parsers; complete receiver images for both ports and SCAP/VCAP',
 'image_sha256':hashlib.sha256(y).hexdigest(),'board_access':False},indent=2)+'\n')
print(logs[-1]['stdout'].strip());print('PASS camera route, parameter and SCAP source comparison')
