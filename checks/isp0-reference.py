#!/usr/bin/env python3
"""Compare the fixed ISP600 RAM image with original pinned source setters."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def run(out,cc):
 out.mkdir(parents=True,exist_ok=False);manifest=json.loads((ROOT/'drivers/camera/isp0-sources.json').read_text());texts={}
 for row in manifest['files']:
  p=Path(row['path']);assert hashlib.sha256(p.read_bytes()).hexdigest()==row['sha256'];texts[p.name]=re.sub(r'^#include.*$','',p.read_text(),flags=re.M)
 preamble='''#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint32_t u32;
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "a733_isp0.h"
#define CONFIG_ARCH_SUN60IW2 1
#define ISP_600 1
#define IS_ENABLED(x) x
#define writel(v,p) (*(volatile uint32_t *)(p)=(v))
#define readl(p) (*(volatile uint32_t *)(p))
typedef uint64_t vin_dma_addr_t;
'''
 main='''
static uint32_t regs[0x1300/4],source_load[0x7700/4],source_save_load[0x3400/4];
static uint8_t actual_load[0x7700],actual_save_load[0x3400],vipp[1024];
int main(void){
 bsp_isp_map_reg_addr(0,(uintptr_t)regs);
 bsp_isp_map_load_dram_addr(0,(uintptr_t)source_load);
 bsp_isp_map_save_load_dram_addr(0,(uintptr_t)source_save_load);
 memcpy(source_load,isp_default_reg,0x1000);
 assert(source_load[0x160/4]==0x1e0c1e1e&&source_load[0x164/4]==0x00240130);
 bsp_isp_set_input_fmt(0,ISP_BGGR);bsp_isp_set_last_blank_cycle(0,5);bsp_isp_set_speed_mode(0,3);
 struct isp_size_settings size={.ob_black={640,480},.ob_valid={640,480},.ob_start={0,0}};
 bsp_isp_set_size(0,&size);bsp_isp_set_ch_input_bit(0,0,12);
 bsp_isp_module0_disable(0,ISP_MODULE0_EN_ALL);bsp_isp_module_disable(0,ISP_MODULE1_EN_ALL);
 assert(a733_isp0_parameters(actual_load,sizeof(actual_load),actual_save_load,sizeof(actual_save_load),vipp,sizeof(vipp))==1);
 assert(!memcmp(actual_load,source_load,sizeof(actual_load)));
 assert(!memcmp(actual_save_load,source_save_load,sizeof(actual_save_load)));
 FILE *f=fopen("load.bin","wb");assert(f);assert(fwrite(actual_load,1,sizeof(actual_load),f)==sizeof(actual_load));assert(!fclose(f));
 f=fopen("save-load.bin","wb");assert(f);assert(fwrite(actual_save_load,1,sizeof(actual_save_load),f)==sizeof(actual_save_load));assert(!fclose(f));
 puts("ISP600 A733 complete load/save-load images match original setters");return 0;
}
'''
 harness=preamble+texts['isp600_reg.h']+texts['isp600_reg_cfg.h']+texts['isp_default_tbl.h']+texts['isp600_reg_cfg.c']+main
 (out/'reference.c').write_text(harness)
 commands=[[cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined','-I'+str(ROOT/'include'),out/'reference.c',ROOT/'drivers/camera/a733_isp0.c','-o',out/'reference'],[out/'reference']]
 for i,c in enumerate(commands):
  r=subprocess.run(list(map(str,c)),cwd=out,capture_output=True,text=True);(out/f'step-{i}.log').write_text(r.stdout+r.stderr);r.check_returncode()
 (out/'result.json').write_text(json.dumps(dict(status='pass',physical_access=False,complete_load_bytes=0x7700,complete_save_load_bytes=0x3400,effective_defines=manifest['effective_defines'],commands=[[str(v) for v in c] for c in commands]),indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--cc',default='/usr/bin/clang-21');a=p.parse_args();run(a.output,a.cc)
