#!/usr/bin/env python3
"""Build apps/g2d-convert/main.c on the host and decode its result with apps/g2d-convert/host.py.

checks/g2d-convert-host/sdk.c supplies the SDK: host buffers for the three large regions, a clock at the approved
points, and an ss_g2d_run that performs each convert on the CPU with the engine's model (the scaler model with each
position rounded to the nearest phase, the derived table with luma kept, U first, the row's last pixel with its own
pair). The decoder must find every conversion predicted (rule kept in the exact region and the derived model's
rectangle CRC-32), the board CPU's conversion equal to the model, and each scale flat, row-invariant, monotone and
equal to the scaler model; the record must be predicted. The decoder's copy of the Lanczos bank must equal
drivers/g2d/a733_g2d_convert.c's. Mutants follow, and the decoder must name each:
- -DFLOOR, luma below 16 raised to 16: the first conversion's rule is raised, the application stops after it, and the
  stage is not predicted.
- -DNO_LOW_CLAMP and -DNO_HIGH_CLAMP, one bound of the output clamp dropped: the first conversion's rectangle matches
  no model, so the luma-sweep frame tests both bounds.
- -DNEAREST_CHROMA, no chroma interpolation: the exact region still has rule kept, the rectangle matches no model.
- -DTRUNC_PHASE, the scaler's positions truncated to their phase: the 224 by 224 middle row differs from the model.
- -DMISS, one pixel changed in the first conversion's exact region: no rule, the application stops, status missed.
- -DFAIL_OP=4, the first scale times out: step 62, its rectangle untouched, the second scale not run.

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
    if not record['predicted'] or record['status'] != 'done' or record['stage'] != 9 or record['main_core'] != 6:
        scales = [{k: o.get(k) for k in ('flat_exact', 'rows_identical', 'monotone', 'middle_row_equals_model')}
                  for o in ops[4:]]
        problems.append(f"predicted outcome not decoded: status {record['status']}, conversions "
                        f"{record['conversion_models']}, scales {scales}")
    for k in range(4):
        want = (f"0x{reference[k]['rect_crc']:08x}", f"0x{reference[k]['exact_crc']:08x}")
        if (ops[k]['rect_crc'], ops[k]['exact_crc']) != want:
            problems.append(f"{ops[k]['name']}'s CRC-32 values are not the reference: {ops[k]['rect_crc']} "
                            f"{ops[k]['exact_crc']}")
    if not (all(o['chroma_model'] == 'lanczos' and o['rule_agrees'] for o in ops[:4]) and record['cpu']['equals_model']):
        problems.append(f"chroma or CPU reference not decoded: {[o['chroma_model'] for o in ops[:4]]}, {record['cpu']}")
    runs = {}
    floor = runs['floor'] = host.decode(build_and_run(a.output, 'floor', ['-DFLOOR']))
    if floor['ops'][0]['board_rule'] != 'raised' or floor['ops'][0]['rect_model'] != 'derived/raised' or \
            not floor['stopped'] or floor['ops'][1]['run'] or floor['status'] != 'missed' or floor['predicted']:
        problems.append(f"the engine with the floor was not named: {floor['ops'][0]['board_rule']}")
    for bound in ('NO_LOW_CLAMP', 'NO_HIGH_CLAMP'):
        clamp = runs[bound.lower()] = host.decode(build_and_run(a.output, bound.lower(), [f'-D{bound}']))
        if clamp['ops'][0]['rect_model'] is not None or clamp['predicted'] or clamp['conversions_predicted']:
            problems.append(f"the converter without {bound} was not named: {clamp['ops'][0]['rect_model']}")
    nearest = runs['nearest_chroma'] = host.decode(build_and_run(a.output, 'nearest-chroma', ['-DNEAREST_CHROMA']))
    if nearest['ops'][0]['board_rule'] != 'kept' or nearest['ops'][0]['rect_model'] is not None or \
            nearest['conversions_predicted'] or nearest['ops'][0]['chroma_model'] != 'nearest':
        problems.append(f"the uninterpolated chroma was not named: {nearest['ops'][0]['chroma_model']}")
    trunc = runs['trunc_phase'] = host.decode(build_and_run(a.output, 'trunc-phase', ['-DTRUNC_PHASE']))
    if trunc['scales_predicted'] or trunc['ops'][5]['middle_row_equals_model'] or \
            not trunc['ops'][4]['middle_row_equals_model'] or not trunc['conversions_predicted']:
        problems.append(f"the truncated scaler phase was not named: {trunc['ops'][5].get('middle_row_mismatches')}")
    missed = runs['miss'] = host.decode(build_and_run(a.output, 'miss', ['-DMISS']))
    if missed['status'] != 'missed' or not missed['stopped'] or missed['ops'][0]['board_rule'] is not None or \
            missed['ops'][1]['run'] or missed['predicted'] or missed['ops'][0]['mismatch']['kept'] != 1:
        problems.append(f"the missed conversion was not reported: {missed['status']}")
    failed = runs['fail_scale'] = host.decode(build_and_run(a.output, 'fail-scale', ['-DFAIL_OP=4']))
    fops = failed['ops']
    if failed['status'] != 'run failed' or failed['fault_step'] != 62 or fops[4]['result'] != -1 or \
            not fops[4]['untouched'] or fops[5]['run'] or failed['predicted'] or not failed['conversions_predicted']:
        problems.append(f"the failed scale was not reported: {failed['status']}, {failed['fault_step']}")
    summary = {'passed': not problems, 'problems': problems, 'record': record, **runs}
    (a.output / 'result.json').write_text(json.dumps(summary, indent=2, default=str) + '\n')
    if problems:
        sys.exit('FAIL ' + '; '.join(problems))
    print(f"PASS g2d-convert plan and decoder agree: conversions {[o['rect_crc'] for o in ops[:4]]} under the derived "
          f"tables with luma kept, scales equal to the scaler model; the floor, each clamp bound, uninterpolated "
          f"chroma, a truncated scaler phase, a missed conversion and a failed scale are named")

if __name__ == '__main__':
    main()
