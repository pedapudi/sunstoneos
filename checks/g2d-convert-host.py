#!/usr/bin/env python3
"""Build apps/g2d-convert/main.c on the host and decode its result with apps/g2d-convert/host.py.

checks/g2d-convert-host/sdk.c supplies the SDK: host buffers for the three large regions, a clock at the approved
points, and an ss_g2d_run that performs each convert on the CPU with the engine's model from stage 7 (the scaler model,
luma below 16 raised to 16, the programmed table rounded down, U first, the row's last pixel with its own pair), and
one outcome per variant: B and D keep luma below 16, C changes nothing, E rounds to nearest. The decoder must find A
predicted (rule raised/down in the exact region and the model's rectangle CRC-32), name each variant's candidate
(kept/down, raised/down, kept/down, raised/nearest), find the board CPU's conversion equal to the model, and each
scale flat, row-invariant, monotone and within 2 of the scaler model; the record must be predicted. The decoder's
copy of the Lanczos bank must equal drivers/g2d/a733_g2d_convert.c's. Mutants follow, and the decoder must name each:
- -DNO_FLOOR, luma below 16 kept everywhere: A's rule is kept/down, the application stops after A, and the stage is
  not predicted.
- -DNEAREST_CHROMA, no chroma interpolation: A's exact region still has rule raised/down, its rectangle matches no
  candidate, and the stage is not predicted.
- -DMISS, one pixel changed in A's exact region: no rule, the application stops after A, status missed.
- -DFAIL_OP=5, the first scale times out: step 62, its rectangle untouched, the second scale not run.

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
    want_variants = {'B fill colours luma 0': 'kept/down', 'C pipe 0 fill colour off': 'raised/down',
                     'D csc0, RGB blend': 'kept/down', 'E write-back rounding': 'raised/nearest'}
    if not record['predicted'] or record['status'] != 'done' or record['stage'] != 8 or record['main_core'] != 6:
        problems.append(f"predicted outcome not decoded: status {record['status']}, A {record['a_predicted']}, scales "
                        f"{[{k: o.get(k) for k in ('flat_exact', 'rows_identical', 'monotone', 'within_2')} for o in ops[5:]]}")
    if ops[0]['rect_crc'] != f"0x{reference[0]['rect_crc']:08x}" or ops[0]['exact_crc'] != f"0x{reference[0]['exact_crc']:08x}":
        problems.append(f"A's CRC-32 values are not the reference: {ops[0]['rect_crc']} {ops[0]['exact_crc']}")
    if record['variants'] != want_variants:
        problems.append(f"the variants were not named: {record['variants']}")
    if not (ops[0]['chroma_model'] == 'lanczos' and record['cpu']['equals_model'] and
            all(o['rule_agrees'] for o in ops[:5])):
        problems.append(f"chroma or CPU reference not decoded: {ops[0]['chroma_model']}, {record['cpu']}")
    floor = host.decode(build_and_run(a.output, 'no-floor', ['-DNO_FLOOR']))
    if floor['ops'][0]['board_rule'] != 'kept/down' or not floor['stopped_after_a'] or floor['ops'][1]['run'] or \
            floor['status'] != 'missed' or floor['predicted']:
        problems.append(f"the engine without the floor was not named: {floor['ops'][0]['board_rule']}")
    nearest = host.decode(build_and_run(a.output, 'nearest-chroma', ['-DNEAREST_CHROMA']))
    if nearest['ops'][0]['board_rule'] != 'raised/down' or nearest['ops'][0]['rect_rule'] is not None or \
            nearest['a_predicted'] or nearest['ops'][0]['chroma_model'] != 'nearest':
        problems.append(f"the uninterpolated chroma was not named: {nearest['ops'][0]['chroma_model']}")
    missed = host.decode(build_and_run(a.output, 'miss', ['-DMISS']))
    if missed['status'] != 'missed' or not missed['stopped_after_a'] or missed['ops'][0]['board_rule'] is not None or \
            missed['ops'][1]['run'] or missed['predicted'] or missed['ops'][0]['mismatch']['raised/down'] != 1:
        problems.append(f"the missed conversion was not reported: {missed['status']}")
    failed = host.decode(build_and_run(a.output, 'fail-scale', ['-DFAIL_OP=5']))
    fops = failed['ops']
    if failed['status'] != 'run failed' or failed['fault_step'] != 62 or fops[5]['result'] != -1 or \
            not fops[5]['untouched'] or fops[6]['run'] or failed['predicted'] or not failed['a_predicted']:
        problems.append(f"the failed scale was not reported: {failed['status']}, {failed['fault_step']}")
    summary = {'passed': not problems, 'problems': problems, 'record': record, 'no_floor': floor,
               'nearest_chroma': nearest, 'miss': missed, 'fail_scale': failed}
    (a.output / 'result.json').write_text(json.dumps(summary, indent=2, default=str) + '\n')
    if problems:
        sys.exit('FAIL ' + '; '.join(problems))
    print(f"PASS g2d-convert plan and decoder agree: A {ops[0]['rect_crc']} under raised/down, variants "
          f"{list(record['variants'].values())}, scales within {max(o['ramp_max_difference'] for o in ops[5:])}; the "
          f"engine without the floor, uninterpolated chroma, a missed conversion and a failed scale are named")


if __name__ == '__main__':
    main()
