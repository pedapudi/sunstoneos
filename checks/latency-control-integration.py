#!/usr/bin/env python3
"""Run the combined eight-core control/latency image as a functional check.

The QEMU virt platform supplies interrupt and scheduler execution. Its timer
counts and histogram totals verify composition; no host timing qualifies an
A733 latency or jitter bound.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'


def reverse_wrappers(build, output):
    """Reverse actual wrapper objects; leave the selected image untouched."""
    entries = json.loads((build / 'compile_commands.json').read_text())
    replacements = {}
    for name, definition in [('latency_irq.c', '__real_vApplicationIRQHandler=ss_control_inner_irq'),
                             ('control_irq.c', '__wrap_vApplicationIRQHandler=ss_control_inner_irq')]:
        original = 'CMakeFiles/latency-control-model.dir/runtime/' + name + '.obj'
        entry = next(row for row in entries if row['output'] == original)
        command = shlex.split(entry['command'])
        if '-DSUNSTONE_LATENCY_CONTROL_MODE=0' not in command:
            raise ValueError('negative wrapper case requires a busy-mode build')
        command = [arg for arg in command if not arg.startswith(('-D__wrap_vApplicationIRQHandler=',
                                                               '-D__real_vApplicationIRQHandler='))]
        command.insert(1, '-D' + definition)
        replacement = output / (name + '.obj')
        command[command.index('-o') + 1] = str(replacement)
        subprocess.run(command, cwd=build, check=True, capture_output=True, text=True)
        replacements[original] = str(replacement)
    image = output / 'reversed-wrappers.elf'
    link = shlex.split((build / 'CMakeFiles/latency-control-model.dir/link.txt').read_text())
    link = [replacements.get(arg, arg) for arg in link]
    link = ['-Wl,-Map,' + str(output / 'reversed-wrappers.map') if arg.startswith('-Wl,-Map,') else arg
            for arg in link]
    link[link.index('-o') + 1] = str(image)
    subprocess.run(link, cwd=build, check=True, capture_output=True, text=True)
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--qemu', type=Path, default=CACHE / 'host/usr/bin/qemu-system-aarch64')
    parser.add_argument('--allow-runtime-error', action='store_true',
                        help='also accept coherent error shutdown as functional evidence; counts remain unqualified')
    parser.add_argument('--negative-wrapper-order', action='store_true',
                        help='reverse only the two wrapper objects; require rejection of secondary timer accounting')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    image = args.build / 'latency-control-model.elf'
    if args.negative_wrapper_order:
        image = reverse_wrappers(args.build.resolve(), args.output.resolve())
    command = [str(args.qemu), '-M', 'virt,gic-version=3,secure=off,virtualization=off',
               '-cpu', 'max', '-smp', '8', '-m', '128M', '-nographic', '-nic', 'none',
               '-monitor', 'none', '-semihosting-config', 'enable=on,target=native',
               '-kernel', str(image)]
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        log, code = result.stdout + result.stderr, result.returncode
    except subprocess.TimeoutExpired as error:
        log = (error.stdout or b'').decode() + (error.stderr or b'').decode()
        code = 124
    (args.output / 'qemu.log').write_text(log)
    counts_complete = 'PASS control qualification model:' in log
    error_shutdown = 'PASS control error shutdown model:' in log
    checks = {
        'exit': code == 0,
        'control_histograms_and_writer_shutdown': counts_complete or (args.allow_runtime_error and error_shutdown),
        'base_eight_core_wakes_and_memory': 'PASS latency model:' in log,
        'no_failure': 'FAIL ' not in log,
    }
    if args.negative_wrapper_order:
        checks = {'reversed_order_rejected': code != 0 and
                  'FAIL control PPI reached base tick recorder' in log}
    report = {'kind': 'functional_only', 'sample_counts_complete': counts_complete,
              'negative_wrapper_order': args.negative_wrapper_order,
              'runtime_error_shutdown': error_shutdown, 'image': str(image),
              'image_sha256': hashlib.sha256(image.read_bytes()).hexdigest(),
              'command': command, 'returncode': code, 'checks': checks,
              'passed': all(checks.values())}
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
