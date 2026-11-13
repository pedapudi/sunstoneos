#!/usr/bin/env python3
"""Reproduce the board's NPU completion failure (ESR 0x4001a072, FAR 0x7fffbffe) from a register model.

The driver's completion path (ticks, window, slot_of and complete of drivers/npu/npu_runtime_queue.c) is
extracted from its source with its line numbers kept, so each crumb site is the board's. Only the system-register
reads, the barriers, the raw MMIO reads and the crumb SVC are replaced; checks/npu-completion-idle.c supplies the
event and idle words.

  board-crumb      the source of the failing images (cb0e09a8 for a9e1c5345580, 519b9d3b for 45f12b20e357):
                   event 1 with the front end and memory controller still busy gives the board's exact words
  events-reading   an event word of 0x7fffbffe would give a different site and register offset
  bounded          the bringup source completes on the same sequence and records busy bits 0x4001
  bounded-stuck    a unit busy past the 100 us bound remains a fault
"""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess

import host_rtos
from harness import environment, save

ROOT = Path(__file__).resolve().parents[1]
DRIVER = 'drivers/npu/npu_runtime_queue.c'
CHECK_INPUTS = [DRIVER, 'include/npu_runtime_queue.h', 'include/a733_npu_idle.h', 'include/sunstone_crumb.h',
                'checks/npu-completion-idle.c', 'checks/npu-completion-idle.py', 'checks/host/strict_mmio.c',
                'checks/host/strict_mmio.h']
# The OS sources of the two failing images, from their queue entries' hypotheses.
FAILING = {'cb0e09a8e095cbd22b23b359b430012d94cce169': 'a9e1c5345580 (1794506252076994875-247497f5)',
           '519b9d3bce2af8c92f46a5b0cc9d640a874b81e4': '45f12b20e357 (1794596079966938149-24a0a868)'}
FUNCTIONS = {'ticks': r'static uint64_t ticks\(void\)', 'window': r'static int window\(struct npu_runtime_queue \*q\)',
             'slot_of': r'static unsigned slot_of\(const struct npu_runtime_queue \*q\)',
             'complete': r'static int complete\(void \*cookie\)'}
REPLACE = [(r'__asm__ volatile("isb\n mrs %0,cntpct_el0" : "=r"(value) :: "memory");', 'value = host_ticks();'),
           ('__asm__ volatile("dsb sy" ::: "memory");', 'host_dsb();'),
           ('*(volatile uint32_t *)(base + ', 'host_read32(base + '),
           ('sunstone_crumb_fail(', 'host_crumb_fail(')]
CASES = {'board-crumb': list(FAILING), 'events-reading': list(FAILING) + [None], 'bounded': [None], 'bounded-stuck': [None]}


def extract(text):
    """The completion functions with #line directives, and the event register's definition."""
    out = [re.search(r'^#define NPU_REG_EVENTS .*$', text, re.M).group(0)]
    for name, signature in FUNCTIONS.items():
        found = re.search('^' + signature + r'\n\{\n.*?^\}\n', text, re.M | re.S)
        if not found:
            assert name == 'slot_of', 'no ' + name + ' in the driver'
            continue
        line = text.count('\n', 0, found.start()) + 1
        out.append('#line %d "%s"\n%s' % (line, DRIVER, found.group(0)))
    body = '\n'.join(out) + '\n'
    for old, new in REPLACE:
        assert old in body, 'the driver no longer has: ' + old
        body = body.replace(old, new)
    assert '__asm__' not in body and 'volatile uint32_t *' not in body, 'an unreplaced device access remains'
    return body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    inputs = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in CHECK_INPUTS}
    builds = {}
    for revision in {r for revisions in CASES.values() for r in revisions}:
        label = revision[:8] if revision else 'worktree'
        text = (subprocess.run(['git', '-C', str(ROOT), 'show', revision + ':' + DRIVER], check=True,
                               capture_output=True, text=True).stdout if revision else (ROOT / DRIVER).read_text())
        generated = args.output / label / 'generated'
        generated.mkdir(parents=True)
        (generated / 'npu_completion_extract.h').write_text(extract(text))
        builds[revision] = host_rtos.build(args.output / label, ['checks/npu-completion-idle.c', 'checks/host/strict_mmio.c'],
                                           'npu-completion-idle', includes=[generated])
    rows = []
    for case, revisions in CASES.items():
        for revision in revisions:
            run = subprocess.run([str(builds[revision]), case], capture_output=True, text=True, env=environment())
            label = revision[:8] if revision else 'worktree'
            (args.output / ('%s-%s.log' % (case, label))).write_text(run.stdout + run.stderr)
            rows.append({'case': case, 'source': label, 'image': FAILING.get(revision, 'bringup'),
                         'passed': run.returncode == 0, 'output': run.stdout.strip().splitlines()[:1]})
            assert run.returncode == 0, (case, label, run.stdout, run.stderr)
    assert inputs == {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in CHECK_INPUTS}
    save(args.output / 'result.json', {'passed': True, 'physical_access': False, 'cases': rows, 'inputs': inputs,
         'limits': 'Driver completion C with modeled event and idle words and a modeled counter; no board timing.'})
    print('PASS the failing images give ESR 0x4001a072 FAR 0x7fffbffe on event 1 with FE and MC busy; bringup completes')


if __name__ == '__main__':
    main()
