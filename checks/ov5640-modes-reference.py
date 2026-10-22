#!/usr/bin/env python3
"""Compare every native OV5640 mode with unchanged Linux mode functions.

The reference program compiles the pinned driver's mode table, format table,
pixel-rate selection and its ov5640_set_mode direct path, then applies the
same controls and frame length as the native driver. Both programs write a
complete 64 KiB modeled register image per mode, and the reported link and
timing rates must agree as well.
"""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess

if not __debug__:
    raise SystemExit('checks require assertions; unset PYTHONOPTIMIZE and do not pass -O')
p = argparse.ArgumentParser()
p.add_argument('--source', type=pathlib.Path, required=True)
p.add_argument('--evidence', type=pathlib.Path, required=True)
p.add_argument('--cc', default='clang-21')
a = p.parse_args()
a.evidence.mkdir(parents=True, exist_ok=True)
raw = a.source.read_bytes()
assert hashlib.sha256(raw).hexdigest() == 'cdebbcd75dd5212d26a4d762144e07fbcdb9355fe9d73fb6b7883f9d76845783'
src = raw.decode()


def balanced(start):
    pos = src.index('{', start) + 1
    depth = 1
    # The selected blocks have no brace characters in literals or comments.
    while depth:
        depth += (src[pos] == '{') - (src[pos] == '}')
        pos += 1
    return pos


def function(name):
    match = re.search(r'^static[^;{}]*\b' + name + r'\([^;{}]*\)\s*\{', src, re.M)
    assert match, name
    return src[match.start():balanced(match.start())] + '\n'


def declaration(pattern):
    match = re.search(pattern, src, re.M)
    assert match, pattern
    end = balanced(match.start())
    assert src[end] == ';', pattern
    return src[match.start():end + 1] + '\n'


SIZES = ['QQVGA', 'QCIF', 'QVGA', 'VGA', 'NTSC', 'PAL', 'XGA', '720P', '1080P', 'QSXGA']
FORMATS = ['MEDIA_BUS_FMT_UYVY8_1X16', 'MEDIA_BUS_FMT_YUYV8_1X16', 'MEDIA_BUS_FMT_SBGGR8_1X8']
# Every size in every format at the reference frame length, then shorter
# and longer frames, including both ends of the admitted range.
cases = [(s, f, 0) for s in range(len(SIZES)) for f in range(len(FORMATS))]
cases += [(3, 0, 2000), (3, 1, 504), (2, 2, 3375), (8, 0, 2208), (9, 0, 3375), (7, 2, 1500)]

defines = '\n'.join(line for line in src.splitlines()
                    if re.match(r'#define OV5640_(REG_|PLL_|SYSDIV_|SCLK|PCLK|LINK_RATE|MIN_VBLANK|MAX_VTS|'
                                r'NATIVE_|PIXEL_ARRAY_)', line))
defines += '\n#define OV5640_PLL_CTRL0_MIPI_MODE_8BIT 0x08\n'
codes = sorted(set(re.findall(r'\b(MEDIA_BUS_FMT_\w+|V4L2_COLORSPACE_\w+)\b', src)))
defines += ''.join(f'#define {name} {i + 1}\n' for i, name in enumerate(codes))
types = ''.join(declaration(r'^enum ' + n + r' \{') for n in
                ['ov5640_mode_id', 'ov5640_frame_rate', 'ov5640_pixel_rate_id',
                 'ov5640_format_mux', 'ov5640_downsize_mode'])
types += declaration(r'^struct ov5640_pixfmt \{') + declaration(r'^struct reg_value \{')
types += declaration(r'^struct ov5640_timings \{') + declaration(r'^struct ov5640_mode_info \{')
data = declaration(r'^static const int ov5640_framerates\[\] = \{')
data += declaration(r'^static const u32 ov5640_pixel_rates\[\] = \{')
data += declaration(r'^static const s64 ov5640_csi2_link_freqs\[\] = \{')
data += declaration(r'^static const struct ov5640_pixfmt ov5640_dvp_formats\[\] = \{')
data += declaration(r'^static const struct ov5640_pixfmt ov5640_csi2_formats\[\] = \{')
for table in ['ov5640_init_setting', 'ov5640_setting_low_res', 'ov5640_setting_720P_1280_720',
              'ov5640_setting_1080P_1920_1080', 'ov5640_setting_QSXGA_2592_1944']:
    data += declaration(r'^static const struct reg_value ' + table + r'\[\] = \{')
