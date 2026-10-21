#!/usr/bin/env python3
"""Check inherited-PLL PHY preparation and its DSI command composition."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
if not __debug__:raise SystemExit('checks require assertions; unset PYTHONOPTIMIZE and do not pass -O')

p = argparse.ArgumentParser()
p.add_argument('--source-root', type=pathlib.Path, required=True)
p.add_argument('--command-reference', type=pathlib.Path, required=True)
p.add_argument('--evidence', type=pathlib.Path, required=True)
p.add_argument('--cc', default='clang-21')
p.add_argument('--case', choices=['all', 'admission'], default='all')
a = p.parse_args()
a.evidence.mkdir(parents=True, exist_ok=True)
root = pathlib.Path(__file__).resolve().parents[1]
prefix = 'bsp/drivers/drm/phy/'
pins = {
    prefix + 'sunxi_dsi_combophy_reg.c': '3353bde71c31a43258cae12e5049ee41eed89047092c5606d3801cb13799e7b0',
    prefix + 'sunxi_dsi_combophy_reg.h': 'a4ccdf0efdf9f79ef555edd000b49665062ed6b881220c3c8099bc632e277350',
    prefix + 'sunxi_dsi_combophy.c': '2e9a21ad8a02cc8f045c42b3a5fa0ecd014d41cb4ac3f6cf0a1b2fe168843de8',
}
src = {}
for name, expected in pins.items():
    raw = (a.source_root / name).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == expected, name
    src[name] = raw.decode()
# Reuse the preserved unchanged-source host preparation; do not rerun packets.
old = a.command_reference.read_bytes()
old_hash = hashlib.sha256(old).hexdigest()
assert old_hash == '443e698ccac37b69dc347e555c1642c9c4a27b53a9e9ef855bfcba3f781b0f1f'


def function(text, name):
    m = re.search(r'^(?:static )?[\w *]+\b' + name + r'\([^;{}]*\)\s*\{', text, re.M)
    assert m, name
    pos, depth = m.end(), 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[m.start():pos] + '\n'


shim = old.decode() + r'''
#include "a733_dsi_phy.h"
typedef uint32_t __u32; typedef uint64_t u64;
#define IS_ENABLED(x) 1
#define DIV_ROUND_CLOSEST(n,d) (((n) + (d) / 2) / (d))
#define DRM_ERROR(...) ((void)0)
enum phy_mode { PHY_MODE_MIPI_DPHY = 1 };
struct phy_configure_opts_mipi_dphy { unsigned lanes; };
static unsigned delay_total;
static void udelay(unsigned us) { delay_total += us; }
'''
shim += re.sub(r'^#include.*\n', '', src[prefix + 'sunxi_dsi_combophy_reg.h'], flags=re.M)
shim += r'''
struct dsi_combophy_data { unsigned id; struct combophy_config phy_config[2]; };
struct clk_hw { unsigned unused; };
struct ccu_nm {
    struct clk_hw hw;
    unsigned vco_enable;
    unsigned long min_rate;
    struct { struct sunxi_dphy_lcd dphy_lcd; } common;
};
#define hw_to_ccu_nm(hw) ((struct ccu_nm *)(hw))
'''
shim += re.search(r'^static const struct dsi_combophy_data sun60iw2_data0 = \{.*?^\};',
                  src[prefix + 'sunxi_dsi_combophy.c'], re.M | re.S)[0] + '\n'
for name in ['get_displl_vco', 'displl_clk_enable', 'displl_clk_set',
             'sunxi_dsi_io_open', 'sunxi_dsi_dphy_cfg', 'sunxi_dsi_lane_set']:
    shim += function(src[prefix + 'sunxi_dsi_combophy_reg.c'], name)
shim += function(src[prefix + 'sunxi_dsi_combophy.c'], 'sunxi_displl_clk_set_rate')
shim += r'''
void phy_reference_factors(uint32_t hs, uint32_t ls, uint32_t factors)
{
    struct dphy_lcd_reg regs = {0};
    regs.dphy_dbg0.bits.lock = 1;
    regs.dphy_pll_reg0.bits.n = 60;
    struct ccu_nm clock = {.vco_enable = 1, .min_rate = 1260000000,
        .common = {.dphy_lcd = {.reg = &regs}}};
    assert(!sunxi_displl_clk_set_rate(&clock.hw, hs, 24000000));
    clock.vco_enable = 0;
    assert(!sunxi_displl_clk_set_rate(&clock.hw, ls, 24000000));
    assert((regs.dphy_pll_reg0.dwval & 0x3f0fff3f) == factors);
}
void phy_reference_compare(const uint32_t *initial_phy, const uint32_t *actual_phy,
                           const uint32_t *initial_host, const uint32_t *actual_host,
                           unsigned lanes)
{
    union { struct dphy_lcd_reg regs; uint32_t full[128]; } phy;
    memcpy(phy.full, initial_phy, sizeof(phy.full));
    struct sunxi_dphy_lcd dphy = {.reg = &phy.regs,
        .phy_config = (struct combophy_config *)&sun60iw2_data0.phy_config[0]};
    delay_total = 0;
    assert(!sunxi_dsi_io_open(&dphy));
    assert(delay_total == 1);
    assert(!sunxi_dsi_dphy_cfg(&dphy));
    assert(!sunxi_dsi_lane_set(&dphy, lanes));
    assert(!memcmp(phy.full, actual_phy, sizeof(phy.full)));
    struct dsi_lcd_reg host;
    memcpy(&host, initial_host, sizeof(host));
    struct sunxi_dsi_lcd dsi = {.reg = &host};
    struct disp_dsi_para para = {.lanes = lanes, .mode_flags = MIPI_DSI_MODE_COMMAND,
        .timings = {.y_res = 1}};
    assert(!dsi_basic_cfg(&dsi, &para));
    /* Compare every word the native LP-only preparer writes. The source also
     * prepares unused HS/video instructions outside this component's scope. */
    static const unsigned offsets[] = {0x10, 0x14, 0x60, 0x78, 0x20, 0x30,
        0x44, 0x54, 0x4c, 0x50, 0x2f8, 0};
    for (unsigned i = 0; i != sizeof(offsets)/sizeof(*offsets); ++i) {
        uint32_t expected;
        memcpy(&expected, (uint8_t *)&host + offsets[i], 4);
        assert(expected == actual_host[offsets[i] / 4]);
    }
    assert((host.dsi_inst_loop_sel.dwval & 0x000f000f) ==
           (actual_host[0x40 / 4] & 0x000f000f));
}
'''
reference = a.evidence / 'reference.c'
reference.write_text(shim)
binary = a.evidence / 'check'
cmd = [a.cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra',
       '-Wno-unused-variable', '-Wno-unused-but-set-variable', '-Wno-sign-compare',
       '-Wno-uninitialized', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
       '-I' + str(root / 'include'), str(root / 'checks/a733-dsi-phy.c'),
       str(root / 'drivers/display/a733_dsi_phy.c'), str(root / 'drivers/display/a733_dsi.c'),
       str(reference), '-o', str(binary)]
with (a.evidence / 'compile.log').open('w') as log:
    subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, check=True)
with (a.evidence / 'run.log').open('w') as log:
    run_cmd = [str(binary)] + ([] if a.case == 'all' else [a.case])
    subprocess.run(run_cmd, stdout=log, stderr=subprocess.STDOUT, check=True)
inputs = ['include/a733_dsi_phy.h', 'drivers/display/a733_dsi_phy.c',
          'checks/a733-dsi-phy.c', 'checks/a733-dsi-phy-reference.py',
          'include/a733_dsi.h', 'drivers/display/a733_dsi.c']
record = {'purpose': 'Inherited PLL/analog/LP preparation, deadlines and first command composition',
          'command': cmd, 'run_command': run_cmd, 'sources': pins, 'command_reference_sha256': old_hash,
          'inputs': {n: hashlib.sha256((root / n).read_bytes()).hexdigest() for n in inputs},
          'reference_sha256': hashlib.sha256(reference.read_bytes()).hexdigest(),
          'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
          'compiler': subprocess.check_output([a.cc, '--version'], text=True),
          'result': (a.evidence / 'run.log').read_text(),
          'limitations': 'Host model only; cold PLL activation, signaling and physical qualification remain open'}
(a.evidence / 'results.json').write_text(json.dumps(record, indent=2) + '\n')
print(record['result'], end='')
