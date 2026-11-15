#!/usr/bin/env python3
"""Build apps/g2d-paths/main.c on the host and decode its result with apps/g2d-paths/host.py.

checks/g2d-paths-host/sdk.c supplies the SDK: host buffers for the two large regions, and an ss_g2d_run that performs
each copy on the CPU with the outcome the hypothesis predicts for its path. The application's plan, pixel indices,
rectangle CRC-32 values, samples and whole-surface CRC-32 values must then equal the decoder's reference, and the
decoder must name each copy's predicted model. Two mutants follow, and the decoder must name each:
- -DALL_ZEROED, an engine that zeroes alpha-0 pixels on every path: every copy's model is "zeroed", from its CRC and
  from its alpha-0 sample alone.
- -DFAIL_PATH=3, an engine whose ROP copy times out: copy 4 has result -1 on path rop_copy with step 62 in the record,
  the copies after it have no record, and the earlier copies keep their models.

Usage: g2d-paths-host.py --output DIR"""
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
    spec = importlib.util.spec_from_file_location('g2d_paths_host', ROOT / 'apps/g2d-paths/host.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_and_run(out, name, defines):
    cc = shutil.which('clang-21') or shutil.which('clang') or shutil.which('gcc')
    binary, result = out / name, out / (name + '.bin')
    command = [cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
               '-fno-sanitize-recover=all', '-Ichecks/g2d-paths-host', '-Iinclude', *defines,
               'apps/g2d-paths/main.c', 'checks/g2d-paths-host/sdk.c', '-o', str(binary)]
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
    problems = []
    record = host.decode(build_and_run(a.output, 'predicted', []))
    crc, ops = record['crc'], record['ops']
    if record['status'] != 'done' or record['stage'] != 5 or not record['plan']['matches']:
        problems.append(f"status {record['status']}, stage {record['stage']}, plan {record['plan']}")
    if not record['predicted'] or not crc.get('output_matches') or not crc['source_unchanged'] or not crc['before_match']:
        problems.append(f'predicted outcome not decoded: {crc}, {[o.get("model") for o in ops]}')
    for k, o in enumerate(ops):
        if not o['picks']['match'] or o['samples_model'] != o['model'] or \
                o['samples']['before']['board'] != o['samples']['before']['expected']:
            problems.append(f'copy {k + 1}: picks {o["picks"]}, samples {o["samples"]}, model {o["model"]}')
    zeroed = host.decode(build_and_run(a.output, 'all-zeroed', ['-DALL_ZEROED']))
    if [o.get('model') for o in zeroed['ops']] != ['zeroed'] * 5 or \
            [o.get('samples_model') for o in zeroed['ops']] != ['zeroed'] * 5 or zeroed['predicted']:
        problems.append(f'the all-zeroed engine was not named: {[(o.get("model"), o.get("samples_model")) for o in zeroed["ops"]]}')
    failed = host.decode(build_and_run(a.output, 'rop-timeout', ['-DFAIL_PATH=3']))
    fops = failed['ops']
    if failed['status'] != 'run failed' or failed['fault_step'] != 62 or (fops[3]['result'], fops[3]['path']) != (-1, 'rop_copy') \
            or [o.get('model') for o in fops[:3]] != ['zeroed', 'fill_marker', 'exact'] or any(o['result'] for o in fops[4:]):
        problems.append(f'the timed-out ROP copy was not named: {failed["status"]}, {failed["fault_step"]}, '
                        f'{[(o["result"], o["path"], o.get("model")) for o in fops]}')
    (a.output / 'result.json').write_text(json.dumps({'passed': not problems, 'problems': problems, 'record': record,
                                                      'all_zeroed': zeroed, 'rop_timeout': failed}, indent=2) + '\n')
    if problems:
        sys.exit('FAIL ' + '; '.join(problems))
    print(f"PASS g2d-paths plan and decoder agree: models {[o['model'] for o in ops]}, output {crc['output']}; the "
          f"all-zeroed engine and the timed-out ROP copy are named")


if __name__ == '__main__':
    main()
