#!/usr/bin/env python3
"""Judge stage 3b (docs/designs/concurrent-accel.md) from the runner's result.json files, one per run, in run order.

The busy phase of the ret-latency-npu image has four sub-phases of 32 wakes per core: NPU off (N0), on (P1, P2), off (N3).
The verdict for each of cores 1-3 follows the stage 3a rule: each run's mean P wake (P1 and P2 pooled) minus its mean N3
wake, then the mean of the runs and their spread, against 0.5 us and 2 us. Cores 4-7 are compared with the in-run null
N3 - N0. The largest-wake form (max P minus max N3 over the runs) and the same difference against N0 and N3 pooled are
reported but do not decide. CPU0 runs the NPU's owner, its interrupt and the producer, so its own busy-wake maximum in the
on and off sub-phases is the cost of that placement; CPU0's tick releases CPU5's wakes (release mask 0x21), so CPU5 is
reported apart. CPU2 is also judged without the wakes at the thermal service's 25 ms period, found from the run's own
thermal rows (run/thermal-readings.bin, apps/os/thermal.c) when the run directory holds them.

Beside the wakes: the NPU's job rate alone (streams off) and in the on sub-phases (streams on cores 4-7), its loss under
the streams, the stream rate of each sub-phase, and the setup, warm-up and drain times.

Usage: npu-judge.py RESULT.json [RESULT.json ...]
"""
import json
import statistics
import struct
import sys
from pathlib import Path

ON, OFF_AFTER, OFF_BEFORE = (1, 2), 3, 0
THERMAL_ROW = 56  # struct os_thermal_reading (apps/os/thermal-report.h)
THERMAL_PERIOD_US = 25000


def thermal_phase(run_dir):
    """The thermal reads' phase in microseconds modulo the period, from the run's readings buffer (rows, then count)."""
    path = run_dir / 'thermal-readings.bin'
    if not path.is_file():
        return None, 0
    data = path.read_bytes()
    rows = (len(data) - 16) // THERMAL_ROW
    count = struct.unpack_from('<I', data, rows * THERMAL_ROW)[0]
    times = [struct.unpack_from('<Q', data, i * THERMAL_ROW)[0] for i in range(min(count, rows))]
    return (statistics.median(t % THERMAL_PERIOD_US for t in times) if times else None), len(times)


def thermal_marks(sub, phase, frequency):
    """(k, i) of the first wake at or after each thermal read in the sub-phases."""
    if phase is None:
        return frozenset()
    return frozenset((k, i) for k, rows in enumerate(sub) for i, ready in enumerate(rows['ready'])
                     if (ready * 1e6 / frequency - phase) % THERMAL_PERIOD_US < 1000.0)


def pick(sub, parts, drop0=False, skip=frozenset()):
    return [us for k in parts for i, us in enumerate(sub[k]['us']) if not (drop0 and i == 0) and (k, i) not in skip]


def load(name):
    path = Path(name)
    decoded = json.loads(path.read_text())['decoded']
    npu = decoded.get('npu')
    if not npu:
        raise SystemExit('%s: no NPU section; not a ret-latency-npu result' % name)
    if npu['model']:
        raise SystemExit('%s: a QEMU model result; only board runs are judged' % name)
    phase, rows = thermal_phase(path.parent)
    return {'name': name, 'frequency': decoded['frequency'], 'npu': npu, 'wakes': npu['wakes'], 'phase': phase, 'rows': rows}


def zone(x):
    return 'under 0.5' if x < 0.5 else 'at least 2' if x >= 2 else 'between'


def spread(values):
    return '%+6.2f (%+.2f..%+.2f)' % (statistics.fmean(values), min(values), max(values))


