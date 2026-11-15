#!/usr/bin/env python3
"""Check the accel-overlap counter decoder (apps/accel-overlap/host.py) against records written through the C layout of
result.h: modeled runs decode with every check in range and the expected GPU and NPU decisions, a rate between the
two GPU models stays undecided, and a stale or missing sample fails a check."""
import argparse
import importlib.util
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / 'apps/accel-overlap'


def load_host():
    spec = importlib.util.spec_from_file_location('accel_overlap_host', APP / 'host.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    writer = args.output / 'writer'
    subprocess.run(['clang-21', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-I' + str(APP),
                    str(ROOT / 'checks/accel-overlap-counters.c'), '-o', str(writer)], check=True,
                   env={**harness.environment(), 'TMPDIR': str(args.output)})
    host = load_host()
    results = []

    def record(name, once, rule):
        path = args.output / (name + '.bin')
        subprocess.run([str(writer), str(path), str(once), rule], check=True)
        raw = bytearray(path.read_bytes())
        if len(raw) != host.COUNTERS_BYTES:
            raise SystemExit(f'FAIL record holds {len(raw)} bytes, the decoder expects {host.COUNTERS_BYTES}')
        return raw

    def decoded(name, raw):
        r = host.decode(bytes(raw))
        misses = [c['name'] for c in r['checks'] if not c['ok']]
        results.append({'case': name, 'misses': misses, 'models': r['models']})
        return r, misses

    # Request-rate outcome (2.6x) with whole-row NPU units, and residency (1.2x) with 256-byte bursts.
    for name, once, rule, model in (('request-rate-rows', 1776, 'rows', 'request rate'),
                                    ('residency-bursts', 808, 'bursts', 'residency')):
        r, misses = decoded(name, record(name, once, rule))
        if misses or r['models']['gpu']['model'] != model or r['models']['npu']['rule'] != rule:
            raise SystemExit(f'FAIL {name}: misses {misses}, models {r["models"]}')
        print(f'{name}: {len(r["checks"])} checks in range, gpu {model}, npu {rule}')
    # Between the two models the limit stays undecided.
    r, misses = decoded('undecided', record('undecided', 942, 'rows'))
    if misses != ['gpu limit decided']:
        raise SystemExit(f'FAIL undecided: misses {misses}')
    print('undecided: only the decision check misses')
    base = host.COUNTERS_BYTES - 8 * 808
    # A pre sample that already shows its own period: phase 1, period 3, port 6 data reads.
    raw = record('stale', 1776, 'rows')
    pre = base + 808 * 1 + 168 + 64 * 2 + 16 + 4 * 2
    post = base + 808 * 1 + 168 + 64 * (5 + 2) + 16 + 4 * 2
    raw[pre:pre + 4] = raw[post:post + 4]
    _, misses = decoded('stale', raw)
    if 'gpu-per-lane port 6 pre samples that are not the previous period' not in misses:
        raise SystemExit(f'FAIL stale pre sample decoded in range: {misses}')
    print('stale: refused')
    # A phase with nine samples.
    raw = record('short', 1776, 'rows')
    at = base + 808 * 3 + 40
    raw[at:at + 4] = (9).to_bytes(4, 'little')
    _, misses = decoded('short', raw)
    if 'samples per phase' not in misses:
        raise SystemExit(f'FAIL nine samples decoded in range: {misses}')
    print('short: refused')
    harness.save(args.output / 'result.json', {'physical_access': False, 'cases': results})


if __name__ == '__main__':
    main()
