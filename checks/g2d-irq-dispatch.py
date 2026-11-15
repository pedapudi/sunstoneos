#!/usr/bin/env python3
"""Check the G2D interrupt dispatch between the mixer and the rotate module (include/a733_g2d_irq.h).

The handler completes an operation only on the armed kind's finish: 0x24 bit 0 for a mixer operation, ROT_INT bit 0
for a rotate. A finish of the other kind is a stray. It is counted and its source is masked at the block (0x20 bit 4
or ROT_INT bit 16 cleared), and the operation stays armed. With nothing armed, every finish is a stray and the line is
masked at the distributor. The other bits of both words never change the outcome: 0x24 keeps cfg_finish (bit 2) set
after a job (run 1794722460072814405-2b77c3eb) and frame_cnt in bits 8-15, and ROT_INT keeps its enable in bit 16.

The check builds the dispatch on the host and runs it two ways:
- on every armed kind, both finish bits, both enables and those other bits, against the table in expected();
- in a level-triggered model of INTID 83 that counts handler entries, with a stray of the other kind before, with
  and after the armed finish. A stray must cost one entry. A source left enabled would re-enter the handler on
  every step until the armed operation finished, an interrupt storm.

Usage: g2d-irq-dispatch.py --output DIR"""
import argparse
import itertools
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NONE, MIXER, ROTATE = 0, 1, 2


def expected(armed, ctl, rcq, rot):
    """(ack_mixer, ack_rotate, mask_mixer, mask_rotate, seen, stray) for one interrupt. A finish counts only while
    its enable is set: 0x20 bit 4 for the mixer, ROT_INT bit 16 for the rotator."""
    mixer, rotate = rcq & ctl >> 4 & 1, rot & rot >> 16 & 1
    if armed == MIXER:
        return (mixer, 0, 0, rotate, 1 if mixer else 3 if rotate else 2, rotate)
    if armed == ROTATE:
        return (0, rotate, mixer, 0, 1 if rotate else 3 if mixer else 2, mixer)
    return (0, 0, 0, 0, 0, mixer | rotate)


STEPS = 3000
# (armed kind, stray step, finish step) and the model's (entries, strays, completed, masked at the distributor).
STORMS = [
    ((MIXER, 10, 1000), (2, 1, 1, 0)),
    ((ROTATE, 10, 1000), (2, 1, 1, 0)),
    ((MIXER, 1000, 1000), (1, 1, 1, 0)),
    ((ROTATE, 1000, 1000), (1, 1, 1, 0)),
    ((MIXER, 1500, 1000), (2, 1, 1, 1)),
    ((ROTATE, 1500, 1000), (2, 1, 1, 1)),
    ((MIXER, STEPS, 1000), (1, 0, 1, 0)),
    ((ROTATE, STEPS, 1000), (1, 0, 1, 0)),
]


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    cc = shutil.which('clang-21') or shutil.which('clang') or shutil.which('gcc')
    binary = a.output / 'g2d-irq-dispatch'
    command = [cc, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
               '-fno-sanitize-recover=all', '-Iinclude', 'checks/g2d-irq-dispatch/main.c', '-o', str(binary)]
    done = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if done.returncode:
        sys.exit('FAIL build: ' + done.stderr[-2000:])
    cases = list(itertools.product((NONE, MIXER, ROTATE), (0, 0x10), (0, 1, 4, 5, 0x105, 0x104),
                                   (0, 1, 0x10000, 0x10001)))
    lines = [f'd {k} {c:x} {r:x} {o:x}\n' for k, c, r, o in cases] + [f's {k} {t} {f} {STEPS}\n' for (k, t, f), _ in STORMS]
    run = subprocess.run([str(binary)], input=''.join(lines), capture_output=True, text=True)
    if run.returncode:
        sys.exit('FAIL run: ' + run.stderr[-2000:])
    out = run.stdout.splitlines()
    problems = []
    for (k, c, r, o), line in zip(cases, out[:len(cases)], strict=True):
        got = tuple(int(x) for x in line.split())
        if got != expected(k, c, r, o):
            problems.append(f'armed {k}, 0x20 0x{c:x}, 0x24 0x{r:x}, ROT_INT 0x{o:x}: got {got}, '
                            f'want {expected(k, c, r, o)}')
    for ((k, t, f), want), line in zip(STORMS, out[len(cases):], strict=True):
        got = tuple(int(x) for x in line.split())
        if got != want:
            problems.append(f'armed {k}, stray at {t}, finish at {f}: (entries, strays, completed, masked) {got}, '
                            f'want {want}')
    storms = [p for p in problems if 'stray at' in p]
    (a.output / 'result.json').write_text(json.dumps({'passed': not problems, 'problems': problems, 'storm_problems': storms,
                                                      'cases': len(cases), 'storms': len(STORMS)}, indent=1) + '\n')
    if problems:
        sys.exit(f'FAIL {len(problems)} of {len(cases)} cases: ' + '; '.join(problems[:5]))
    print(f'PASS G2D interrupt dispatch: {len(cases)} cases and {len(STORMS)} line models; only the armed kind '
          f'completes, a stray of the other kind is counted and masked at its block in one handler entry, and '
          f'nothing is acknowledged unarmed')


if __name__ == '__main__':
    main()
