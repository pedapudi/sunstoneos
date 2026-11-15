#!/usr/bin/env python3
"""Check that trace instruction counts leave out block entries QEMU stopped before their first instruction.

harness.trace_sections counts CPU 0's instructions from a QEMU log written with -d int,exec,nochain,in_asm. QEMU logs
"Trace" on entering a block and, when the block leaves at its first check for an exit request or a spent instruction
budget, "Stopped execution of TB chain before HOST [PC]"; it then enters the block again. Those stops fall where the
host's exit requests fall, so a count that keeps the stopped entry differs between runs of identical images. The
2026-11-15 gate of work/sched-split-a-1114 failed checks/device-memory-capacity.py that way: one extra 4-instruction
flush-loop entry in one section, from the log gate-sched-split-a-gate6-1115/evidence/images/device-memory-capacity/
trace-local-uncached.log.

The cases below use that log's lines: the flush loop block at 0x410035e0 (DC CIVAC, ADD, CMP, B.LO), its host block
address, and the semihosting lines that start sections. A copy of trace_sections without the correction must count
the stopped entry, so the case fails without the fix.
"""
import argparse
import inspect
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

LOOP, LOOP_HOST = 0x410035e0, '0x747d57806480'
OTHER, OTHER_HOST = 0x41001128, '0x747d57802140'
BLOCKS = ['IN: ', '0x410035e0:  ', 'OBJD-T: 207e0bd5000001911f0001eba3ffff54', '',
          'IN: ', '0x41001128:  ', 'OBJD-T: 1f2003d5', '']
CALL = ['Taking exception 16 [Semihosting call] on CPU 0', '...from EL3 to EL0',
        '...handling as semihosting call 0x4', '----------------']
DC_CIVAC = 0xd50b7e20


def trace(cpu, pc, host):
    return f'Trace {cpu}: {host} [00000000/{pc:016x}/000004c1/ff022200] '


def stopped(pc, host):
    return f'Stopped execution of TB chain before {host} [{pc:016x}] '


def classify(word):
    return ['dc'] if word == DC_CIVAC else []


def counted(function, out, name, lines):
    path = out / (name + '.log')
    path.write_text('\n'.join(lines) + '\n')
    return function(path, classify, ['dc'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output
    out.mkdir(parents=True, exist_ok=False)
    cases = {}

    # Five loop entries in the counted section, the third stopped as in the gate log: four executed entries.
    loop = [trace(0, LOOP, LOOP_HOST)] * 3 + [stopped(LOOP, LOOP_HOST)] + [trace(0, LOOP, LOOP_HOST)] * 2
    gate = BLOCKS + CALL + loop + CALL
    expected = [{'instructions': 16, 'dc': 4}, {'instructions': 0, 'dc': 0}]
    assert counted(harness.trace_sections, out, 'stopped-loop', gate) == expected
    cases['stopped-entry-removed'] = True

    # The same entries without the stop count the same: the stop only moves where the block runs.
    plain = BLOCKS + CALL + [trace(0, LOOP, LOOP_HOST)] * 4 + CALL
    assert counted(harness.trace_sections, out, 'unstopped-loop', plain) == expected
    cases['stop-position-independent'] = True

    # A stop of another CPU's entry leaves CPU 0's count unchanged, also when that CPU runs the same block.
    other = BLOCKS + CALL + [trace(0, LOOP, LOOP_HOST), trace(1, LOOP, LOOP_HOST), stopped(LOOP, LOOP_HOST),
                             trace(0, OTHER, OTHER_HOST), trace(2, OTHER, OTHER_HOST),
                             stopped(OTHER, OTHER_HOST)] + CALL
    assert counted(harness.trace_sections, out, 'other-cpu', other) == [{'instructions': 5, 'dc': 1},
                                                                         {'instructions': 0, 'dc': 0}]
    cases['other-cpu-stop-kept'] = True

    # A stop that does not name the nearest entry's block, or follows no entry, is refused.
    for name, lines in (('wrong-block', [trace(0, LOOP, LOOP_HOST), stopped(OTHER, OTHER_HOST)]),
                        ('wrong-host', [trace(0, LOOP, LOOP_HOST), stopped(LOOP, OTHER_HOST)]),
                        ('repeated', [trace(0, LOOP, LOOP_HOST), stopped(LOOP, LOOP_HOST),
                                      stopped(LOOP, LOOP_HOST)]),
                        ('interleaved', [trace(0, LOOP, LOOP_HOST), trace(1, OTHER, OTHER_HOST),
                                         stopped(LOOP, LOOP_HOST)])):
        try:
            counted(harness.trace_sections, out, name, BLOCKS + CALL + lines + CALL)
        except RuntimeError as error:
            assert 'follows no entry of that block' in str(error), error
        else:
            raise AssertionError(f'{name}: an unpaired stop was accepted')
    cases['unpaired-stop-refused'] = True

    # Mutation: trace_sections without the correction keeps the stopped entry and fails the first case.
    source = inspect.getsource(harness.trace_sections)
    mutated = source.replace("entry[2]['instructions'] -= 1", 'pass').replace('entry[2][name] -= 1', 'pass')
    assert mutated.count('pass') == source.count('pass') + 2
    scope = {'re': re, 'Path': Path}
    exec(mutated, scope)
    kept = counted(scope['trace_sections'], out, 'mutated', gate)
    assert kept == [{'instructions': 20, 'dc': 5}, {'instructions': 0, 'dc': 0}], kept
    assert kept != expected
    cases['mutation-keeps-bad-count'] = True

    harness.save(out / 'result.json', {'passed': True, 'physical_access': False, 'cases': cases})
    print(f'PASS {len(cases)} QEMU trace stop cases')


if __name__ == '__main__':
    main()