def main():
    runs = [load(name) for name in sys.argv[1:]]
    if not runs:
        raise SystemExit(__doc__)
    print('runs: %d' % len(runs))
    for n, run in enumerate(runs, 1):
        npu = run['npu']
        print('run %d: NPU alone %.0f jobs/s (%d jobs, %.1f ms); on sub-phases %.0f jobs/s; loss %.1f%%' % (
            n, npu['alone']['jobs_per_second'], npu['alone']['jobs'], npu['alone']['seconds'] * 1e3,
            npu['loaded_jobs_per_second'], npu['loss'] * 100))
        print('  sub-phases: ' + ', '.join('%s%d %.0f jobs/s %.2f GB/s (%.1f ms)' % ('P' if s_on else 'N', k, s['jobs_per_second'],
                                         s['gb_per_second'], s['seconds'] * 1e3)
                                         for k, (s_on, s) in enumerate(zip(npu['on'], npu['subphases']))))
        print('  setup: open %.0f us, register %.0f us, close %.0f us; warm-up %s us; drain %s us; thermal phase %s (%d rows)' % (
            npu['open_us'], npu['register_us'], npu['close_us'], ', '.join('%.0f' % r['warm_us'] for r in npu['runs']),
            ', '.join('%.0f' % r['drain_us'] for r in npu['runs']),
            'n/a' if run['phase'] is None else '%.0f us' % run['phase'], run['rows']))
        early = sum(rows[k]['early'] for rows in run['wakes'] for k in range(4))
        if early:
            print('  early wakes (ready before their sub-phase state change): %d' % early)
    losses = [run['npu']['loss'] for run in runs]
    print('NPU loss under the streams: %s%%' % ', '.join('%.1f' % (x * 100) for x in losses) +
          ('; mean %.1f%%' % (statistics.fmean(losses) * 100) if len(losses) > 1 else ''))

    print('\nper sub-phase: max over runs / mean over runs (us); in-run nulls N3-N0 and P2-P1 (max, mean)')
    for core in range(8):
        cells, nulls = [], []
        for k in range(4):
            cells.append('%s%d %6.2f/%5.2f' % ('P' if k in ON else 'N', k, max(run['wakes'][core][k]['max_us'] for run in runs),
                                              statistics.fmean(run['wakes'][core][k]['mean_us'] for run in runs)))
        for a, b in ((0, 3), (1, 2)):
            dm = max(run['wakes'][core][b]['max_us'] for run in runs) - max(run['wakes'][core][a]['max_us'] for run in runs)
            dn = statistics.fmean(run['wakes'][core][b]['mean_us'] - run['wakes'][core][a]['mean_us'] for run in runs)
            nulls.append('%+.2f/%+.2f' % (dm, dn))
        note = '  (NPU core)' if core == 0 else '  (released by CPU0)' if core == 5 else ''
        print('CPU%d  %s   N3-N0 %s  P2-P1 %s%s' % (core, '  '.join(cells), nulls[0], nulls[1], note))

    print('\nCPU0 busy-wake maximum, the cost of the placement: on %.2f us, off %.2f us (max over runs)' % (
        max(max(run['wakes'][0][k]['max_us'] for k in ON) for run in runs),
        max(max(run['wakes'][0][k]['max_us'] for k in (OFF_BEFORE, OFF_AFTER)) for run in runs)))

    for label, drop0, thermal in (('all wakes', False, False), ('without wake 0', True, False),
                                  ('CPU2 without thermal-period wakes', False, True)):
        print('\n%s' % label)
        print('verdict on cores 1-3: mean(P1+P2) - mean(N3) per run, mean of runs (spread); cores 4-7 against the null N3-N0;')
        print('beside it, not deciding: the max form against N3, and the mean difference against N0 and N3 pooled')
        print('core  P-N3 mean (spread)        zone          | max: P  N3  P-N3     | P-Npooled mean (spread) | N3-N0 mean (spread)')
        for core in ([2] if thermal else (1, 2, 3, 4, 6, 7, 5)):
            d3, dp, d30, mp, mn3, excluded = [], [], [], [], [], []
            for run in runs:
                sub = run['wakes'][core]
                skip = thermal_marks(sub, run['phase'], run['frequency']) if thermal else frozenset()
                excluded.append(len(skip))
                p, n3, n0 = pick(sub, ON, drop0, skip), pick(sub, (OFF_AFTER,), drop0, skip), pick(sub, (OFF_BEFORE,), drop0, skip)
                d3.append(statistics.fmean(p) - statistics.fmean(n3))
                dp.append(statistics.fmean(p) - statistics.fmean(n0 + n3))
                d30.append(statistics.fmean(n3) - statistics.fmean(n0))
                mp.append(max(p))
                mn3.append(max(n3))
            if core <= 3:
                verdict = zone(statistics.fmean(d3))
            else:
                inside = min(d30) - 1e-9 <= statistics.fmean(d3) <= max(d30) + 1e-9 if len(runs) > 1 else None
                verdict = ('inside null' if inside else 'outside null') if inside is not None else 'one run'
            print('CPU%d  %s  %-12s | %6.2f %6.2f %+6.2f | %s | %s%s%s' % (
                core, spread(d3), verdict, max(mp), max(mn3), max(mp) - max(mn3), spread(dp), spread(d30),
                '  (apart: released by CPU0)' if core == 5 else '', ('  excluded %s' % excluded) if thermal else ''))


if __name__ == '__main__':
    main()
