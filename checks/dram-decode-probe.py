#!/usr/bin/env python3
"""Check the address-test DRAM decode check against a memory model with decode faults."""
import argparse
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    binary = args.output / 'dram-decode-probe'
    commands = [
        ['clang-21', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I' + str(ROOT / 'include'),
         str(ROOT / 'checks/dram-decode-probe.c'), str(ROOT / 'drivers/dram/a733_dram_decode_probe.c'), '-DA733_DRAM_DECODE_PROBE_HOST', '-o', str(binary)],
        [str(binary)],
    ]
    results = []
    source = (ROOT / 'drivers/dram/a733_dram_decode_probe.c').read_text()
    # Mutants that must fail the model: the write moved into an application range, the alias read of one address bit removed.
    mutants = [('the token written at the start of application memory', 'io->write(io->context, scratch_pa, token(scratch_pa));',
                'io->write(io->context, UINT64_C(0x48000000), token(scratch_pa));'),
               ('the bit sweep removed', 'for (uint64_t bit = BLOCK; bit <= UINT64_C(1) << 33; bit <<= 1) {', 'for (uint64_t bit = BLOCK; bit < BLOCK; bit <<= 1) {')]
    for index, command in enumerate(commands[:2]):
        result = subprocess.run(command, capture_output=True, text=True,
                                env={**harness.environment(), 'ASAN_OPTIONS': 'detect_leaks=0'})
        (args.output / f'{index}.log').write_text(result.stdout + result.stderr)
        results.append({'command': command, 'exit': result.returncode})
        harness.save(args.output / 'result.json', {'physical_access': False, 'steps': results})
        print(result.stdout + result.stderr, end='')
        result.check_returncode()
    for name, old, new in mutants:
        text = source.replace(old, new)
        assert text != source, name
        mutant = args.output / ('mutant-%s.c' % name.replace(' ', '-'))
        mutant.write_text(text)
        binary_m = args.output / ('probe-%s' % name.replace(' ', '-'))
        subprocess.run(['clang-21', '-std=c11', '-O1', '-g', '-w', '-fsanitize=address,undefined', '-I' + str(ROOT / 'include'),
                        str(ROOT / 'checks/dram-decode-probe.c'), str(mutant), '-DA733_DRAM_DECODE_PROBE_HOST', '-o', str(binary_m)],
                       check=True, capture_output=True, env={**harness.environment()})
        run = subprocess.run([str(binary_m)], capture_output=True, text=True, env={**harness.environment(), 'ASAN_OPTIONS': 'detect_leaks=0'})
        assert run.returncode != 0, 'the check did not notice ' + name
        results.append({'mutant': name, 'exit': run.returncode})
        print('mutant fails as required:', name, '(exit %d)' % run.returncode)
    harness.save(args.output / 'result.json', {'physical_access': False, 'steps': results})


if __name__ == '__main__':
    main()