data += declaration(r'^static const struct ov5640_mode_info ov5640_mode_data\[OV5640_NUM_MODES\] = \{')
names = ['ov5640_formats', 'ov5640_code_to_pixfmt', 'ov5640_code_to_bpp',
         'ov5640_compute_sys_clk', 'ov5640_calc_sys_clk', 'ov5640_set_mipi_pclk',
         'ov5640_set_timings', 'ov5640_load_regs', 'ov5640_set_autoexposure',
         'ov5640_set_autogain', 'ov5640_get_sysclk', 'ov5640_get_hts', 'ov5640_get_vts',
         'ov5640_get_light_freq', 'ov5640_set_bandingfilter', 'ov5640_set_ae_target',
         'ov5640_set_binning', 'ov5640_set_virtual_channel', 'ov5640_set_mode_direct',
         'ov5640_set_mode', 'ov5640_set_framefmt', 'ov5640_set_ctrl_white_balance',
         'ov5640_set_ctrl_hflip', 'ov5640_set_ctrl_vflip', 'ov5640_set_ctrl_light_freq',
         'ov5640_set_ctrl_vblank', '__v4l2_ctrl_vblank_update', 'ov5640_calc_pixel_rate',
         'ov5640_update_pixel_rate',
         'ov5640_set_power_mipi']
preamble = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t s32; typedef int64_t s64;
#define BIT(n) (1u<<(n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define abs(x) ((long)(x)<0?-(long)(x):(long)(x))
#define ilog2(x) (__builtin_ctz((unsigned)(x)))
#define clamp_t(t,v,lo,hi) ((t)(v)<(t)(lo)?(t)(lo):(t)(v)>(t)(hi)?(t)(hi):(t)(v))
#define WARN_ON(x) assert(!(x))
#define V4L2_CID_POWER_LINE_FREQUENCY_AUTO 3
#define V4L2_CID_POWER_LINE_FREQUENCY_50HZ 1
#define V4L2_CID_POWER_LINE_FREQUENCY_60HZ 2
#define V4L2_EXPOSURE_AUTO 0
#define V4L2_MBUS_CSI2_DPHY 5
#define dev_err(dev,...) ((void)(dev))
static unsigned virtual_channel;
struct i2c_client { int dev; };
struct v4l2_rect { unsigned left,top,width,height; };
struct v4l2_mbus_framefmt { u32 code; };
struct v4l2_ctrl { s32 val,minimum; };
static void __v4l2_ctrl_s_ctrl_int64(struct v4l2_ctrl *c,s64 v){(void)c;(void)v;}
static void __v4l2_ctrl_s_ctrl(struct v4l2_ctrl *c,s32 v){(void)c;(void)v;}
static void __v4l2_ctrl_modify_range(struct v4l2_ctrl *c,s64 lo,s64 hi,u64_t step,s64 def);
'''.replace('u64_t', 'uint64_t') + r'''
static void __v4l2_ctrl_modify_range(struct v4l2_ctrl *c,s64 lo,s64 hi,uint64_t step,s64 def){
 (void)c;(void)lo;(void)hi;(void)step;(void)def;}
