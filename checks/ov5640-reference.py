#!/usr/bin/env python3
"""Compare a native sensor image with isolated, unchanged Linux functions."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--source', type=pathlib.Path, required=True)
p.add_argument('--evidence', type=pathlib.Path, required=True)
p.add_argument('--cc', default='clang-21')
a = p.parse_args()
a.evidence.mkdir(parents=True, exist_ok=True)
raw = a.source.read_bytes()
assert hashlib.sha256(raw).hexdigest() == 'cdebbcd75dd5212d26a4d762144e07fbcdb9355fe9d73fb6b7883f9d76845783'
src = raw.decode()

def function(name):
    match = re.search(r'^static[^;{}]*\b' + name + r'\([^;{}]*\)\s*\{', src, re.M)
    assert match, name
    start = match.start()
    pos = match.end()
    depth = 1
    # These selected functions have no brace characters in literals/comments.
    while depth:
        depth += (src[pos] == '{') - (src[pos] == '}')
        pos += 1
    return src[start:pos] + '\n'

names = ['ov5640_compute_sys_clk', 'ov5640_calc_sys_clk', 'ov5640_set_mipi_pclk',
         'ov5640_set_timings', 'ov5640_load_regs', 'ov5640_get_sysclk',
         'ov5640_get_hts', 'ov5640_get_vts', 'ov5640_set_bandingfilter',
         'ov5640_set_ae_target', 'ov5640_set_binning', 'ov5640_set_virtual_channel',
         'ov5640_set_framefmt', 'ov5640_set_ctrl_white_balance',
         'ov5640_set_ctrl_hflip', 'ov5640_set_ctrl_vflip',
         'ov5640_set_ctrl_light_freq', 'ov5640_set_power_mipi']
defines = '\n'.join(line for line in src.splitlines()
                    if re.match(r'#define OV5640_(REG_|PLL_|SYSDIV_|SCLK|PCLK|LINK_RATE)', line))
# Include the one PLL mode macro that is not in the above prefix groups.
defines += '\n#define OV5640_PLL_CTRL0_MIPI_MODE_8BIT 0x08\n'
tables = '\n'.join(re.search(r'static const struct reg_value ' + n +
                            r'\[\] = \{.*?\n\};', src, re.S)[0]
                   for n in ['ov5640_init_setting', 'ov5640_setting_low_res'])
preamble = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define BIT(n) (1u<<(n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define abs(x) ((long)(x)<0?-(long)(x):(long)(x))
#define ilog2(x) (__builtin_ctz((unsigned)(x)))
#define MEDIA_BUS_FMT_JPEG_1X8 1
#define V4L2_CID_POWER_LINE_FREQUENCY_AUTO 3
#define V4L2_CID_POWER_LINE_FREQUENCY_50HZ 1
#define dev_err(dev,...) ((void)(dev))
static unsigned virtual_channel;
struct i2c_client { int dev; };
struct v4l2_rect { unsigned left,top,width,height; };
struct ov5640_timings { struct v4l2_rect analog_crop,crop; unsigned htot,vblank_def; };
struct ov5640_mode_info { unsigned width,height; struct ov5640_timings csi2_timings; };
struct v4l2_mbus_framefmt { unsigned code; };
struct ov5640_pixfmt { u8 ctrl00,mux; };
struct ctrl { unsigned val; };
struct ov5640_dev {
 unsigned xclk_freq; unsigned long current_link_freq;
 struct { struct { struct { unsigned num_data_lanes; } mipi_csi2; } bus; } ep;
 struct v4l2_mbus_framefmt fmt;
 unsigned prev_sysclk,prev_hts,ae_low,ae_high; bool upside_down;
 struct i2c_client *i2c_client;
 struct { struct ctrl *red_balance,*blue_balance; } ctrls;
};
struct reg_value { u16 reg_addr; u8 val,mask; u32 delay_ms; };
static u8 regs[65536];
static unsigned long delays;
static void usleep_range(unsigned long lo,unsigned long hi) { assert(hi>=lo); delays+=lo; }
static int ov5640_write_reg(struct ov5640_dev *s,u16 r,u8 v) { (void)s;regs[r]=v;return 0; }
static int ov5640_read_reg(struct ov5640_dev *s,u16 r,u8 *v) { (void)s;*v=regs[r];return 0; }
static int ov5640_write_reg16(struct ov5640_dev *s,u16 r,u16 v) {
 ov5640_write_reg(s,r,v>>8); return ov5640_write_reg(s,r+1,v);
}
static int ov5640_read_reg16(struct ov5640_dev *s,u16 r,u16 *v) {
 (void)s;*v=((u16)regs[r]<<8)|regs[r+1];return 0;
}
static int ov5640_mod_reg(struct ov5640_dev *s,u16 r,u8 mask,u8 v) {
 u8 old; ov5640_read_reg(s,r,&old); return ov5640_write_reg(s,r,(old&~mask)|(v&mask));
}
static bool ov5640_is_csi2(struct ov5640_dev *s) { (void)s; return true; }
static int ov5640_set_jpeg_timings(struct ov5640_dev *s,const struct ov5640_mode_info *m) {
 (void)s;(void)m;abort();
}
static const struct ov5640_timings *ov5640_timings(struct ov5640_dev *s,const struct ov5640_mode_info *m) {
 (void)s;return &m->csi2_timings;
}
static const struct ov5640_pixfmt *ov5640_code_to_pixfmt(struct ov5640_dev *s,unsigned code) {
 static const struct ov5640_pixfmt f={0x3f,0}; (void)s;assert(code==2);return &f;
}
'''
main = r'''
int main(int argc,char **argv) {
 assert(argc==2); memset(regs,0x80,sizeof(regs)); regs[0x300a]=0x56;regs[0x300b]=0x40;
 struct ov5640_dev s={.xclk_freq=24000000,.current_link_freq=192000000,
                     .ep.bus.mipi_csi2.num_data_lanes=2,.fmt.code=2};
 const struct ov5640_mode_info m={640,480,{{16,14,2592,1944},{2,4,640,480},1600,520}};
 ov5640_write_reg(&s,0x3008,0x42);ov5640_write_reg(&s,0x300e,0x40);ov5640_write_reg(&s,0x4202,15);
 ov5640_load_regs(&s,ov5640_init_setting,ARRAY_SIZE(ov5640_init_setting));
 ov5640_mod_reg(&s,OV5640_REG_SYS_ROOT_DIVIDER,0x3f,1);
 ov5640_mod_reg(&s,OV5640_REG_AEC_PK_MANUAL,3,3);
 assert(!ov5640_set_mipi_pclk(&s));
 ov5640_load_regs(&s,ov5640_setting_low_res,ARRAY_SIZE(ov5640_setting_low_res));
 assert(!ov5640_set_timings(&s,&m));
 ov5640_mod_reg(&s,OV5640_REG_AEC_PK_MANUAL,3,0);
 assert(!ov5640_set_binning(&s,true));assert(!ov5640_set_ae_target(&s,52));
 assert(!ov5640_set_bandingfilter(&s));assert(!ov5640_set_virtual_channel(&s));
 assert(!ov5640_set_framefmt(&s,&s.fmt));assert(!ov5640_set_ctrl_white_balance(&s,1));
 assert(!ov5640_set_ctrl_hflip(&s,0));assert(!ov5640_set_ctrl_vflip(&s,0));
 assert(!ov5640_set_ctrl_light_freq(&s,V4L2_CID_POWER_LINE_FREQUENCY_50HZ));
 ov5640_write_reg(&s,OV5640_REG_PRE_ISP_TEST_SET1,0x84);
 assert(!ov5640_set_power_mipi(&s,true));assert(delays==300500);
 FILE *f=fopen(argv[1],"wb");assert(f);assert(fwrite(regs,1,sizeof(regs),f)==sizeof(regs));assert(!fclose(f));
 return 0;
}
'''
ref = a.evidence / 'reference.c'
ref.write_text(preamble + defines + '\n' + tables + '\n' + ''.join(map(function,names)) + main)
flags = ['-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer']
commands = [[a.cc,*flags,str(ref),'-o',str(a.evidence/'reference')],
            [a.cc,*flags,'-Iinclude','checks/ov5640.c','drivers/camera/ov5640.c','-o',str(a.evidence/'check')],
            [str(a.evidence/'reference'),str(a.evidence/'reference.bin')],
            [str(a.evidence/'check'),str(a.evidence/'native.bin')]]
logs=[]
for command in commands:
    result=subprocess.run(command,text=True,capture_output=True)
    logs.append({'command':command,'returncode':result.returncode,'stdout':result.stdout,'stderr':result.stderr})
    (a.evidence/'commands.json').write_text(json.dumps(logs,indent=2)+'\n')
    if result.returncode:
        print(result.stdout+result.stderr);raise SystemExit(result.returncode)
expected=(a.evidence/'reference.bin').read_bytes(); actual=(a.evidence/'native.bin').read_bytes()
diffs=[hex(i) for i,(x,y) in enumerate(zip(expected,actual)) if x!=y]
assert len(expected)==len(actual)==65536 and not diffs,diffs
files=[pathlib.Path(x) for x in ['include/ov5640.h','drivers/camera/ov5640.c','drivers/camera/ov5640_tables.h',
                                'checks/ov5640.c','checks/ov5640-reference.py']]+[a.source,ref]
result={'result':'PASS','comparison':'complete 65536-byte modeled sensor image',
        'reference_functions':names,'sensor_mode':'24 MHz XCLK; 640x480 UYVY; two lanes; 192 MHz link',
        'physical_execution':False,'inputs':{str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in files},
        'image_sha256':hashlib.sha256(actual).hexdigest()}
(a.evidence/'results.json').write_text(json.dumps(result,indent=2)+'\n')
print(logs[-1]['stdout'].strip())
print('PASS unchanged Linux clock/timing/control functions and full sensor image comparison')
