#!/usr/bin/env python3
"""Check the accel-overlap counter decoder (apps/accel-overlap/host.py) against records written through the C layout of
result.h: a modeled run of each form at 144 values per row decodes with every check in range and that form decided,
counts that fit no form leave it open, a control that misses voids it, and a stale or missing sample fails a check."""
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

    def record(name, form, control=1.0):
        path = args.output / (name + '.bin')
        subprocess.run([str(writer), str(path), form, str(control)], check=True)
        raw = bytearray(path.read_bytes())
        if len(raw) != host.COUNTERS_BYTES:
            raise SystemExit(f'FAIL record holds {len(raw)} bytes, the decoder expects {host.COUNTERS_BYTES}')
        return raw

    def decoded(name, raw):
        r = host.decode(bytes(raw))
        misses = [c['name'] for c in r['checks'] if not c['ok']]
        results.append({'case': name, 'misses': misses, 'models': r['models']})
        return r, misses

    # Each form at 144 values per row decodes with every check in range and that form decided.
    for form in ('A', 'B', 'C'):
        name = 'form-' + form.lower()
        r, misses = decoded(name, record(name, form))
        if misses or r['models']['npu']['form'] != form:
            raise SystemExit(f'FAIL {name}: misses {misses}, models {r["models"]}')
        print(f'{name}: {len(r["checks"])} checks in range, form {form}')
    # Counts that fit no form leave the decision open.
    r, misses = decoded('none', record('none', 'none'))
    if misses != ['npu w144 form decided'] or r['models']['npu']['form'] != 'none':
        raise SystemExit(f'FAIL none: misses {misses}, models {r["models"]}')
    print('none: only the decision check misses')
    # A control 3% off voids the decision even when 144 fits a form.
    r, misses = decoded('void', record('void', 'A', 1.03))
    if r['models']['npu']['form'] != 'void' or 'npu-w176 dt_wr per job' not in misses:
        raise SystemExit(f'FAIL void: misses {misses}, models {r["models"]}')
    print('void: the control miss voids form A')
    base = host.COUNTERS_BYTES - 8 * 808
    # A pre sample that already shows its own period: phase 1, period 3, port 6 data reads.
    raw = record('stale', 'A')
    pre = base + 808 * 1 + 168 + 64 * 2 + 16 + 4 * 2
    post = base + 808 * 1 + 168 + 64 * (5 + 2) + 16 + 4 * 2
    raw[pre:pre + 4] = raw[post:post + 4]
    _, misses = decoded('stale', raw)
    if 'gpu-per-lane port 6 pre samples that are not the previous period' not in misses:
        raise SystemExit(f'FAIL stale pre sample decoded in range: {misses}')
    print('stale: refused')
    # A phase with nine samples.
    raw = record('short', 'A')
    at = base + 808 * 3 + 40
    raw[at:at + 4] = (9).to_bytes(4, 'little')
    _, misses = decoded('short', raw)
    if 'samples per phase' not in misses:
        raise SystemExit(f'FAIL nine samples decoded in range: {misses}')
    print('short: refused')
    harness.save(args.output / 'result.json', {'physical_access': False, 'cases': results})


if __name__ == '__main__':
    main()
