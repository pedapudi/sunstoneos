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
CHECK_INPUTS = ['apps/npu-finite/host.py', 'apps/npu-finite/idle-compare.py', 'apps/npu-finite/idle-client.h',
                'apps/npu-finite/queue-client.c', 'apps/npu-finite/build.py', 'apps/npu-interrupt/host.py']
FREQUENCY = 24000000
GAPS = (9000, 11000, 100000)
CYCLES = 100
WINDOWS = 5
WINDOW_JOBS = 8000
JOBS = 41301
DEADLINE = 3000000000
REFERENCES = [bytes(range(64)), bytes(range(64, 128))]
POLICIES = ('control', 'gate', 'baseline')


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def hexword(value):
    return f'0x{value:016x}\n'


def hexbytes(data):
    return ''.join(data[at:at + 64].hex() + '\n' for at in range(0, len(data), 64))


def digest(jobs):
    return hashlib.sha256(hashlib.sha256(REFERENCES[0]).digest() * jobs).hexdigest()


def scenario(policy, throughput=1.0, cpu=1.0, latency=1.0, jitter=0):
    """The modeled counters of one run. The scale factors move timing only."""
    gate, owned = policy == 'gate', policy != 'baseline'
    base = DEADLINE - 470000000
    durations = [240 + i % 20 for i in range(1000)]
    ordered = sorted(durations)
    warm = {'start': base, 'end': base + 1200000, 'durations': durations, 'total': sum(durations),
            'quantiles': (ordered[499], ordered[989], ordered[999]), 'client': 300000, 'worker': 80000, 'irq': 60000,
            'owner_ticks': 12000 if owned else 0, 'owner_elapsed': 11000 if owned else 0, 'wakes': 6 if owned else 0}
    at = warm['end'] + 1000
    windows = []
    for w in range(WINDOWS):
        length = int(WINDOW_JOBS * 900 * throughput) + jitter * w
        windows.append({'index': w, 'jobs': WINDOW_JOBS, 'checked': WINDOW_JOBS, 'bad': 0, 'start': at, 'end': at + length,
                        'sum': WINDOW_JOBS * 250, 'p50': 245, 'p99': int(262 * latency), 'max': int(290 * latency + w), 'overflow': 0,
                        'client': int(length * 0.4 * cpu), 'worker': int(length * 0.1), 'owner': int(length * 0.01 * cpu) if owned else 0,
                        'idle_task': int(length * 0.2), 'irq': int(length * 0.05),
                        'wakes': length // 240000 - 1 if owned else 0, 'first_bad': 2 ** 64 - 1, 'first_offset': 2 ** 64 - 1,
                        'first_byte': 2 ** 64 - 1, 'output': digest(WINDOW_JOBS)})
        at += length + 1000
    groups = []
    for gap in GAPS:
        gating = gate and gap > 10000
        idle_ticks = [gap * 24 + 700 + i for i in range(CYCLES)]
        admission = [70 if gating else 0 for i in range(CYCLES)]
        cold = sorted(int(420 + i % 7 + (admission[i] * 24 if gating else 0)) for i in range(CYCLES))
        length = sum(idle_ticks) + sum(cold) + 20000
        groups.append({'gap': gap, 'cycles': CYCLES, 'checked': CYCLES, 'bad': 0, 'start': at, 'end': at + length,
                       'gates': CYCLES if gating else 0, 'resumes': CYCLES if gating else 0, 'writes': 6 * CYCLES if gating else 0,
                       'gated': sum(idle_ticks) - CYCLES * 240000 - 5000 if gating else 0, 'requests': CYCLES if gating else 0,
                       'driver_max': 70 if gating else 0, 'owner_ticks': (3000 if gating else 800) if owned else 0,
                       'owner_elapsed': (4000 if gating else 900) if owned else 0, 'client': 20000, 'worker': 9000,
                       'idle_task': length // 2, 'irq': 7000, 'idle_min': min(idle_ticks), 'idle_max': max(idle_ticks),
                       'idle_sum': sum(idle_ticks), 'first': 2 ** 64 - 1, 'first_offset': 2 ** 64 - 1, 'first_byte': 2 ** 64 - 1,
                       'pending': CYCLES if gating else 0, 'cold50': cold[49], 'cold99': cold[98], 'cold_max': cold[99],
                       'cold_sum': sum(cold), 'adm50': admission[49], 'adm99': admission[98], 'adm_max': max(admission),
                       'output': digest(CYCLES)})
        at += length + 1000
    return {'policy': policy, 'warm': warm, 'windows': windows, 'groups': groups, 'output': digest(1000)}


