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
import subprocess
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/latency/host.py', 'kernel/scheduler/latency_trace.h',
                'kernel/scheduler/latency_trace.c', 'checks/scheduler-latency-history.c',
                'checks/sunstone-image-link.py', 'include/sunstone_latency.h']
GATE_RUNS = [['--output', '{output}']]
FREQUENCY = 24_000_000
MAGIC = 0x5353534348545231
RECORDS = 32


def command():
    return [2, 3, 21, 1000, 1010, 1040, 1050, 1200, 1210, 1220]


def wait(*, start=3000, end=3240, holder=2, flags=15):
    return [start, end, 0x1000, 0, 7, 8, flags,
            holder, start - 100, end - 40, 0x2000, 2]


def fixture(version=1):
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
    if version >= 2:
        for core in cores:
            core['wait_max'].extend([0] * 4)
            for record in core['waits']:
                record.extend([0] * 4)
    report = {
        'header': [MAGIC, version, 2, 3], 'cores': cores,
        'startup': [[500 + 2 * c, 600 + 2 * c] for c in range(8)],
        'tick_count': 1, 'tick_dropped': 0, 'ticks': [[3050, 3100]],
    }
    if version >= 3:
        report['wake'] = [[0] * 72 for _ in range(8)]
        report['wake'][1] = [1900, 2200, 7, 3,
            2000, 2010, 2020, 2100, 2120, 10, 10, 10, 10, 10,
            1, 1, 5, 1800, 2130, 2170, 3, 7, 1800, 5] + [0] * 48
        wake_summary(report['wake'][1], 1)
    if version >= 4:
        report['history_capacity'] = 16
        report['history'] = [[0] * 10 for _ in range(8)]
        report['history'][1] = [2000, 2120, 80, 1, 5, 0, 0, 0, 0, 0]
    if version == 5:
        report['window'] = {'header': [0, 0, 0, 16], 'irqs': [], 'selections': []}
    return report


