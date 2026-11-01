#!/usr/bin/env python3
"""Compare the baseline, always-on control and gating runs against the limits.

Inputs are board runner result files (or bare decoded latency reports). The
baseline creates no power owner. The control runs the power owner with
register writes disabled. The gating run differs from the control only in
hardware policy. Each run holds five windows of back-to-back jobs; every
statistic below is the median over those windows.

Limits, each applied to the control against the baseline and to the gating run
against the control: throughput at least 98 percent, p99 latency within 105
percent, maximum latency within 110 percent and charged CPU time within 105
percent. Charged CPU time is client, runtime worker and power-owner runtime
per job. Non-idle time (window length minus idle-task runtime) is reported
beside it. Every resume must finish within 100 us and a 9 ms gap must never
gate. Cold-job latency is reported separately. A run whose owner wakes fewer
than 20 times per window is refused, because the owner cost would be unsampled.
"""
import argparse
import json
from pathlib import Path
import statistics

MIN_WAKES = 20


def report(path):
    data = json.loads(Path(path).read_text())
    value = data.get('return_report', data)
    if not isinstance(value, dict) or 'idle' not in value or 'timing' not in value:
        raise SystemExit(f'{path}: no decoded idle report')
    return value


def medians(run):
    windows = run['idle']['windows']
    jobs = windows[0]['jobs']
    column = lambda f: statistics.median(f(w) for w in windows)
    return {'jobs_per_second': column(lambda w: w['jobs_per_second']),
            'p99': column(lambda w: w['latency_ticks']['p99']),
            'max': column(lambda w: w['latency_ticks']['max']),
            'charged': column(lambda w: w['charged_cpu_ticks'] / jobs),
            'non_idle': column(lambda w: w['non_idle_ticks'] / jobs),
            'wakes': column(lambda w: w['owner_wakes']),
            'spread': {'jobs_per_second': [min(w['jobs_per_second'] for w in windows), max(w['jobs_per_second'] for w in windows)],
                       'max': [min(w['latency_ticks']['max'] for w in windows), max(w['latency_ticks']['max'] for w in windows)]}}


def row(name, a, b, limit, ratio, unit, ok):
    return {'criterion': name, 'reference': a, 'measured': b, 'limit': limit, 'ratio': ratio, 'unit': unit, 'passed': ok}


def limits(label, ref, run):
    rows = []
    ratio = run['jobs_per_second'] / ref['jobs_per_second']
    rows.append(row(f'{label}: throughput', ref['jobs_per_second'], run['jobs_per_second'], '>= 98%', ratio, 'jobs/s', ratio >= 0.98))
    for name, key, limit in (('p99 latency', 'p99', 1.05), ('maximum latency', 'max', 1.10), ('charged CPU time per job', 'charged', 1.05)):
        ratio = run[key] / ref[key]
        rows.append(row(f'{label}: {name}', ref[key], run[key], f'<= {limit * 100:.0f}%', ratio, 'ticks', ratio <= limit))
    rows.append(row(f'{label}: non-idle time per job (reported)', ref['non_idle'], run['non_idle'], 'reported', run['non_idle'] / ref['non_idle'], 'ticks', True))
    return rows


def compare(baseline, control, gating):
    for want, run in (('baseline', baseline), ('control', control), ('gate', gating)):
        if run['idle']['policy'] != want:
            raise SystemExit(f'Expected the {want} run, found {run["idle"]["policy"]}.')
        if not run['functional_success'] or run['mismatches'] or not run['idle']['functional_success']:
            raise SystemExit(f'The {want} run has an output mismatch; no timing comparison is valid.')
    stats = {name: medians(run) for name, run in (('baseline', baseline), ('control', control), ('gating', gating))}
    for name in ('control', 'gating'):
        if stats[name]['wakes'] < MIN_WAKES:
            raise SystemExit(f'The {name} run woke its owner {stats[name]["wakes"]} times per window; at least {MIN_WAKES} are required.')
    rows = limits('control vs baseline', stats['baseline'], stats['control']) + limits('gating vs control', stats['control'], stats['gating'])
    groups = gating['idle']['groups']
    resume = max((max(g['driver_resume_us_max'], g['admission_us']['max']) for g in groups if g['gates']), default=0)
    rows.append(row('Resume from suspended clocks', None, resume, '<= 100', None, 'us', 0 < resume <= 100))
    short = groups[0]
    rows.append(row('Gating during the 9 ms gap', 0, short['gates'], '0', None, 'gates', short['gates'] == 0 and short['gap_us'] == 9000))
    rows.append(row('Gating after the 11 ms and 100 ms gaps', None, [g['gates'] for g in groups[1:]], 'every cycle', None, 'gates',
                    all(g['gates'] == g['cycles'] for g in groups[1:])))
    cold = [{'gap_us': c['gap_us'], 'baseline_us': b['cold_job_us'], 'control_us': c['cold_job_us'], 'gating_us': g['cold_job_us'],
             'gating_admission_us': g['admission_us'], 'gates': g['gates']}
            for b, c, g in zip(baseline['idle']['groups'], control['idle']['groups'], groups)]
    idle_ticks = sum(g['idle_ticks']['sum'] for g in groups)
    return {'passed': all(r['passed'] for r in rows), 'criteria': rows, 'medians': stats, 'cold_job_latency': cold,
            'gated_fraction_of_cold_idle': gating['idle']['totals']['gated_ticks'] / idle_ticks}


def table(result):
    def cell(value):
        return '-' if value is None else f'{value:.4g}' if isinstance(value, float) else str(value)
    lines = ['| Criterion | Reference | Measured | Limit | Ratio | Result |', '| --- | --- | --- | --- | --- | --- |']
    for r in result['criteria']:
        lines.append(f"| {r['criterion']} ({r['unit']}) | {cell(r['reference'])} | {cell(r['measured'])} | {r['limit']} | "
                     f"{cell(r['ratio'])} | {'pass' if r['passed'] else 'fail'} |")
    lines += ['', '| Gap | Policy | p50 (us) | p99 (us) | max (us) | Gates |', '| --- | --- | --- | --- | --- | --- |']
    for c in result['cold_job_latency']:
        for policy, key in (('baseline', 'baseline_us'), ('control', 'control_us'), ('gating', 'gating_us')):
            v = c[key]
            lines.append(f"| {c['gap_us'] // 1000} ms | {policy} | {v['p50']:.1f} | {v['p99']:.1f} | {v['max']:.1f} | "
                         f"{c['gates'] if policy == 'gating' else 0} |")
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('baseline', 'control', 'gating'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--output', type=Path, help='write the comparison as JSON')
    args = parser.parse_args()
    result = compare(report(args.baseline), report(args.control), report(args.gating))
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(table(result))
    print('\nPASS' if result['passed'] else '\nFAIL')
    raise SystemExit(0 if result['passed'] else 1)


if __name__ == '__main__':
    main()
