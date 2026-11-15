#!/usr/bin/env python3
"""Build apps/g2d-check/main.c on the host and decode its result with apps/g2d-check/host.py.

checks/g2d-check-host/sdk.c supplies the SDK: host buffers for the two large regions, and an ss_g2d_run that performs
each copy on the CPU, converting ARGB8888 to BGRA8888 by reversing each word's bytes and writing a pixel with source
alpha 0 as 0x00000000 (board run 1794723622887180087-445859c1). The application's plan, surfaces, samples and CRC-32
values must then equal the decoder's reference from the seed: both surfaces' values before, the output after both
copies, the unchanged source, and every sample of both operations, including the samples at an alpha-0 and an
alpha-0xff pixel. Two mutants follow, and the decoder must name each:
- -DNO_CONVERT, an engine that copies without converting: the output does not match and equals the unconverted
  reference.
- -DEXACT_TRANSPARENT, an engine that copies alpha-0 pixels exactly: the output equals the exact-transparent reference,
  and each operation's alpha-0 sample fails, so the samples alone catch it.

Usage: g2d-check-host.py --output DIR"""
import argparse
import importlib.util
import json
import shutil
import subprocess
import sys
from pathlib import Path

GATE_RUNS = [['--output', '{output}']]
ROOT = Path(__file__).resolve().parents[1]


def decoder():
    spec = importlib.util.spec_from_file_location('g2d_check_host', ROOT / 'apps/g2d-check/host.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_and_run(out, name, defines):
    cc = shutil.which('clang-21') or shutil.which('clang') or shutil.which('gcc')
    binary, result = out / name, out / (name + '.bin')
    command = [cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
               '-fno-sanitize-recover=all', '-Ichecks/g2d-check-host', '-Iinclude', *defines,
               'apps/g2d-check/main.c', 'checks/g2d-check-host/sdk.c', '-o', str(binary)]
    done = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if done.returncode:
        sys.exit(done.stderr)
    done = subprocess.run([str(binary), str(result)], capture_output=True, text=True)
    if done.returncode:
        sys.exit(f'{name} exited {done.returncode}: {done.stderr}')
    return result.read_bytes()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    host = decoder()
    record = host.decode(build_and_run(a.output, 'converting', []))
    crc, ops = record['crc'], record['ops']
    problems = []
    if record['status'] != 'done' or record['stage'] != 3:
        problems.append(f"status {record['status']}, stage {record['stage']}")
    if not record['plan']['matches_seed']:
        problems.append('the application\'s plan differs from the decoder\'s')
    for key in ('output_matches', 'source_unchanged', 'before_match'):
        if not crc[key]:
            problems.append(f'{key} is false: {crc}')
    if crc['output_unconverted']:
        problems.append('the converted output must differ from the unconverted reference')
    for k, o in enumerate(ops):
        if o['result'] != 1 or not o['samples_match']:
            problems.append(f'operation {k + 1}: result {o["result"]}, samples {o["samples"]}')
    mutant = host.decode(build_and_run(a.output, 'unconverting', ['-DNO_CONVERT']))['crc']
    if mutant['output_matches'] or not mutant['output_unconverted']:
        problems.append(f'the unconverting engine was not named: {mutant}')
    exact = host.decode(build_and_run(a.output, 'exact-transparent', ['-DEXACT_TRANSPARENT']))
    if exact['crc']['output_matches'] or not exact['crc']['output_exact_transparent']:
        problems.append(f'the exact-transparent engine was not named: {exact["crc"]}')
    for k, o in enumerate(exact['ops']):
        s = o['samples']
        if o['samples_match'] or s['alpha_0']['board'] == s['alpha_0']['expected'] or \
                s['alpha_ff']['board'] != s['alpha_ff']['expected']:
            problems.append(f'exact-transparent operation {k + 1}: the alpha-0 sample alone must fail: {s}')
    (a.output / 'result.json').write_text(json.dumps({'passed': not problems, 'problems': problems, 'record': record,
                                                      'mutant_crc': mutant, 'exact_transparent': exact},
                                                     indent=2) + '\n')
    if problems:
        sys.exit('FAIL ' + '; '.join(problems))
    print(f"PASS g2d-check plan and decoder agree: output {crc['output']}, source {crc['source']} unchanged, "
          f"{sum(len(o['samples']) for o in ops)} samples; the unconverting engine is named ({mutant['output']}); "
          f"the exact-transparent engine is named ({exact['crc']['output']}) and fails both alpha-0 samples")


if __name__ == '__main__':
    main()
