#!/usr/bin/env python3
"""Validate xHCI return ordering with delayed reference-backed MMIO and sanitizers."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from harness import CLANG, SANITIZE, environment

ROOT = Path(__file__).resolve().parents[1]
CASES = [f'phase-{i}' for i in range(17)] + [
    'empty', 'active', 'irq', 'address-zero', 'enable-pending', 'hub', 'borrowed', 'iso', 'halted-endpoint', 'recovery-stop', 'recovery-reset',
    'port-reset', 'busy', 'unknown-phase', 'publication', 'reset-timeout',
    'port-reset-timeout', 'command-timeout', 'transfer-deadline', 'wrong-completion',
    'deadline-extension', 'halt-timeout', 'halt-hse', 'halt-hce', 'halt-cnr', 'lost', 'error']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cases', nargs='+', choices=CASES, default=CASES)
    parser.add_argument('--skip-mutations', action='store_true')
    parser.add_argument('--mutations-only', action='store_true')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    driver = ROOT/'drivers/usb/a733_xhci.c'
    fixture = ROOT/'checks/a733-xhci-stop.c'
    units = [fixture, driver, ROOT/'runtime/device_memory_pool.c']
    inputs = units + [ROOT/'checks/a733-xhci-detach.c', ROOT/'include/a733_xhci.h',
                     ROOT/'include/device_memory_pool.h', ROOT/'checks/harness.py', Path(__file__).resolve()]
    steps = {}

    def run(label, command, expect_failure=False):
        result = subprocess.run(command, capture_output=True, text=True, env=environment())
        (out/(label+'.log')).write_text(result.stdout+result.stderr)
        steps[label] = {'command': command, 'exit': result.returncode, 'output': result.stdout.strip()}
        if bool(result.returncode) != expect_failure:
            raise SystemExit(f'{label} failed; see {out/(label+".log")}')

    def build(label, source):
        binary = out/label
        run('build-'+label, [str(CLANG), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            *SANITIZE, '-DSUNSTONE_USB_HOST_GENERIC=1', '-I'+str(ROOT/'include'),
                            str(fixture), str(source), str(units[2]), '-o', str(binary)])
        return binary

    binary = build('stop', driver)
    cases = [] if args.mutations_only else args.cases
    for case in cases:
        run(case, [str(binary), case])
    if not args.skip_mutations:
        source = driver.read_text()
        begin = source.index('    case S_DRAIN:', source.index('int a733_xhci_stop('))
        end = source.index('    case S_DISABLE_IRQ:', begin)
        mutations = {
            'halt-before-drain': (source[:begin] + '    case S_DRAIN:\n'
                                  '        (void)reset_pending;x->stop_phase=S_DISABLE_IRQ;return 0;\n' + source[end:], 'active'),
            'accept-before-hch': (source.replace('        if(!(status&1))return 0;',
                                                 '        /* Mutated: accept a running pipeline. */'), 'empty'),
        }
        for name, (changed, case) in mutations.items():
            assert changed != source
            mutant = out/(name+'.c')
            mutant.write_text(changed)
            mutant_binary = build(name, mutant)
            run('mutation-'+name, [str(mutant_binary), case], expect_failure=True)
    record = {'result': 'PASS', 'hardware_access': False, 'cases': cases, 'steps': steps,
              'inputs': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs},
              'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'reference': {'document': 'xHCI 1.2, May 2019', 'sections': ['5.4.1', '5.4.1.1', '5.4.2'],
                            'pdf_sha256': '9257ca24f34bff08e6020623f707c788ca0f0df657aa713309ff489a68b1731e'},
              'boundary': 'Production stop, IRQ and teardown paths. Host register effects and DMA writes are modeled; '
                          'physical completion, interconnect behavior and timing remain unqualified.'}
    (out/'result.json').write_text(json.dumps(record, indent=2)+'\n')
    print(json.dumps({'result': record['result'], 'cases': cases}))


if __name__ == '__main__':
    main()