'''
device = r'''
struct ov5640_dev {
 unsigned xclk_freq; s64 current_link_freq;
 struct { int bus_type; struct { struct { unsigned num_data_lanes; } mipi_csi2; } bus; } ep;
 struct v4l2_mbus_framefmt fmt;
 const struct ov5640_mode_info *current_mode,*last_mode;
 enum ov5640_frame_rate current_fr; bool pending_mode_change;
 u32 prev_sysclk,prev_hts,ae_low,ae_high,ae_target; bool upside_down;
 struct i2c_client *i2c_client;
 struct { struct v4l2_ctrl *red_balance,*blue_balance,*auto_gain,*auto_exp,*exposure,
          *pixel_rate,*link_freq,*hblank,*vblank; } ctrls;
};
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
static bool ov5640_is_csi2(const struct ov5640_dev *s) { return s->ep.bus_type==V4L2_MBUS_CSI2_DPHY; }
static const struct ov5640_timings *ov5640_timings(const struct ov5640_dev *s,const struct ov5640_mode_info *m) {
 (void)s;return &m->csi2_timings;
}
/* Paths that the selected modes never take. */
static int ov5640_set_jpeg_timings(struct ov5640_dev *s,const struct ov5640_mode_info *m) { (void)s;(void)m;abort(); }
static int ov5640_set_dvp_pclk(struct ov5640_dev *s) { (void)s;abort(); }
static int ov5640_set_mode_exposure_calc(struct ov5640_dev *s,const struct ov5640_mode_info *m) { (void)s;(void)m;abort(); }
'''
main = r'''
int main(int argc,char **argv) {
 static const u32 codes[]={%s};
 assert(argc>=3); FILE *f=fopen(argv[1],"wb"); assert(f);
 for(int i=2;i<argc;i++) {
  unsigned size,format,vtotal; assert(sscanf(argv[i],"%%u:%%u:%%u",&size,&format,&vtotal)==3);
  struct v4l2_ctrl gain={1,0},exp={V4L2_EXPOSURE_AUTO,0},exposure={0,4},other={0,0};
  struct ov5640_dev s={.xclk_freq=24000000,.ep.bus_type=V4L2_MBUS_CSI2_DPHY,
   .ep.bus.mipi_csi2.num_data_lanes=2,.fmt.code=codes[format],.ae_target=52,
   .current_mode=&ov5640_mode_data[size],.last_mode=&ov5640_mode_data[size],
   .ctrls={&other,&other,&gain,&exp,&exposure,&other,&other,&other,&other}};
  memset(regs,0x80,sizeof(regs)); regs[0x300a]=0x56;regs[0x300b]=0x40; delays=0;
  assert(!ov5640_update_pixel_rate(&s));
  /* Native packet-disable prefix, then ov5640_restore_mode. */
  ov5640_write_reg(&s,0x3008,0x42);ov5640_write_reg(&s,0x300e,0x40);ov5640_write_reg(&s,0x4202,15);
  ov5640_load_regs(&s,ov5640_init_setting,ARRAY_SIZE(ov5640_init_setting));
  assert(!ov5640_mod_reg(&s,OV5640_REG_SYS_ROOT_DIVIDER,0x3f,
   (ilog2(OV5640_SCLK2X_ROOT_DIV)<<2)|ilog2(OV5640_SCLK_ROOT_DIV)));
  assert(!ov5640_set_mode(&s)); assert(!ov5640_set_framefmt(&s,&s.fmt));
  /* Control defaults that the native driver applies. */
  assert(!ov5640_set_ctrl_white_balance(&s,1));
  assert(!ov5640_set_ctrl_hflip(&s,0));assert(!ov5640_set_ctrl_vflip(&s,0));
  assert(!ov5640_set_ctrl_light_freq(&s,V4L2_CID_POWER_LINE_FREQUENCY_50HZ));
  if(vtotal)assert(!ov5640_set_ctrl_vblank(&s,(int)(vtotal-ov5640_mode_data[size].height)));
  ov5640_write_reg(&s,OV5640_REG_PRE_ISP_TEST_SET1,0x84);
  assert(!ov5640_set_power_mipi(&s,true));
  unsigned long link=(unsigned long)s.current_link_freq;
  if(link>OV5640_LINK_RATE_MAX)link/=2;
  u16 hts,vts; ov5640_read_reg16(&s,OV5640_REG_TIMING_HTS,&hts);ov5640_read_reg16(&s,OV5640_REG_TIMING_VTS,&vts);
  printf("%%u:%%u:%%u link=%%lu pixel=%%u htotal=%%u vtotal=%%u delays=%%lu\n",size,format,vtotal,link,
   (unsigned)ov5640_get_sysclk(&s)*10000u,hts,vts,delays);
  assert(fwrite(regs,1,sizeof(regs),f)==sizeof(regs));
 }
 assert(!fclose(f)); return 0;
}
''' % ','.join(FORMATS)
ref = a.evidence / 'reference.c'
ref.write_text(preamble + defines + types + device + data + ''.join(map(function, names)) + main)
flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
arguments = [f'{s}:{f}:{v}' for s, f, v in cases]
commands = [[a.cc, *flags, str(ref), '-o', str(a.evidence / 'reference')],
            [a.cc, *flags, '-Iinclude', 'checks/ov5640-modes.c', 'drivers/camera/ov5640.c',
             '-o', str(a.evidence / 'check')],
            [str(a.evidence / 'reference'), str(a.evidence / 'reference.bin'), *arguments],
            [str(a.evidence / 'check'), str(a.evidence / 'native.bin'), *arguments]]
logs = []
for command in commands:
    result = subprocess.run(command, text=True, capture_output=True)
    logs.append({'command': command, 'returncode': result.returncode,
                 'stdout': result.stdout, 'stderr': result.stderr})
    (a.evidence / 'commands.json').write_text(json.dumps(logs, indent=2) + '\n')
    if result.returncode:
        print(result.stdout + result.stderr)
        raise SystemExit(result.returncode)
expected = (a.evidence / 'reference.bin').read_bytes()
actual = (a.evidence / 'native.bin').read_bytes()
assert len(expected) == len(actual) == 65536 * len(cases)
for i, case in enumerate(cases):
    x, y = expected[65536 * i:65536 * (i + 1)], actual[65536 * i:65536 * (i + 1)]
    assert x == y, (case, [hex(k) for k in range(65536) if x[k] != y[k]])
pattern = r'(\d+):(\d+):(\d+) link=(\d+) pixel=(\d+) (?:(\d+)x(\d+) )?htotal=(\d+) vtotal=(\d+)'
reference = [re.match(pattern, line).groups() for line in logs[2]['stdout'].splitlines()]
native = [re.match(pattern, line).groups() for line in logs[3]['stdout'].splitlines()[:len(cases)]]
rates = []
for case, r, n in zip(cases, reference, native):
    assert r[:5] + r[7:] == n[:5] + n[7:], (case, r, n)
    link, pixel, width, height, htotal, vtotal = (int(x) for x in n[3:])
    rates.append(dict(size=SIZES[case[0]], format=FORMATS[case[1]], width=width, height=height,
                      link_hz=link, pixel_hz=pixel, htotal=htotal, vtotal=vtotal,
                      frames_per_second=round(pixel / htotal / vtotal, 3)))
files = [pathlib.Path(x) for x in ['include/ov5640.h', 'drivers/camera/ov5640.c',
                                   'drivers/camera/ov5640_tables.h', 'checks/ov5640-modes.c',
                                   'checks/ov5640-modes-reference.py']] + [a.source, ref]
(a.evidence / 'results.json').write_text(json.dumps({
    'result': 'PASS', 'comparison': 'complete 65536-byte modeled sensor image per mode',
    'modes': rates, 'reference_functions': names, 'physical_execution': False,
    'inputs': {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in files},
    'images_sha256': hashlib.sha256(actual).hexdigest()}, indent=2) + '\n')
for mode in rates:
    print('{size} {format}: {width}x{height}, link {link_hz} Hz, {frames_per_second} frames/s'.format(**mode))
print(logs[3]['stdout'].splitlines()[-1])
print(f'PASS {len(cases)} modes: native images and rates match unchanged Linux mode functions')
