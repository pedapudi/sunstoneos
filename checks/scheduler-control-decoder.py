#!/usr/bin/env python3
"""Check control-report parsing, percentile arithmetic and qualification boundaries.

The binary fixtures test report consistency and layout combinations. Their
constructed timings do not establish a hardware latency bound.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/latency/host.py', 'apps/latency/control.h',
                'include/sunstone_control.h', 'checks/scheduler-latency-trace.py']
GATE_RUNS = [['--output', '{output}']]
HZ = 24_000_000


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def fixture(mode=0, count=10001, frequency=HZ):
    report = {'header': [0x53534354524C3031, 1, frequency, 10000,
                         0xfe, mode, 0xfe, 0xfe, 1, 0, 0xfe if count > 10000 else 0, 1],
              'cores': []}
    ns = lambda ticks: ticks * 1000000000 // frequency
    for core, rate in enumerate((1000, 2000, 4000, 8000, 10000, 4000, 4000) if mode == 0 else (10000,) * 7, 1):
        task_count = count if mode == 0 else 0
        percentile = ns(4) if count >= 10000 else ns(1)
        jitter = ns(4) if count - 1 >= 10000 else ns(1) if count > 1 else 0
        control = [0, core, rate, count, 0, 0, 0, 0,
                   ns(1) if count else 0, percentile if count else 0, ns(1) if count > 1 else 0, jitter]
        task = [1, core, rate, task_count, task_count, 0, 0, 0,
                ns(2) if count else 0, percentile if count else 0, ns(1) if count > 1 else 0, jitter] if mode == 0 else [0] * 12
        if count < 10000 and mode == 0:
            task[9] = task[8]
        histograms = [[task_count] + [0] * 191, [task_count] + [0] * 191,
                      [max(0, count - 1)] + [0] * 191]
        report['cores'].append({'identity': [core, rate, 3 if mode == 0 else 1, 0],
            'stats': [control, task], 'extra': [1, 1, 4 if count >= 10000 else 1,
                4 if count >= 10000 else 1, count, task_count] if task_count else [0, 0, 0, 0, count, 0],
            'histograms': histograms})
    return report


def pack(report):
    result = bytearray(struct.pack('<4Q8I', *report['header']))
    for row in report['cores']:
        result.extend(struct.pack('<4I', *row['identity']))
        for stats in row['stats']:
            result.extend(struct.pack('<3I4x5Q4I', *stats))
        result.extend(struct.pack('<4I2Q', *row['extra']))
        for bins in row['histograms']:
            result.extend(struct.pack('<192I', *bins))
    assert len(result) == 17536
    return bytes(result)


def base_payload(elapsed=1000, *, lock=24, masked=24):
    data = bytearray(63872)
    struct.pack_into('<3Q4I', data, 0, 0x53534C4154454E31, HZ, elapsed, 0xff, 0, 0, 1)
    for p, bounds in enumerate(((0, 100), (100, 1000), (1000, 5000))):
        struct.pack_into('<2Q', data, 20736 + p * 16, *bounds)
    for core in range(8):
        struct.pack_into('<4Q', data, 40 + core * 32, 2, 2097152, 2097152, 0)
        struct.pack_into('<Q', data, 20784 + core * 8, 0x1000)
        struct.pack_into('<2Q', data, 20224 + core * 16, 0, 1)
        for metric, value in ((1, masked), (4, lock)):
            struct.pack_into('<7Q', data, 320 + core * 640 + metric * 56,
                             1, value, 10000, 0x1000, 0, 0, 0)
        if core == 0:
            struct.pack_into('<7Q', data, 320, 1, 1, 10000, 0x1000, 0, 0, 0)
        for metric, phase, count, ready, delay in ((6, 1, 64, 200, 4), (7, 2, 128, 1200, 8)):
            struct.pack_into('<7Q', data, 320 + core * 640 + metric * 56,
                             count, delay, ready, 0x1000, 0, 0, 0)
            struct.pack_into('<7Q', data, 5440 + (phase * 8 + core) * 616 + metric * 56,
                             count, delay, delay, delay, ready, 0x1000, 0)
        for index in range(192):
            ready, delay, metric, flag = (200 + index * 5, 4, 6, 1) if index < 64 else (1200 + (index - 64) * 5, 8, 7, 2)
            struct.pack_into('<2Q2I', data, 20848 + (core * 192 + index) * 24,
                             ready, ready + delay, metric, flag)
    return bytes(data)


def scheduler_payload(source, version):
    module = load(source / 'checks/scheduler-latency-trace.py', 'trace_fixture')
    report = module.fixture(version)
    report['header'][2:] = [16, 512]
    if version >= 3:
        for core in range(8):
            report['wake'][core] = [1200, 1208, 7, core + 1,
                1201, 1202, 1202, 1203, 1204, 0, 0, 0, 0, 0,
                1, 1, 1, 0, 1205, 1206, core + 1, 1, 0, 1] + [0] * 48
            report['wake'][core][40:56] = [128, 1024, 8, 0, 0, 0, 0, 0, 128, 896, 7, 0, 0, 0, 0, 0]
            if version >= 4:
                report['history'][core] = [1201, 1204, 1, 1, 1, 0, 0, 0, 0, 0]
    if version == 5:
        report['window'] = {'header': [1, 1, 0, 16],
            'irqs': [[1, 30 | (0x3c0 << 16), 1200, 1201, 0x41001000, 1201, 1204]],
            'selections': [[1205, 1206, 1, 1]]}
    return module.pack(report)


def run(decoder, source):
    cases = []

    def accept(name, report, inspect=None):
        result = decoder.decode_control(pack(report), report['header'][2])
        if inspect:
            inspect(result)
        cases.append(name)
        return result

    def reject(name, payload, function=None):
        try:
            (function or (lambda data: decoder.decode_control(data, HZ)))(payload)
        except ValueError:
            cases.append(name)
        else:
            raise AssertionError(name + ': malformed report accepted')

    busy = fixture()
    result = accept('busy dual paths qualify', busy)
    assert result['measurement_qualified'] and result['task_wake_target']['qualified']
    assert result['cores'][0]['task_latency']['max_ticks_interval'] == [2, 2]
    idle = accept('idle control-only qualifies its selected paths', fixture(mode=1))
    assert idle['measurement_qualified'] and idle['task_wake_target']['met'] is None
    tickless = copy.deepcopy(busy)
    tickless['header'][-1] = 3
    assert accept('tickless flag labels jitter results', tickless)['tickless']
    for flags in (5, 7):
        restore = copy.deepcopy(busy)
        restore['header'][-1] = flags
        value = accept(f'restore-span flag in mode {flags}', restore)
        assert value['restore_spans'] and value['tickless'] == bool(flags & 2)
    for count in (0, 1, 9999, 10000):
        short = accept(f'{count} entries retain insufficient evidence', fixture(count=count))
        assert not short['measurement_qualified']
    for field, name in ((5, 'sequence gaps'), (6, 'dropped samples'), (7, 'missed periods')):
        data = copy.deepcopy(busy)
        data['cores'][0]['stats'][0][field] = 1
        assert not accept(name + ' remain unqualified', data)['measurement_qualified']
    for error in (1, 2, 4, 8):
        data = copy.deepcopy(busy)
        data['header'][9] = data['cores'][0]['identity'][3] = error
        assert not accept(f'error bit {error} remains unqualified', data)['measurement_qualified']
    for field, value, name in ((6, 0xfc, 'missing armed core'), (7, 0xfc, 'missing quiescent core'),
                               (8, 0, 'consumer still active')):
        data = copy.deepcopy(busy)
        data['header'][field] = value
        assert not accept(name, data)['measurement_qualified']
    # One upper-tail sample does not change rank9999; two samples do.
    for high_count, maximum, percentile in ((1, 1000, 4), (2, 1000, 1000)):
        data = fixture(count=10000)
        row = data['cores'][0]
        row['histograms'][0] = [10000 - high_count] + [0] * 190 + [high_count]
        row['stats'][1][8:10] = [maximum * 1000000000 // HZ, percentile * 1000000000 // HZ]
        value = accept(f'nearest rank with {high_count} saturated samples', data)
        assert value['cores'][0]['task_latency']['p9999_upper_ticks'] == percentile
    data = copy.deepcopy(busy)
    data['header'][9] = data['cores'][0]['identity'][3] = 4
    data['cores'][0]['histograms'][0] = [0] * 191 + [10001]
    data['cores'][0]['stats'][1][8:10] = [0xffffffff, 0xffffffff]
    assert not accept('nanosecond saturation remains unqualified', data)['measurement_qualified']
    for ticks in (239, 240, 241):
        data = copy.deepcopy(busy)
        row = data['cores'][0]
        row['histograms'][0] = [0] * 192
        row['histograms'][0][ticks // 4] = 10001
        row['stats'][1][8:10] = [ticks * 1000000000 // HZ, ((ticks // 4 + 1) * 4) * 1000000000 // HZ]
        value = accept(f'upper bin conservatively bounds {ticks} tick latency', data)
        assert value['task_wake_target']['met'] is (ticks == 239)
    rounded = fixture(frequency=1_000_001)
    row = rounded['cores'][0]
    row['histograms'][0] = [0] * 191 + [10001]
    row['stats'][1][8:10] = [999999, 999999]
    assert accept('inverse floor conversion preserves possible ticks', rounded)['cores'][0]['task_latency']['max_ticks_interval'] == [1000, 1000]
    rounded = fixture(frequency=4_000_000_000)
    row = rounded['cores'][0]
    row['histograms'][0] = [0] * 191 + [10001]
    row['stats'][1][8:10] = [10000, 10000]
    value = accept('floor nanoseconds cannot qualify ambiguous saturated maximum', rounded)
    assert value['cores'][0]['task_latency']['max_ticks_interval'] == [40000, 40003]
    assert not value['task_wake_target']['met']
    too_fast = fixture(frequency=0x100000000)
    reject('counter frequency exceeds runtime admission', pack(too_fast),
           lambda data: decoder.decode_control(data, 0x100000000))

    for field, value, name in ((0, 0, 'magic'), (1, 2, 'version'), (2, HZ + 1, 'frequency'),
            (3, 9999, 'minimum'), (4, 0xfc, 'requested mask'), (5, 2, 'mode'),
            (6, 0xff, 'ready mask'), (7, 0xff, 'quiescent mask'), (8, 2, 'parked flag'),
            (9, 16, 'error flags'), (10, 0xfc, 'completion summary'),
            (11, 2, 'mode flags'), (11, 9, 'unknown flag')):
        data = copy.deepcopy(busy)
        data['header'][field] = value
        reject('invalid ' + name, pack(data))
    for field, value, name in ((0, 2, 'core'), (1, 10000, 'rate'), (2, 1, 'paths'), (3, 16, 'errors')):
        data = copy.deepcopy(busy)
        data['cores'][0]['identity'][field] = value
        reject('invalid row ' + name, pack(data))
    for path, field, value, name in ((0, 0, 1, 'path'), (0, 1, 2, 'core'), (0, 2, 2, 'rate'),
            (0, 3, 10000, 'published count'), (0, 4, 1, 'control consumed'),
            (1, 4, 10000, 'task consumed'), (1, 8, 166, 'maximum bin'),
            (1, 9, 0, 'percentile'), (1, 11, 10000, 'jitter percentile'),
            (0, 9, 77, 'nonrepresentable control percentile'),
            (1, 11, 77, 'nonrepresentable task jitter percentile')):
        data = copy.deepcopy(busy)
        data['cores'][0]['stats'][path][field] = value
        reject('invalid statistics ' + name, pack(data))
    for histogram in range(3):
        data = copy.deepcopy(busy)
        data['cores'][0]['histograms'][histogram][0] -= 1
        reject(f'histogram {histogram} misses a sample', pack(data))
    data = fixture(mode=1)
    data['cores'][0]['extra'][0] = 1
    reject('idle report has task-entry data', pack(data))
    data = copy.deepcopy(busy)
    data['cores'][0]['extra'][3] = 5
    reject('task-gap percentile differs', pack(data))
    data = copy.deepcopy(busy)
    data['cores'][0]['extra'][0:3:2] = [764, 768]
    reject('saturated task-entry percentile exceeds exact maximum', pack(data))
    data = copy.deepcopy(busy)
    data['cores'][0]['identity'][3] = 1
    reject('error summary omits a core error', pack(data))
    for data, name in ((pack(busy)[:-1], 'truncated'), (pack(busy) + b'\0', 'trailing byte')):
        reject(name, data)

    base = base_payload()
    for version in (None, 1, 2, 3, 4, 5):
        scheduler = scheduler_payload(source, version) if version else b''
        for pmu in (b'', bytes(1216)):
            for control in (b'', pack(busy)):
                result = decoder.decode(base + pmu + scheduler + control)
                assert bool(result.get('control')) == bool(control)
                assert bool(result.get('scheduler_trace')) == bool(scheduler)
                if control:
                    assert result['qualification']['recorded_targets_met']
                    if version != 5 or not pmu:
                        assert len(base + pmu + scheduler + control) <= 131072
                    if version == 4:
                        assert len(base + pmu + scheduler + control) == (130816 if pmu else 129600)
                    if version == 5 and not pmu:
                        assert len(base + scheduler + control) == 131072
                cases.append(f'layout scheduler={version}, PMU={bool(pmu)}, control={bool(control)}')
    for elapsed, combined, valid in ((10000, False, True), (10001, False, False),
                                     (15000, True, True), (15001, True, False)):
        data = base_payload(elapsed) + (pack(busy) if combined else b'')
        if valid:
            decoder.decode(data)
            cases.append(f'elapsed {elapsed} with control={combined}')
        else:
            reject(f'elapsed {elapsed} with control={combined}', data, decoder.decode)
    for name, kwargs in (('lock', {'lock': 601}), ('mask', {'masked': 601})):
        result = decoder.decode(base_payload(**kwargs) + pack(busy))
        assert not result['qualification']['recorded_targets_met'] and result['control']['measurement_qualified']
        cases.append(name + ' miss is valid but unqualified')
    reject('unexpected data before control', base + bytes(8) + pack(busy), decoder.decode)
    reject('control before PMU', base + pack(busy) + bytes(1216), decoder.decode)
    reject('unknown control magic in combined layout', base + b'\0' * 8 + pack(busy)[8:], decoder.decode)
    reject('truncated base', base[:-1], decoder.decode)
    trace = load(source / 'checks/scheduler-latency-trace.py', 'startup_fixture')
    startup = trace.fixture(4)
    startup['startup'][1][0] = 0
    decoded = decoder.decode_scheduler_trace(trace.pack(startup), HZ)
    assert decoded['startup'][0]['start_kind'] == 'primary_running_flag_boundary'
    assert decoded['startup'][1]['start_kind'] is None
    cases.append('startup labels distinguish primary boundary from absent markers')
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    source = args.source.resolve()
    decoder = load(source / 'apps/latency/host.py', 'latency_control_decoder')
    cases = run(decoder, source)
    result = {'passed': True, 'physical_access': False, 'constructed_records': True, 'cases': cases,
              'inputs': {name: hashlib.sha256((source / name).read_bytes()).hexdigest()
                         for name in CHECK_INPUTS}}
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'passed': True, 'cases': len(cases), 'physical_access': False}))


if __name__ == '__main__':
    main()
