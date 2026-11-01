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
    expected.update({0x064c + i: 'NPU_' + name for i, name in enumerate(
        ('LOCAL_STOP_WRITE', 'LOCAL_OPEN_WRITE', 'PLL_OPEN_WRITE', 'SRAM_TO_CPU',
         'SRAM_TO_NPU', 'POWER_PRECHECK', 'POWER_REFUSED', 'PPU_DELAY_WRITE',
         'PPU_POLICY', 'PPU_STATUS'))})
    assert len(set(stages.values())) == len(stages)
    for number, name in expected.items():
        assert stages.get(number) == name, (hex(number), stages.get(number), name)
    epoch = 0x1234
    for number, name in expected.items():
        words = dict.fromkeys(decoder.WORDS, 0)
        words.update(tag=0x5c0d0000 | epoch, last=number << 16 | 0xabcd)
        decoded = decoder.decode(words, epoch)
        assert decoded['last']['name'] == name and decoded['last']['value'] == 0xabcd
    for group, name in ((7, 'gpu'), (10, 'npu')):
        words = dict.fromkeys(decoder.WORDS, 0)
        words.update(tag=0x5c0d0000 | epoch, fault=0xfa0b0400,
                     esr=group << 12 | 123, elr=0, far=0x80000001)
        failure = decoder.decode(words, epoch)['board_failure']
        assert failure['group'] == name and failure['site'] == 123
        assert failure['value'] == '0x80000001' and failure['site_kind'] == 'source line'
    for flag, auxiliary, field, expected_value in ((defines['NPU_REGISTER'], 0x2b04 >> 2, 'register_offset', '0x2b04'),
                                              (defines['NPU_STARTUP'], 3, 'startup_stage', 3)):
        words.update(esr=10 << 12 | 123 | flag | auxiliary << 16)
        decoded = decoder.decode(words, epoch)
        assert decoded['board_failure'][field] == expected_value
        assert decoded['board_failure']['value'] == '0x80000001'
    measurement_cases=0
    for stage in (23,28):
        for job in (0,199,255):
            for high in (False,True):
                flag=defines['NPU_MEASUREMENT'] | (defines['NPU_MEASUREMENT_HIGH_WORD'] if high else 0)
                assert not flag & (defines['NPU_REGISTER'] | defines['NPU_STARTUP'])
                words.update(esr=10 << 12 | 123 | flag | (stage-23) << 24 | job << 16,far=0x12345678)
                decoded=decoder.decode(words,epoch);failure=decoded['board_failure']
                assert failure['measurement_stage']==stage and failure['job_index']==(None if job==255 else job)
                assert failure['observed_word_index']==int(high) and failure['observed_word']=='0x12345678'
                assert 'register_offset' not in failure and 'startup_stage' not in failure
                assert ('observed high word' if high else 'observed low word') in decoded['summary']
                measurement_cases+=1
    # Failure groups share one retained wire field and must be distinct.
    groups = {name: value for name, value in defines.items() if name.startswith('FAIL_')
              and name not in ('FAIL_SVC', 'FAIL_LEVEL', 'FAIL_RETURN')}
    assert len(set(groups.values())) == len(groups), groups
    words.update(esr=8 << 12 | 123, far=1014 << 8)
    decoded = decoder.decode(words, epoch)
    assert decoded['board_failure']['group'] == 'cpu_retained'
    assert 'target 1014 MHz' in decoded['summary']
    words.update(esr=9 << 12 | 123 | 69 << 16, far=0xf8804500)
    failure = decoder.decode(words, epoch)['board_failure']
    assert failure['group'] == 'cpu_pll' and failure['expected_n'] == 69
    assert failure['observed_n'] == 69

    # Model the old finite NPU ELF identity; generic mixed service images and
    # undefined finite producers cannot reclassify CPU retention records.
    for kind, expected_group in (('finite', 'npu'), ('mixed', 'cpu_retained'),
                                  ('undefined', 'cpu_retained')):
        fixture = args.out / (kind + '.c')
        fixture.write_text('int npu_service; int main(void) { return 0; }\n' +
            ('int npu_finite_receipt; void npu_finite_rom_return(void) {}\n' if kind == 'finite' else
             'extern int npu_finite_receipt; extern void npu_finite_rom_return(void);\n'
             'void *keep[] = {&npu_finite_receipt, npu_finite_rom_return};\n'
             if kind == 'undefined' else ''))
        image = args.out / (kind + '.elf')
        subprocess.run(['/usr/bin/clang-21', '-g', *(['-c'] if kind == 'undefined' else []),
                        str(fixture), '-o', str(image)], check=True)
        words.update(esr=8 << 12 | 123 | defines['NPU_STARTUP'] | 3 << 16, far=0x80000001)
        decoded = decoder.decode(words, epoch, image)
        assert decoded['board_failure']['group'] == expected_group
        if kind == 'finite':
            assert decoded['board_failure']['startup_stage'] == 3
            assert 'startup stage 3' in decoded['summary']
            assert decoded['board_failure']['group_source'] == 'finite NPU ELF (legacy group 8)'
    bad_elf = args.out / 'malformed.elf'
    bad_elf.write_bytes(b'not an ELF')
    for image in (bad_elf, args.out / 'missing.elf'):
        assert decoder.decode(words, epoch, image)['board_failure']['group'] == 'cpu_retained'

    source = args.out / 'crumb-values.c'
    assertions = [f'_Static_assert(NPU_CRUMB_{name} == {0x600+i}, "finite {name}");'
                  for i, name in enumerate(FINITE, 1)]
    assertions += ['_Static_assert(NPU_CRUMB_RETAINED == 0x06ff, "retained");',
                   '_Static_assert(SUNSTONE_CRUMB_FAIL_GPU == 7, "GPU failure");',
                   '_Static_assert(SUNSTONE_CRUMB_FAIL_NPU == 10, "NPU failure");',
                   '_Static_assert(SUNSTONE_CRUMB_NPU_MEASUREMENT == 0x10000000u, "measurement flag");',
                   '_Static_assert(SUNSTONE_CRUMB_NPU_MEASUREMENT_HIGH_WORD == 0x08000000u, "wide operand flag");']
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
        decoded_stages=len(expected), measurement_cases=measurement_cases, failure_groups=[7, 8, 9, 10], physical_access=False), indent=2) + '\n')
    print(f'PASS NPU breadcrumbs: {len(expected)} stages, finite/helper/pass constants, GPU/NPU first-failure decoding')


if __name__ == '__main__':
    main()
