#!/usr/bin/env python3
"""Compare enabled and disabled native reports without accessing hardware."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('cost_host', ROOT / 'apps/latency-cost/host.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--enabled', type=Path, required=True)
    parser.add_argument('--disabled', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    reports = [host.decode(p.read_bytes()) for p in (args.enabled, args.disabled)]
    on, off = reports
    assert on['enabled'] and not off['enabled']
    assert on['frequency'] == off['frequency']
    delta = []
    for a, b in zip(on['cores'], off['cores']):
        row = {'core': a['core']}
        for selection in ('all_batches', 'no_tick_or_extra_switch'):
            x, y = a[selection], b[selection]
            row[selection] = {key + '_difference_ns': x[key] - y[key]
                              for key in ('minimum_ns', 'mean_ns', 'maximum_ns')} if x and y else None
        delta.append(row)
    irq_delta = {}
    for n in on['timer_handler_by_tick_count'].keys() & off['timer_handler_by_tick_count'].keys():
        a, b = on['timer_handler_by_tick_count'][n], off['timer_handler_by_tick_count'][n]
        irq_delta[n] = {key + '_difference_ns': a[key] - b[key]
                        for key in ('minimum_ns', 'mean_ns', 'maximum_ns')}
    output = dict(enabled=on, disabled=off, difference=delta, timer_handler_difference_by_tick_count=irq_delta,
                  sources=[dict(path=str(p), sha256=hashlib.sha256(p.read_bytes()).hexdigest())
                           for p in (args.enabled, args.disabled)],
                  interpretation='Differences are per-switch-normalized round-trip costs. A difference between observed maxima is not an upper bound on individual switch overhead. Independent IRQ and cache histories can affect paired runs.')
    args.output.write_text(json.dumps(output, indent=2) + '\n')
    print(json.dumps(delta, indent=2))


if __name__ == '__main__':
    main()
