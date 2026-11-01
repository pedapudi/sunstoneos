#!/usr/bin/env python3
"""Validate phase distributions produced by the linked eight-core latency workload."""
import argparse
import json
from pathlib import Path
import runpy

BASE = runpy.run_path(str(Path(__file__).with_name('latency.py')))


def upper(value):
    if value < 16:
        return value
    shift = value.bit_length() - 5
    return min((1 << 64) - 1, ((value >> shift) + 1) * (1 << shift) - 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    text = args.log.read_text()
    aggregate = BASE['decode'](text)
    phases, acquisitions = {}, {}
    core = None
    for line in text.splitlines():
        values = BASE['fields'](line)
        if line.startswith('core='):
            core = values['core']
        elif line.startswith('isr_acquisitions='):
            assert core not in acquisitions
            acquisitions[core] = values
        elif line.startswith('phase='):
            key = core, values['phase'], values['metric']
            assert key not in phases
            phases[key] = values
    assert len(phases) == 8 * 3 * 11 and len(acquisitions) == 8
    for core in range(8):
        for lock, name in enumerate(('isr', 'task')):
            assert acquisitions[core][name + '_acquisitions'] >= aggregate['cores'][core][f'lock{lock}_wait']['count']
        for metric, name in enumerate(BASE['REGIONS']):
            total = 0
            for phase in range(3):
                d = phases[core, phase, metric]
                n = d['count']
                total += n
                if not n:
                    assert not any(d[k] for k in ('p50_upper_ticks', 'p99_upper_ticks', 'max_ticks', 'max_start', 'max_site', 'overflow_bin_count'))
                else:
                    assert d['p50_upper_ticks'] <= d['p99_upper_ticks'] <= upper(d['max_ticks'])
                    assert d['max_ticks'] <= aggregate['cores'][core][name]['max_ticks']
                    assert d['overflow_bin_count'] <= n
                    if n < 100:
                        assert d['p99_upper_ticks'] >= d['max_ticks']
                if metric in (6, 7):
                    assert n == (64 if phase == 1 and metric == 6 else 128 if phase == 2 and metric == 7 else 0)
            assert total <= aggregate['cores'][core][name]['count']
    args.output.write_text(json.dumps({'evidence': 'linked QEMU workload; no physical timing',
        'quantile_precision': 'sixteen bins per power of two; nearest-rank conservative upper endpoints',
        'phase_metric_rows': len(phases), 'sample': phases[6, 2, 7]}, indent=2) + '\n')
    print('PASS 264 linked phase/core/metric distributions, lock counts and complete wake attribution')


if __name__ == '__main__':
    main()
