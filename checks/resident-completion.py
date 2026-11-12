#!/usr/bin/env python3
"""Check deliberate completion and failure discrimination in resident supervision."""
import argparse
import json
from pathlib import Path
import subprocess

import host_rtos
from harness import environment

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--out', '{output}']]
CHECK_INPUTS = ['apps/os/services.c', 'apps/os/application.c', 'apps/os/main.patch',
                'checks/resident-completion.c']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    rows = []
    for diagnostic in (False, True):
        definitions = ['SUNSTONE_BOARD_SUPERVISED=1', 'SUNSTONE_BOARD_USB2_DUAL=1',
                       'SUNSTONE_A733_OS=1', 'SUNSTONE_A733_FAULT_PAGE=1',
                       'SERVICES_SOURCE="%s"' % (ROOT / 'apps/os/services.c')]
        if diagnostic:
            definitions.append('SUNSTONE_A733_OS_RESIDENT_STOP_ON_FINISH=1')
        binary = host_rtos.build(args.out, ['checks/resident-completion.c', 'runtime/deadline.c'],
                                 'diagnostic' if diagnostic else 'production', defines=definitions,
                                 includes=[ROOT / 'checks/usb-diagnostic-protocol/include',
                                           ROOT / 'platform/a733-smp-cached', ROOT / 'apps/os', ROOT / 'apps/board'])
        for mode, name in enumerate(('liveness-bypass', 'silent-services', 'failed-result',
                                     'fault-before-stop', 'stale-service', 'successful-completion')):
            subprocess.run([str(binary), str(mode)], check=True, env=environment(), capture_output=True)
            rows.append({'diagnostic': diagnostic, 'case': name, 'passed': True})
    (args.out / 'results.json').write_text(json.dumps({'physical_access': False, 'rows': rows}, indent=2) + '\n')
    print('PASS 12 resident completion cases; no device access')


if __name__ == '__main__':
    main()
