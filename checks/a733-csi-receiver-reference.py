#!/usr/bin/env python3
"""Compare receiver registers with the pinned A733 camera register functions."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
if not __debug__:raise SystemExit('checks require assertions; unset PYTHONOPTIMIZE and do not pass -O')
p=argparse.ArgumentParser()
p.add_argument('--sources',type=pathlib.Path,required=True)
p.add_argument('--evidence',type=pathlib.Path,required=True)
p.add_argument('--cc',default='clang-21')
a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=True)
paths=['vin-mipi/protocol.h','utility/bsp_common.h','vin-mipi/combo_csi/combo_csi_reg_i.h',
       'vin-mipi/combo_csi/combo_csi_reg.h','vin-csi/parser_reg_i.h','vin-csi/parser_reg.h',
       'vin-mipi/combo_csi/combo_csi_reg.c','vin-csi/parser_reg.c']
# Hashes identify the source files; callers cannot silently substitute a branch.
pins={'vin-mipi/protocol.h': '6354fdc27d6cb14cd24d5889feb7820aa82ea4e120c4cc9a57beeea3d7e026dc', 'utility/bsp_common.h': 'a72a8854139d9b6a1f1fafad2548f84ef9033077fbf80fde3974c52b85104cb6', 'vin-mipi/combo_csi/combo_csi_reg_i.h': '33cd56ebac15fcb1b9457b221d983f004576a0ebf0766224f4b256dbafdf282d', 'vin-mipi/combo_csi/combo_csi_reg.h': '3c32c83634cdc9fe236855b3d1345a728fde35109b3edc7db84efbecceff863f', 'vin-csi/parser_reg_i.h': '8789c3a43f64f10c954c8a95f35899f64e3fbd2899ecd414da609158f776cd3c', 'vin-csi/parser_reg.h': '0dbfc03a226100d24cce9a34e3d49b601642652f7f997b7bbd014be6e89c8b05', 'vin-mipi/combo_csi/combo_csi_reg.c': 'e6368908f8393707c82aafaf7704af52a1c2b26c829b54553c123ed8bcfe09b4', 'vin-csi/parser_reg.c': '3a95de6e3b1bd32bc07f5989895fd2b00860c99a7ce9f8ffe003b5d6f90f8eb8'}
texts=[];inputs={}
for path in paths:
    f=a.sources/path;data=f.read_bytes();h=hashlib.sha256(data).hexdigest()
    assert h==pins[path],path;inputs[str(f)]=h
    text=re.sub(r'^#include[^\n]*\n','',data.decode(),flags=re.M)
    if path=='vin-csi/parser_reg.c':
        text=text[:text.index('void csic_prs_sync_en_cfg')]
    texts.append(text)
preamble=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
typedef uint32_t u32;
#define __iomem
#define CONFIG_ARCH_SUN60IW2 1
#define IS_ENABLED(x) x
#define VIN_MAX_MIPI 3
#define VIN_MAX_CSI 3
#define PARSER_MULTI_CH_SRC_TYPE
#define BIT(n) (1u<<(n))
enum v4l2_field { V4L2_FIELD_ANY, V4L2_FIELD_NONE };
enum v4l2_mbus_type { V4L2_MBUS_CSI2_DPHY };
struct csi_sync_ctrl;
static uint32_t regs[4][256];
static uint32_t vin_reg_readl(volatile void *p){return *(volatile uint32_t *)p;}
static void vin_reg_writel(volatile void *p,uint32_t v){
 if(p==&regs[2][0x118/4] || p==&regs[3][0x44/4])*(volatile uint32_t *)p&=~v;
 else *(volatile uint32_t *)p=v;
}
static void vin_reg_clr_set(volatile void *p,uint32_t mask,uint32_t v){vin_reg_writel(p,(vin_reg_readl(p)&~mask)|(v&mask));}
static void vin_reg_clr(volatile void *p,uint32_t mask){vin_reg_writel(p,vin_reg_readl(p)&~mask);}
static void vin_reg_set(volatile void *p,uint32_t mask){vin_reg_writel(p,vin_reg_readl(p)|mask);}
'''
main=r'''
int main(int argc,char **argv){
 assert(argc==2);FILE *f=fopen(argv[1],"wb");assert(f);
 for(unsigned port=0;port<2;port++){
  memset(regs,0,sizeof(regs));regs[0][0]=0xb0f;regs[0][1]=0xdeadbeef;
  regs[1][0]=0x2000;regs[2][0]=0x100000;regs[3][0]=0x20;
  cmb_csi_set_top_base_addr((unsigned long)regs[0]);
  cmb_csi_set_phy_base_addr(port,(unsigned long)regs[1]);
  cmb_csi_set_port_base_addr(port,(unsigned long)regs[2]);
  csic_prs_set_base_addr(port,(unsigned long)regs[3]);
  struct combo_csi_cfg cfg={.phy_lane_cfg={.phy_lanedt_en=DT_2LANE,.phy_laneck_en=CK_1LANE,
   .phy_mipi_lpck_en=LPCK_1LANE,.phy_mipi_lpdt_en=LPDT_2LANE},.lane_num=2,.total_rx_ch=1,
   .mipi_datatype={MIPI_YUV422},.field={V4L2_FIELD_NONE}};
  cmb_phy_lane_num_en(port,cfg.phy_lane_cfg);cmb_phy0_work_mode(port,0);
  cmb_phy0_ofscal_cfg(port);cmb_term_ctl(port,cfg.phy_lane_cfg);cmb_hs_ctl(port,cfg.phy_lane_cfg);
  cmb_s2p_ctl(port,port?255:42,cfg.phy_lane_cfg);cmb_mipirx_ctl(port,cfg.phy_lane_cfg);
  cmb_phy0_en(port,1);cmb_phy0_freq_en(port,1);cmb_phy_deskew1_cfg(port,port?7:2,false);
  for(unsigned i=0;i<2;i++)cfg.mipi_lane[i]=cmb_port_set_lane_map(port,i);
  cmb_port_mipi_ch_trigger_en(port,0,1);cmb_port_lane_num(port,2);
  cmb_port_out_num(port,ONE_DATA);cmb_port_out_chnum(port,0);
  cmb_port_lane_map(port,cfg.mipi_lane);cmb_port_mipi_cfg(port,UYVY);
  cmb_port_set_mipi_datatype(port,&cfg);cmb_port_mipi_set_field(port,1,&cfg);
  struct prs_mcsi_if_cfg mcsi={.seq=SEQ_UYVY,.mode=FIELD_MODE};
  struct prs_output_size size={.hor_len=640,.ver_len=480};
  struct prs_fps_ds fps={0};struct prs_cap_mode mode={.mode=VCAP};
  csic_prs_pclk_en(port,1);csic_prs_enable(port);csic_prs_disable(port);csic_prs_enable(port);
  csic_prs_mode(port,PRS_MCSI);csic_prs_mcsi_if_cfg(port,&mcsi);csic_prs_mcsi_en(port,1);
  csic_prs_input_fmt_cfg(port,0,FMT_YUV422);csic_prs_output_size_cfg(port,0,&size);
  csic_prs_fps_ds(port,&fps);csic_prs_capture_start(port,1,&mode);cmb_port_enable(port);
  assert(fwrite(regs,1,sizeof(regs),f)==sizeof(regs));
 }
 assert(!fclose(f));return 0;
}
'''
ref=a.evidence/'reference.c';ref.write_text(preamble+'\n'.join(texts)+main)
flags=['-std=c11','-O1','-g','-Wall','-Wextra','-Werror']
# Reference headers use signed 1<<31 masks. Preserve their register behavior;
# sanitize the native implementation rather than rewriting those expressions.
commands=[[a.cc,*flags,str(ref),'-o',str(a.evidence/'reference')],
          [a.cc,*flags,'-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-Iinclude',
           'checks/a733-csi-receiver.c','drivers/camera/a733_csi_receiver.c','-o',str(a.evidence/'check')],
          [str(a.evidence/'reference'),str(a.evidence/'reference.bin')],
          [str(a.evidence/'check'),str(a.evidence/'native.bin')]]
logs=[]
for command in commands:
    r=subprocess.run(command,capture_output=True,text=True)
    logs.append({'command':command,'returncode':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
    (a.evidence/'commands.json').write_text(json.dumps(logs,indent=2)+'\n')
    if r.returncode:print(r.stdout+r.stderr);raise SystemExit(r.returncode)
x=(a.evidence/'reference.bin').read_bytes();y=(a.evidence/'native.bin').read_bytes()
assert len(x)==len(y)==8192
assert x==y,[(hex(i),u,v) for i,(u,v) in enumerate(zip(x,y)) if u!=v]
for f in map(pathlib.Path,['include/a733_csi_receiver.h','drivers/camera/a733_csi_receiver.c',
                         'checks/a733-csi-receiver.c','checks/a733-csi-receiver-reference.py']):
    inputs[str(f)]=hashlib.sha256(f.read_bytes()).hexdigest()
inputs[str(ref)]=hashlib.sha256(ref.read_bytes()).hexdigest()
result={'result':'PASS','inputs':inputs,'comparison':'Both supported ports; complete four-region register image per port',
        'native_sanitizers':['address','undefined'],'reference_sanitized':False,'physical_execution':False,
        'image_sha256':hashlib.sha256(y).hexdigest()}
(a.evidence/'results.json').write_text(json.dumps(result,indent=2)+'\n')
print(logs[-1]['stdout'].strip());print('PASS full receiver register images against pinned unmodified register functions')
