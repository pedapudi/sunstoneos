#!/usr/bin/env python3
"""Retained xHCI DMA ownership with real core rings and modeled hardware."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from harness import CLANG, SANITIZE, environment

ROOT = Path(__file__).resolve().parents[1]
CASES = ['echo', 'detach', 'generation', 'iso', 'iso-detach', 'full-speed', 'control-stall', 'interrupt-out', 'six-records', 'irq-dma', 'address-reuse']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cases', nargs='+', choices=CASES+['legacy-iso'])
    parser.add_argument('--devices', type=int, help='device records; the arena grows to the largest the layout limit allows')
    parser.add_argument('--profile', choices=['generic', 'legacy'], default='generic')
    args = parser.parse_args()
    args.cases = args.cases or (CASES if args.profile == 'generic' else ['legacy-iso'])
    assert all((case == 'legacy-iso') == (args.profile == 'legacy') for case in args.cases)
    out = args.output.resolve()
    out.mkdir(parents=True)
    units = [ROOT/'checks/a733-xhci-dma.c', ROOT/'drivers/usb/a733_xhci.c', ROOT/'runtime/device_memory_pool.c']
    inputs = units+[ROOT/'checks/a733-xhci-detach.c', ROOT/'include/a733_xhci.h',
                    ROOT/'include/device_memory_pool.h', ROOT/'checks/harness.py', Path(__file__).resolve()]
    binary = out/'dma'
    steps = {}

    def run(name, command):
        result = subprocess.run(command, capture_output=True, text=True, env=environment())
        (out/(name+'.log')).write_text(result.stdout+result.stderr)
        steps[name] = {'command': command, 'exit': result.returncode, 'output': result.stdout.strip()}
        if result.returncode:
            raise SystemExit(f'{name} failed; see {out/(name+".log")}')

    run('build', [str(CLANG), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                  *SANITIZE, '-DSUNSTONE_USB_HOST_GENERIC='+str(int(args.profile == 'generic')),
                  *(['-DSUNSTONE_USB_HOST_DEVICES=%d' % args.devices, '-DA733_USB_MEMORY_END=0x43000000'] if args.devices else []), '-I'+str(ROOT/'include'), *map(str, units), '-o', str(binary)])
    for case in args.cases:
        run(case, [str(binary), case])
    record = {'result': 'PASS', 'hardware_access': False, 'profile': args.profile, 'devices': args.devices, 'cases': args.cases, 'steps': steps,
              'inputs': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},
              'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'compiler': {'path': str(CLANG), 'sha256': hashlib.sha256(CLANG.resolve().read_bytes()).hexdigest(),
                           'version': subprocess.check_output([str(CLANG), '--version'], text=True)},
              'boundary': 'Actual core DMA submission, completion and teardown. Register/context effects '
                          'and DMA writes are modeled; physical coherence and USB timing are unqualified.'}
    (out/'result.json').write_text(json.dumps(record, indent=2)+'\n')
    print(json.dumps({'result': record['result'], 'cases': args.cases}))


if __name__ == '__main__':
    main()
