#!/usr/bin/env python3
"""Build apps/g2d-convert/main.c on the host and decode its result with apps/g2d-convert/host.py.

checks/g2d-convert-host/sdk.c supplies the SDK: host buffers for the three large regions, a clock at the approved
points, and an ss_g2d_run that performs each convert on the CPU with the predicted outcome (the scaler model, then the
programmed table rounded to nearest, U first). The decoder must find each conversion's predicted model, the lanczos
chroma model in the sampled rows, the board CPU's conversion equal to the model and to the engine's output, and each
scale flat, row-invariant, monotone and within 2 of the scaler model; the record must be predicted. The decoder's
copy of the Lanczos bank must equal drivers/g2d/a733_g2d_convert.c's. Mutants follow, and the decoder must name each:
- -DFLOOR, rounding down: both conversions match the down/uv model, the board's rule is 2, the scales still run, and the
  stage is not predicted.
- -DSWAP_UV, V read first: the nearest/vu model and rule 3.
- -DNEAREST_CHROMA, no chroma interpolation: the exact region still matches the predicted model, the sampled rows name
  the nearest chroma model, and the board CPU's conversion differs from the engine's output.
- -DMISS, one pixel changed in the first conversion's exact region: no model, rule 0, the scales skipped and the
  status missed.
- -DFAIL_OP=2, the first scale times out: step 62, its rectangle untouched, the second scale not run.

Usage: g2d-convert-host.py --output DIR"""
import argparse
import importlib.util
import json
import shutil
import subprocess
import sys
from pathlib import Path

GATE_RUNS = [['--output', '{output}']]
ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_and_run(out, name, defines):
    cc = shutil.which('clang-21') or shutil.which('clang') or shutil.which('gcc')
    binary, result = out / name, out / (name + '.bin')
    command = [cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
               '-fno-sanitize-recover=all', '-Ichecks/g2d-convert-host', '-Iinclude', '-Idrivers/g2d', *defines,
               'apps/g2d-convert/main.c', 'checks/g2d-convert-host/sdk.c', '-o', str(binary)]
    done = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if done.returncode:
        sys.exit(done.stderr)
    done = subprocess.run([str(binary), str(result)], capture_output=True, text=True)
    if done.returncode:
        sys.exit(f'{name} exited {done.returncode}: {done.stderr}')
    return result.read_bytes()


def models(record):
    return [o.get('model') for o in record['ops'][:2]]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    host = load('g2d_convert_host', 'apps/g2d-convert/host.py')
    words = load('g2d_convert_words', 'checks/g2d-convert-words.py')
    problems = []
    if host.LAN2 != words.lan2_bank(ROOT / 'drivers/g2d/a733_g2d_convert.c'):
        problems.append('the decoder\'s Lanczos bank differs from drivers/g2d/a733_g2d_convert.c\'s')
    record = host.decode(build_and_run(a.output, 'predicted', []))
    reference = host.predicted()
    ops = record['ops']
    if not record['predicted'] or record['status'] != 'done' or record['stage'] != 7 or record['main_core'] != 6:
        problems.append(f"predicted outcome not decoded: status {record['status']}, models {models(record)}, "
                        f"scales {[{k: o.get(k) for k in ('flat_exact', 'rows_identical', 'monotone', 'within_2')} for o in ops[2:]]}")
    for k, (o, want) in enumerate(zip(ops, reference)):
        same = o['rect_crc'] == f"0x{want['rect_crc']:08x}" if 'rect_crc' in want else o.get('middle_row_equals_model')
        if not same:
            problems.append(f'operation {k + 1}: {o["rect_crc"]} is not the reference')
    first = ops[0]
    if not (first['chroma_model'] == 'lanczos' and first['cpu_equals_full_model'] and first['cpu_equals_engine'] and
            first['rule_agrees'] and ops[1]['rule_agrees']):
        problems.append(f"chroma or CPU reference not decoded: {first['chroma_model']}, {first.get('cpu_equals_engine')}")
    floor = host.decode(build_and_run(a.output, 'floor', ['-DFLOOR']))
    if models(floor) != ['601@0x00/down/uv', '709@0x18/down/uv'] or floor['ops'][0]['board_rule'] != 'down/uv' or \
            floor['predicted'] or floor['stopped_before_scales'] or not all(o['run'] for o in floor['ops']):
        problems.append(f'the rounding-down engine was not named: {models(floor)}')
    swapped = host.decode(build_and_run(a.output, 'swap-uv', ['-DSWAP_UV']))
    if models(swapped) != ['601@0x00/nearest/vu', '709@0x18/nearest/vu'] or \
            swapped['ops'][0]['board_rule'] != 'nearest/vu' or swapped['predicted']:
        problems.append(f'the V-first engine was not named: {models(swapped)}')
    nearest = host.decode(build_and_run(a.output, 'nearest-chroma', ['-DNEAREST_CHROMA']))
    if models(nearest) != ['601@0x00/nearest/uv', '709@0x18/nearest/uv'] or \
            nearest['ops'][0]['chroma_model'] != 'nearest' or nearest['ops'][0]['cpu_equals_engine'] or \
            not nearest['conversions_predicted']:
        problems.append(f'the uninterpolated chroma was not named: {nearest["ops"][0]["chroma_model"]}')
    missed = host.decode(build_and_run(a.output, 'miss', ['-DMISS']))
    mops = missed['ops']
    if missed['status'] != 'missed' or not missed['stopped_before_scales'] or mops[0]['model'] is not None or \
            mops[0]['board_rule'] is not None or mops[2]['run'] or mops[3]['run'] or missed['predicted'] or \
            mops[0]['mismatch']['nearest/uv'] != 1:
        problems.append(f'the missed conversion was not reported: {missed["status"]}, {models(missed)}')
    failed = host.decode(build_and_run(a.output, 'fail-scale', ['-DFAIL_OP=2']))
    fops = failed['ops']
    if failed['status'] != 'run failed' or failed['fault_step'] != 62 or fops[2]['result'] != -1 or \
            not fops[2]['untouched'] or fops[3]['run'] or failed['predicted'] or not failed['conversions_predicted']:
        problems.append(f'the failed scale was not reported: {failed["status"]}, {failed["fault_step"]}')
    summary = {'passed': not problems, 'problems': problems, 'record': record, 'floor': floor, 'swap_uv': swapped,
               'nearest_chroma': nearest, 'miss': missed, 'fail_scale': failed}
    (a.output / 'result.json').write_text(json.dumps(summary, indent=2, default=str) + '\n')
    if problems:
        sys.exit('FAIL ' + '; '.join(problems))
    print(f"PASS g2d-convert plan and decoder agree: models {models(record)}, chroma {first['chroma_model']}, scales "
          f"within {max(o['ramp_max_difference'] for o in ops[2:])}; rounding down, V first, uninterpolated chroma, a "
          f"missed conversion and a failed scale are named")


if __name__ == '__main__':
    main()
