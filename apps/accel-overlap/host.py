#!/usr/bin/env python3
"""Decode the result of the accel-overlap application (result.h, struct accel_overlap_result) and compare it with the ranges the
run's hypothesis predicted (docs/designs/concurrent-accel.md). This module never opens a board."""
import struct

MAGIC = 0x4c564f41
VERSION = 1
BYTES = 1200
PHASES = 5
TICKS_PER_US = 24
NAMES = ('gpu-alone', 'npu-alone', 'gpu-npu', 'gpu-npu-stream', 'pll-cycles')
DEVICES = (1, 2, 3, 7, 15)
BUDGET_US = {'gpu': 1000.0, 'npu': 1000.0}


def us(ticks):
    return None if ticks == 0xffffffff else round(ticks / TICKS_PER_US, 3)


def distribution(raw, at):
    count, missing, overflow, low, p50, p99, p999, high = struct.unpack_from('<8I', raw, at)
    return {'count': count, 'missing': missing, 'overflow': overflow, 'min_us': us(low) if count + overflow else None,
            'p50_us': us(p50), 'p99_us': us(p99), 'p999_us': us(p999), 'max_us': us(high)}


def device(raw, at):
    serial, pipelined, depth, mismatches, ticks = struct.unpack_from('<4IQ', raw, at + 64)
    return {'wake': distribution(raw, at), 'trip': distribution(raw, at + 32), 'serial_jobs': serial,
            'pipelined_jobs': pipelined, 'depth': depth, 'mismatches': mismatches,
            'jobs_per_second': round(pipelined * TICKS_PER_US * 1e6 / ticks, 1) if ticks else None}


def decode(raw):
    if len(raw) != BYTES:
        raise ValueError('The result must hold exactly %d bytes (a failure record, or another image).' % BYTES)
    magic, version, size, phases, clock0, clock1 = struct.unpack_from('<4I2Q', raw, 0)
    if (magic, version, size, phases) != (MAGIC, VERSION, BYTES, PHASES):
        raise ValueError('The result header differs (a failure record, or another image).')
    initial_top, final_top, workers, worker_bytes = struct.unpack_from('<4I', raw, 32)
    (reference, diff, last, unlocked, writes, in_flight, completions, first_site, first_word, cycles, cycle_mismatches,
     close_ticks, open_ticks) = struct.unpack_from('<4I3Q4I2Q', raw, 48)
    pll = {'reference': '0x%08x' % reference, 'diff': '0x%08x' % diff, 'last': '0x%08x' % last, 'unlocked': unlocked,
           'writes': writes, 'writes_in_flight': in_flight, 'completion_reads': completions, 'first_site': first_site,
           'first_word': '0x%08x' % first_word, 'cycles': cycles, 'cycle_mismatches': cycle_mismatches,
           'close_ms_max': round(close_ticks / TICKS_PER_US / 1000, 3), 'open_ms_max': round(open_ticks / TICKS_PER_US / 1000, 3)}
    result = {'clock': {'cpu_l_mhz': clock0 & 0xffff, 'cpu_b_mhz': clock0 >> 16 & 0xffff, 'cpu_l_mv': clock1 & 0xffff,
                        'cpu_b_mv': clock1 >> 16 & 0xffff, 'dsu_mhz': clock1 >> 32 & 0xffff},
              'initial_top': initial_top, 'final_top': final_top, 'stream_workers': workers,
              'stream_bytes_per_worker': worker_bytes, 'pll': pll, 'phases': []}
    for i in range(PHASES):
        at = 120 + 216 * i
        devices, throttled = struct.unpack_from('<2I', raw, at)
        overlap, ticks, sweeps, stream_ticks = struct.unpack_from('<4Q', raw, at + 184)
        if devices != DEVICES[i]:
            raise ValueError('Phase %d ran devices 0x%x, not 0x%x.' % (i, devices, DEVICES[i]))
        phase = {'name': NAMES[i], 'devices': devices, 'throttled': throttled, 'gpu': device(raw, at + 8),
                 'npu': device(raw, at + 96), 'overlap': overlap, 'ms': round(ticks / TICKS_PER_US / 1000, 3),
                 'stream_sweeps': sweeps,
                 'stream_gb_per_second': round(sweeps * worker_bytes * 2 * TICKS_PER_US * 1e6 / stream_ticks / 1e9, 2)
                 if stream_ticks else None}
        result['phases'].append(phase)
    result['checks'] = checks(result)
    result['in_range'] = all(c['ok'] for c in result['checks'])
    return result


