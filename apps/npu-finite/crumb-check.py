#!/usr/bin/env python3
"""Check the NPU stage encoding and retained first-failure decoder offline."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
FINITE = ('WAIT_STARTUP', 'OPEN', 'BOOTSTRAP_READY', 'SUBMIT', 'JOB_DONE',
          'OUTPUT_COPIED', 'REAPED', 'CLOSE', 'LOCAL_STOP', 'QUIESCENT', 'EXIT')
HELPER = {0x20: 'OPEN', 0x21: 'LOAD', 0x22: 'SUBMIT', 0x23: 'DONE',
          0x24: 'COPIED', 0x25: 'CLOSE', 0x2e: 'REFUSED', 0x2f: 'FAILED'}
PASS = ('DOMAIN', 'REFERENCE', 'SRAM', 'AHB', 'NSI', 'MBUS', 'PERI0', 'PERI0_OUTPUT',
        'VIDEO_ENC', 'VIDEO_DEC', 'VIDEO_BUS', 'MODULE', 'RESETS', 'DRAM_CLOCK',
        'DRAM_PHY', 'CLOCK_MODES', 'PLL_PROFILE', 'PLL_GATES', 'PLL_TUNING', 'DONE')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('npu_crumb_decoder', ROOT / 'tools/a733-crumbs.py')
    decoder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(decoder)
    defines, stages = decoder.header()
    expected = {0x0600 + i: 'NPU_' + name for i, name in enumerate(FINITE, 1)}
    expected[0x06ff] = 'NPU_RETAINED'
    expected.update({0x0600 + i: 'NPU_HELPER_' + name for i, name in HELPER.items()})
    expected.update({0x0630 + i: 'NPU_PASS_' + name for i, name in enumerate(PASS)})
    expected.update({0x0644 + i: 'NPU_' + name for i, name in enumerate(
        ('FIRST_CLOSE', 'STOPPED', 'RESTART_READY', 'REOPEN'))})
    expected.update({0x0648 + i: 'NPU_POWER_' + name for i, name in enumerate(
        ('OFF_REQUEST', 'OFF_ACK', 'ON_REQUEST', 'ON_ACK'))})
    for number, name in expected.items():
        assert stages.get(number) == name, (hex(number), stages.get(number), name)
    epoch = 0x1234
    for number, name in expected.items():
        words = dict.fromkeys(decoder.WORDS, 0)
        words.update(tag=0x5c0d0000 | epoch, last=number << 16 | 0xabcd)
        decoded = decoder.decode(words, epoch)
        assert decoded['last']['name'] == name and decoded['last']['value'] == 0xabcd
    for group, name in ((7, 'gpu'), (8, 'npu')):
        words = dict.fromkeys(decoder.WORDS, 0)
        words.update(tag=0x5c0d0000 | epoch, fault=0xfa0b0400,
                     esr=group << 12 | 123, elr=0, far=0x80000001)
        failure = decoder.decode(words, epoch)['board_failure']
        assert failure['group'] == name and failure['site'] == 123
        assert failure['value'] == '0x80000001' and failure['site_kind'] == 'source line'
    for flag, auxiliary, field, expected_value in ((defines['NPU_REGISTER'], 0x2b04 >> 2, 'register_offset', '0x2b04'),
                                              (defines['NPU_STARTUP'], 3, 'startup_stage', 3)):
        words.update(esr=8 << 12 | 123 | flag | auxiliary << 16)
        decoded = decoder.decode(words, epoch)
        assert decoded['board_failure'][field] == expected_value
        assert decoded['board_failure']['value'] == '0x80000001'
    source = args.out / 'crumb-values.c'
    assertions = [f'_Static_assert(NPU_CRUMB_{name} == {0x600+i}, "finite {name}");'
                  for i, name in enumerate(FINITE, 1)]
    assertions += ['_Static_assert(NPU_CRUMB_RETAINED == 0x06ff, "retained");',
                   '_Static_assert(SUNSTONE_CRUMB_FAIL_GPU == 7, "GPU failure");',
                   '_Static_assert(SUNSTONE_CRUMB_FAIL_NPU == 8, "NPU failure");']
    assertions += [f'_Static_assert(NPU_JOB_CRUMB_{name} == {0x600+i}, "helper {name}");'
                   for i, name in HELPER.items()]
    assertions += [f'_Static_assert(SUNSTONE_CRUMB_NPU_PASS_{name} == {0x630+i}, "pass {name}");'
                   for i, name in enumerate(PASS)]
    source.write_text('#include "crumb.h"\n#include "npu_crumb.h"\n' +
                      '\n'.join(assertions) + '\nint main(void) { return 0; }\n')
    subprocess.run(['/usr/bin/clang-21', '-std=c11', '-Wall', '-Werror',
                    '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'apps/npu-finite'),
                    str(source), '-o', str(args.out / 'crumb-values')], check=True)
    subprocess.run([str(args.out / 'crumb-values')], check=True)
    (args.out / 'result.json').write_text(json.dumps(dict(passed=True,
        decoded_stages=len(expected), failure_groups=[7, 8], physical_access=False), indent=2) + '\n')
    print(f'PASS NPU breadcrumbs: {len(expected)} stages, finite/helper/pass constants, GPU/NPU first-failure decoding')


if __name__ == '__main__':
    main()
