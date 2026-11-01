#!/usr/bin/env python3
"""Check the NPU idle benchmark report decoder and its limit comparison.

The fixture writes the report grammar of apps/npu-finite/idle-client.h and
apps/npu-finite/queue-client.c for the control and the gating policy, decodes
it with the production decoder, applies the production comparison, and then
corrupts each constraint once. A separate step compares the decoder's expected
labels and field counts with the producer source, so a producer change cannot
drift from the decoder unnoticed. No board is accessed and the producer is not
executed; the physical report is unqualified until the board runs.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct

from harness import refused

ROOT = Path(__file__).resolve().parents[1]
FREQUENCY = 24000000
GAPS = (9000, 11000, 100000)
CYCLES = 100
DEADLINE = 3000000000
REFERENCES = [bytes(range(64)), bytes(range(64, 128))]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def hexword(value):
    return f'0x{value:016x}\n'


def hexbytes(data):
    return ''.join(data[at:at + 64].hex() + '\n' for at in range(0, len(data), 64))


def scenario(enabled, warm=1.0, cold=1.0, owner=1.0):
    """The modeled counters of one run. Scale factors move gating-run timing only."""
    base = DEADLINE - 470000000
    durations = [int((240 + i % 20) * warm) for i in range(1000)]
    start = base
    end = start + int(1200000 * warm)
    total = sum(durations)
    ordered = sorted(durations)
    warm_fields = {'start': start, 'end': end, 'durations': durations, 'total': total,
                   'quantiles': (ordered[499], ordered[989], ordered[999]),
                   'client': int(300000 * owner), 'worker': 80000, 'irq': 60000, 'owner_ticks': int(12000 * owner),
                   'owner_elapsed': int(11000 * owner), 'wakes': 6}
    groups = []
    at = end + 1000
    for gap in GAPS:
        gating = enabled and gap > 10000
        idle_ticks = [gap * 24 + 700 + i for i in range(CYCLES)]
        admission = [70 if gating else 0 for i in range(CYCLES)]
        durations_cold = [int((420 + i % 7) * cold) + (admission[i] * 24 if gating else 0) for i in range(CYCLES)]
        length = sum(idle_ticks) + sum(durations_cold) + 20000
        group = {'gap': gap, 'cycles': CYCLES, 'checked': CYCLES, 'bad': 0, 'start': at, 'end': at + length,
                 'gates': CYCLES if gating else 0, 'resumes': CYCLES if gating else 0,
                 'writes': 6 * CYCLES if gating else 0,
                 'gated': sum(idle_ticks) - CYCLES * 240000 - 5000 if gating else 0,
                 'requests': CYCLES if gating else 0, 'driver_max': 70 if gating else 0,
                 'owner_ticks': 3000 if gating else 800, 'owner_elapsed': 4000 if gating else 900,
                 'client': 20000, 'worker': 9000, 'irq': 7000,
                 'idle_min': min(idle_ticks), 'idle_max': max(idle_ticks), 'idle_sum': sum(idle_ticks),
                 'first_cycle': 2 ** 64 - 1, 'first_offset': 2 ** 64 - 1, 'first_byte': 2 ** 64 - 1,
                 'pending': CYCLES if gating else 0, 'cold': durations_cold, 'admission': admission,
                 'output': hashlib.sha256(hashlib.sha256(REFERENCES[0]).digest() * CYCLES).hexdigest()}
        groups.append(group)
        at += length + 1000
    return {'enabled': enabled, 'warm': warm_fields, 'groups': groups,
            'output': hashlib.sha256(hashlib.sha256(REFERENCES[0]).digest() * 1000).hexdigest()}


def render(s):
    w = s['warm']
    raw = struct.pack('<1000I', *w['durations'])
    values = [FREQUENCY, 1, 1000, 1, 1301, 1000, 0, w['start'], w['end'], *w['quantiles'], w['total'], 5000, 6000,
              2 ** 64 - 1, 2 ** 64 - 1, 2 ** 64 - 1, 1000, 1000, w['irq'], w['client'], w['worker']]
    text = ('NPU_STAGE=WAIT_STARTUP\nNPU_STAGE=WARMUP\nNPU_STAGE=LATENCY\nNPU_STAGE=CLOSE\n'
            'NPU_STAGE=WAIT_LOCAL_STOP\nNPU_IRQ_COMPLETIONS=' + hexword(1302)[:-1] + '\n'
            'NPU_IRQ_STOPPED=0x0000000000000001\nNPU_STAGE=QUIESCENT\nNPU_LATENCY=\n')
    text += ''.join(hexword(v) for v in values)
    text += 'NPU_SUBMIT_TO_DOORBELL=\n' + ''.join(hexword(v) for v in (1000, 100, 110, 150, 100000))
    text += 'NPU_DURATION_LE32=\n' + hexbytes(raw)
    text += 'NPU_DURATION_SHA256=\n' + hashlib.sha256(raw).hexdigest() + '\n'
    text += 'NPU_OUTPUT_SHA256=\n' + s['output'] + '\n'
    g = s['groups']
    totals = [int(s['enabled']), w['owner_ticks'], w['owner_elapsed'], w['wakes'], 0, 0, 0,
              sum(x['gates'] for x in g), sum(x['resumes'] for x in g), sum(x['writes'] for x in g),
              sum(x['gated'] for x in g), 1680 if s['enabled'] else 0,
              w['owner_elapsed'] + sum(x['owner_elapsed'] for x in g) + 500, w['wakes'] + 40, w['irq']]
    text += 'NPU_IDLE=\n' + ''.join(hexword(v) for v in totals)
    for x in g:
        fields = [x[k] for k in ('gap', 'cycles', 'checked', 'bad', 'start', 'end', 'gates', 'resumes', 'writes', 'gated',
                                 'requests', 'driver_max', 'owner_ticks', 'owner_elapsed', 'client', 'worker', 'irq',
                                 'idle_min', 'idle_max', 'idle_sum', 'first_cycle', 'first_offset', 'first_byte', 'pending')]
        cold, admission = struct.pack('<100I', *x['cold']), struct.pack('<100H', *x['admission'])
        text += 'NPU_IDLE_GROUP=\n' + ''.join(hexword(v) for v in fields)
        text += 'NPU_IDLE_DURATION_LE32=\n' + hexbytes(cold) + 'NPU_IDLE_ADMISSION_LE16=\n' + hexbytes(admission)
        text += 'NPU_IDLE_SAMPLES_SHA256=\n' + hashlib.sha256(cold + admission).hexdigest() + '\n'
        text += 'NPU_IDLE_OUTPUT_SHA256=\n' + x['output'] + '\n'
    return text + 'NPU_INFERENCE=MATCH; inference_jobs=1301\n'


def geometry(enabled):
    return {'runtime': True, 'references': [r.hex() for r in REFERENCES],
            'latency': {'mode': 'queue', 'measured_jobs': 1000, 'warmup_jobs': 1, 'model': 0, 'sequence_repeats': 0,
                        'doorbell_timing': True,
                        'idle': {'enabled': bool(enabled), 'cycles': 100, 'gaps_us': list(GAPS),
                                 'threshold_us': 10000, 'resume_budget_us': 100}}}


def decode(host, s):
    return host.latency_report(render(s), 0, DEADLINE, geometry(s['enabled']))


def source_labels():
    """Labels and field counts the producer prints, in order, from its source."""
    text = (ROOT / 'apps/npu-finite/idle-client.h').read_text()
    report = text[text.index('static void idle_report'):]
    labels = re.findall(r'platform_puts\("(NPU_IDLE[A-Z0-9_]*)=\\n"\)', report)
    totals = re.search(r'const uint64_t values\[\]=\{(.*?)\};', report, re.S)[1]
    stats = re.search(r'const uint64_t stats\[\]=\{(.*?)\};', report, re.S)[1]
    count = lambda body: len([part for part in re.split(r',(?![^()]*\))', body.replace('\n', ' ')) if part.strip()])
    return labels, count(totals), count(stats)


GATING = 'Idle gating or resume bound differs.'
UNGATED = 'Idle interval gated or wrote registers outside the enabled policy.'
GEOMETRY = 'Idle gap, duration or task counters differ.'
IDENTITY = 'Idle group identity, order or sample digest differs.'
MUTATIONS = [
    ('a gating cycle without a gate', 1, lambda s: s['groups'][1].update(gates=99), GATING),
    ('a gate under the control policy', 0, lambda s: s['groups'][2].update(gates=1), UNGATED),
    ('a gate at the 9 ms gap', 1, lambda s: s['groups'][0].update(gates=1, writes=6), UNGATED),
    ('a resume above the budget', 1, lambda s: s['groups'][1].update(driver_max=101), GATING),
    ('gated time above the idle time', 1, lambda s: s['groups'][2].update(gated=10 ** 12), GATING),
    ('the 9 ms gap reaching the threshold', 1, lambda s: s['groups'][0].update(idle_max=240001), 'Idle gap below the threshold reached it.'),
    ('an idle minimum below its gap', 1, lambda s: s['groups'][1].update(idle_min=1), GEOMETRY),
    ('a cold output mismatch not counted', 1, lambda s: s['groups'][0].update(output='0' * 64), 'Idle output status disagrees with its digest.'),
    ('a group that starts before the previous ends', 1, lambda s: s['groups'][1].update(start=s['groups'][0]['start']), IDENTITY),
    ('a zero-length cold job', 1, lambda s: s['groups'][1]['cold'].__setitem__(3, 0), GEOMETRY),
    ('owner time beyond the group', 1, lambda s: s['groups'][2].update(owner_ticks=10 ** 12), GEOMETRY),
    ('a short cycle count', 1, lambda s: s['groups'][0].update(cycles=99), IDENTITY),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    host = load('npu_finite_idle_host', 'apps/npu-interrupt/host.py').finite
    compare = load('npu_idle_compare', 'apps/npu-finite/idle-compare.py')
    cases = []

    control, gating = decode(host, scenario(0)), decode(host, scenario(1))
    assert control['idle']['policy'] == 'control' and gating['idle']['policy'] == 'gate'
    assert [g['gates'] for g in gating['idle']['groups']] == [0, 100, 100]
    assert gating['idle']['groups'][1]['admission_us']['max'] == 70
    assert control['idle']['totals']['gates'] == 0 and control['idle']['warm']['charged_cpu_ticks'] > 0
    cases.append('decodes both policies')

    result = compare.compare(control, gating)
    assert result['passed'] and len(result['criteria']) == 7, result
    (args.output / 'comparison.md').write_text(compare.table(result) + '\n')
    cases.append('comparison passes within the limits')

    slow = decode(host, scenario(1, warm=1.03))
    assert not compare.compare(control, slow)['passed']
    failed = {r['criterion'] for r in compare.compare(control, slow)['criteria'] if not r['passed']}
    assert 'Throughput' in failed, failed
    busy = decode(host, scenario(1, owner=1.5))
    failed = {r['criterion'] for r in compare.compare(control, busy)['criteria'] if not r['passed']}
    assert failed == {'Charged CPU time per job'}, failed
    spike = copy.deepcopy(gating)
    spike['timing']['latency_ticks']['max'] = int(control['timing']['latency_ticks']['max'] * 1.11) + 1
    spike['timing']['latency_ticks']['p99'] = int(control['timing']['latency_ticks']['p99'] * 1.06) + 1
    failed = {r['criterion'] for r in compare.compare(control, spike)['criteria'] if not r['passed']}
    assert failed == {'p99 latency', 'Maximum latency'}, failed
    wrong = copy.deepcopy(gating)
    wrong['idle']['groups'][0]['gates'] = 1
    assert 'Gating during the 9 ms gap' in {r['criterion'] for r in compare.compare(control, wrong)['criteria'] if not r['passed']}
    cases.append('comparison refuses each limit violation')

    for label, enabled, mutate, reason in MUTATIONS:
        s = scenario(enabled)
        mutate(s)
        refused(lambda: decode(host, s), reason, ValueError)
        cases.append('refuses ' + label)
    refused(lambda: host.latency_report(render(scenario(1)), 0, DEADLINE, geometry(0)), 'Idle policy or warm owner counters differ.', ValueError)
    cases.append('refuses a report whose policy differs from the image marker')

    labels, total_count, group_count = source_labels()
    expected = ['NPU_IDLE'] + ['NPU_IDLE_GROUP', 'NPU_IDLE_DURATION_LE32', 'NPU_IDLE_ADMISSION_LE16', 'NPU_IDLE_SAMPLES_SHA256', 'NPU_IDLE_OUTPUT_SHA256']
    assert labels == expected, labels
    assert (total_count, group_count) == (15, 24), (total_count, group_count)
    assert len(render(scenario(1)).split('NPU_INFERENCE')[0]) < 16127, 'idle report exceeds the platform log'
    cases.append('producer labels and field counts match the decoder; report fits the platform log')

    (args.output / 'result.json').write_text(json.dumps({
        'passed': True, 'physical_access': False, 'cases': cases,
        'limits': 'Synthetic report grammar and decoder arithmetic only. The producer is not executed and the physical report is unqualified.',
        'comparison': result}, indent=2) + '\n')
    print('PASS', len(cases), 'NPU idle report decoder and comparison cases')


if __name__ == '__main__':
    main()
