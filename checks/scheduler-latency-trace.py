#!/usr/bin/env python3
"""Validate scheduler timing report decoding with constructed binary records.

The cases exercise the report ABI and conservative attribution rules. Their
timestamps do not model hardware execution or establish a latency bound.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/latency/host.py', 'kernel/scheduler/latency_trace.h']
GATE_RUNS = [['--output', '{output}']]
FREQUENCY = 24_000_000
MAGIC = 0x5353534348545231
RECORDS = 32


def command():
    return [2, 3, 21, 1000, 1010, 1040, 1050, 1200, 1210, 1220]


def wait(*, start=3000, end=3240, holder=2, flags=15):
    return [start, end, 0x1000, 0, 7, 8, flags,
            holder, start - 100, end - 40, 0x2000, 2]


def fixture():
    empty = {
        'counts': [0] * 9, 'command': [0] * 10, 'self_suspend': [0] * 10,
        'irq': [0] * 11, 'drain': [0] * 4, 'records': [],
        'wait_max': [0] * 12, 'waits': [],
    }
    cores = [copy.deepcopy(empty) for _ in range(8)]
    cores[1].update({
        'counts': [1, 10, 30, 150, 10, 1, 1, 1, 0],
        'command': command(), 'self_suspend': command(),
        'irq': [2000, 2010, 2100, 2120, 10, 10, 10, 10, 10, 1, 1],
        'drain': [1100, 1200, 1, 0],
        'records': [[3, 0, 1110, 1120, 1140, 1150, 1160]],
        'wait_max': wait(), 'waits': [wait()],
    })
    return {
        'header': [MAGIC, 1, 2, 3], 'cores': cores,
        'startup': [[500 + 2 * c, 600 + 2 * c] for c in range(8)],
        'tick_count': 1, 'tick_dropped': 0, 'ticks': [[3050, 3100]],
    }


def pack(report):
    words = list(report['header'])
    waits, ticks = words[2:4]
    for core in report['cores']:
        for key in ('counts', 'command', 'self_suspend', 'irq', 'drain'):
            words.extend(core[key])
        for record in core['records']:
            words.extend(record)
        words.extend([0] * (7 * (RECORDS - len(core['records']))))
        words.extend(core['wait_max'])
        for record in core['waits']:
            words.extend(record)
        words.extend([0] * (12 * (waits - len(core['waits']))))
    for pair in report['startup']:
        words.extend(pair)
    words.extend([report['tick_count'], report['tick_dropped']])
    for pair in report['ticks']:
        words.extend(pair)
    words.extend([0] * (2 * (ticks - len(report['ticks']))))
    payload = struct.pack(f'<{len(words)}Q', *words)
    return payload + bytes((-len(payload)) % 64)


def load_decoder(source):
    path = source / 'apps/latency/host.py'
    spec = importlib.util.spec_from_file_location('scheduler_latency_decoder', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.decode_scheduler_trace, path


def run(decode):
    cases = []

    def accepted(name, data, inspect):
        decoded = decode(pack(data), FREQUENCY)
        inspect(decoded)
        cases.append(name)

    def rejected(name, payload, frequency=FREQUENCY):
        try:
            decode(payload, frequency)
        except ValueError:
            cases.append(name)
            return
        raise AssertionError(f'{name}: malformed report was accepted')

    def cause(expected):
        def inspect(decoded):
            core = decoded['cores'][1]
            assert core['wait_max']['cause'] == expected, core['wait_max']
            assert core['waits'][0]['cause'] == expected, core['waits'][0]
        return inspect

    base = fixture()
    accepted('observed holder contention', base, cause('observed_holder_contention'))
    overlap = copy.deepcopy(base)
    overlap['cores'][1]['wait_max'][7] = 0
    overlap['cores'][1]['waits'][0][7] = 0
    accepted('CPU0 holder tick overlap', overlap, cause('holder_tick_overlap'))
    no_overlap = copy.deepcopy(overlap)
    no_overlap['ticks'] = [[4000, 4100]]
    accepted('CPU0 holder without tick overlap', no_overlap, cause('observed_holder_contention'))

    for name, change in (
            ('ticket snapshot missing', lambda record: record.__setitem__(6, 7)),
            ('multiple ticket predecessors', lambda record: record.__setitem__(5, 9))):
        data = copy.deepcopy(overlap)
        for record in (data['cores'][1]['wait_max'], data['cores'][1]['waits'][0]):
            change(record)
        accepted(name, data, cause('unclassified'))

    for name, lock, maximum in (('global ticket wrap', 0, 0xffff),
                                ('object ticket wrap', 1, 0xffffffff)):
        wrapped = copy.deepcopy(base)
        for record in (wrapped['cores'][1]['wait_max'], wrapped['cores'][1]['waits'][0]):
            record[3:6] = [lock, maximum, 0]
        accepted(name, wrapped, cause('observed_holder_contention'))
        for record in (wrapped['cores'][1]['wait_max'], wrapped['cores'][1]['waits'][0]):
            record[4] = maximum - 1
        accepted(name + ' with multiple predecessors', wrapped, cause('unclassified'))

    unrelated_tick = copy.deepcopy(overlap)
    unrelated_tick['ticks'] = [[3210, 3230]]
    accepted('tick after holder release is unrelated', unrelated_tick,
             cause('observed_holder_contention'))

    for name, flags in (('unknown holder snapshot', 8), ('unmatched holder release', 11)):
        data = copy.deepcopy(overlap)
        for record in (data['cores'][1]['wait_max'], data['cores'][1]['waits'][0]):
            record[6] = flags
            record[9] = 0
            if flags == 8:
                record[7:] = [0] * 5
        accepted(name, data, cause('unclassified'))

    partial = copy.deepcopy(no_overlap)
    partial['tick_count'] = 3
    partial['tick_dropped'] = 1
    partial['ticks'] = [[1000, 1100], [2000, 2100], [2500, 2600]]
    accepted('tick overflow cannot establish absence', partial, cause('unclassified'))
    covered = copy.deepcopy(partial)
    covered['ticks'] = [[1000, 1100], [2000, 2100], [4000, 4100]]
    accepted('later tick overflow preserves earlier coverage', covered,
             cause('observed_holder_contention'))
    partial_overlap = copy.deepcopy(partial)
    partial_overlap['ticks'] = [[1000, 1100], [2000, 2100], [3050, 3100]]
    accepted('observed overlap survives later tick overflow', partial_overlap,
             cause('holder_tick_overlap'))

    overflow = copy.deepcopy(base)
    core = overflow['cores'][1]
    core['counts'][-2:] = [2, 1]
    core['waits'].append(wait(start=4000, end=4300))
    core['wait_max'] = wait(start=5000, end=5400)

    def retained(decoded):
        row = decoded['cores'][1]
        assert row['wait_count'] == 2 and row['wait_dropped'] == 1
        assert len(row['waits']) == 2
        assert row['wait_max']['start'] == 5000 and row['wait_max']['end'] == 5400
    accepted('maximum survives wait log overflow', overflow, retained)

    for name, index, value in (('wrong magic', 0, 0), ('unsupported version', 1, 999),
                               ('zero wait capacity', 2, 0), ('zero tick capacity', 3, 0)):
        raw = bytearray(pack(base))
        struct.pack_into('<Q', raw, index * 8, value)
        rejected(name, raw)
    rejected('truncated trace', pack(base)[:-8])
    rejected('trailing trace bytes', pack(base) + bytes(8))
    rejected('zero counter frequency', pack(base), 0)

    bad = copy.deepcopy(base)
    bad['cores'][1]['command'][5] = 999
    rejected('backward command stage', pack(bad))
    bad = copy.deepcopy(base)
    bad['cores'][1]['irq'][2] = 2005
    rejected('backward interrupt stage', pack(bad))
    bad = copy.deepcopy(base)
    bad['cores'][1]['records'][0][-1] = 1201
    rejected('control record outside drain', pack(bad))
    bad = copy.deepcopy(base)
    bad['cores'][1]['records'][0][3] = 1109
    rejected('backward control record', pack(bad))
    bad = copy.deepcopy(base)
    bad['cores'][1]['waits'][0][1] = 2999
    rejected('backward lock wait', pack(bad))
    bad = copy.deepcopy(base)
    bad['ticks'] = [[3100, 3050]]
    rejected('backward tick interval', pack(bad))
    bad = copy.deepcopy(base)
    bad['startup'][1][1] = 1
    rejected('backward startup interval', pack(bad))
    bad = copy.deepcopy(base)
    bad['cores'][1]['counts'][-1] = 2
    rejected('wait overflow before log capacity', pack(bad))
    bad = copy.deepcopy(base)
    bad['cores'][1]['wait_max'][1] = 3100
    bad['cores'][1]['wait_max'][9] = 3090
    rejected('retained maximum below log sample', pack(bad))
    bad = copy.deepcopy(base)
    bad['cores'][1]['waits'][0][7] = 8
    rejected('holder outside admitted cores', pack(bad))
    bad = copy.deepcopy(base)
    bad['tick_dropped'] = 1
    rejected('tick overflow before log capacity', pack(bad))
    raw = bytearray(pack(base))
    raw[-1] = 1
    rejected('nonzero alignment padding', raw)
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    decode, path = load_decoder(args.source.resolve())
    cases = run(decode)
    result = dict(passed=True, physical_access=False, constructed_report_records=True,
                  decoder=str(path), decoder_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                  cases=cases)
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(dict(passed=True, cases=len(cases), physical_access=False)))


if __name__ == '__main__':
    main()
