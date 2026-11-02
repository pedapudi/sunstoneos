#!/usr/bin/env python3
"""Check USB software batching against delayed controller completions."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import host_rtos
from harness import CLANG, environment

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    units = [ROOT/'checks/usb-host-ready-work.c', ROOT/'drivers/usb/a733_xhci.c',
             ROOT/'runtime/device_memory_pool.c']
    inputs = units + [ROOT/'checks/usb-host-irq.c', ROOT/'apps/board/host_irq.c', ROOT/'apps/board/host_irq.h',
                      ROOT/'checks/a733-xhci-detach.c', ROOT/'checks/register_latency.h',
                      ROOT/'include/a733_xhci.h', ROOT/'checks/harness.py', Path(__file__).resolve()]
    steps = {}

    def run(label, command, expected=0):
        result = subprocess.run(list(map(str, command)), capture_output=True, text=True, env=environment())
        (out/(label+'.log')).write_text(result.stdout+result.stderr)
        steps[label] = dict(command=list(map(str, command)), exit=result.returncode,
                            output=result.stdout.strip())
        if expected == 0:
            assert result.returncode == 0, f'{label}: see {out/(label+".log")}'
        else:
            assert result.returncode != 0 and expected in result.stderr, result.stderr
        return result.stdout.strip()

    link = ['-Wl,--wrap=ulTaskGenericNotifyTake']
    def build(binary, extra=()):
        # Build through the shared host shim; the log keeps the same name as before.
        host_rtos.build(out, ['checks/usb-host-ready-work.c', 'drivers/usb/a733_xhci.c', 'runtime/device_memory_pool.c'],
                        binary.name, defines=['SUNSTONE_USB_HOST_GENERIC=1', *extra],
                        includes=[ROOT/'checks', ROOT/'apps/board'], link=link)
        steps[binary.name+'-build'] = dict(command=['host_rtos.build', binary.name], exit=0, output='')

    binary = out/'ready-work'
    build(binary)
    for case in ('normal', 'long-delay', 'stuck'):
        run(case, [binary, case])
    # Compile the original wait condition into the same real-core fixture.
    source = (ROOT/'apps/board/host_irq.c').read_text()
    condition = '(irq.draining||(board.host.core_started&&a733_xhci_work_pending(&board.host.core)))'
    assert source.count(condition) == 1
    original = out/'host-irq-original.c'
    original.write_text(source.replace(condition, 'irq.draining'))
    baseline = out/'original-wait'
    build(baseline, ['SUNSTONE_HOST_IRQ_SOURCE="'+str(original)+'"'])
    run('baseline', [baseline, 'baseline'])
    run('reject-original-wait', [baseline], 'ready software step slept for a tick')
    # The owner's interrupt mask, claim and rearm code on the same kernel and shim.
    irq_binary = host_rtos.build(out, ['checks/usb-host-irq.c'], 'host-irq', defines=['SUNSTONE_USB_HOST_GENERIC=1'],
                                 includes=[ROOT/'checks', ROOT/'apps/board'], link=link + ['-Wl,--wrap=xTaskGetSchedulerState'])
    run('host-irq', [irq_binary])
    # Negative control: claiming a pending interrupt without masking the source must fail it.
    mutant = host_rtos.mutated(out, 'apps/board/host_irq.c',
                               'else if(irq.draining||atomic_load_explicit(&irq.pending,memory_order_acquire))r=quiescent_mask();',
                               'else if(irq.draining)r=quiescent_mask();', 'claim-unmasked.c')
    control = host_rtos.build(out, ['checks/usb-host-irq.c'], 'host-irq-control', flags=['-w'],
                              defines=['SUNSTONE_USB_HOST_GENERIC=1', 'SUNSTONE_HOST_IRQ_SOURCE="%s"' % mutant],
                              includes=[ROOT/'checks', ROOT/'apps/board'], link=link + ['-Wl,--wrap=xTaskGetSchedulerState'])
    run('reject-unmasked-claim', [control], 'Assertion')
    result = dict(result='PASS', hardware_access=False, steps=steps,
                  inputs={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},
                  compiler=dict(path=str(CLANG), version=subprocess.check_output([str(CLANG), '--version'], text=True)),
                  scope='Actual board wait and xHCI core with deferred reset/run register effects and delayed command/TD events. '
                        'Kernel, clock, device effects and GIC are modeled. Ready work batches; idle and hardware waits retain '
                        'the existing tick timeout. No physical timing or full real-time qualification.')
    (out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(dict(result=result['result'],
                         baseline=steps['baseline']['output'], fixed=steps['normal']['output']), indent=2))


if __name__ == '__main__':
    main()
