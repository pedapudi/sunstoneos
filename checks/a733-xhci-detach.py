#!/usr/bin/env python3
"""xHCI core detach, slot disable and reattach against a modeled controller; no board access."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CASES = ['direct', 'hub', 'address-detach', 'address-error', 'stop-race', 'transaction-error', 'error-expiry',
         'hub-error-expiry', 'tier-order', 'no-csc', 'generation', 'stop-timeout', 'disable-fail', 'stale-event']


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--cases', nargs='+', choices=CASES, default=CASES)
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True)
    compiler = Path(shutil.which('clang-21')).resolve()
    units = [ROOT/'checks/a733-xhci-detach.c', ROOT/'drivers/usb/a733_xhci.c', ROOT/'runtime/device_memory_pool.c']
    inputs = {str(u): hashlib.sha256(u.read_bytes()).hexdigest()
              for u in units+[ROOT/'include/a733_xhci.h', ROOT/'include/device_memory_pool.h', Path(__file__).resolve()]}
    binary = out/'detach'
    command = [str(compiler), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
               '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-I'+str(ROOT/'include'), *map(str, units), '-o', str(binary)]
    steps = {}

    def run(label, argv):
        r = subprocess.run(argv, capture_output=True, text=True)
        (out/(label+'.log')).write_text(r.stdout+r.stderr)
        steps[label] = {'command': argv, 'exit': r.returncode, 'output': r.stdout.strip()}
        if r.returncode:
            raise SystemExit(f'{label} failed; see {out/(label+".log")}')

    run('build', command)
    for case in a.cases:
        run(case, [str(binary), case])
    result = {'result': 'PASS', 'cases': a.cases, 'hardware_access': False, 'inputs': inputs,
              'compiler': {'path': str(compiler), 'version': subprocess.check_output([str(compiler), '--version'], text=True)},
              'steps': steps, 'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'scope': 'Actual xHCI core with a register, ring and context model. Controller timing, '
                       'DMA visibility and physical disconnect behavior are not established.'}
    (out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k: result[k] for k in ('result', 'cases')}, indent=2))


if __name__ == '__main__':
    main()
