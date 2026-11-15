#!/usr/bin/env python3
"""Run the host model of the shared device-service fault report (apps/os/device-fault.h) and refuse its mutants.

checks/os-device-fault.c runs the header's own functions on two threads, the owner task's sequence and one quiesce
step, and places the step's loads at every point among the owner's stores. The model fails when the concurrent step
reports a fault that a close is about to settle, reports a value other than the first fault's, when the step after
the owner finishes returns the wrong result, or when a store or load of `reported` lacks release or acquire order.
The image-level checks (os-vdec-fault-return.py, os-g2d-fault-return.py, os-rtc-fault-return.py) run the same
helper inside each service; this model is where its mutants are refused.
"""
import argparse
from pathlib import Path
import subprocess

import host_rtos
from harness import environment, save

GATE_RUNS = [['--output', '{output}']]
ROOT = Path(__file__).resolve().parents[1]
HEADER = 'apps/os/device-fault.h'
CHECK_INPUTS = ['apps/os/device-fault.h', 'apps/os/quiesce.h', 'include/sunstone_crumb.h', 'checks/os-device-fault.c', 'checks/host_rtos.py']
MUTANTS = [
    ('if(atomic_load_explicit(&f->reported, memory_order_acquire)) {',
     'if(atomic_load_explicit(&f->phase, memory_order_seq_cst) == OS_DEVICE_FAULT &&\n'
     '       atomic_load_explicit(&f->close_started, memory_order_seq_cst)) {',
     'the step reads phase and close_started'),
    ('    if(!f->fault_step){f->fault_step=step;f->fault_value=value;}\n'
     '    atomic_store_explicit(&f->phase, OS_DEVICE_FAULT, memory_order_seq_cst);\n'
     '    if(atomic_load_explicit(&f->close_started, memory_order_seq_cst) || unsettled)\n'
     '        atomic_store_explicit(&f->reported, 1, memory_order_release);\n',
     '    atomic_store_explicit(&f->phase, OS_DEVICE_FAULT, memory_order_seq_cst);\n'
     '    if(atomic_load_explicit(&f->close_started, memory_order_seq_cst) || unsettled)\n'
     '        atomic_store_explicit(&f->reported, 1, memory_order_release);\n'
     '    if(!f->fault_step){f->fault_step=step;f->fault_value=value;}\n',
     'reported stored before the fault fields'),
    ('if(!f->fault_step){f->fault_step=step;f->fault_value=value;}', '{f->fault_step=step;f->fault_value=value;}',
     'a later fault rewrites the fields'),
    ('|| unsettled)', ')', 'an unsettled fault waits for a close'),
    ('atomic_store_explicit(&f->reported, 1, memory_order_release);', 'atomic_store_explicit(&f->reported, 1, memory_order_relaxed);',
     'reported stored without release'),
    ('if(atomic_load_explicit(&f->reported, memory_order_acquire)) {', 'if(atomic_load_explicit(&f->reported, memory_order_relaxed)) {',
     'reported loaded without acquire'),
]


def build(output, name, header_dir):
    return host_rtos.build(output, ['checks/os-device-fault.c'], name, includes=[header_dir, ROOT / 'apps/os'], shim=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    exe = build(args.output, 'os-device-fault', ROOT / 'apps/os')
    done = subprocess.run([str(exe)], capture_output=True, text=True, env=environment(), timeout=300)
    assert done.returncode == 0, ('model failed', done.stdout, done.stderr)
    refused = {}
    for index, (old, new, why) in enumerate(MUTANTS):
        directory = args.output / ('mutant-%d' % index)
        source = host_rtos.mutated(directory, HEADER, old, new)
        mutant = build(directory, 'os-device-fault', source.parent)
        run = subprocess.run([str(mutant)], capture_output=True, text=True, env=environment(), timeout=300)
        assert run.returncode == 1, ('mutant survived', why, run.stdout, run.stderr)
        refused[why] = run.stdout.strip().splitlines()[-1]
    save(args.output / 'result.json', {'passed': True, 'model': done.stdout, 'mutants': refused})
    print(done.stdout, end='')
    print('PASS device fault report: %d mutants refused' % len(refused))


if __name__ == '__main__':
    main()
