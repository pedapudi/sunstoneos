#!/usr/bin/env python3
"""Compare the baseline, always-on control and gating runs against the limits.

Inputs are board runner result files (or bare decoded latency reports), one or
more per variant. The baseline creates no power owner. The control runs the
power owner with register writes disabled. The gating run differs from the
control only in hardware policy. Each run holds five windows of back-to-back
jobs. Every statistic is the median over the windows of one boot, then the
median over boots, so a boot-to-boot drift moves a run's figure only by its
median share; the spread across boots is reported.

Limits, each applied to the control against the baseline and to the gating run
against the control: throughput at least 98 percent, p99 latency within 105
percent, maximum latency within 110 percent and charged CPU time within 105
percent. Charged CPU time is client, runtime worker and power-owner runtime
per job. Non-idle time (window length minus idle-task runtime) and NPU
interrupt time per job are reported beside it.

A busy window never lets the gating image gate, so gating against control in
the windows measures noise, not the gating cost. The gating cost is judged by
the cold groups: every resume within 100 us and no gating during 9 ms gaps.
Cold-job latency is reported separately. A run whose owner wakes fewer than 20
times per window is refused, because the owner cost would be unsampled.
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


def window_medians(run):
    windows = run['idle']['windows']
    jobs = windows[0]['jobs']
    column = lambda f: statistics.median(f(w) for w in windows)
    return {'jobs_per_second': column(lambda w: w['jobs_per_second']),
            'p99': column(lambda w: w['latency_ticks']['p99']),
            'max': column(lambda w: w['latency_ticks']['max']),
            'charged': column(lambda w: w['charged_cpu_ticks'] / jobs),
            'non_idle': column(lambda w: w['non_idle_ticks'] / jobs),
            'isr': column(lambda w: w['irq_driver_elapsed_ticks'] / jobs),
            'wakes': column(lambda w: w['owner_wakes'])}


def medians(runs):
    """Median over boots of each boot's median over windows, with the spread across boots."""
    per_boot = [window_medians(run) for run in runs]
    result = {key: statistics.median(boot[key] for boot in per_boot) for key in per_boot[0]}
    result['boots'] = len(runs)
    result['spread'] = {key: [min(boot[key] for boot in per_boot), max(boot[key] for boot in per_boot)]
                        for key in ('jobs_per_second', 'p99', 'max', 'charged')}
    result['min_wakes'] = min(boot['wakes'] for boot in per_boot)
    return result


def row(name, a, b, limit, ratio, unit, ok):
    return {'criterion': name, 'reference': a, 'measured': b, 'limit': limit, 'ratio': ratio, 'unit': unit, 'passed': ok}


def limits(label, ref, run, note=''):
    rows = []
    ratio = run['jobs_per_second'] / ref['jobs_per_second']
    rows.append(row(f'{label}: throughput{note}', ref['jobs_per_second'], run['jobs_per_second'], '>= 98%', ratio, 'jobs/s', ratio >= 0.98))
    for name, key, limit in (('p99 latency', 'p99', 1.05), ('maximum latency', 'max', 1.10), ('charged CPU time per job', 'charged', 1.05)):
        ratio = run[key] / ref[key]
        rows.append(row(f'{label}: {name}{note}', ref[key], run[key], f'<= {limit * 100:.0f}%', ratio, 'ticks', ratio <= limit))
    for name, key in (('non-idle time per job', 'non_idle'), ('NPU ISR time per job', 'isr')):
        rows.append(row(f'{label}: {name} (reported)', ref[key], run[key], 'reported', run[key] / ref[key] if ref[key] else None, 'ticks', True))
    return rows


def compare(baselines, controls, gatings):
    for want, runs in (('baseline', baselines), ('control', controls), ('gate', gatings)):
        for run in runs:
            if run['idle']['policy'] != want:
                raise SystemExit(f'Expected the {want} run, found {run["idle"]["policy"]}.')
            if not run['functional_success'] or run['mismatches'] or not run['idle']['functional_success']:
                raise SystemExit(f'A {want} run has an output mismatch; no timing comparison is valid.')
    stats = {name: medians(runs) for name, runs in (('baseline', baselines), ('control', controls), ('gating', gatings))}
    for name in ('control', 'gating'):
        if stats[name]['min_wakes'] < MIN_WAKES:
            raise SystemExit(f'A {name} run woke its owner {stats[name]["min_wakes"]} times per window; at least {MIN_WAKES} are required.')
    rows = limits('control vs baseline', stats['baseline'], stats['control'])
    noise = limits('gating vs control', stats['control'], stats['gating'], ' (noise: no gate in busy windows)')
    groups = [[run['idle']['groups'][i] for run in gatings] for i in range(3)]
    resume = max((max(g['driver_resume_us_max'], g['admission_us']['max']) for gs in groups for g in gs if g['gates']), default=0)
    cold_rows = [row('Resume from suspended clocks', None, resume, '<= 100', None, 'us', 0 < resume <= 100),
                 row('Gating during the 9 ms gap', 0, sum(g['gates'] for g in groups[0]), '0', None, 'gates',
                     all(g['gates'] == 0 and g['gap_us'] == 9000 for g in groups[0])),
                 row('Gating after the 11 ms and 100 ms gaps', None, [sum(g['gates'] for g in gs) for gs in groups[1:]], 'every cycle', None, 'gates',
                     all(g['gates'] == g['cycles'] for gs in groups[1:] for g in gs))]
    med = lambda runs, i, field: statistics.median(r['idle']['groups'][i]['cold_job_us'][field] for r in runs)
    cold = []
    for i in range(3):
        entry = {'gap_us': groups[i][0]['gap_us'], 'gates': sum(g['gates'] for g in groups[i]),
                 'gating_admission_us': {'max': max(g['admission_us']['max'] for g in groups[i])}}
        for name, runs in (('baseline', baselines), ('control', controls), ('gating', gatings)):
            entry[name + '_us'] = {f: med(runs, i, f) for f in ('p50', 'p99', 'max')}
        cold.append(entry)
    idle_ticks = sum(g['idle_ticks']['sum'] for gs in groups for g in gs)
    gated = sum(r['idle']['totals']['gated_ticks'] for r in gatings)
    return {'passed': all(r['passed'] for r in rows + noise + cold_rows), 'cold_passed': all(r['passed'] for r in cold_rows),
            'control_passed': all(r['passed'] for r in rows), 'criteria': rows + noise + cold_rows, 'medians': stats,
            'cold_job_latency': cold, 'gated_fraction_of_cold_idle': gated / idle_ticks}


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
        parser.add_argument('--' + name, type=Path, nargs='+', required=True)
    parser.add_argument('--output', type=Path, help='write the comparison as JSON')
    args = parser.parse_args()
    result = compare([report(p) for p in args.baseline], [report(p) for p in args.control], [report(p) for p in args.gating])
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(table(result))
    print(f"\nCold groups: {'PASS' if result['cold_passed'] else 'FAIL'}; control vs baseline: "
          f"{'PASS' if result['control_passed'] else 'FAIL'}; all criteria: {'PASS' if result['passed'] else 'FAIL'}")
    raise SystemExit(0 if result['passed'] else 1)


if __name__ == '__main__':
    main()