GROUP_FIELDS = ('gap', 'cycles', 'checked', 'bad', 'start', 'end', 'gates', 'resumes', 'writes', 'gated', 'requests', 'driver_max',
                'owner_ticks', 'owner_elapsed', 'client', 'worker', 'idle_task', 'irq', 'idle_min', 'idle_max', 'idle_sum',
                'first', 'first_offset', 'first_byte', 'pending', 'cold50', 'cold99', 'cold_max', 'cold_sum', 'adm50', 'adm99', 'adm_max')
WINDOW_FIELDS = ('index', 'jobs', 'checked', 'bad', 'start', 'end', 'sum', 'p50', 'p99', 'max', 'overflow', 'client', 'worker',
                 'owner', 'idle_task', 'irq', 'wakes', 'first_bad', 'first_offset', 'first_byte')


def render(s):
    w, g = s['warm'], s['groups']
    raw = struct.pack('<1000I', *w['durations'])
    values = [FREQUENCY, 1, 1000, 1, JOBS, 1000, 0, w['start'], w['end'], *w['quantiles'], w['total'], 5000, 6000,
              2 ** 64 - 1, 2 ** 64 - 1, 2 ** 64 - 1, 1000, 1000, w['irq'], w['client'], w['worker']]
    text = ('NPU_STAGE=WAIT_STARTUP\nNPU_STAGE=WARMUP\nNPU_STAGE=LATENCY\nNPU_STAGE=CLOSE\n'
            'NPU_STAGE=WAIT_LOCAL_STOP\nNPU_IRQ_COMPLETIONS=' + hexword(JOBS + 1)[:-1] + '\n'
            'NPU_IRQ_STOPPED=0x0000000000000001\nNPU_STAGE=QUIESCENT\nNPU_LATENCY=\n')
    text += ''.join(hexword(v) for v in values)
    text += 'NPU_SUBMIT_TO_DOORBELL=\n' + ''.join(hexword(v) for v in (1000, 100, 110, 150, 100000))
    text += 'NPU_DURATION_LE32=\n' + hexbytes(raw)
    text += 'NPU_DURATION_SHA256=\n' + hashlib.sha256(raw).hexdigest() + '\n'
    text += 'NPU_OUTPUT_SHA256=\n' + s['output'] + '\n'
    gate = s['policy'] == 'gate'
    totals = [POLICIES.index(s['policy']), w['owner_ticks'], w['owner_elapsed'], w['wakes'],
              sum(x['gates'] for x in g), sum(x['resumes'] for x in g), sum(x['writes'] for x in g), sum(x['gated'] for x in g),
              1680 if gate else 0, 0, w['owner_elapsed'] + sum(x['owner_elapsed'] for x in g) + (500 if s['policy'] != 'baseline' else 0),
              (w['wakes'] + 40) if s['policy'] != 'baseline' else 0, w['irq'], 0]
    text += 'NPU_IDLE=\n' + ''.join(hexword(v) for v in totals)
    for x in s['windows']:
        text += 'NPU_IDLE_WINDOW=\n' + ''.join(hexword(x[k]) for k in WINDOW_FIELDS) + 'NPU_IDLE_WINDOW_OUTPUT_SHA256=\n' + x['output'] + '\n'
    for x in g:
        text += 'NPU_IDLE_GROUP=\n' + ''.join(hexword(x[k]) for k in GROUP_FIELDS) + 'NPU_IDLE_OUTPUT_SHA256=\n' + x['output'] + '\n'
    return text + f'NPU_INFERENCE=MATCH; inference_jobs={JOBS}\n'


