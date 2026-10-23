#!/usr/bin/env python3
"""Build and run the host model of the resident reboot path.

checks/reset-latency.c compiles apps/os/services.c with a modeled counter and
WDT0 and parks each core at every phase of the renewal second. The changed
image must reboot after every park within 4.02 s (one 20 ms supervisor pass
plus the 4 s watchdog interval) and report the reason as the supervisor's
stop detail. With --baseline-services the same model runs the services.c of
an earlier tree, which has no pending word. With --capture it also requires
that checks/fault-capture.py saw every kind of park on every core store its
pending byte, which is what the model's park does. No hardware is accessed.
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HZ = 24000000
FLAGS = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-sanitize-recover=all', '-DSUNSTONE_BOARD_SUPERVISED=1', '-DSUNSTONE_BOARD_USB2_DUAL=1',
         '-DSUNSTONE_BOARD_RESIDENT=1']


def build_and_run(services, output, name, fault_page, step, compiler):
    binary = output / name
    flags = FLAGS + [f'-DSERVICES_SOURCE="{services}"', '-I' + str(ROOT / 'include'),
                     '-I' + str(ROOT / 'platform/a733-smp-cached'), '-I' + str(ROOT / 'apps/os'),
                     '-I' + str(ROOT / 'apps/board')]
    if fault_page:
        flags.append('-DSUNSTONE_A733_FAULT_PAGE=1')
    subprocess.run([compiler, *flags, str(ROOT / 'checks/reset-latency.c'), '-o', str(binary)], check=True)
    out = subprocess.run([str(binary), str(step)], check=True, capture_output=True, text=True).stdout
    return [json.loads(line) for line in out.splitlines()]


def summarize(rows):
    parks = [r for r in rows if r['case'] == 'park']
    rebooted = [r for r in parks if r['expired']]
    latency = [r['expired'] - r['at'] for r in rebooted]
    by_core = {}
    for r in parks:
        by_core.setdefault(r['core'], []).append(bool(r['expired']))
    return dict(parks=len(parks), rebooted=len(rebooted),
                cores_always_rebooted=sorted(c for c, v in by_core.items() if all(v)),
                max_latency_s=max(latency) / HZ if latency else None)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline-services', type=Path)
    parser.add_argument('--capture', type=Path, help='result.json of checks/fault-capture.py')
    parser.add_argument('--phase-step-ms', type=int, default=1)
    parser.add_argument('--compiler', default='clang')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    services = ROOT / 'apps/os/services.c'
    rows = build_and_run(services, args.output, 'reset-latency', True, args.phase_step_ms, args.compiler)
    changed = summarize(rows)
    parks = [r for r in rows if r['case'] == 'park']
    for r in parks:
        assert r['expired'], ('no reboot', r)
        assert r['expired'] - r['at'] <= 4 * HZ + HZ // 50, ('late reboot', r)
        if r['core']:
            assert r['refused'] and r['refused'] - r['at'] <= HZ // 50 and r['detail'] == 0, r
    for r in rows:
        if r['case'] in ('stall', 'silent'):
            assert r['expired'], r
        if r['case'] == 'silent':
            assert r['expired'] == 4 * HZ and not r['reloads'], r
        if r['case'] == 'healthy':
            assert not r['expired'] and not r['refused'] and r['reloads'] >= 28, r
    # The detail names the stalled or silent service: one plus its index.
    for case in ('stall', 'silent'):
        details = [r['detail'] for r in rows if r['case'] == case]
        assert details == list(range(1, len(details) + 1)), (case, details)
    result = dict(passed=True, physical_access=False, changed=changed,
                  services=sum(r['case'] == 'stall' for r in rows), phase_step_ms=args.phase_step_ms,
                  services_sha256=hashlib.sha256(services.read_bytes()).hexdigest(),
                  model_sha256=hashlib.sha256((ROOT / 'checks/reset-latency.c').read_bytes()).hexdigest(),
                  limit='Modeled counter, WDT0 and task placement; physical expiry and cold reboot are unproven.')
    if args.capture:
        capture = json.loads(args.capture.read_text())
        kinds = {(c['core'], c['kind']) for c in capture['cases']}
        assert len(kinds) == len(capture['cases']) and {c for c, _ in kinds} == set(range(8)), kinds
        result['capture_parks'] = len(kinds)
        result['capture_kinds'] = sorted({k for _, k in kinds})
    if args.baseline_services:
        base = build_and_run(args.baseline_services.resolve(), args.output, 'reset-latency-baseline', False,
                             args.phase_step_ms, args.compiler)
        result['baseline'] = summarize(base)
        result['baseline_services_sha256'] = hashlib.sha256(args.baseline_services.read_bytes()).hexdigest()
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f"changed: {changed['rebooted']} of {changed['parks']} parks reboot, at most "
          f"{changed['max_latency_s']:.3f} s after the park")
    if 'baseline' in result:
        b = result['baseline']
        print(f"baseline: {b['rebooted']} of {b['parks']} parks reboot (cores {b['cores_always_rebooted']}); "
              'the others leave renewal running')
    print('PASS: every park of every core stops renewal and the watchdog resets the board')


if __name__ == '__main__':
    main()
