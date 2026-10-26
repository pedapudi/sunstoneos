#!/usr/bin/env python3
"""Check the video PLL profile against pinned sources and the board dump, then run the decoder cold-start model."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home()/'.cache/sunstoneos'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--clang', default='clang')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
paths = {
    'manual': CACHE/'sources/camera-research/A733_User_Manual_V1.00.txt',
    'ccu': CACHE/'sources/board-kernel/ccu-sun60iw2.c',
    'nm': CACHE/'sources/i2s-platform/ccu_nm.c',
    'gate': CACHE/'sources/i2s-platform/ccu_gate.c',
    'common': CACHE/'sources/i2s-platform/ccu_common.c',
    'cedar': CACHE/'sources/video-codecs/cedar_ve.c',
    'plat': CACHE/'sources/video-codecs/ve_plat_sun60iw2.c',
    'dtsi': CACHE/'sources/dma-platform/sun60iw2p1.dtsi',
    'vf': CACHE/'sources/npu-platform/sun60iw2p1-cpu-vf.dtsi',
    'dump': CACHE/'evidence/board-session-20261025-24/port-3-3/clock-audit-b/session.json',
    'rom': CACHE/'sources/brom/ns-brom.bin',
    'jpeg': CACHE/'video-codecs/fixtures/baseline-48x32.jpg',
}
expected = {
    'manual': '219d3c21e5377d18a096765f429555d0693f7fa9b2ae2eb5bb3f225d8b6a09c2',
    'ccu': '1d32bb8555f8ff205d35210f638f9d42cc28aa9295c46963aa1c810d47b06d77',
    'nm': '105b20f6d9826c34b27ef5aa9bf200ecad1e268308cc65bd52e06e254376631f',
    'gate': '3d8c88d8b00d83b64819a88e6cacaa80eef941dd4d1c164d00238ba29054cd51',
    'common': 'bbeebb34c55060ea74b973713e35557927a0fa06b3d2597312620bf46ca21185',
    'cedar': '774558140b78e743925f6e26ffcfe3e86751963ace27b208530b471d8c303f7b',
    'plat': 'cda151be0ba896a685444f85fbb14264635510130721bfe12c2f97c8d6d23bdf',
    'dtsi': '1c7313f655bdc4d018201664adcb1fa1d100280388b8975e193ebbfc6ad4ddfa',
    'vf': 'd5535630db3d0acdeace832d1dfad684fab9ab0c31c482b493e36a7f3387e5ad',
    'dump': 'd9335d5957e8a61bba56c8cfb615a508288604d6478c8095a7848462263fa261',
    'rom': '95be69c916846a8259578565f3193a6b9ab0625294d5374f3dc6c2f667fc6169',
    'jpeg': '6522f2719f1f05e23a95aa71a18d52f2e91b19b4b6c94381b4100de4c79fd573',
}
for key, path in paths.items():
    assert hashlib.sha256(path.read_bytes()).hexdigest() == expected[key], key
s = {key: path.read_text() for key, path in paths.items() if key not in ('rom', 'jpeg', 'dump')}

def ordered(text, terms):
    positions = [text.index(term) for term in terms]
    assert positions == sorted(positions), terms

# The manual: enable method 2 and the PLL_VE0 word.
manual = s['manual']
method2 = manual[manual.index('Method 2:'):manual.index('Method 2:')+2000]
ordered(method2, ['Step 1   Set PLL_LDO_EN', 'Step 2   Set the values of N', 'Step 3   Set PLL_EN',
                  'Step 4   Set LOCK_ENABLE', 'Step 5   Wait the LOCK', 'Step 6   Wait 20 us',
                  'Step 7   Set the PLL_OUTPUT_GATE'])
ve0 = manual[manual.index('4.1.6.36          0x0220 PLL_VE0 Control Register'):manual.index('4.1.6.37 ')]
assert '(Default Value: 0x4030_5A00)' in ve0 and 'InputFreq*N/M1/P0' in ve0
for term in ['P0=PLL_OUTPUT_DIV + 1', 'N= PLL_N +1', 'M1=PLL_INPUT_DIV + 1', '26:23     /']:
    assert term in ve0, term
for bit, name in [(31, 'PLL_EN'), (30, 'PLL_LDO_EN'), (29, 'LOCK_ENABLE'), (27, 'PLL_OUTPUT_GATE')]:
    assert re.search(name + r'\.?[^\f]*?\b' + str(bit) + r'\s+R/W', ve0, re.S), name
assert re.search(r'28\s+R\s', ve0)

# The board kernel: the PLLs, the module clocks' parent order and the rate rule.
ccu = s['ccu']
for reg, name in [('0x0220', 'pll_ve0_clk'), ('0x0240', 'pll_ve1_clk')]:
    body = re.search(r'static struct ccu_nm ' + name + r' = \{(.*?)\n\};', ccu, re.S).group(1)
    for term in ['_SUNXI_CCU_MULT_MIN_MAX(8, 8, 53, 105)', '_SUNXI_CCU_DIV(20, 3)', '.reg\t\t= ' + reg,
                 '"pll-ref"', 'CLK_SET_RATE_UNGATE', '.lock\t\t= BIT(28)', '.output\t\t= BIT(27)']:
        assert term in body, (name, term)
parents = {n: re.search(n + r'\[\] = \{(.*?)\};', ccu).group(1).replace('"', '').replace(' ', '').split(',')
           for n in ('ve_dec_parents', 've_enc0_parents')}
assert parents['ve_dec_parents'][1] == 'pll-ve0' and parents['ve_enc0_parents'][1] == 'pll-ve1'
for name, reg in [('ve_dec_clk', '0x0A88'), ('ve_enc0_clk', '0x0A80')]:
    body = re.search(r'SUNXI_CCU_M_WITH_MUX_GATE\(' + name + r',(.*?)\);', ccu, re.S).group(1)
    assert reg in body and '0, 5,' in body and '24, 3,' in body and 'BIT(31)' in body and 'CLK_SET_RATE_PARENT' in body
assert re.search(r'pll_ve0_auto_clk, "pll-ve0-auto",\s*"dcxo", 0x1918,\s*BIT\(0\)', ccu)
# ccu_nm_find_best walks N upward from 53 and M from 1 and keeps the first exact rate.
assert 'for (_n = nm->min_n; _n <= nm->max_n; _n++)' in s['nm'] and '(rate - tmp_rate) < (rate - best_rate)' in s['nm']
best = None
for n in range(53, 106):
    for m in range(1, 9):
        r = 24000000*n//m
        if r <= 624000000 and (best is None or 624000000 - r < 624000000 - best[0]):
            best = (r, n, m)
assert best == (624000000, 78, 3)
factors = ((78 - 1) << 8) | ((3 - 1) << 20)
assert factors == 0x00204d00 and 24000000*78 == 1872000000
for term in ['read_times != 100', 'lock_times != 3', 'udelay(3)', 'udelay(20)']:
    assert term in s['common'], term
assert 'writel(reg | ldo_en' in s['gate'] and 'writel(reg | output' in s['gate']

# The vendor operating point and parent selection.
table = s['vf'][s['vf'].index('ve-opp-table'):]
rows = re.findall(r'opp-(vf\d+) \{\s*opp-hz-0 = /bits/ 64 <(\d+)>;\s*opp-microvolt-0 = <(\d+)>;', table)
assert len(rows) == 11 and all(hz == '624000000' and uv == '800000' for _, hz, uv in rows), rows
assert '#define VE_DVFS_MAX_FREQ\t(624)' in s['plat'] and 'attr->default_freq = attr->dvfs_array[j].ve_freq;' in s['plat']
setup = s['cedar'][s['cedar'].index('static long setup_ve_freq'):]
ordered(setup, ['disable_cedar_hw_clk_module', 'clk_set_rate(cedar_devp->ve_clk', 'enable_cedar_hw_clk_module'])
assert 'clk_set_parent(cedar_devp->ve_clk, cedar_devp->pll_ve)' in s['cedar']
dtsi = s['dtsi']
dec = dtsi[dtsi.index('ve: ve@1c0e000'):dtsi.index('ve1: ve1@1c0e000')]
enc = dtsi[dtsi.index('ve2: ve2@1c10000'):]
enc = enc[:enc.index('};')]
assert 'CLK_PLL_VE0' in dec and 'CLK_VE_DEC>' in dec and 'clock-names = "pll_ve"' in dec
assert 'CLK_PLL_VE1' in enc and 'CLK_VE_ENC0>' in enc and 'clock-names = "pll_ve"' in enc

# The source constants agree with the derivation.
unit = (ROOT/'include/a733_video_unit.h').read_text()
for name, value in [('A733_VIDEO_VENDOR_MODULE', 0x01000000), ('A733_VIDEO_VENDOR_HZ', 624000000),
                    ('A733_VIDEO_PLL_FACTORS', factors)]:
    assert int(re.search(r'#define ' + name + r' UINT32_C\((\w+)\)', unit).group(1), 0) == value, name

# The board: port 3-3 at FEL entry, read by the clock dump.
words = {int(k, 16): v for k, v in json.loads(paths['dump'].read_text())['words'].items()}
board = {hex(k): hex(words[k]) for k in (0x2002220, 0x2002228, 0x200222c, 0x2003918, 0x2003998, 0x2002a88)}
assert board == {'0x2002220': '0x41305a00', '0x2002228': '0x0', '0x200222c': '0x30000000',
                 '0x2003918': '0x30003', '0x2003998': '0x30000', '0x2002a88': '0x0'}, board
pll = words[0x2002220]
assert not pll & 0xb8000000 and not pll & ~(0xf870ff02 | 1 << 24), 'entry state'
assert 24000000*(((pll >> 8) & 255) + 1)//(((pll >> 20) & 7) + 1) == 546000000
cold = (ROOT/'checks/a733-video-cold.c').read_text()
assert '0x41305a00u' in cold and '0x00030003u' in cold and 'm->ccu[0x1998/4]=0x00030000' in cold

# The decoder cold-start model and the inherited regression, with sanitizers.
sources = ['drivers/video/a733_video_platform.c', 'drivers/video/a733_video_unit.c', 'drivers/video/a733_video_jpeg.c',
           'drivers/video/a733_video_jpeg_run.c', 'drivers/gpu/a733_gpu_resources.c', 'drivers/clock/a733_clock_leases.c']
flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined', '-Iinclude']
runs = {}
for name in ('cold', 'platform'):
    binary = a.output/name
    subprocess.run([a.clang, *flags, *sources, 'checks/a733-video-' + name + '.c', '-o', binary], cwd=ROOT, check=True)
    arguments = [paths['jpeg'], paths['rom']] if name == 'cold' else [paths['jpeg'], a.output/'platform-traces']
    if name == 'platform':
        (a.output/'platform-traces').mkdir()
    for extra in ([], ['--reset']) if name == 'cold' else ([],):
        r = subprocess.run([binary, *map(str, arguments), *extra], cwd=ROOT, capture_output=True, text=True,
                           env={'UBSAN_OPTIONS': 'halt_on_error=1'})
        assert r.returncode == 0, r.stderr
        runs[' '.join([name, *extra])] = r.stdout.splitlines()
assert any(line.startswith('PASS: PLL_VE0 at the vendor 624 MHz') for line in runs['cold'])
assert any(line.startswith('PASS: native VE binding model') for line in runs['platform'])
for target in ('drivers/video/a733_video_unit.c', 'drivers/video/a733_video_platform.c',
               'drivers/video/a733_video_encode_platform.c'):
    subprocess.run([a.clang, '--target=aarch64-none-elf', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                    '-ffreestanding', '-Iinclude', '-c', target, '-o', a.output/(Path(target).stem + '.o')],
                   cwd=ROOT, check=True)
record = {
    'source_sha256': expected,
    'profile': {'module_word': '0x01000000', 'module_hz': 624000000, 'N': 78, 'P0': 3, 'M1': 1,
                'vco_hz': 1872000000, 'factor_bits': hex(factors), 'microvolts': 800000},
    'board_entry': board,
    'model': runs,
    'scope': 'Source, dump and model comparison. No board access; physical LOCK, rate and decoding are unmeasured.',
}
(a.output/'result.json').write_text(json.dumps(record, indent=2) + '\n')
print('PASS video PLL profile, vendor sources, board entry state, cold-start model and AArch64 compilation')