def geometry(policy):
    return {'runtime': True, 'references': [r.hex() for r in REFERENCES],
            'latency': {'mode': 'queue', 'measured_jobs': 1000, 'warmup_jobs': 1, 'model': 0, 'sequence_repeats': 0,
                        'doorbell_timing': True,
                        'idle': {'policy': policy, 'cycles': 100, 'gaps_us': list(GAPS), 'threshold_us': 10000,
                                 'resume_budget_us': 100, 'windows': WINDOWS, 'window_jobs': WINDOW_JOBS}}}


def decode(host, s):
    return host.latency_report(render(s), 0, DEADLINE, geometry(s['policy']))


def source_labels():
    """Labels and field counts the producer prints, in order, from its source."""
    text = (ROOT / 'apps/npu-finite/idle-client.h').read_text()
    report = text[text.index('static void idle_report'):]
    labels = re.findall(r'platform_puts\("(NPU_IDLE[A-Z0-9_]*)=\\n"\)', report)
    count = lambda body: len([part for part in body.replace('\n', ' ').split(',') if part.strip()])
    arrays = [count(m) for m in re.findall(r'const uint64_t (?:values|stats)\[\]=\{(.*?)\};', report, re.S)]
    return labels, arrays


GATING = 'Idle gating or resume bound differs.'
UNGATED = 'Idle interval gated or wrote registers outside the enabled policy.'
GEOMETRY = 'Idle gap, duration or task counters differ.'
IDENTITY = 'Idle group identity or order differs.'
WINDOW = 'Idle window identity, order or timing differs.'


def group(i):
    return lambda s: s['groups'][i]


def window(i):
    return lambda s: s['windows'][i]


