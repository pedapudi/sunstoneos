#!/usr/bin/env python3
"""Check USB waiting against a frozen copy of the common deadline runtime."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from harness import CLANG, SANITIZE, environment

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--deadline-source', type=Path, required=True,
                        help='Checkout containing the shared deadline runtime and header.')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime = args.deadline_source/'runtime/deadline.c'
    header = args.deadline_source/'include/sunstone_deadline.h'
    (out/'deadline-native.inc').write_bytes(runtime.read_bytes())
    (out/'sunstone_deadline.h').write_bytes(header.read_bytes())
    # Types and APIs are supplied by the fixture before the real runtime.
    (out/'FreeRTOS.h').write_text('')
    (out/'task.h').write_text('')
    command = [str(CLANG), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               *SANITIZE, '-I'+str(out), '-I'+str(ROOT/'include'),
               str(ROOT/'checks/usb-host-deadline.c'), '-o', str(out/'waiter')]
    compiled = subprocess.run(command, capture_output=True, text=True, env=environment())
    (out/'compile.log').write_text(compiled.stdout+compiled.stderr)
    assert compiled.returncode == 0, compiled.stderr
    result = subprocess.run([out/'waiter'], capture_output=True, text=True, env=environment())
    (out/'run.log').write_text(result.stdout+result.stderr)
    assert result.returncode == 0, result.stderr
    inputs = [runtime, header, ROOT/'apps/board/host_irq.c', ROOT/'apps/board/host_irq.h',
              ROOT/'checks/usb-host-deadline.c', Path(__file__)]
    record = dict(result='PASS', hardware_access=False, command=command,
                  inputs={str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs},
                  scope='Actual host waiter and frozen common runtime; task scheduling, compare programming and GIC effects modeled. No physical IRQ latency or full service supervision qualification.')
    (out/'result.json').write_text(json.dumps(record, indent=2)+'\n')
    print(result.stdout, end='')


if __name__ == '__main__':
    main()
