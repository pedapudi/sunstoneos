#!/usr/bin/env python3
"""Build apps/g2d-convert/main.c on the host and decode its result with apps/g2d-convert/host.py.

checks/g2d-convert-host/sdk.c supplies the SDK: host buffers for the three large regions, a clock at the approved
points, and an ss_g2d_run that performs each packed convert on the CPU with the engine's model (each pair read in the
format code's order, the scaler's horizontal and then vertical pass with each position rounded to the nearest phase,
the linear vertical bank or with the variant the Lanczos one, the derived table with luma kept, an unscaled row's last
pixel with its own pair). It is a second implementation of the model in host.py. The decoder must find every
operation predicted: both unscaled conversions with their byte order and the model's CRC-32, the frame-R columns with
the model's vertical bank and phase rounding, and the frame-Q outputs equal to the model under the predicted bank,
pass order and phase rounding; the board CPU's conversion must equal the model. The decoder's Lanczos bank must equal
drivers/g2d/a733_g2d_convert.c's and its linear bank g2d_scal.c's. Mutants follow, and the decoder must name each:
- -DORDER_SWAP, format 0x20 read as UYVY and 0x21 as YUYV: both conversions' board order is the other one.
- -DNEAREST_CHROMA, no chroma interpolation: the conversions keep their order, and the sampled rows name nearest.
- -DTRUNC_PHASE, positions truncated to their phase: the frame-R columns name trunc for both banks.
- -DV_THEN_H, the vertical pass first, and -DWIDE, the horizontal sum kept whole: the frame-Q outputs name that order.
- -DIGNORE_VARIANT, the linear bank kept for the variant: the Lanczos frame-R column names linear.
- -DMISS, one even pixel changed in the first conversion's exact region: no order for it, and the stage is not
  predicted.
- -DFAIL_OP=4, the first frame-Q scale times out: step 62, its rectangle untouched, the last scale not run.

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
               '-fno-sanitize-recover=all', '-DSUNSTONE_G2D_CONVERT_VARIANTS=1', '-Ichecks/g2d-convert-host', '-Iinclude',
               '-Idrivers/g2d', *defines,
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
    if host.LINEAR != words.linear_bank(ROOT / 'drivers/g2d/a733_g2d_convert.c'):
        problems.append('the decoder\'s linear bank differs from drivers/g2d/a733_g2d_convert.c\'s')
    frame_p, stored = host.frames()
    if not (host.convert_model(stored['p'], 'yuyv', host.FW, host.FH) == host.conversion_frame(frame_p, host.CSC)).all():
        problems.append('the decoder\'s unscaled model differs from its stage-9 conversion of frame P')
    record = host.decode(build_and_run(a.output, 'predicted', []))
    ops = record['ops']
    if not record['predicted'] or record['status'] != 'done' or record['stage'] != 10 or record['main_core'] != 6:
        problems.append(f"predicted outcome not decoded: status {record['status']}, "
                        f"{[(o['name'], o.get('predicted')) for o in ops]}")
    if [o.get('board_order') for o in ops[:2]] != ['yuyv', 'uyvy'] or \
            [o.get('chroma_model') for o in ops[:2]] != ['lanczos', 'lanczos']:
        problems.append(f"orders or chroma not decoded: {[(o.get('board_order'), o.get('chroma_model')) for o in ops[:2]]}")
    if [o.get('column_models') for o in ops[2:4]] != [['linear/nearest'], ['lanczos/nearest']]:
        problems.append(f"frame-R columns not named: {[o.get('column_models') for o in ops[2:4]]}")
    if [o.get('output_models') for o in ops[4:]] != [['linear/h-then-v/nearest'], ['lanczos/h-then-v/nearest']]:
        problems.append(f"frame-Q outputs not named: {[o.get('output_models') for o in ops[4:]]}")
    if not record['cpu']['equals_model']:
        problems.append(f"the CPU reference differs from the model: {record['cpu']}")
    runs = {}

    def mutant(name, define):
        runs[name] = host.decode(build_and_run(a.output, name, [define]))
        return runs[name]

    swap = mutant('order_swap', '-DORDER_SWAP')
    if [o['board_order'] for o in swap['ops'][:2]] != ['uyvy', 'yuyv'] or swap['predicted']:
        problems.append(f"the swapped byte order was not named: {[o['board_order'] for o in swap['ops'][:2]]}")
    nearest = mutant('nearest_chroma', '-DNEAREST_CHROMA')
    if [o['board_order'] for o in nearest['ops'][:2]] != ['yuyv', 'uyvy'] or nearest['predicted'] or \
            nearest['ops'][0]['chroma_model'] != 'nearest' or nearest['ops'][0]['rect_predicted']:
        problems.append(f"the uninterpolated chroma was not named: {nearest['ops'][0]['chroma_model']}")
    trunc = mutant('trunc_phase', '-DTRUNC_PHASE')
    if [o['column_models'] for o in trunc['ops'][2:4]] != [['linear/trunc'], ['lanczos/trunc']] or trunc['predicted']:
        problems.append(f"the truncated phase was not named: {[o['column_models'] for o in trunc['ops'][2:4]]}")
    for name, define, order in (('v_then_h', '-DV_THEN_H', 'v-then-h'), ('wide', '-DWIDE', 'wide')):
        m = mutant(name, define)
        want = [[f'linear/{order}/nearest'], [f'lanczos/{order}/nearest']]
        if [o['output_models'] for o in m['ops'][4:]] != want or m['predicted'] or \
                not all(o['predicted'] for o in m['ops'][:4]):
            problems.append(f"the {order} scaler was not named: {[o['output_models'] for o in m['ops'][4:]]}")
    ignore = mutant('ignore_variant', '-DIGNORE_VARIANT')
    if ignore['ops'][3]['column_models'] != ['linear/nearest'] or ignore['ops'][5]['output_models'] != \
            ['linear/h-then-v/nearest'] or ignore['predicted']:
        problems.append(f"the ignored variant was not named: {ignore['ops'][3]['column_models']}")
    missed = mutant('miss', '-DMISS')
    if missed['ops'][0]['board_order'] is not None or missed['ops'][0]['mismatch']['yuyv'] != 1 or \
            missed['predicted'] or not missed['ops'][1]['predicted']:
        problems.append(f"the missed conversion was not reported: {missed['ops'][0]['mismatch']}")
    failed = mutant('fail_scale', '-DFAIL_OP=4')
    fops = failed['ops']
    if failed['status'] != 'run failed' or failed['fault_step'] != 62 or fops[4]['result'] != -1 or \
            not fops[4]['untouched'] or fops[5]['run'] or failed['predicted'] or \
            not all(o['predicted'] for o in fops[:4]):
        problems.append(f"the failed scale was not reported: {failed['status']}, {failed['fault_step']}")
    summary = {'passed': not problems, 'problems': problems, 'record': record, **runs}
    (a.output / 'result.json').write_text(json.dumps(summary, indent=2, default=str) + '\n')
    if problems:
        sys.exit('FAIL ' + '; '.join(problems))
    print(f"PASS g2d-convert plan and decoder agree: {[o['rect_crc'] for o in ops]} under the packed model; a swapped "
          f"byte order, uninterpolated chroma, a truncated phase, the vertical pass first, a whole horizontal sum, an "
          f"ignored variant, a missed pixel and a failed scale are named")

if __name__ == '__main__':
    main()