MUTATIONS = [
    ('a gating cycle without a gate', 'gate', lambda s: s['groups'][1].update(gates=99), GATING),
    ('a gate under the control policy', 'control', lambda s: s['groups'][2].update(gates=1), UNGATED),
    ('a gate at the 9 ms gap', 'gate', lambda s: s['groups'][0].update(gates=1, writes=6), UNGATED),
    ('a resume above the budget', 'gate', lambda s: s['groups'][1].update(driver_max=101), GATING),
    ('an admission above the budget', 'gate', lambda s: s['groups'][2].update(adm_max=101, adm99=101), GATING),
    ('gated time above the idle time', 'gate', lambda s: s['groups'][2].update(gated=10 ** 12), GATING),
    ('the 9 ms gap reaching the threshold', 'gate', lambda s: s['groups'][0].update(idle_max=240001, idle_sum=s['groups'][0]['idle_sum'] + 1), 'Idle gap below the threshold reached it.'),
    ('an idle minimum below its gap', 'gate', lambda s: s['groups'][1].update(idle_min=1), GEOMETRY),
    ('a cold output mismatch not counted', 'gate', lambda s: s['groups'][0].update(output='0' * 64), 'Idle output status disagrees with its digest.'),
    ('a group that starts before the previous ends', 'gate', lambda s: s['groups'][1].update(start=s['groups'][0]['start']), IDENTITY),
    ('a cold quantile order', 'gate', lambda s: s['groups'][1].update(cold50=s['groups'][1]['cold99'] + 1), GEOMETRY),
    ('owner time beyond the group', 'gate', lambda s: s['groups'][2].update(owner_ticks=10 ** 12), GEOMETRY),
    ('a short cycle count', 'gate', lambda s: s['groups'][0].update(cycles=99), IDENTITY),
    ('a window output mismatch not counted', 'control', lambda s: s['windows'][2].update(output='0' * 64), 'Idle window output status disagrees with its digest.'),
    ('a window p99 above its maximum', 'control', lambda s: s['windows'][1].update(p99=400), WINDOW),
    ('window task time beyond its length', 'control', lambda s: s['windows'][3].update(idle_task=10 ** 9), WINDOW),
    ('more owner wakes than alarms', 'control', lambda s: s['windows'][0].update(wakes=10 ** 4), WINDOW),
    ('owner wakes in the baseline', 'baseline', lambda s: s['windows'][0].update(wakes=3), WINDOW),
    ('a window that starts before the previous ends', 'control', lambda s: s['windows'][1].update(start=s['windows'][0]['start']), WINDOW),
    ('a histogram overflow with an in-range maximum', 'control', lambda s: s['windows'][4].update(overflow=5), WINDOW),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    host = load('npu_finite_idle_host', 'apps/npu-interrupt/host.py').finite
    compare = load('npu_idle_compare', 'apps/npu-finite/idle-compare.py')
    cases = []

    baseline, control, gating = (decode(host, scenario(p)) for p in ('baseline', 'control', 'gate'))
    assert [r['idle']['policy'] for r in (baseline, control, gating)] == ['baseline', 'control', 'gate']
    assert [g['gates'] for g in gating['idle']['groups']] == [0, 100, 100]
    assert gating['idle']['groups'][1]['admission_us']['max'] == 70
    assert baseline['idle']['totals']['owner_wakes'] == 0 and len(gating['idle']['windows']) == WINDOWS
    cases.append('decodes the baseline, control and gating policies')

    result = compare.compare(baseline, control, gating)
    assert result['passed'] and len(result['criteria']) == 13, result
    (args.output / 'comparison.md').write_text(compare.table(result) + '\n')
    cases.append('comparison passes within the limits')

    def failed(c, g, b=baseline):
        return {r['criterion'] for r in compare.compare(b, c, g)['criteria'] if not r['passed']}
    assert failed(control, decode(host, scenario('gate', throughput=1.03))) == {'gating vs control: throughput'}
    assert failed(control, decode(host, scenario('gate', cpu=1.5))) == {'gating vs control: charged CPU time per job'}
    assert failed(control, decode(host, scenario('gate', latency=1.12))) == {'gating vs control: p99 latency', 'gating vs control: maximum latency'}
    assert failed(decode(host, scenario('control', throughput=1.03)), gating) >= {'control vs baseline: throughput'}
    assert failed(decode(host, scenario('control', cpu=1.5)), gating) >= {'control vs baseline: charged CPU time per job'}
    wrong = copy.deepcopy(gating)
    wrong['idle']['groups'][0]['gates'] = 1
    assert 'Gating during the 9 ms gap' in failed(control, wrong)
    few = copy.deepcopy(control)
    for w in few['idle']['windows']:
        w['owner_wakes'] = 3
    refused(lambda: compare.compare(baseline, few, gating), 'at least 20 are required', SystemExit)
    cases.append('comparison refuses each limit violation and an unsampled owner')
    # One slow window does not move the medians; the spread is reported.
    jittery = decode(host, scenario('gate', jitter=20000))
    assert compare.compare(baseline, control, jittery)['passed']
    assert compare.medians(jittery)['spread']['jobs_per_second'][0] < compare.medians(control)['spread']['jobs_per_second'][0]

    for label, policy, mutate, reason in MUTATIONS:
        s = scenario(policy)
        mutate(s)
        refused(lambda: decode(host, s), reason, ValueError)
        cases.append('refuses ' + label)
    refused(lambda: host.latency_report(render(scenario('gate')), 0, DEADLINE, geometry('control')), 'Idle policy or warm owner counters differ.', ValueError)
    cases.append('refuses a report whose policy differs from the image marker')

    labels, arrays = source_labels()
    expected = ['NPU_IDLE', 'NPU_IDLE_WINDOW', 'NPU_IDLE_WINDOW_OUTPUT_SHA256', 'NPU_IDLE_GROUP', 'NPU_IDLE_OUTPUT_SHA256']
    assert labels == expected, labels
    assert arrays == [14, len(WINDOW_FIELDS), len(GROUP_FIELDS)], arrays
    size = len(render(scenario('gate')).split('NPU_INFERENCE')[0])
    assert size < 16127, f'idle report of {size} bytes exceeds the platform log'
    cases.append(f'producer labels and field counts match the decoder; report of {size} bytes fits the platform log')

    (args.output / 'result.json').write_text(json.dumps({
        'passed': True, 'physical_access': False, 'cases': cases,
        'limits': 'Synthetic report grammar and decoder arithmetic only. The producer is not executed and the physical report is unqualified.',
        'comparison': result}, indent=2) + '\n')
    print('PASS', len(cases), 'NPU idle report decoder and comparison cases')


if __name__ == '__main__':
    main()
