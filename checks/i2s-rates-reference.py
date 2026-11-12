#!/usr/bin/env python3
"""Compare the rates that the I2S0 binding admits with the pinned vendor tables.

Usage: i2s-rates-reference.py --output DIR [--i2s DIR] [--platform DIR]

The check compiles checks/i2s-platform.c with the I2S0 binding into DIR and
runs it with --rates, which offers every integer rate from 1 Hz to 400 kHz.
--i2s holds the vendor audio controller (snd_sunxi_i2s.c) and --platform the
A733 clock setup (snd_sun60iw2_i2s.c) and CCU (ccu-sun60iw2.c); they default
to the i2s and i2s-platform directories of the pinned vendor sources.

The vendor driver picks the I2S module clock by rate family: 24.576 MHz from
AUDIO1 DIV5 for 8 to 192 kHz multiples of 4 kHz, and 22.5792 MHz from the
sigma-delta-modulated AUDIO0 4x PLL for the 44.1 kHz family. The binding owns
only AUDIO1 with modulation disabled, so it must admit exactly the first
family and none of the second. Reads local cache only.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
VENDOR = Path.home() / '.cache/sunstoneos/sources'
BINDING = ['checks/i2s-platform.c', 'drivers/audio/a733_i2s_platform.c', 'drivers/audio/a733_i2s.c',
           'drivers/audio/a733_i2s_stream.c', 'drivers/audio/a733_audio_leases.c', 'drivers/clock/a733_clock_leases.c',
           'drivers/pinctrl/a733_pins.c', 'drivers/pinctrl/a733_pin_tables.c', 'drivers/dma/a733_dma.c',
           'drivers/dma/a733_dma_controller.c']

SOURCES = {'snd_sunxi_i2s.c': 'd3446ec1e11a3fd69d53161f5612a0155e43391ee14569f1fecf942d7daec3a3',
           'snd_sun60iw2_i2s.c': '0dc8d0c0912fced3dac450ffd98f5694bf0146e7608c922e6a562e6e7ed795d5',
           'ccu-sun60iw2.c': '1d32bb8555f8ff205d35210f638f9d42cc28aa9295c46963aa1c810d47b06d77'}


def read(directory, name):
    raw = (Path(directory) / name).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == SOURCES[name], name
    return raw.decode()


def vendor_families(i2s, platform):
    """{module clock Hz: sorted rates} from the vendor rate switch, after
    checking which PLL serves each family."""
    families = {}
    body = re.search(r'switch \(dts->clk_mode_lrck_freq\) \{(.*?)default:', read(i2s, 'snd_sunxi_i2s.c'), re.S)[1]
    for cases, freq in re.findall(r'((?:\s*case \d+:)+)\s*freq2 = (\d+);', body):
        families[int(freq)] = sorted(int(c) for c in re.findall(r'case (\d+):', cases))
    assert sorted(families) == [22579200, 24576000], families
    clock = read(platform, 'snd_sun60iw2_i2s.c')
    assert re.search(r'if \(freq_in % 24576000 == 0\) \{\s*if \(clk_set_parent\(clk->clk_i2s, clk->clk_pll_audio1_div5\)', clock)
    assert re.search(r'\} else \{\s*if \(clk_set_parent\(clk->clk_i2s, clk->clk_pll_audio0_4x\)', clock)
    ccu = read(platform, 'ccu-sun60iw2.c')
    audio0 = re.search(r'pll_audio0_4x_sdm_table\[\] = \{(.*?)\};', ccu, re.S)[1]
    assert re.search(r'\.rate = 90316800,\s*\.pattern = 0x[0-9A-Fa-f]+', audio0), audio0
    assert 90316800 == 4 * 22579200
    return families


def compare(families, observed):
    assert len(observed) == 1, observed
    v = observed[0]
    assert v['module_hz'] == 24576000 and v['slot_bits'] == 32, v
    assert v['rates'] == families[24576000], (v['rates'], families[24576000])
    assert not set(v['rates']) & set(families[22579200])
    assert v['refused_module_hz'] == [22579200]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--i2s', type=Path, default=VENDOR / 'i2s')
    parser.add_argument('--platform', type=Path, default=VENDOR / 'i2s-platform')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    families = vendor_families(args.i2s, args.platform)
    binary = args.output / 'i2s-platform-check'
    subprocess.run(['clang-21', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I' + str(ROOT / 'include'), *(str(ROOT / s) for s in BINDING), '-o', str(binary)], check=True)
    log = subprocess.run([str(binary), '--rates'], check=True, capture_output=True, text=True).stdout
    (args.output / 'rates.log').write_text(log)
    observed = [json.loads(line) for line in log.splitlines() if line.startswith('{')]
    compare(families, observed)
    # Mutation: a binding that also admitted the 44.1 kHz family, or lost one rate, must fail the comparison.
    for mutated in (dict(observed[0], rates=sorted(observed[0]['rates'] + [44100])),
                    dict(observed[0], rates=observed[0]['rates'][1:]),
                    dict(observed[0], refused_module_hz=[])):
        try:
            compare(families, [mutated])
        except AssertionError:
            continue
        raise SystemExit('a mutated rate set passed the comparison')
    v = observed[0]
    print('PASS I2S0 rates: binding admits exactly the vendor 24.576 MHz family',
          v['rates'], '; 22.5792 MHz family refused', families[22579200])


if __name__ == '__main__':
    main()
