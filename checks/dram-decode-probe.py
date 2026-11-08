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
        [str(binary), str(args.output / 'region-before.bin'), str(args.output / 'region-after.bin')],
    ]
    results = []
    source = (ROOT / 'drivers/dram/a733_dram_decode_probe.c').read_text()
    restore = 'for (unsigned i = count; i-- > 0;) sweep_reverse(&w, ranges[i].base, ranges[i].base + ranges[i].size);'
    assert source.count(restore) == 1
    # The probe must put back what it overwrote, last word first. Two mutants must fail the model's check that every cell is unchanged:
    # no restore at all, and a restore in the order of the writes. The second fails even on the linear model: the sweep visits some addresses
    # twice (2 MiB is both a step and a power-of-two offset), the second visit keeps the first one's token, and only the reverse order ends with
    # the original word. An aliasing decode (two addresses, one cell) fails the same way.
    forward = [('the restore removed', restore, ''),
               ('the restore in forward order', restore + '\n    ', 'for (unsigned i = 0; i < count; ++i) sweep(&w, RESTORE, ranges[i].base, ranges[i].base + ranges[i].size);'),
               ]
    for index, command in enumerate(commands):
        result = subprocess.run(command, capture_output=True, text=True,
                                env={**harness.environment(), 'ASAN_OPTIONS': 'detect_leaks=0'})
        (args.output / f'{index}.log').write_text(result.stdout + result.stderr)
        results.append({'command': command, 'exit': result.returncode})
        harness.save(args.output / 'result.json', {'physical_access': False, 'steps': results})
        print(result.stdout + result.stderr, end='')
        result.check_returncode()
    import hashlib
    before, after = (hashlib.sha256((args.output / n).read_bytes()).hexdigest() for n in ('region-before.bin', 'region-after.bin'))
    assert before == after, 'the INPUT region at 0x48000000 changed through the probe'
    results.append({'input_region_0x48000000_sha256_before_and_after': before})
    print('INPUT region of 14,696,448 bytes at 0x48000000 keeps its SHA-256 through the probe:', before)
    for name, old, new in forward:
        text = source.replace(old, new)
        if name == 'the restore in forward order':
            text = text.replace('w.index = total;', 'w.index = 0;').replace('w->saved[--w->index]', 'w->saved[w->index++]')
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
