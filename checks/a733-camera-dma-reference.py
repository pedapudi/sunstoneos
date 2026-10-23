#!/usr/bin/env python3
"""Compare DMA140 configuration with pinned, unchanged register functions."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

p=argparse.ArgumentParser()
p.add_argument('--sources',type=Path,required=True,help='Pinned vin-video/dma140 directory')
p.add_argument('--evidence',type=Path,required=True)
p.add_argument('--cc',default='clang-21')
a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=True)
pins={'dma140_reg.h':'ead254388dbe91c64b8c6e0384477449ac08b0e2b8328e961ef9f00b81e1ef70',
      'dma140_reg_i.h':'0cc2e0ff1e1c3f1754f5f1461918beca539176ffd028bf098cdc4ff1e3121b1b',
      'dma140_reg.c':'25841e1ede699400180dd37e4f4dd837730d4a21a276e7bc630c06a4bf9cd7c0'}
texts={};inputs={}
for name,h in pins.items():
    f=a.sources/name;data=f.read_bytes();assert hashlib.sha256(data).hexdigest()==h,name
    inputs[str(f)]=h;texts[name]=re.sub(r'^#include[^\n]*\n','',data.decode(),flags=re.M)
source=texts['dma140_reg.c']
prefix=source[:source.index('int csic_dma_set_base_addr')]
names=['csic_dma_set_base_addr','csic_dma_top_enable','csic_dma_sdr_wr_size',
       'csic_ve_online_hs_disable','csic_ve_online_ch_sel','csic_dma_buf_length_software_enable',
       'csi_dam_flip_software_enable','csic_dma_mul_ch_enable','__csic_buf_addr_fifo_en',
       'csic_buf_addr_fifo_en','__csic_dma_enable','csic_dma_enable',
       '__csic_dma_frame_drop_disable','csic_dma_frame_drop_disable','__csic_dma_config',
       'csic_dma_config','__csic_dma_10bit_cut2_8bit_disable','csic_dma_10bit_cut2_8bit_disable',
       '__csic_dma_output_size_cfg','csic_dma_output_size_cfg','__csic_dma_buffer_address',
       'csic_dma_buffer_address','__csic_dma_buffer_length','csic_dma_buffer_length',
       '__csic_dma_flip_size','csic_dma_flip_size','__csic_dma_int_clear_status',
       'csic_dma_int_clear_status']
functions=[]
for name in names:
    match=re.search(r'^(?:static )?(?:void|int) '+name+r'\(',source,re.M)
    assert match,name
    start=match.start();body=source.index('{',match.end());depth=1;end=body+1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}');end+=1
    functions.append(source[start:end])
preamble=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define __iomem
#define IS_ENABLED(x) x
#define VIN_MAX_SCALER 18
struct csi_sync_ctrl;
static uint32_t regs[1024];
static uint32_t vin_reg_readl(volatile void *p){return *(volatile uint32_t *)p;}
static void vin_reg_writel(volatile void *p,uint32_t v){
 if(p==&regs[0x254/4])*(volatile uint32_t *)p&=~v;
 else *(volatile uint32_t *)p=v;
}
static void vin_reg_clr_set(volatile void *p,uint32_t mask,uint32_t v){
 vin_reg_writel(p,(vin_reg_readl(p)&~mask)|(v&mask));
}
'''
main=r'''
static void configure(FILE *f,unsigned id,unsigned w,unsigned h,unsigned long y,unsigned long uv){
  memset(regs,0,sizeof(regs));regs[0]=0x80000000;regs[0x204/4]=0x40000000;
  regs[0x254/4]=0x2fe0e7;
  csic_dma_set_base_addr(id,(unsigned long)regs);
  csic_dma_top_enable(id);csic_dma_sdr_wr_size(id,SDR_256B);
  csic_ve_online_hs_disable(id);csic_ve_online_ch_sel(id,CH0);
  csic_dma_buf_length_software_enable(id,0);csi_dam_flip_software_enable(id,0);
  csic_dma_mul_ch_enable(id,0);csic_buf_addr_fifo_en(id,0);csic_dma_frame_drop_disable(id);
  struct csic_dma_cfg cfg={.fmt=FIELD_UV_CB_YUV422,.field=FIELD_EITHER};
  csic_dma_config(id,&cfg);csic_dma_10bit_cut2_8bit_disable(id);
  /* vin_video.c NV16: luminance and chroma lines of w bytes, flip size
   * of w * 2 by h. */
  struct dma_output_size size={.hor_len=w,.ver_len=h};
  struct dma_buf_len len={w,w};struct dma_flip_size flip={h,w*2};
  csic_dma_output_size_cfg(id,&size);csic_dma_buffer_length(id,&len);csic_dma_flip_size(id,&flip);
  csic_dma_buffer_address(id,CSI_BUF_0_A,y);
  csic_dma_buffer_address(id,CSI_BUF_1_A,uv);
  csic_dma_buffer_address(id,CSI_BUF_2_A,0);
  csic_dma_int_clear_status(id,0x2fe0e7);csic_dma_enable(id);
  assert(fwrite(regs,1,sizeof(regs),f)==sizeof(regs));
}
int main(int argc,char **argv){
 assert(argc==2);FILE *f=fopen(argv[1],"wb");assert(f);
 const unsigned ids[]={0,4,8,12,16,17};
 for(unsigned block=0;block<6;block++)configure(f,ids[block],640,480,0x140000000ul,0x140100000ul);
 /* Further sizes on block 0, with planes 16 MiB apart. */
 configure(f,0,320,240,0x140000000ul,0x141000000ul);
 configure(f,0,1280,720,0x140000000ul,0x141000000ul);
 configure(f,0,2592,1944,0x140000000ul,0x141000000ul);
 configure(f,0,176,144,0x140000000ul,0x141000000ul);
 assert(!fclose(f));return 0;
}
'''
ref=a.evidence/'reference.c'
ref.write_text(preamble+texts['dma140_reg_i.h']+texts['dma140_reg.h']+prefix+'\n'.join(functions)+main)
flags=['-std=c11','-O1','-g','-Wall','-Wextra','-Werror']
commands=[[a.cc,*flags,str(ref),'-o',str(a.evidence/'reference')],
          [a.cc,*flags,'-fsanitize=address,undefined','-fno-omit-frame-pointer','-Iinclude',
           'checks/a733-camera-dma.c','drivers/camera/a733_camera_dma.c','-o',str(a.evidence/'check')],
          [str(a.evidence/'reference'),str(a.evidence/'reference.bin')],
          [str(a.evidence/'check'),str(a.evidence/'native.bin')]]
logs=[]
for command in commands:
    r=subprocess.run(command,capture_output=True,text=True)
    logs.append({'command':command,'returncode':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
    (a.evidence/'commands.json').write_text(json.dumps(logs,indent=2)+'\n')
    if r.returncode:print(r.stdout+r.stderr);raise SystemExit(r.returncode)
x=(a.evidence/'reference.bin').read_bytes();y=(a.evidence/'native.bin').read_bytes()
assert len(x)==len(y)==10*4096
assert x==y,[(hex(i),u,v) for i,(u,v) in enumerate(zip(x,y)) if u!=v]
for f in map(Path,['include/a733_camera_dma.h','drivers/camera/a733_camera_dma.c',
                   'checks/a733-camera-dma.c','checks/a733-camera-dma-reference.py',str(ref)]):
    inputs[str(f)]=hashlib.sha256(f.read_bytes()).hexdigest()
(a.evidence/'results.json').write_text(json.dumps({'result':'PASS','inputs':inputs,
    'comparison':'Full 4 KiB DMA register image for channel 0 of each physical block at 640x480, and of block 0 at four further sizes',
    'native_sanitizers':['address','undefined'],'reference_sanitized':False,
    'physical_execution':False,'image_sha256':hashlib.sha256(y).hexdigest()},indent=2)+'\n')
print(logs[-1]['stdout'].strip())
print('PASS full DMA register images against pinned register functions')
