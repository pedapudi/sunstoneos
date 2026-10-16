#!/usr/bin/env python3
"""Compare authenticated PLL activation/retuning with the pinned A733 path."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--source-root', type=pathlib.Path, required=True)
p.add_argument('--evidence', type=pathlib.Path, required=True)
p.add_argument('--cc', default='clang-21')
a = p.parse_args()
root = pathlib.Path(__file__).resolve().parents[1]
a.evidence.mkdir(parents=True, exist_ok=False)
prefix = 'bsp/drivers/drm/phy/'
pins = {
    prefix + 'sunxi_dsi_combophy_reg.c': '3353bde71c31a43258cae12e5049ee41eed89047092c5606d3801cb13799e7b0',
    prefix + 'sunxi_dsi_combophy_reg.h': 'a4ccdf0efdf9f79ef555edd000b49665062ed6b881220c3c8099bc632e277350',
}
source = {}
for name, sha in pins.items():
    raw = (a.source_root / name).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == sha, name
    source[name] = raw.decode()


def function(text, name):
    m = re.search(r'^(?:static )?[\w *]+\b' + name + r'\([^;{}]*\)\s*\{', text, re.M)
    assert m, name
    pos, depth = m.end(), 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[m.start():pos] + '\n'


reference = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
typedef uint32_t u32; typedef uint32_t __u32;
#define IS_ENABLED(x) 1
#define DRM_ERROR(...) assert(0)
enum phy_mode { PHY_MODE_MIPI_DPHY = 1 };
struct phy_configure_opts_mipi_dphy;
'''
reference += re.sub(r'^#include.*\n', '', source[prefix + 'sunxi_dsi_combophy_reg.h'], flags=re.M)
reference += r'''
static uint32_t *trace_regs, *trace_enabled, *trace_multiplied;
static unsigned settle_count, total_delay;
static void udelay(unsigned us)
{
    total_delay += us;
    if (us == 20) {
        uint32_t *out = settle_count++ ? trace_multiplied : trace_enabled;
        assert(settle_count <= 2);
        memcpy(out, trace_regs, 128 * sizeof(uint32_t));
    } else assert(us == 5);
}
'''
for name in ['displl_clk_enable', 'displl_clk_set']:
    reference += function(source[prefix + 'sunxi_dsi_combophy_reg.c'], name)
reference += r'''
void pll_reference(const uint32_t *initial, uint32_t factors,
                   uint32_t *enabled, uint32_t *multiplied, uint32_t *final)
{
    union { struct dphy_lcd_reg regs; uint32_t words[128]; } image;
    memcpy(image.words, initial, sizeof(image.words));
    trace_regs = image.words; trace_enabled = enabled; trace_multiplied = multiplied;
    settle_count = total_delay = 0;
    struct sunxi_dphy_lcd dphy = {.reg = &image.regs};
    struct displl_div div = {.n = (factors >> 8) & 255, .p = 0,
        .m0 = ((factors >> 4) & 3) + 1, .m1 = (factors & 15) + 1,
        .m2 = ((factors >> 28) & 3) + 1, .m3 = ((factors >> 24) & 15) + 1};
    displl_clk_set(&dphy, &div);
    assert(settle_count == 2 && total_delay == 55);
    memcpy(final, image.words, sizeof(image.words));
}
'''
ref = a.evidence / 'reference.c'
ref.write_text(reference)
binary = a.evidence / 'check'
inputs = ['include/a733_display_pll.h', 'drivers/display/a733_display_pll.c',
          'checks/a733-display-pll.c', 'checks/a733-display-pll.py',
          'include/a733_dsi_phy.h', 'drivers/display/a733_dsi_phy.c',
          'include/a733_clock_leases.h', 'drivers/clock/a733_clock_leases.c',
          'runtime/include/string.h']
commands = []


def run(cmd, log):
    commands.append(cmd)
    with (a.evidence / log).open('w') as output:
        subprocess.run(cmd, cwd=root, stdout=output, stderr=subprocess.STDOUT, check=True)


run([a.cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
     '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-Iinclude',
     'checks/a733-display-pll.c', 'drivers/display/a733_display_pll.c',
     'drivers/display/a733_dsi_phy.c', 'drivers/clock/a733_clock_leases.c',
     str(ref), '-o', str(binary)], 'host-compile.log')
run([str(binary)], 'run.log')
obj = a.evidence / 'a733_display_pll.o'
run([a.cc, '--target=aarch64-none-elf', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
     '-ffreestanding', '-fno-builtin', '-mgeneral-regs-only', '-mstrict-align',
     '-mno-outline-atomics', '-fstack-usage', '-Iinclude', '-Iruntime/include',
     '-c', 'drivers/display/a733_display_pll.c', '-o', str(obj)], 'native-compile.log')
run(['/usr/lib/llvm-21/bin/llvm-size', str(obj)], 'size.log')
run(['/usr/lib/llvm-21/bin/llvm-nm', '-u', str(obj)], 'undefined.log')
record = {
    'purpose': 'Source-ordered PLL activation/retuning, intermediate rates, shared ownership and retained faults',
    'commands': commands, 'references': pins,
    'inputs': {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in inputs},
    'artifacts': {name: hashlib.sha256((a.evidence / name).read_bytes()).hexdigest()
                  for name in ['reference.c', 'check', 'a733_display_pll.o', 'a733_display_pll.su']},
    'compiler': subprocess.check_output([a.cc, '--version'], text=True),
    'result': (a.evidence / 'run.log').read_text(),
    'limits': 'No qualified hardware initial profile, analog qualification, consumer publication or board access',
}
(a.evidence / 'results.json').write_text(json.dumps(record, indent=2) + '\n')
print(record['result'], end='')
print((a.evidence / 'size.log').read_text(), end='')