def wake_summary(words, kind):
    words[24:] = [0] * 48
    def summary(ticks):
        return [1, ticks, ticks, *(int(ticks > FREQUENCY * limit // 1_000_000)
                                  for limit in (10, 25, 50, 80, 100))]
    words[24 + kind * 16:32 + kind * 16] = summary(words[1] - words[0])
    if kind != 2:
        words[32 + kind * 16:40 + kind * 16] = summary(words[1] - words[4])


def pack(report):
    words = list(report['header'])
    waits, ticks = words[2:4]
    wait_words = 12 if words[1] == 1 else 16
    for core in report['cores']:
        for key in ('counts', 'command', 'self_suspend', 'irq', 'drain'):
            words.extend(core[key])
        for record in core['records']:
            words.extend(record)
        words.extend([0] * (7 * (RECORDS - len(core['records']))))
        words.extend(core['wait_max'])
        for record in core['waits']:
            words.extend(record)
        words.extend([0] * (wait_words * (waits - len(core['waits']))))
    for pair in report['startup']:
        words.extend(pair)
    words.extend([report['tick_count'], report['tick_dropped']])
    for pair in report['ticks']:
        words.extend(pair)
    words.extend([0] * (2 * (ticks - len(report['ticks']))))
    if report['header'][1] >= 3:
        for wake in report['wake']:
            words.extend(wake)
    if report['header'][1] >= 4:
        words.append(report['history_capacity'])
        for history in report['history']:
            words.extend(history)
    if report['header'][1] == 5:
        window = report['window']
        words.extend(window['header'])
        for row in window['irqs']:
            words.extend(row)
        words.extend([0] * (7 * (window['header'][3] - len(window['irqs']))))
        for row in window['selections']:
            words.extend(row)
        words.extend([0] * (4 * (window['header'][3] - len(window['selections']))))
    payload = struct.pack(f'<{len(words)}Q', *words)
    return payload + bytes((-len(payload)) % 64)


def load_decoder(source):
    path = source / 'apps/latency/host.py'
    spec = importlib.util.spec_from_file_location('scheduler_latency_decoder', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.decode_scheduler_trace, path


def check_symbols(source):
    """Execute the boundary checker with constructed nm and ELF inventories."""
    path = source / 'checks/sunstone-image-link.py'
    spec = importlib.util.spec_from_file_location('scheduler_symbol_boundary', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    bounds = [{'name': prefix + name, 'shndx': 1, 'value': 0}
              for name in ('sunstone_gpu_program', 'sunstone_region_initializers',
                           'sunstone_large_regions')
              for prefix in ('__start_', '__stop_')]
    cases = []
    for symbol, admitted in (('eTaskGetState', True), ('ss_latency_task_suspended', True),
                             ('ss_sched_trace_snapshot', True),
                             ('ss_sched_trace_wake', True),
                             ('os_cpu_clock_step', False), ('eTaskPrivateProbe', False),
                             ('ss_sched_trace_lock_release', False)):
        elf = SimpleNamespace(symbols=[*bounds, {'name': symbol, 'shndx': 1}],
                              names=[], sections=[], data=b'')
        empty = SimpleNamespace(names=[], sections=[], data=b'')

        def nm(command, **unused):
            return SimpleNamespace(stdout=(symbol + ' U\n')
                                   if '--undefined-only' in command else 'fixture_app T 0 4\n')

        with patch.object(module.subprocess, 'run', side_effect=nm), \
                patch.object(module, 'Elf', return_value=empty), \
                patch.object(module, 'application_manifest', return_value={'regions': []}):
            try:
                result = module.application_symbols(elf, [Path(__file__)], [], [], 'fixture-nm')
            except AssertionError as error:
                assert not admitted, (symbol, str(error))
                assert str(error) == 'application references private OS symbols: ' + symbol
            else:
                assert admitted, f'private symbol admitted: {symbol}'
                assert result['external_symbols'] == 1
        cases.append(('admit ' if admitted else 'refuse ') + symbol)
    return cases, path


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

    v2 = fixture(version=2)
    accepted('version 2 without an actual sample', v2,
             cause('observed_holder_contention'))

    def actual(data, start=3030, end=3210, serving=7, ticket=8):
        for record in (data['cores'][1]['wait_max'], data['cores'][1]['waits'][0]):
            record[6] |= 16
            record[12:] = [start, end, serving, ticket]
        return data

    contended = actual(copy.deepcopy(v2))
    accepted('actual single FIFO predecessor', contended,
             cause('observed_holder_contention'))
    empty = actual(copy.deepcopy(v2), serving=8, ticket=8)

    def no_predecessor(decoded):
        cause('uncontended_FIFO')(decoded)
        row = decoded['cores'][1]['wait_max']
        assert row['fifo_ticks'] == 180
        assert row['wrapper_residual_ticks'] == 60
    accepted('actual empty FIFO separates wrapper residual', empty, no_predecessor)
    unknown = copy.deepcopy(empty)
    for record in (unknown['cores'][1]['wait_max'], unknown['cores'][1]['waits'][0]):
        record[6] = 16
        record[7:12] = [0] * 5
    accepted('actual empty FIFO needs no holder snapshot', unknown, no_predecessor)

    tick = copy.deepcopy(contended)
    for record in (tick['cores'][1]['wait_max'], tick['cores'][1]['waits'][0]):
        record[7] = 0
    accepted('actual single predecessor overlaps CPU0 tick', tick,
             cause('holder_tick_overlap'))
    before = copy.deepcopy(tick)
    before['ticks'] = [[3000, 3020]]
    accepted('tick before actual FIFO is unrelated', before,
             cause('observed_holder_contention'))

    for name, serving, ticket in (('multiple actual predecessors', 7, 9),
                                   ('actual holder differs from early snapshot', 8, 9),
                                   ('actual ticket differs from early next', 7, 10)):
        data = actual(copy.deepcopy(v2), serving=serving, ticket=ticket)
        accepted(name, data, cause('unclassified'))
    unmatched = copy.deepcopy(contended)
    for record in (unmatched['cores'][1]['wait_max'], unmatched['cores'][1]['waits'][0]):
        record[6] &= ~4
        record[9] = 0
    accepted('actual predecessor without matched release is unknown', unmatched,
             cause('unclassified'))

    wrap = actual(copy.deepcopy(v2), serving=0xffff, ticket=0)
    for record in (wrap['cores'][1]['wait_max'], wrap['cores'][1]['waits'][0]):
        record[4:6] = [0xffff, 0]
    accepted('actual reservation wraps at 16 bits', wrap,
             cause('observed_holder_contention'))

    for name, field, value in (('FIFO starts before wrapper', 12, 2999),
                                ('FIFO ends after wrapper', 13, 3241),
                                ('FIFO timestamps go backward', 13, 3029),
                                ('FIFO sample has zero start', 12, 0),
                                ('FIFO serving exceeds 16 bits', 14, 0x10000),
                                ('FIFO ticket exceeds 16 bits', 15, 0x10000),
                                ('actual FIFO belongs to another lock', 3, 1)):
        bad = copy.deepcopy(contended)
        bad['cores'][1]['wait_max'][field] = value
        rejected(name, pack(bad))
    bad = copy.deepcopy(contended)
    bad['cores'][1]['wait_max'][6] &= ~16
    rejected('FIFO fields without actual flag', pack(bad))
    bad = copy.deepcopy(base)
    bad['cores'][1]['wait_max'][6] |= 16
    rejected('version 1 cannot assert actual FIFO sampling', pack(bad))
    rejected('truncated version 2 record', pack(contended)[:-8])
    v3 = fixture(version=3)

    def classified(kind, *, tail=None):
        def inspect(decoded):
            row = decoded['wake_chains'][1]
            assert row['scope'] == 'busy' and row['count'] == 1
            assert row['maximum']['class'] == kind
            assert row['classes'][kind]['whole_wake']['count'] == 1
            if tail is not None:
                assert row['maximum']['owner_exit_to_running_ticks'] == tail
        return inspect
    accepted('version 3 linked slow busy wake', v3, classified('slow', tail=200))
    fast = copy.deepcopy(v3)
    w = fast['wake'][1]
    w[5:9] = [2010] * 4
    w[9:14] = [0] * 5
    w[14:16] = [0, 3]
    wake_summary(w, 0)
    accepted('version 3 fast wake has zero service', fast, classified('fast', tail=200))
    inside = copy.deepcopy(v3)
    inside['wake'][1][0] = 2050
    wake_summary(inside['wake'][1], 1)
    accepted('ready inside owner exit permits a longer exit-to-running interval', inside,
             classified('slow', tail=200))
    for name, field, value, flags in (
            ('intervening ordinary IRQ', 17, 1950, 27),
            ('intervening saved-frame switch', 22, 1950, 27),
            ('switch belongs to another IRQ', 23, 4, 19),
            ('switch selected another task', 20, 4, 19),
            ('switch after running observation', 19, 2210, 19),
            ('IRQ did not request switch', 15, 0, 19)):
        data = copy.deepcopy(v3)
        w = data['wake'][1]
        w[field], w[2] = value, flags
        wake_summary(w, 2)
        accepted(name, data, classified('unmatched'))
    missing = copy.deepcopy(v3)
    w = missing['wake'][1]
    w[4:18] = [0] * 14
    w[2] = 18
    wake_summary(w, 2)
    accepted('missing owner exit remains unmatched', missing, classified('unmatched'))
    for name, field, value in (
            ('backward guard timestamp', 5, 1999),
            ('backward switch timestamp', 19, 2129),
            ('incorrect linked selected task', 20, 4),
            ('incorrect linked IRQ sequence', 23, 4),
            ('incorrect linked flag across intervening IRQ', 17, 1950),
            ('IRQ service exceeds measured work', 9, 1000),
            ('missing IRQ start with complete sequence', 4, 0),
            ('unknown wake flags', 2, 39),
            ('invalid probe slot', 3, 128),
            ('summary threshold above sample count', 43, 2),
            ('retained maximum below summary maximum', 42, 301)):
        bad = copy.deepcopy(v3)
        bad['wake'][1][field] = value
        rejected(name, pack(bad))
    bad = copy.deepcopy(fast)
    bad['wake'][1][9] = 1
    rejected('fast chain cannot contain service', pack(bad))
    bad = copy.deepcopy(missing)
    bad['wake'][1][64:72] = [1, 200, 200, 0, 0, 0, 0, 0]
    rejected('unmatched samples cannot claim an IRQ tail', pack(bad))
    rejected('truncated version 3 record', pack(v3)[:-8])
    v4 = fixture(version=4)
    accepted('version 4 history preserves selecting exit', v4, classified('slow', tail=200))
    prior_yield = copy.deepcopy(v4)
    prior_yield['wake'][1][15] = prior_yield['history'][1][3] = 0
    accepted('version 4 existing port yield links result zero to actual selection',
             prior_yield, classified('slow', tail=200))
    future = copy.deepcopy(v4)
    future['history'][1][-1] = 3
    accepted('post-running IRQs do not replace selecting chain', future, classified('slow', tail=200))
    trailing = copy.deepcopy(v4)
    trailing['history'][1] = [2180, 2190, 6, 0, 6, 1, 6, 0, 0, 0]
    trailing['wake'][1][2] = 27
    wake_summary(trailing['wake'][1], 2)
    accepted('post-switch pre-running IRQ remains visible', trailing, classified('unmatched'))
    trailing['wake'][1][15] = 0
    accepted('result zero with a later ordinary IRQ remains unmatched', trailing, classified('unmatched'))
    rotated = copy.deepcopy(v4)
    rotated['history'][1][7] = 99
    rotated['history'][1][4] = 115
    rotated['wake'][1][16] = rotated['wake'][1][23] = 115
    accepted('ordinary history rotation does not imply lost wake', rotated, classified('slow', tail=200))
    for name, field, value in (('latest IRQ ends after captured running', 1, 2201),
                                ('trailing count differs from sequence', 5, 2),
                                ('trailing work exceeds interval', 6, 1000),
                                ('future count exceeds retained history', 9, 17)):
        bad = copy.deepcopy(trailing)
        bad['history'][1][field] = value
        rejected(name, pack(bad))
    bad = copy.deepcopy(v4)
    bad['wake'][1][2] = 51
    wake_summary(bad['wake'][1], 2)
    rejected('loss flag requires overwritten history', pack(bad))
    for name, field, value in (('selecting IRQ was already overwritten', 7, 5),
                                ('switch was already overwritten', 8, 7),
                                ('overwrite count differs from latest IRQ', 7, 99)):
        bad = copy.deepcopy(v4)
        bad['history'][1][field] = value
        rejected(name, pack(bad))
    bad = copy.deepcopy(v4)
    bad['wake'][1][16] = bad['wake'][1][23] = 17
    bad['history'][1][4] = 17
    rejected('retained IRQ exceeds history window', pack(bad))
    bad = copy.deepcopy(v4)
    bad['history'][0][9] = 1
    rejected('empty wake cannot retain history observations', pack(bad))
    rejected('truncated version 4 record', pack(v4)[:-8])
    v5 = fixture(version=5)
    def startup_scope(value):
        assert value['startup'][0]['restore_kind'] == 'final_software_return_point'
        assert 'remaining assembly restore and ERET' in value['startup'][0]['restore_scope']
    accepted('version 5 states the startup software boundary and remaining return tail', v5, startup_scope)
    v5['wake'][0] = copy.deepcopy(v5['wake'][1])
    v5['wake'][0][2] = 27
    v5['wake'][0][17] = 1980
    v5['wake'][0][22] = 1989
    wake_summary(v5['wake'][0], 2)
    v5['history'][0] = copy.deepcopy(v5['history'][1])
    v5['window'] = {'header': [3, 2, 0, 16], 'irqs': [
        [4, 30 | (0x3c0 << 16), 1901, 1950, 0x41000100, 1951, 1980],
        [5, 0x3c0 << 16, 1990, 1999, 0x41000200, 2000, 2120],
        [0, 27 | (0x3c5 << 16), 2180, 2190, 0x41000300, 0, 0]],
        'selections': [[1985, 1989, 4, 1], [2130, 2170, 5, 3]]}
    def window_ids(value):
        window = value['cpu0_wake_window']
        assert [row['interrupt'] for row in window['irqs']] == [30, 0, 27]
        assert window['irqs'][-1]['spsr'] == 0x3c5 and not window['flags']
    accepted('version 5 retains earlier timer, SGI and guarded IRQ dispatches', v5, window_ids)
    svc = copy.deepcopy(v5)
    svc['wake'][0][2] = 26
    svc['wake'][0][4:18] = [0] * 14
    svc['wake'][0][18:24] = [2191, 2199, 3, 8, 2170, 0]
    wake_summary(svc['wake'][0], 2)
    svc['window']['header'][1] = 3
    svc['window']['selections'].append([2191, 2199, 0, 3])
    accepted('version 5 distinguishes a later voluntary SVC selection', svc,
             lambda value: value['cpu0_wake_window']['selections'][-1]['origin'] == 'svc')
    lost = copy.deepcopy(v5)
    lost['window']['header'][:3] = [0, 2, 5]
    lost['window']['irqs'] = []
    accepted('version 5 exposes unavailable IRQ history', lost,
             lambda value: value['cpu0_wake_window']['incomplete'])
    for name, field, value in (('IRQ count exceeds capacity', 0, 17),
                               ('selection count exceeds capacity', 1, 17),
                               ('unknown window flags', 2, 8), ('window capacity differs', 3, 15)):
        bad = copy.deepcopy(v5); bad['window']['header'][field] = value
        rejected(name, pack(bad))
    for name, field, value in (('invalid interrupt ID', 1, 1023),
                               ('invalid saved mode', 1, 30 | (2 << 16)),
                               ('saved status exceeds 32 bits', 1, 1 << 48),
                               ('future C dispatch', 3, 2201), ('owner before C dispatch end', 5, 1902),
                               ('duplicate ordinary owner sequence', 0, 5), ('missing interrupted PC', 4, 0)):
        bad = copy.deepcopy(v5); bad['window']['irqs'][0][field] = value
        rejected(name, pack(bad))
    bad = copy.deepcopy(v5); bad['window']['irqs'][-1][5:] = [2190, 2191]
    rejected('guarded IRQ must not claim owner work', pack(bad))
    bad = copy.deepcopy(v5); bad['window']['selections'][0][2] = 3
    rejected('selection origin missing without coverage flag', pack(bad))
    bad = copy.deepcopy(v5); bad['window']['selections'][-1][1] = 2169
    rejected('window must retain the latest selecting event', pack(bad))
    bad = copy.deepcopy(v5); bad['window']['selections'].append([2191, 2192, 0, 3])
    rejected('unused selection row contains data', pack(bad))
    rejected('truncated version 5 record', pack(v5)[:-8])
    direct = copy.deepcopy(v5)
    direct['cores'][1]['self_suspend'] = [2, 3, 69, 1000, 1010, 1040, 1050, 1060, 1080, 1110]
    def direct_scope(value):
        record = value['cores'][1]['self_suspend']
        assert record['masked_c_ticks'] == 60 and record['ticks'] == 110
        assert record['steps_ticks'] == dict(reference=10, publish=30, local_commit=10, save_frame=20, select=30)
    accepted('direct suspension separates unmasked admission from masked selection', direct, direct_scope)
    bad = copy.deepcopy(direct); bad['cores'][1]['self_suspend'][2] |= 16
    rejected('direct suspension cannot claim a control drain', pack(bad))
    bad = fixture(version=4); bad['cores'][1]['self_suspend'][2] = 69
    rejected('older trace cannot reinterpret direct suspension stamps', pack(bad))
    hole = copy.deepcopy(v5)
    hole['wake'][0][2] |= 64
    hole['window']['header'][2] |= 4
    accepted('an explicitly incomplete snapshot remains unattributed', hole,
             lambda value: value['wake_chains'][0]['maximum']['snapshot_incomplete'])
    busy_record = copy.deepcopy(v5)
    busy_record['cores'][1]['records'][0] = [3, 1 << 63, 1110, 1120, 1140, 1140, 1140]
    def busy_steps(value):
        row = value['cores'][1]['drain']['records'][0]
        assert row['record_type'] == 'fifo_busy' and row['fifo_try_ticks'] == 10 and row['park_ticks'] == 20
    accepted('busy FIFO attempt retains try and park cost', busy_record, busy_steps)
    bad = copy.deepcopy(busy_record); bad['header'][1] = 4
    rejected('older format rejects a busy FIFO record', pack(bad))
    bad = copy.deepcopy(busy_record); bad['cores'][1]['records'][0][1] |= 1
    rejected('busy attempt cannot claim an unread transfer phase', pack(bad))
    bad = copy.deepcopy(busy_record); bad['cores'][1]['records'][0][-1] += 1
    rejected('busy attempt cannot claim unlock or reference-release work', pack(bad))
    return cases


def check_history(source, output):
    """Run the actual recorder with a controlled host counter, then decode it."""
    fixture_source = source / 'checks/scheduler-latency-history.c'
    recorder = source / 'kernel/scheduler/latency_trace.c'
    binary, payload = output / 'history-check', output / 'history-report.bin'
    command = ['/usr/bin/clang', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', '-DSUNSTONE_LATENCY=1', '-DSUNSTONE_SCHED_LATENCY_TRACE=1',
               '-DSS_SCHED_TRACE_WAITS=2', '-DSS_SCHED_TRACE_TICKS=3', '-DSS_SCHED_TRACE_HISTORY=16',
               '-DSS_SCHED_TRACE_HOST_TEST=1', '-I' + str(source / 'include'),
               str(fixture_source), str(recorder), '-o', str(binary)]
    compiled = subprocess.run(command, capture_output=True, text=True)
    (output / 'history-compile.log').write_text(compiled.stdout + compiled.stderr)
    compiled.check_returncode()
    interrupted_payload = output / 'history-interrupted.bin'
    ran = subprocess.run([str(binary), str(payload), str(interrupted_payload)], capture_output=True, text=True)
    (output / 'history-run.log').write_text(ran.stdout + ran.stderr)
    ran.check_returncode()
    decode, _ = load_decoder(source)
    report = decode(payload.read_bytes(), FREQUENCY)
    assert report['version'] == 5 and report['history_capacity'] == 16
    expected = ('unmatched', 'unmatched', 'unmatched', 'unmatched', 'fast', 'slow', 'unmatched', 'unmatched')
    assert tuple(row['maximum']['class'] for row in report['wake_chains']) == expected
    window = report['cpu0_wake_window']
    assert len(window['irqs']) == 8 and len(window['selections']) == 4 and not window['flags']
    assert window['irqs'][-2]['interrupt'] == 27 and window['irqs'][-1]['interrupt'] == 0
    assert window['selections'][-1]['origin'] == 'svc'
    assert report['cores'][7]['self_suspend']['masked_c_ticks'] == 60
    assert report['cores'][7]['self_suspend']['steps_ticks']['select'] == 30
    busy = report['cores'][6]['drain']['records'][0]
    assert busy['record_type'] == 'fifo_busy' and busy['fifo_try_ticks'] == 10 and busy['park_ticks'] == 20
    interrupted = decode(interrupted_payload.read_bytes(), FREQUENCY)
    assert interrupted['wake_chains'][0]['maximum']['snapshot_incomplete']
    assert interrupted['wake_chains'][0]['maximum']['class'] == 'unmatched'
    assert interrupted['cpu0_wake_window']['incomplete']
    return ['recorder retains a parked FIFO visit without locked work',
            'recorder unmasked snapshot excludes an injected future event',
            'recorder unmasked snapshot exposes an injected overwrite without retry',
            'recorder CPU0 dispatch IDs and voluntary selection origin', 'recorder history: future IRQ', 'recorder history: trailing IRQ',
            'recorder history: selecting IRQ overwritten', 'recorder history: latest switch task differs',
            'recorder history: old rotation retained', 'recorder history: future switches',
            'recorder history: selection overwritten', 'recorder history: preceding IRQ ambiguity']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=('decoder', 'symbols', 'history'), action='append')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    source = args.source.resolve()
    selected = args.case or ('decoder', 'symbols', 'history')
    cases, inputs = [], {}
    if 'decoder' in selected:
        decode, path = load_decoder(source)
        cases.extend(run(decode))
        inputs['decoder'] = dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    if 'symbols' in selected:
        symbol_cases, path = check_symbols(source)
        cases.extend(symbol_cases)
        inputs['symbol_boundary'] = dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    if 'history' in selected:
        cases.extend(check_history(source, args.output))
        for name in ('kernel/scheduler/latency_trace.c', 'checks/scheduler-latency-history.c'):
            path = source / name
            inputs[name] = dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    result = dict(passed=True, physical_access=False, constructed_report_records=True,
                  inputs=inputs, cases=cases)
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(dict(passed=True, cases=len(cases), physical_access=False)))


if __name__ == '__main__':
    main()
