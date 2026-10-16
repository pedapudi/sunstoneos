#!/usr/bin/env python3
"""Compare the bounded PLL profile and sequence with pinned primary sources."""
import argparse
import hashlib
import json
from pathlib import Path
import re

p = argparse.ArgumentParser()
p.add_argument('--sources', type=Path, default=Path.home()/'.cache/sunstoneos/sources')
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
paths = {
    'manual': a.sources/'camera-research/A733_User_Manual_V1.00.txt',
    'pdf': a.sources/'camera-research/A733_User_Manual_V1.00.pdf',
    'ccu': a.sources/'board-kernel/ccu-sun60iw2.c',
    'common': a.sources/'i2s-platform/ccu_common.c',
    'nm': a.sources/'i2s-platform/ccu_nm.c',
    'gate': a.sources/'i2s-platform/ccu_gate.c',
}
expected = {
    'manual': '219d3c21e5377d18a096765f429555d0693f7fa9b2ae2eb5bb3f225d8b6a09c2',
    'pdf': 'cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d',
    'ccu': '1d32bb8555f8ff205d35210f638f9d42cc28aa9295c46963aa1c810d47b06d77',
    'common': 'bbeebb34c55060ea74b973713e35557927a0fa06b3d2597312620bf46ca21185',
    'nm': '105b20f6d9826c34b27ef5aa9bf200ecad1e268308cc65bd52e06e254376631f',
    'gate': '3d8c88d8b00d83b64819a88e6cacaa80eef941dd4d1c164d00238ba29054cd51',
}
for key, path in paths.items():
    assert hashlib.sha256(path.read_bytes()).hexdigest() == expected[key], key
s = {key: path.read_text() for key, path in paths.items() if key != 'pdf'}
manual = s['manual']
def section(start, end):
    return manual[manual.index(start):manual.index(end)]
def ordered(text, terms):
    positions = [text.index(term) for term in terms]
    assert positions == sorted(positions), terms
method2 = section('Method 2:', 'PLL Disable\n')
ordered(method2, ['Step 1   Set PLL_LDO_EN', 'Step 2   Set the values of N',
                  'Step 3   Set PLL_EN', 'Step 4   Set LOCK_ENABLE',
                  'Step 5   Wait the LOCK', 'Step 6   Wait 20 us',
                  'Step 7   Set the PLL_OUTPUT_GATE'])
retune = section('Follow the steps below to perform PLL frequency modulation.', '4.1.4.2       PLL Lock Status Judgement')
ordered(retune, ['Step 2   Set PLL_OUTPUT_GATE', 'Step 3   Configure N',
                 'Step 4   Write 0 to LOCK_ENABLE', 'Step 5   Wait the LOCK',
                 'Step 6   Wait 20 us', 'Step 7   Set the PLL_OUTPUT_GATE'])
lock = section('4.1.4.2       PLL Lock Status Judgement', '4.1.4.3       Bus Clock Configuration')
assert 'after 3 us' in lock and 'three times' in lock
assert 'PLL enable/disable will lead' in manual and 'disturbance among PLLs' in manual
pll = section('4.1.6.56 ', '4.1.6.57 ')
assert '24MHz*N/M1/P0' in pll and 'P0=PLL_OUTPUT_DIV + 1' in pll
assert 'N= PLL_N +1' in pll and 'M1=PLL_INPUT_DIV + 1' in pll
for bits, name in [(31, 'PLL_EN'), (30, 'PLL_LDO_EN'), (29, 'LOCK_ENABLE'), (27, 'PLL_OUTPUT_GATE')]:
    assert re.search(name+r'\.?[^\f]*?\b'+str(bits)+r'\s+R/W', pll, re.S), name
assert re.search(r'28\s+R\s', pll)
board_pll = re.search(r'static struct ccu_nm pll_npu_clk = \{(.*?)\n\};', s['ccu'], re.S).group(1)
for term in ['_SUNXI_CCU_MULT_MIN_MAX(8, 8, 53, 105)', '_SUNXI_CCU_DIV(20, 3)', '0x02A0', '"pll-ref"']:
    assert term in board_pll, term
assert 53 <= 82 <= 105 and 24000000*82//4 == 492000000
assert 1260000000 <= 24000000*82 <= 2520000000
for term in ['read_times != 100', 'lock_times != 3', 'udelay(3)', 'udelay(20)']:
    assert term in s['common'], term
assert 'reg &= ~(nm->lock_enable)' in s['nm'] and 'reg |= (nm->lock_enable)' in s['nm']
# Check actual alternative consumers rather than inferring them from NPU's node.
for name, body in [('mbus', 'mbus_parents'), ('ve', 've_enc0_parents'), ('dram', 'dram0_parents')]:
    match = re.search(body+r'\[\] = \{(.*?)\};', s['ccu'], re.S)
    assert match and '"pll-npu"' in match.group(1), name
record = {
    'source_sha256': expected,
    'primary_sections': ['4.1.4.1', '4.1.4.2', '4.1.4.6', '4.1.6.56-60', '4.1.6.76', '4.1.6.114-116', '4.1.6.120-124', '4.1.6.255', '4.1.6.264'],
    'profile': {'input_hz': 24000000, 'N': 82, 'M1': 1, 'P0': 4, 'vco_hz': 1968000000, 'output_hz': 492000000, 'factor_bits': '0x00305100'},
    'locking': {'minimum_sample_spacing_us': 3, 'consecutive_samples': 3, 'maximum_samples': 100, 'settle_us': 20},
    'admission': 'Cold enable requires authenticated pre-DRAM SRAM boot authority; retune requires every NPU-parent consumer and writer excluded. Host source comparison does not establish physical authority.',
}
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(json.dumps(record, indent=2)+'\n')
print('Pinned PLL sequence, factors, lock timing and additional consumers PASS')
