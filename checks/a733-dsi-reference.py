#!/usr/bin/env python3
"""Compare native command packets/register images with unchanged source functions."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--source-root', type=pathlib.Path, required=True)
parser.add_argument('--evidence', type=pathlib.Path, required=True)
parser.add_argument('--cc', default='clang-21')
args = parser.parse_args()
args.evidence.mkdir(parents=True, exist_ok=True)
root = pathlib.Path(__file__).resolve().parents[1]
prefix = 'bsp/drivers/drm/sunxi_device/hardware/lowlevel_lcd/'
pins = {
    prefix + 'dsi_v1.c': 'd00bd765f6c9a97f120ecae9610f53a531d4d7dfe99b07b9bf453bf48ab08880',
    prefix + 'dsi_v1.h': '1a51d36b0cb3f9b49b930771ebe3ac29873930f5a1d3216d25960ea8a154c972',
    prefix + 'dsi_v1_type.h': 'bf93642dddd434ca152467bb21a5e9e9e92f47512f04ab5d22c5bc90851c3159',
    'drivers/gpu/drm/sun4i/sun6i_mipi_dsi.c': 'a9f359b8480f456fb224086811b564d43d15c0b735d67898fe9c2779b6c20672',
    'drivers/gpu/drm/drm_mipi_dsi.c': '27aaa644e3484c17a43fb815ee6305fe0449eec8f2cdc2aa8b24cd441ba0ec1c',
    'include/video/mipi_display.h': '36fc5778eda7503f25df5d75dcd8dd70e9bf7df20b387b3adcd05185391f23ab',
}
sources = {}
for name, expected in pins.items():
    raw = (args.source_root / name).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == expected, name
    sources[name] = raw.decode()


def function(src, name):
    match = re.search(r'^(?:static )?[\w *]+\b' + name + r'\([^;{}]*\)\s*\{', src, re.M)
    assert match, name
    pos, depth = match.end(), 1
    # Selected functions contain no braces in string literals or comments.
    while depth:
        depth += (src[pos] == '{') - (src[pos] == '}')
        pos += 1
    return src[match.start():pos] + '\n'


src = sources[prefix + 'dsi_v1.c']
header = sources[prefix + 'dsi_v1.h']
types = re.sub(r'^#include.*\n', '', sources[prefix + 'dsi_v1_type.h'], flags=re.M)
enums = '\n'.join(re.findall(r'enum __dsi_\w+\s*\{.*?\};', header, re.S))
defines = '\n'.join(line for line in header.splitlines() if line.startswith('#define MIPI_DSI_'))
common = sources['drivers/gpu/drm/drm_mipi_dsi.c']
other = sources['drivers/gpu/drm/sun4i/sun6i_mipi_dsi.c']
preamble = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "a733_dsi.h"
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t s32;
#define BIT(n) (1u << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define printk(...) ((void)0)
static void dsi_delay_us(unsigned n) { (void)n; abort(); }
struct disp_video_timings {
    u32 x_res, y_res, pixel_clk, hor_total_time, hor_back_porch;
    u32 hor_front_porch, hor_sync_time, ver_total_time, ver_back_porch;
    u32 ver_front_porch, ver_sync_time;
};
struct disp_dsi_para {
    unsigned lanes, dsi_div, format;
    unsigned long mode_flags;
    struct disp_video_timings timings;
};
struct mipi_dsi_msg { u8 channel, type; size_t tx_len; const void *tx_buf; };
struct mipi_dsi_packet { size_t size; u8 header[4]; size_t payload_length; const u8 *payload; };
'''
shim = preamble + types + enums + '\n' + defines + '\n'
shim += sources['include/video/mipi_display.h']
shim += r'''
struct sunxi_dsi_lcd { volatile struct dsi_lcd_reg *reg; };
u32 dsi_pixel_bits[4] = {24, 24, 18, 16};
u32 dsi_lane_den[4] = {1, 3, 7, 15};
'''
for name in ['dsi_basic_cfg', 'dsi_start', 'dsi_dcs_wr', 'dsi_ecc_pro', 'dsi_crc_pro']:
    shim += function(src, name)
shim += re.search(r'static const u32 sun6i_dsi_ecc_array\[\] = \{.*?\n\};', other, re.S)[0] + '\n'
shim += function(other, 'sun6i_dsi_ecc_compute')
for name in ['mipi_dsi_packet_format_is_short', 'mipi_dsi_packet_format_is_long', 'mipi_dsi_create_packet']:
    shim += function(common, name)
shim += r'''
void reference_initial(uint32_t *out, unsigned lanes)
{
    struct dsi_lcd_reg regs = {0};
    struct sunxi_dsi_lcd dsi = {.reg = &regs};
    struct disp_dsi_para para = {.lanes = lanes, .dsi_div = 6,
        .mode_flags = MIPI_DSI_MODE_COMMAND, .format = 0,
        .timings = {.x_res = 640, .y_res = 480, .pixel_clk = 25000000,
            .hor_total_time = 800, .hor_back_porch = 48, .hor_front_porch = 16,
            .hor_sync_time = 96, .ver_total_time = 525, .ver_back_porch = 33,
            .ver_front_porch = 10, .ver_sync_time = 2}};
    assert(sizeof(regs) == 0x400);
    assert(!dsi_basic_cfg(&dsi, &para));
    memcpy(out, &regs, sizeof(regs));
}
void reference_packet(const struct a733_dsi_packet *actual, unsigned type,
                      unsigned channel, const void *payload, size_t length)
{
    struct mipi_dsi_msg msg = {.type = type, .channel = channel,
        .tx_buf = payload, .tx_len = length};
    struct mipi_dsi_packet packet;
    uint8_t expected[256] = {0};
    assert(!mipi_dsi_create_packet(&packet, &msg));
    memcpy(expected, packet.header, 4);
    u32 header = expected[0] | (u32)expected[1] << 8 | (u32)expected[2] << 16;
    expected[3] = sun6i_dsi_ecc_compute(header);
    /* The board helper leaves bits 6/7 undefined; compare its six parity bits. */
    assert((dsi_ecc_pro(header) & 0x3f) == expected[3]);
    size_t size = 4;
    if (packet.payload_length) {
        memcpy(expected + 4, packet.payload, packet.payload_length);
        /* The common packet length and upstream long-write path define length.
         * Do not call the board wrapper that reads payload_length + 1 bytes. */
        u16 checksum = dsi_crc_pro((u8 *)packet.payload, packet.payload_length);
        size += packet.payload_length;
        expected[size++] = checksum;
        expected[size++] = checksum >> 8;
    }
    assert(actual->size == size);
    assert(!memcmp(actual->bytes, expected, sizeof(expected)));
}
void reference_registers(const uint32_t *initial, const uint32_t *actual,
                         const struct a733_dsi_packet *packet)
{
    struct dsi_lcd_reg regs;
    memcpy(&regs, initial, sizeof(regs));
    struct sunxi_dsi_lcd dsi = {.reg = &regs};
    assert(!dsi_dcs_wr(&dsi, (u8 *)packet->bytes, packet->size));
    assert(regs.dsi_basic_ctl0.bits.inst_st == 1);
    /* Model hardware completion after the unchanged source start sequence. */
    regs.dsi_basic_ctl0.bits.inst_st = 0;
    assert(!memcmp(&regs, actual, sizeof(regs)));
}
'''
reference = args.evidence / 'reference.c'
reference.write_text(shim)
binary = args.evidence / 'check'
command = [args.cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra',
           '-Wno-unused-variable', '-Wno-sign-compare', '-Wno-uninitialized',
           '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
           '-I' + str(root / 'include'), str(root / 'checks/a733-dsi.c'),
           str(root / 'drivers/display/a733_dsi.c'), str(reference), '-o', str(binary)]
with (args.evidence / 'compile.log').open('w') as log:
    subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
with (args.evidence / 'run.log').open('w') as log:
    subprocess.run([str(binary)], stdout=log, stderr=subprocess.STDOUT, check=True)
inputs = ['include/a733_dsi.h', 'drivers/display/a733_dsi.c',
          'checks/a733-dsi.c', 'checks/a733-dsi-reference.py']
result = {
    'purpose': 'Packet framing, prepared-host validation, bounded completion and retained failures',
    'command': command, 'sources': pins,
    'inputs': {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in inputs},
    'reference_sha256': hashlib.sha256(reference.read_bytes()).hexdigest(),
    'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
    'compiler': subprocess.check_output([args.cc, '--version'], text=True),
    'result': (args.evidence / 'run.log').read_text(),
    'limitations': 'Host model only; no PHY signaling, panel response, display scanout or physical qualification',
}
(args.evidence / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
print(result['result'], end='')
