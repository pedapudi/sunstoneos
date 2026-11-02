#!/usr/bin/env python3
"""Check USB waiting against the common deadline runtime on the real FreeRTOS kernel."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import host_rtos

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--deadline-source', type=Path, default=ROOT,
                        help='Checkout containing the shared deadline runtime and header.')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime = args.deadline_source/'runtime/deadline.c'
    header = args.deadline_source/'include/sunstone_deadline.h'
    link = ['-Wl,--wrap=ulTaskGenericNotifyTake']
    includes = [ROOT/'apps/board', ROOT/'apps/os', ROOT/'checks/usb-diagnostic-protocol/include']
    def build(name, deadline_c, defines=()):
        return host_rtos.build(out, ['checks/usb-host-deadline.c', deadline_c], name, defines=defines,
                               includes=[args.deadline_source/'include', *includes], link=link, deadline=True,
                               flags=['-w'] if name != 'waiter' else [])
    waiter = build('waiter', runtime)
    command = [str(waiter)]
    result = host_rtos.run(waiter)
    (out/'run.log').write_text(result.stdout+result.stderr)
    assert result.returncode == 0, result.stderr
    # Negative controls: each reintroduced bug must fail the waiter.
    controls = {}
    for name, old, new in (('batch-bound', '(irq.draining||schedule.ready)&&++irq.burst<32', '(irq.draining||schedule.ready)&&++irq.burst<33'),
                           ('retry-rounded-up', 'now+50', 'now+5000')):
        mutant = host_rtos.mutated(out, 'apps/board/host_irq.c', old, new, name+'.c')
        done = host_rtos.run(build('control-'+name, runtime, ['HOST_IRQ_SOURCE="%s"' % mutant]))
        assert done.returncode != 0, ('control passed', name)
        controls[name] = 'fails'
    inputs = [runtime, header, ROOT/'checks/host/host_rtos.c', ROOT/'apps/board/host_irq.c', ROOT/'apps/board/host_irq.h',
              ROOT/'checks/usb-host-deadline.c', Path(__file__)]
    record = dict(result='PASS', hardware_access=False, negative_controls=controls, command=command,
                  inputs={str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs},
                  scope='Actual host waiter and common runtime on the real FreeRTOS kernel; compare programming and GIC effects modeled. No physical IRQ latency or full service supervision qualification.')
    (out/'result.json').write_text(json.dumps(record, indent=2)+'\n')
    print(result.stdout, end='')


if __name__ == '__main__':
    main()