def checks(r):
    """The hypothesis ranges of the stage-1 run (docs/designs/concurrent-accel.md, "Stage 1: predictions")."""
    out = []

    def check(name, value, low=None, high=None):
        ok = value is not None and (low is None or value >= low) and (high is None or value <= high)
        out.append({'name': name, 'value': value, 'low': low, 'high': high, 'ok': ok})

    p = r['phases']
    wake = lambda i, d, k: p[i][d]['wake'][k]
    check('gpu wake alone p50 us', wake(0, 'gpu', 'p50_us'), 2.5, 4)
    check('gpu wake alone p99 us', wake(0, 'gpu', 'p99_us'), 8, 15)
    check('gpu wake alone max us', wake(0, 'gpu', 'max_us'), None, 150)
    check('npu wake alone p50 us', wake(1, 'npu', 'p50_us'), 1.5, 4)
    check('npu wake alone p99 us', wake(1, 'npu', 'p99_us'), 4, 12)
    check('npu wake alone max us', wake(1, 'npu', 'max_us'), 10, 80)
    for d, alone in (('gpu', 0), ('npu', 1)):
        base50, base_max = wake(alone, d, 'p50_us'), wake(alone, d, 'max_us')
        delta = lambda i, k, base: None if wake(i, d, k) is None or base is None else round(wake(i, d, k) - base, 3)
        check('%s wake both p50 minus alone us' % d, delta(2, 'p50_us', base50), -1, 1)
        check('%s wake both max minus alone us' % d, delta(2, 'max_us', base_max), None, 30)
        check('%s wake streaming p50 minus alone us' % d, delta(3, 'p50_us', base50), 0.5, 3)
        check('%s wake streaming max us' % d, wake(3, d, 'max_us'), 40, 250)
        for i in (2, 3):
            check('%s wake %s max within budget us' % (d, p[i]['name']), wake(i, d, 'max_us'), None, BUDGET_US[d])
            check('%s wake %s overflow' % (d, p[i]['name']), p[i][d]['wake']['overflow'], 0, 0)
    for d, alone, low, high in (('gpu', 0, 0, 15), ('npu', 1, 5, 30)):
        base, both = p[alone][d]['jobs_per_second'], p[3][d]['jobs_per_second']
        loss = None if not base or both is None else round(100 * (1 - both / base), 2)
        check('%s throughput streaming loss percent' % d, loss, low, high)
    check('npu completions with a gpu job in flight', p[2]['overlap'] + p[3]['overlap'], 1000)
    check('mismatches', sum(ph[d]['mismatches'] for ph in p for d in ('gpu', 'npu')) + r['pll']['cycle_mismatches'], 0, 0)
    check('pll cycles', r['pll']['cycles'], 3, 3)
    check('pll_npu unlocked reads', r['pll']['unlocked'], 0, 0)
    check('pll_npu first differing site', r['pll']['first_site'], 0, 0)
    # 14 writes for each close and reopen (4 off, 10 on: drivers/gpu/a733_gpu_power.c), and the 10 of the first power-on when
    # the ring opened before it.
    check('pll_gpu0 writes seen', r['pll']['writes'], 42, 52)
    check('pll_gpu0 writes with an npu job in flight', r['pll']['writes_in_flight'], 21)
    check('pll_npu reads at completion', r['pll']['completion_reads'], 1)
    check('throttled phases', sum(1 for ph in p if ph['throttled']), 0, 0)
    check('top point at start and end', r['initial_top'] + r['final_top'], 2, 2)
    return out


if __name__ == '__main__':
    import json
    import sys
    print(json.dumps(decode(open(sys.argv[1], 'rb').read()), indent=2))
