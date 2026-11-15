#!/usr/bin/env python3
"""Build apps/g2d-rotate/main.c on the host and decode its result with apps/g2d-rotate/host.py.

checks/g2d-rotate-host/sdk.c supplies the SDK: host buffers for the two large regions, and an ss_g2d_run that performs
each rotate on the CPU with the predicted outcome (exact pixels; identity, half turn, clockwise for 90 and
counter-clockwise for 270). The application's plan, rectangle CRC-32 values, corner samples and whole-surface CRC-32
values must then equal the decoder's reference, and the decoder must name each rotate's predicted model. Three mutants
follow, and the decoder must name each:
- -DZEROED, an engine that writes alpha-0 pixels as 0: every model's rule is "zeroed" and the stage is not exact.
- -DSWAP_TURN, an engine whose 90 turns counter-clockwise and 270 clockwise: the models are ccw and cw, still inverse
  to each other, and the prediction fails.
- -DFAIL_DEGREES=90, an engine whose 90-degree rotate times out: rotate 3 has result -1 with step 62 in the record,
  its rectangle is measured and named untouched, rotate 4 has no record, and the first two keep their models.
- -DONE_PIXEL, an engine that changes one pixel of every rectangle: no rotate matches a model, and the decoder
  reports that without failing.

Usage: g2d-rotate-host.py --output DIR"""
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
    spec = importlib.util.spec_from_file_location('g2d_rotate_host', ROOT / 'apps/g2d-rotate/host.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_and_run(out, name, defines):
    cc = shutil.which('clang-21') or shutil.which('clang') or shutil.which('gcc')
    binary, result = out / name, out / (name + '.bin')
    command = [cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
               '-fno-sanitize-recover=all', '-Ichecks/g2d-rotate-host', '-Iinclude', *defines,
               'apps/g2d-rotate/main.c', 'checks/g2d-rotate-host/sdk.c', '-o', str(binary)]
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
    reference = host.predicted()
    if record['status'] != 'done' or record['stage'] != 6 or not record['plan']['matches']:
        problems.append(f"status {record['status']}, stage {record['stage']}, plan {record['plan']}")
    if not (record['predicted'] and record['exact'] and record['inverse_90_270']) or not crc.get('output_matches') or \
            not crc['source_unchanged'] or not crc['before_match'] or crc['output'] != reference['output_crc']:
        problems.append(f'predicted outcome not decoded: {crc}, {[o.get("model") for o in ops]}')
    for k, (o, want) in enumerate(zip(ops, reference['ops'])):
        if o['rect_crc'] != want['rect_crc'] or o['samples']['corners'] != want['corners'] or not o['samples_match']:
            problems.append(f'rotate {k + 1}: {o["rect_crc"]} {o["samples"]}, reference {want}')
    zeroed = host.decode(build_and_run(a.output, 'zeroed', ['-DZEROED']))
    if [o.get('model') for o in zeroed['ops']] != [m.replace('/exact', '/zeroed') for m in
                                                   ('identity/exact', 'rot180/exact', 'cw/exact', 'ccw/exact')] or \
            zeroed['exact'] or zeroed['predicted'] or not zeroed['crc'].get('output_matches'):
        problems.append(f'the zeroing engine was not named: {[o.get("model") for o in zeroed["ops"]]}')
    swapped = host.decode(build_and_run(a.output, 'swap-turn', ['-DSWAP_TURN']))
    if [o.get('model') for o in swapped['ops']] != ['identity/exact', 'rot180/exact', 'ccw/exact', 'cw/exact'] or \
            swapped['predicted'] or not swapped['inverse_90_270'] or not swapped['exact']:
        problems.append(f'the swapped turn was not named: {[o.get("model") for o in swapped["ops"]]}')
    failed = host.decode(build_and_run(a.output, 'timeout-90', ['-DFAIL_DEGREES=90']))
    fops = failed['ops']
    if failed['status'] != 'run failed' or failed['fault_step'] != 62 or (fops[2]['result'], fops[2]['degrees']) != (-1, 90) \
            or [o.get('model') for o in fops[:3]] != ['identity/exact', 'rot180/exact', 'untouched'] or fops[3]['run'] or \
            fops[3]['done'] is not None or failed['crc']['source_unchanged'] is not None or failed['predicted']:
        problems.append(f'the timed-out 90-degree rotate was not named: {failed["status"]}, {failed["fault_step"]}, '
                        f'{[(o["result"], o["degrees"], o.get("model")) for o in fops]}')
    unmatched = host.decode(build_and_run(a.output, 'one-pixel', ['-DONE_PIXEL']))
    if [o.get('model') for o in unmatched['ops']] != [None] * 4 or unmatched['exact'] or unmatched['predicted'] or \
            unmatched['status'] != 'done' or 'output_matches' in unmatched['crc']:
        problems.append(f'the unmatched rectangles were not reported: {[o.get("model") for o in unmatched["ops"]]}')
    (a.output / 'result.json').write_text(json.dumps({'passed': not problems, 'problems': problems, 'record': record,
                                                      'reference': reference, 'zeroed': zeroed, 'swap_turn': swapped,
                                                      'timeout_90': failed, 'one_pixel': unmatched}, indent=2) + '\n')
    if problems:
        sys.exit('FAIL ' + '; '.join(problems))
    print(f"PASS g2d-rotate plan and decoder agree: models {[o['model'] for o in ops]}, output {crc['output']}; the "
          f"zeroing engine, the swapped turn, the timed-out 90-degree rotate (untouched) and unmatched rectangles are named")


if __name__ == '__main__':
    main()
