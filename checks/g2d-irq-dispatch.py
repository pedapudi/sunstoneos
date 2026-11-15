#!/usr/bin/env python3
"""Check the G2D interrupt dispatch between the mixer and the rotate module (include/a733_g2d_irq.h).

The handler completes an operation only on the armed kind's finish: 0x24 bit 0 for a mixer operation, ROT_INT bit 0
for a rotate. A finish of the other kind is counted as a stray and not acknowledged. With nothing armed, every finish
is a stray and nothing is acknowledged. The other bits of both words never change the outcome: 0x24 keeps cfg_finish
(bit 2) set after a job (run 1794722460072814405-2b77c3eb) and frame_cnt in bits 8-15, and ROT_INT keeps its enable
in bit 16.

The check builds the dispatch on the host and runs it on every armed kind, both finish bits and those other bits,
against the table below.

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


def expected(armed, rcq, rot):
    """(ack_mixer, ack_rotate, seen, stray) for one interrupt."""
    mixer, rotate = rcq & 1, rot & 1
    if armed == MIXER:
        return (mixer, 0, 1 if mixer else 2, rotate)
    if armed == ROTATE:
        return (0, rotate, 1 if rotate else 2, mixer)
    return (0, 0, 0, mixer | rotate)


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
    cases = list(itertools.product((NONE, MIXER, ROTATE), (0, 1, 4, 5, 0x105, 0x104), (0, 1, 0x10000, 0x10001)))
    run = subprocess.run([str(binary)], input=''.join(f'{k} {r:x} {o:x}\n' for k, r, o in cases), capture_output=True,
                         text=True)
    if run.returncode:
        sys.exit('FAIL run: ' + run.stderr[-2000:])
    problems = []
    for (k, r, o), line in zip(cases, run.stdout.splitlines(), strict=True):
        got = tuple(int(x) for x in line.split())
        if got != expected(k, r, o):
            problems.append(f'armed {k}, 0x24 0x{r:x}, ROT_INT 0x{o:x}: got {got}, want {expected(k, r, o)}')
    (a.output / 'result.json').write_text(json.dumps({'passed': not problems, 'problems': problems,
                                                      'cases': len(cases)}, indent=1) + '\n')
    if problems:
        sys.exit(f'FAIL {len(problems)} of {len(cases)} cases: ' + '; '.join(problems[:5]))
    print(f'PASS G2D interrupt dispatch: {len(cases)} cases; only the armed kind completes, the other kind is a '
          f'counted stray, and nothing is acknowledged unarmed')


if __name__ == '__main__':
    main()
