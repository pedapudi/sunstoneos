#!/usr/bin/env python3
"""Compare LCD1/DSI1 preparation and lifecycle with pinned source/manual."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
if not __debug__:raise SystemExit('checks require assertions; unset PYTHONOPTIMIZE and do not pass -O')

p = argparse.ArgumentParser()
p.add_argument('--source-root', type=pathlib.Path, required=True)
p.add_argument('--manual', type=pathlib.Path, required=True)
p.add_argument('--text', type=pathlib.Path, required=True)
p.add_argument('--evidence', type=pathlib.Path, required=True)
p.add_argument('--cc', default='clang-21')
p.add_argument('--case', choices=['all','faults'], default='all')
a = p.parse_args()
a.evidence.mkdir(parents=True, exist_ok=True)
root = pathlib.Path(__file__).resolve().parents[1]
prefix = 'bsp/drivers/drm/sunxi_device/hardware/'
pins = {
    prefix+'lowlevel_lcd/tcon_lcd.c': '299eebcaf1f90ae08470569c41a5ed5a6151042c54a6aee8b36ed688a67e6f9e',
    prefix+'lowlevel_lcd/tcon_lcd_type.h': 'f7fb71db0c0962e5c054d79d0e1cc8adbe9afdd68eba4803237470c39606b763',
    prefix+'lowlevel_tcon/tcon_top.c': '53144710599550158839bd0c779c4b5582b0d1443dfe834517dae06b8b98c502',
    prefix+'lowlevel_tcon/tcon_top_type.h': 'd6cf833f14f7830af3d6ef5ff2369c5cf98c813638333b63057b5825bcc08810',
}
src = {}
for name, h in pins.items():
    data = (a.source_root/name).read_bytes()
    assert hashlib.sha256(data).hexdigest() == h, name
    src[name] = data.decode()
assert hashlib.sha256(a.manual.read_bytes()).hexdigest() == 'cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d'
t = a.text.read_bytes()
assert hashlib.sha256(t).hexdigest() == '219d3c21e5377d18a096765f429555d0693f7fa9b2ae2eb5bb3f225d8b6a09c2'
manual = ' '.join(t.decode().split())
assert '0: Data source is TCON_LCD1 1: Data source is TCON_LCD0' in manual
assert 'If dclk1&dclk2 is used, DCLKDIV >=6' in manual
assert 'If only dclk is used, DCLKDIV >=1' in manual
gctl = manual.split('Register Name: LCD_GCTL_REG', 1)[1].split('15.1.10.2', 1)[0]
assert 'When it’s disabled, the module will be reset to' in gctl and 'idle state.' in gctl


def function(text, name):
    m = re.search(r'^(?:static )?[\w *]+\b'+name+r'\([^;{}]*\)\s*\{', text, re.M)
    assert m, name
    pos, depth = m.end(), 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[m.start():pos]+'\n'


shim = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "a733_tcon_lcd.h"
typedef uint32_t u32; typedef uint8_t u8; typedef int32_t s32;
#define WARN(...) ((void)0)
#define MIPI_DSI_MODE_VIDEO 1u
#define MIPI_DSI_MODE_COMMAND (1u<<15)
#define MIPI_DSI_EN_3DFIFO (1u<<16)
#define MIPI_DSI_SLAVE_MODE (1u<<17)
#define DEVICE_DSI_NUM 2u
/* Only the admitted video path executes. IRQ positions match the manual. */
enum __lcd_irq_id_t { LCD_IRQ_TCON0_CNTR=10, LCD_IRQ_TCON0_TRIF=11, LCD_IRQ_TCON0_VBLK=15 };
enum __lcd_src_t { LCD_SRC_DE=0 };
struct disp_video_timings {
 u32 x_res,y_res,hor_total_time,hor_back_porch,hor_sync_time;
 u32 ver_total_time,ver_back_porch,ver_sync_time,pixel_clk;
};
struct disp_dsi_para { unsigned mode_flags,dual_dsi,dsi_div; struct disp_video_timings timings; };
static unsigned slept;
static void msleep(unsigned n) { slept += n; }
'''
for n in ['lowlevel_lcd/tcon_lcd_type.h', 'lowlevel_tcon/tcon_top_type.h']:
    h = re.sub(r'^#include.*\n', '', src[prefix+n], flags=re.M)
    if n.startswith('lowlevel_lcd'):
        h = h[:h.index('s32 tcon0_frm(')]+'\n#endif\n'
    shim += h
shim += r'''
struct sunxi_tcon_lcd { unsigned tcon_index; volatile struct tcon_lcd_reg *reg; };
static volatile struct __tcon_top_dev_t *tcon_top[1];
'''
for n in ['tcon_lcd_dsi_src_sel', 'tcon_lcd_dsi_clk_source', 'tcon_lcd_dsi_clk_enable']:
    shim += function(src[prefix+'lowlevel_tcon/tcon_top.c'], n)
for n in ['tcon_lcd_init', 'tcon_lcd_exit', 'tcon_lcd_irq_enable', 'tcon_lcd_irq_disable',
          'tcon_lcd_src_select', 'tcon_lcd_set_dclk_div', 'tcon_lcd_timing', 'tcon_lcd_cfg_tri',
          'tcon_lcd_3d_fifo', 'tcon_lcd_rgb_if', 'tcon_dsi_cfg', 'tcon_dsi_open', 'tcon_dsi_close']:
    shim += function(src[prefix+'lowlevel_lcd/tcon_lcd.c'], n)
shim += r'''
static void compare(const struct tcon_lcd_reg *r, const struct __tcon_top_dev_t *top,
                    const uint32_t *actual, const uint32_t *actual_top)
{
 for(unsigned i=0;i<sizeof(*r)/4;i++){
  uint32_t expected;memcpy(&expected,(const uint8_t *)r+4*i,4);
  if(expected!=actual[i]){fprintf(stderr,"LCD %x expected %08x actual %08x\n",4*i,expected,actual[i]);abort();}
 }
 assert(!memcmp(top,actual_top,sizeof(*top)));
}
void tcon_reference(const struct a733_tcon_lcd_timing *t,const uint32_t *initial,
                    const uint32_t *initial_top,const uint32_t *actual,const uint32_t *actual_top,unsigned phase)
{
 struct tcon_lcd_reg regs;struct __tcon_top_dev_t top;
 memcpy(&regs,initial,sizeof(regs));memcpy(&top,initial_top,sizeof(top));tcon_top[0]=&top;
 struct sunxi_tcon_lcd lcd={.tcon_index=1,.reg=&regs};
 struct disp_dsi_para para={.mode_flags=MIPI_DSI_MODE_VIDEO,.dsi_div=6,
  .timings={.x_res=t->dsi.width,.y_res=t->dsi.height,
   .hor_total_time=t->dsi.width+t->dsi.hsync+t->dsi.hback+t->dsi.hfront,
   .hor_back_porch=t->dsi.hback,.hor_sync_time=t->dsi.hsync,
   .ver_total_time=t->dsi.height+t->dsi.vsync+t->dsi.vback+t->dsi.vfront,
   .ver_back_porch=t->dsi.vback,.ver_sync_time=t->dsi.vsync,.pixel_clk=t->actual_dclk_hz}};
 assert(!tcon_lcd_init(&lcd));assert(!tcon_lcd_set_dclk_div(&lcd,1));
 assert(!tcon_lcd_dsi_clk_source(1,0));assert(!tcon_lcd_dsi_src_sel(1,0));
 assert(!tcon_dsi_cfg(&lcd,&para));assert(!tcon_lcd_src_select(&lcd,LCD_SRC_DE));
 /* The source enables global/output/trigger/output-gate during configuration.
  * Native READY explicitly defers these four enables. No other normalization. */
 if(!phase){
  assert(regs.tcon_gctl.bits.tcon_en==1 && regs.tcon0_ctl.bits.tcon0_en==1);
  assert(regs.tcon0_cpu_ctl.bits.trigger_en==1 && top.tcon_clk_gate.bits.dsi1_clk_gate==1);
  regs.tcon_gctl.bits.tcon_en=0;regs.tcon0_ctl.bits.tcon0_en=0;
  regs.tcon0_cpu_ctl.bits.trigger_en=0;top.tcon_clk_gate.bits.dsi1_clk_gate=0;
 }else{
  slept=0;assert(!tcon_dsi_open(&lcd,&para));assert(slept==100);
  /* Manual requires divider>=6 if phases1/2 are used. At divider1, native
   * uses only phase0. Assert the exact source value before this correction. */
  assert(regs.tcon0_dclk.bits.tcon0_dclk_div==1 && regs.tcon0_dclk.bits.tcon0_dclk_en==15);
  regs.tcon0_dclk.bits.tcon0_dclk_en=1;
  if(phase==2){assert(tcon_dsi_close(&lcd)==1);assert(!tcon_lcd_exit(&lcd));}
 }
 compare(&regs,&top,actual,actual_top);
}
'''
reference = a.evidence/'reference.c'
reference.write_text(shim)
inputs = ['include/a733_tcon_lcd.h','drivers/display/a733_tcon_lcd.c','checks/a733-tcon-lcd.c',
          'checks/a733-tcon-lcd.py','include/a733_dsi_video.h','drivers/display/a733_dsi_video.c',
          'include/a733_dsi_phy.h','drivers/display/a733_dsi_phy.c','runtime/include/string.h']
commands = []


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    commands.append(dict(command=cmd,returncode=r.returncode,stdout=r.stdout,stderr=r.stderr))
    (a.evidence/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    assert r.returncode == 0, commands[-1]
    return r.stdout


binary = a.evidence/'check'
run([a.cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
     '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-I'+str(root/'include'),
     str(root/'checks/a733-tcon-lcd.c'),str(root/'drivers/display/a733_tcon_lcd.c'),
     str(root/'drivers/display/a733_dsi_video.c'),str(root/'drivers/display/a733_dsi_phy.c'),
     str(reference),'-o',str(binary)])
result = run([str(binary)] + ([] if a.case == 'all' else [a.case]))
obj = a.evidence/'a733_tcon_lcd.o'
run([a.cc,'--target=aarch64-none-elf','-std=c11','-O2','-Wall','-Wextra','-Werror',
     '-ffreestanding','-fno-builtin','-mgeneral-regs-only','-mstrict-align',
     '-mno-outline-atomics','-fstack-usage','-I'+str(root/'include'),'-I'+str(root/'runtime/include'),
     '-c',str(root/'drivers/display/a733_tcon_lcd.c'),'-o',str(obj)])
size = run(['/usr/lib/llvm-21/bin/llvm-size',str(obj)])
undefined = run(['/usr/lib/llvm-21/bin/llvm-nm','-u',str(obj)])
record = dict(result=result, sources=pins, inputs={n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in inputs},
              reference_sha256=hashlib.sha256(reference.read_bytes()).hexdigest(),
              binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
              object_sha256=hashlib.sha256(obj.read_bytes()).hexdigest(),size=size,undefined=undefined,
              stack=obj.with_suffix('.su').read_text(),scope='Host source/model and native compilation; no physical execution')
(a.evidence/'results.json').write_text(json.dumps(record,indent=2)+'\n')
print(result,end='')
