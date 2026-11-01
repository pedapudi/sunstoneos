#!/usr/bin/env python3
"""Reject overlapping G2D sessions and wrapping display command spans before access.

Runs CPU admission and command construction under ASan/UBSan. Device effects,
physical pixels, interrupts and retirement are outside this check's scope.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import harness

ROOT = Path(__file__).resolve().parents[1]
BASELINE = 'aada8515'
G2D = 'drivers/g2d/a733_g2d.c'
DE = 'drivers/display/a733_display_engine.c'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    commands = []

    def run(command, expected_failure=False):
        result = subprocess.run(command, cwd=ROOT, env=harness.environment(),
                                capture_output=True, text=True)
        commands.append(dict(command=list(map(str, command)), returncode=result.returncode,
                             stdout=result.stdout, stderr=result.stderr))
        (args.output/'commands.json').write_text(json.dumps(commands, indent=2)+'\n')
        if expected_failure:
            assert result.returncode != 0, 'baseline unexpectedly passed the negative case'
        else:
            result.check_returncode()
        return result.stdout

    baseline = {}
    for name in (G2D, DE):
        source = subprocess.check_output(['git', 'show', f'{BASELINE}:{name}'], cwd=ROOT)
        destination = args.output/('baseline-'+Path(name).name)
        destination.write_bytes(source)
        baseline[name] = dict(path=destination, sha256=hashlib.sha256(source).hexdigest())
    cc = str(harness.CLANG)
    common = [cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
              *harness.SANITIZE, '-ffunction-sections', '-fdata-sections',
              '-I'+str(ROOT/'include'), '-I'+str(ROOT/'drivers/g2d')]
    previous = args.output/'previous.o'
    run([*common, '-Da733_g2d_commands=previous_commands', '-c',
         str(baseline[G2D]['path']), '-o', str(previous)])
    fixture = ROOT/'checks/display-command-admission.c'
    old = args.output/'baseline-check'
    run([*common, str(fixture), str(baseline[G2D]['path']), str(baseline[DE]['path']),
         str(previous), '-Wl,--gc-sections', '-o', str(old)])
    run([str(old), 'overlap'], expected_failure=True)
    assert 'a733_g2d_commands(commands, 4096, s)' in commands[-1]['stderr']
    run([str(old), 'ring-wrap'], expected_failure=True)
    assert 'AddressSanitizer' in commands[-1]['stderr']
    current = args.output/'check'
    run([*common, str(fixture), str(ROOT/G2D), str(ROOT/DE), str(previous),
         '-Wl,--gc-sections', '-o', str(current)])
    result = run([str(current)])
    freestanding = []
    for name in (G2D, DE):
        obj = args.output/(Path(name).stem+'.o')
        run([cc, '--target=aarch64-none-elf', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
             '-ffreestanding', '-fno-builtin', '-mgeneral-regs-only', '-mstrict-align',
             '-I'+str(ROOT/'include'), '-I'+str(ROOT/'runtime/include'),
             '-I'+str(ROOT/'drivers/g2d'), '-c', str(ROOT/name), '-o', str(obj)])
        freestanding.append(dict(source=name, sha256=hashlib.sha256(obj.read_bytes()).hexdigest()))
    inputs = [G2D, DE, 'checks/display-command-admission.c', 'checks/display-command-admission.py',
              'checks/harness.py', 'include/a733_range.h', 'include/a733_g2d.h',
              'include/a733_g2d_application.h', 'include/a733_board_application.h',
              'include/a733_display_engine.h', 'include/a733_tcon_lcd.h',
              'include/a733_dsi_video.h', 'drivers/g2d/g2d_mixer_type.h']
    report = dict(passed=True, physical_access=False, scope=__doc__, baseline_revision=BASELINE,
                  baseline_sha256={name:value['sha256'] for name,value in baseline.items()},
                  compiler=run([cc, '--version']), result=result, freestanding=freestanding,
                  inputs={name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in inputs})
    (args.output/'result.json').write_text(json.dumps(report, indent=2)+'\n')
    print(result, end='')


if __name__ == '__main__':
    main()
