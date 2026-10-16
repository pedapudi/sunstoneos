#!/usr/bin/env python3
"""Compare bounded video preparation and start/stop with pinned display source."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--source-root', type=pathlib.Path, required=True)
p.add_argument('--command-reference', type=pathlib.Path, required=True)
p.add_argument('--evidence', type=pathlib.Path, required=True)
p.add_argument('--cc', default='clang-21')
p.add_argument('--case', choices=['all', 'admission'], default='all')
a = p.parse_args()
a.evidence.mkdir(parents=True, exist_ok=True)
root = pathlib.Path(__file__).resolve().parents[1]
source_name = 'bsp/drivers/drm/sunxi_device/hardware/lowlevel_lcd/dsi_v1.c'
raw = (a.source_root / source_name).read_bytes()
assert hashlib.sha256(raw).hexdigest() == 'd00bd765f6c9a97f120ecae9610f53a531d4d7dfe99b07b9bf453bf48ab08880'
old = a.command_reference.read_bytes()
assert hashlib.sha256(old).hexdigest() == '443e698ccac37b69dc347e555c1642c9c4a27b53a9e9ef855bfcba3f781b0f1f'


def function(text, name):
    m = re.search(r'^(?:static )?[\w *]+\b' + name + r'\([^;{}]*\)\s*\{', text, re.M)
    assert m, name
    pos, depth = m.end(), 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[m.start():pos] + '\n'


# Only the host shims gain the source's single/dual-link selector. Existing
# functions are unchanged, and no LP packet corpus is rerun.
shim = old.decode().replace('unsigned lanes, dsi_div, format;',
                            'unsigned lanes, dsi_div, format, dual_dsi;')
shim += '#include "a733_dsi_video.h"\n'
shim += 'static unsigned delay_ms; static void dsi_delay_ms(unsigned n) { delay_ms += n; }\n'
for name in ['dsi_crc_pro_pd_repeat', 'dsi_packet_cfg', 'dsi_open_hs_mode', 'dsi_close']:
    shim += function(raw.decode(), name)
shim += r'''
void video_reference(const struct a733_dsi_video_timing *t, const uint32_t *initial,
                     const uint32_t *actual)
{
    struct dsi_lcd_reg regs;
    memcpy(&regs, initial, sizeof(regs));
    struct sunxi_dsi_lcd dsi = {.reg = &regs};
    struct disp_dsi_para para = {.lanes = 4, .dsi_div = 6, .format = t->format,
        .mode_flags = MIPI_DSI_MODE_VIDEO | (t->mode == A733_DSI_BURST ? MIPI_DSI_MODE_VIDEO_BURST : 0),
        .timings = {.pixel_clk = t->ls_hz, .x_res = t->width, .y_res = t->height,
            .hor_sync_time = t->hsync, .hor_back_porch = t->hback, .hor_front_porch = t->hfront,
            .hor_total_time = t->width + t->hsync + t->hback + t->hfront,
            .ver_sync_time = t->vsync, .ver_back_porch = t->vback, .ver_front_porch = t->vfront,
            .ver_total_time = t->height + t->vsync + t->vback + t->vfront}};
    assert(!dsi_basic_cfg(&dsi, &para));
    assert(!dsi_packet_cfg(&dsi, &para));
    struct a733_dsi_video_plan plan;
    assert(a733_dsi_video_plan(t, &plan));
    assert(plan.count == 42);
    for (unsigned offset = 0; offset != 1024; offset += 4) {
        /* Source also prepares unused extended instructions 8..14. */
        if ((offset >= 0x120 && offset <= 0x138) || offset == 0x140) continue;
        uint32_t expected; memcpy(&expected, (uint8_t *)&regs + offset, 4);
        uint32_t mask = UINT32_MAX;
        /* Published ECC helper leaves its reserved top two bits uninitialized.
         * Compare all defined parity/header bits; native reserved bits are zero. */
        if (offset == 0x90 || (offset >= 0xb0 && offset <= 0xbc) ||
            offset == 0xc0 || offset == 0xc8 || offset == 0xd0 || offset == 0xe0 || offset == 0xe8) {
            mask = 0x3fffffff; assert(!(actual[offset / 4] & ~mask));
        }
        if ((expected & mask) != (actual[offset / 4] & mask)) {
            fprintf(stderr, "offset %x expected %08x actual %08x\n", offset, expected, actual[offset / 4]);
            abort();
        }
    }
}
void video_reference_start(const uint32_t *initial, const uint32_t *actual)
{
    struct dsi_lcd_reg regs; memcpy(&regs, initial, sizeof(regs));
    struct sunxi_dsi_lcd dsi = {.reg = &regs};
    struct disp_dsi_para para = {0};
    assert(!dsi_open_hs_mode(&dsi, &para));
    assert(!memcmp(&regs, actual, sizeof(regs)));
}
void video_reference_stop(const uint32_t *initial, const uint32_t *actual)
{
    struct dsi_lcd_reg regs; memcpy(&regs, initial, sizeof(regs));
    struct sunxi_dsi_lcd dsi = {.reg = &regs};
    delay_ms = 0; assert(!dsi_close(&dsi)); assert(delay_ms == 30);
    /* Model hardware has separately observed the conditional graph reach END. */
    regs.dsi_basic_ctl0.bits.inst_st = 0;
    assert(!memcmp(&regs, actual, sizeof(regs)));
}
'''
reference = a.evidence / 'reference.c'
reference.write_text(shim)
binary = a.evidence / 'check'
cmd = [a.cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra',
       '-Wno-unused-variable', '-Wno-unused-but-set-variable', '-Wno-sign-compare',
       '-Wno-uninitialized', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
       '-I' + str(root / 'include'), str(root / 'checks/a733-dsi-video.c'),
       str(root / 'drivers/display/a733_dsi_video.c'), str(root / 'drivers/display/a733_dsi_phy.c'),
       str(root / 'drivers/display/a733_dsi.c'), str(reference), '-o', str(binary)]
with (a.evidence / 'compile.log').open('w') as log:
    subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, check=True)
with (a.evidence / 'run.log').open('w') as log:
    run_cmd = [str(binary)] + ([] if a.case == 'all' else [a.case])
    subprocess.run(run_cmd, stdout=log, stderr=subprocess.STDOUT, check=True)
inputs = ['include/a733_dsi_video.h', 'drivers/display/a733_dsi_video.c',
          'checks/a733-dsi-video.c', 'checks/a733-dsi-video-reference.py',
          'include/a733_dsi.h', 'drivers/display/a733_dsi.c',
          'include/a733_dsi_phy.h', 'drivers/display/a733_dsi_phy.c']
record = {'purpose': 'Continuous-clock four-lane video setup and bounded engine lifecycle',
          'command': cmd, 'run_command': run_cmd, 'source_sha256': hashlib.sha256(raw).hexdigest(),
          'command_reference_sha256': hashlib.sha256(old).hexdigest(),
          'inputs': {n: hashlib.sha256((root / n).read_bytes()).hexdigest() for n in inputs},
          'reference_sha256': hashlib.sha256(reference.read_bytes()).hexdigest(),
          'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
          'compiler': subprocess.check_output([a.cc, '--version'], text=True),
          'result': (a.evidence / 'run.log').read_text(),
          'limitations': 'Host model only; PLL/producer/panel integration and physical qualification remain open'}
(a.evidence / 'results.json').write_text(json.dumps(record, indent=2) + '\n')
print(record['result'], end='')
