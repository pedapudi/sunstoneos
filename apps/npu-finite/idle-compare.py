#!/usr/bin/env python3
"""Compare the NPU idle-gating run with its always-on control against the limits.

Both inputs are board runner result files (or bare decoded latency reports).
The control runs the same power owner with register writes disabled, so the
two runs differ only in the hardware policy. The limits are throughput at
least 98 percent of the control, p99 within 105 percent, maximum within 110
percent, charged CPU time within 105 percent, every resume within 100 us and
no gating when the idle gap is 9 ms. Cold-job latency is reported separately.
"""
import argparse
import json
from pathlib import Path


def report(path):
    data = json.loads(Path(path).read_text())
    value = data.get('return_report', data)
    if not isinstance(value, dict) or 'idle' not in value or 'timing' not in value:
        raise SystemExit(f'{path}: no decoded idle report')
    return value


def row(name, control, gate, limit, ratio, unit, ok):
    return {'criterion': name, 'control': control, 'gating': gate, 'limit': limit, 'ratio': ratio, 'unit': unit, 'passed': ok}


def compare(control, gate):
    if control['idle']['policy'] != 'control' or gate['idle']['policy'] != 'gate':
        raise SystemExit('Inputs must be the control and the gating run, in that order.')
    for name, value in (('control', control), ('gating', gate)):
        if not value['functional_success'] or value['mismatches'] or not value['idle']['functional_success']:
            raise SystemExit(f'The {name} run has an output mismatch; no timing comparison is valid.')
    if control['output_digest'] != gate['output_digest']:
        raise SystemExit('The two runs produced different warm output digests.')
    ct, gt = control['timing'], gate['timing']
    jobs = control['measured_jobs']
    if jobs != gate['measured_jobs']:
        raise SystemExit('The runs measured different job counts.')
    charged = lambda r: r['idle']['warm']['charged_cpu_ticks'] / r['measured_jobs']
    rows = []
    ratio = gt['jobs_per_second'] / ct['jobs_per_second']
    rows.append(row('Throughput', ct['jobs_per_second'], gt['jobs_per_second'], '>= 98%', ratio, 'jobs/s', ratio >= 0.98))
    for name, key, limit in (('p99 latency', 'p99', 1.05), ('Maximum latency', 'max', 1.10)):
        c, g = ct['latency_ticks'][key], gt['latency_ticks'][key]
        rows.append(row(name, c, g, f'<= {limit * 100:.0f}%', g / c, 'ticks', g <= limit * c))
    ratio = charged(gate) / charged(control)
    rows.append(row('Charged CPU time per job', charged(control), charged(gate), '<= 105%', ratio, 'ticks', ratio <= 1.05))
    groups = gate['idle']['groups']
    resume = max(max(g['driver_resume_us_max'], g['admission_us']['max']) for g in groups if g['gates'])
    rows.append(row('Resume from suspended clocks', None, resume, '<= 100', None, 'us', 0 < resume <= 100))
    short = groups[0]
    rows.append(row('Gating during the 9 ms gap', 0, short['gates'], '0', None, 'gates', short['gates'] == 0 and short['gap_us'] == 9000))
    rows.append(row('Gating after the 11 ms and 100 ms gaps', None, [g['gates'] for g in groups[1:]], 'every cycle', None, 'gates',
                    all(g['gates'] == g['cycles'] for g in groups[1:])))
    cold = [{'gap_us': c['gap_us'],
             'control_us': c['cold_job_us'], 'gating_us': g['cold_job_us'],
             'gating_admission_us': g['admission_us'], 'gates': g['gates']}
            for c, g in zip(control['idle']['groups'], groups)]
    gated_ticks = gate['idle']['totals']['gated_ticks']
    idle_ticks = sum(g['idle_ticks']['sum'] for g in groups)
    return {'passed': all(r['passed'] for r in rows), 'criteria': rows, 'cold_job_latency': cold,
            'gated_fraction_of_cold_idle': gated_ticks / idle_ticks,
            'owner_wakes': {'control': control['idle']['totals']['owner_wakes'], 'gating': gate['idle']['totals']['owner_wakes']}}


def table(result):
    def cell(value):
        return '-' if value is None else f'{value:.4g}' if isinstance(value, float) else str(value)
    lines = ['| Criterion | Control | Gating | Limit | Ratio | Result |', '| --- | --- | --- | --- | --- | --- |']
    for r in result['criteria']:
        lines.append(f"| {r['criterion']} ({r['unit']}) | {cell(r['control'])} | {cell(r['gating'])} | {r['limit']} | "
                     f"{cell(r['ratio'])} | {'pass' if r['passed'] else 'fail'} |")
    lines += ['', '| Gap | Policy | p50 (us) | p99 (us) | max (us) | Admission max (us) | Gates |', '| --- | --- | --- | --- | --- | --- | --- |']
    for c in result['cold_job_latency']:
        for policy, key in (('control', 'control_us'), ('gating', 'gating_us')):
            v = c[key]
            lines.append(f"| {c['gap_us'] // 1000} ms | {policy} | {v['p50']:.1f} | {v['p99']:.1f} | {v['max']:.1f} | "
                         f"{c['gating_admission_us']['max'] if policy == 'gating' else '-'} | {c['gates'] if policy == 'gating' else 0} |")
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--control', type=Path, required=True)
    parser.add_argument('--gating', type=Path, required=True)
    parser.add_argument('--output', type=Path, help='write the comparison as JSON')
    args = parser.parse_args()
    result = compare(report(args.control), report(args.gating))
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(table(result))
    print('\nPASS' if result['passed'] else '\nFAIL')
    raise SystemExit(0 if result['passed'] else 1)


if __name__ == '__main__':
    main()
