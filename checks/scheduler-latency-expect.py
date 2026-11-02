#!/usr/bin/env python3
"""Generate and check the complete CPU expectation when kernel locks can be absent."""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import runpy
import struct

if not __debug__:
    raise SystemExit('checks require assertions')

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/latency/expect.py', 'apps/latency/workload.h', 'tools/board.py']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--pmu', action='store_true',
                        help='the report carries the PMU tail of SUNSTONE_LATENCY_PMU (1216 more bytes, no constraint)')
    parser.add_argument('--timing-open', action='store_true',
                        help='accept any timing maximum, overrun context and quantile: a measurement run, not a qualification')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    original = runpy.run_path(str(ROOT / 'apps/latency/expect.py'))['expectation']()
    expected = copy.deepcopy(original)
    # Owner scheduling can use no task/ISR compatibility lock on a core. All
    # measured maxima, quantile bounds and workload sample counts stay strict.
    counts = set()
    for core in range(8):
        for metric in range(2, 6):
            counts.add((320 + core * 640 + metric * 56) // 4)
        for lock in range(2):
            counts.add((20224 + core * 16 + lock * 8) // 4)
    for index in counts:
        rule = original['words'][index]
        assert rule == {'min': 1, 'max': 0xffffffff}, (index, rule)
        expected['words'][index] = {'min': 0, 'max': rule['max']}
    assert {i for i, (a, b) in enumerate(zip(original['words'], expected['words']))
            if a != b} == counts
    if args.timing_open:
        # As the lock worker's matched comparison: coverage, metadata and wake counts stay exact.
        anyword = {'min': 0, 'max': 0xffffffff}
        words = expected['words']
        for core in range(8):
            for metric in range(8):
                if metric == 0 and core: continue
                base = 320 + core * 640 + metric * 56
                for delta in (8, 32, 40, 48): words[(base + delta) // 4:(base + delta) // 4 + 2] = [anyword, anyword]
        for phase in range(3):
            for core in range(8):
                for metric in range(8):
                    if metric == 0 and core: continue
                    if metric in (6, 7) and not (phase == 1 and metric == 6 or phase == 2 and metric == 7): continue
                    base = 5440 + (phase * 8 + core) * 616 + metric * 56
                    for delta in (8, 16, 24): words[(base + delta) // 4:(base + delta) // 4 + 2] = [anyword, anyword]
    if args.pmu:
        expected['words'] += [{'min': 0, 'max': 0xffffffff}] * (1216 // 4)
    assert len(expected['words']) * 4 == 63872 + (1216 if args.pmu else 0)
    path = args.output / 'expect.json'
    path.write_text(json.dumps(expected, indent=2) + '\n')

    spec = importlib.util.spec_from_file_location('scheduler_board_expect', ROOT / 'tools/board.py')
    board = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(board)
    admitted = board.expectation(str(path))
    # Exercise the actual runner matcher. These numeric boundary fixtures test
    # expectation acceptance, not the producer or its cross-field consistency.
    values = [word if type(word) is int else word['min'] for word in expected['words']]

    def payload(words):
        return struct.pack('<' + 'I' * len(words), *words)

    assert board.matches_expectation(admitted, payload(values))
    if args.pmu or args.timing_open:
        (args.output / 'result.json').write_text(json.dumps({'passed': True, 'physical_access': False,
            'output_bytes': len(values) * 4, 'pmu': args.pmu, 'timing_open': args.timing_open}, indent=2) + '\n')
        print('PASS CPU expectation: PMU tail %s, timing %s' % ('on' if args.pmu else 'off', 'open' if args.timing_open else 'bounded'))
        return
    legacy = {'mode': 'u32le', 'words': original['words']}
    assert not board.matches_expectation(legacy, payload(values))
    for index in counts:
        held = values.copy()
        held[index] = 1
        assert board.matches_expectation(admitted, payload(held))
    # Every remaining bounded/fixed field must still reject its first invalid
    # word. Unconstrained 32-bit ranges have no representable invalid value.
    refusals = 0
    for index, rule in enumerate(expected['words']):
        if type(rule) is int:
            bad = rule ^ 1
        elif rule['max'] < 0xffffffff:
            bad = rule['max'] + 1
        elif rule['min']:
            bad = rule['min'] - 1
        else:
            continue
        broken = values.copy()
        broken[index] = bad
        assert not board.matches_expectation(admitted, payload(broken)), index
        refusals += 1
    assert not board.matches_expectation(admitted, payload(values)[:-1])
    assert not board.matches_expectation(admitted, payload(values) + bytes(4))
    (args.output / 'result.json').write_text(json.dumps({
        'passed': True, 'physical_access': False, 'output_bytes': 63872,
        'changed_count_words': len(counts), 'bounded_word_refusals': refusals,
        'expectation': str(path.resolve())}, indent=2) + '\n')
    print(f'PASS CPU expectation: {len(counts)} absent-lock counts; {refusals} strict word refusals; full payload length')


if __name__ == '__main__':
    main()
