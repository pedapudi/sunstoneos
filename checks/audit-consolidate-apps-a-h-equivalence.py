#!/usr/bin/env python3
"""Application mapping, deadline and range helpers match the inline code they replaced.

Builds checks/audit-consolidate-apps-a-h-equivalence.c on the host at -O0 and
-O2 with -fsanitize=address,undefined and runs it. The program compares each
former application form (mapping checks from the ADC, Bluetooth, camera,
crypto, deinterlace, DisplayPort, G2D, GPU, video and HDMI audio applications;
the deadline samplers; the DSI deadline sum; the board radio range test) with
the shared header call that replaced it, over a seeded translation model and
edge values. The check uses no board, emulator or FreeRTOS scheduler.
"""
import argparse
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'checks/audit-consolidate-apps-a-h-equivalence.c'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='new directory for binaries and result.json')
    parser.add_argument('--cc', default='clang')
    args = parser.parse_args()
    out = args.output
    out.mkdir(parents=True)
    results = {}
    for level in ('-O0', '-O2'):
        binary = out / f'equivalence{level}'
        command = [args.cc, level, '-g', '-std=c11', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                   '-I', str(ROOT / 'include'), str(SOURCE), '-o', str(binary)]
        subprocess.run(command, check=True)
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        (out / f'run{level}.log').write_text(run.stdout + run.stderr)
        assert run.returncode == 0, run.stdout[-4000:] + run.stderr[-4000:]
        results[level] = run.stdout.strip().splitlines()[-1]
    (out / 'result.json').write_text(json.dumps({'passed': True, 'runs': results}, indent=2) + '\n')
    print('PASS application helper equivalence:', results['-O2'])


if __name__ == '__main__':
    main()
