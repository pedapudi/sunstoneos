#!/usr/bin/env python3
"""Decode the result of the accel-overlap application (result.h: struct accel_overlap_result, or struct
accel_overlap_sweep_result from the streaming sweep build) and compare it with the ranges the run's hypothesis predicted
(docs/designs/concurrent-accel.md). This module never opens a board."""
import struct

MAGIC = 0x4c564f41
VERSION = 1
BYTES = 1200
SWEEP_VERSION = 2
SWEEP_BYTES = 1488
LARGE_VERSION = 3
# The four-elements-per-lane GPU run (ACCEL_OVERLAP_WIDTH): the larger-job layout with only phase 0, the GPU alone.
WIDTH_VERSION = 4
LARGE_BYTES = 2944
# The NSI counter calibration (ACCEL_OVERLAP_COUNTERS): the larger-job layout with four phases, followed by the counter
# record (result.h: struct accel_overlap_counter_result).
COUNTERS_VERSION = 5
COUNTERS_BYTES = 10088
COUNTER_NAMES = ('idle', 'gpu-alone', 'npu-alone', 'core-2', 'gpu-window', 'npu-window')
COUNTER_DEVICES = (0, 1, 2, 4, 1, 2)
COUNTER_MODES = (0, 0, 0, 0x01, 0, 0)
COUNTED_PHASES = 4       # counted with CYCLE 0xffffffff on both ports; the last two phases are the window probes
WINDOW_CYCLES = 60000000 # the probes' period: 100 ms at the 600 MHz NSI clock
COUNTER_WORDS = ('rq_rd', 'rq_wr', 'dt_rd', 'dt_wr', 'la_rd', 'la_wr')
COUNTER_PORTS = (6, 18)
DATA_UNIT = 16           # bytes per data count of an initiator port (sun60iw2p1.dtsi ia_pmu_data_unit)
LARGE_PHASES = 9
LARGE_NAMES = ('gpu-alone', 'npu-alone', 'none', 'core-2', 'core-7', 'cores-2-4', 'cores-2-5-7', 'core-2-x3', 'core-7-x3')
LARGE_DEVICES = (1, 2, 3, 7, 7, 7, 7, 7, 7)
LARGE_MODES = (0, 0, 0, 0x01, 0x10, 0x07, 0x1f, 0x101, 0x110)
# Stage-1b losses in percent (docs/evidence/latency.jsonl, entry 1794709286170370328-b5d81897): small jobs, both devices.
STAGE_1B_LOSS = {'core-2': {'gpu': 25.4, 'npu': 11.42}, 'core-7': {'gpu': 32.54, 'npu': 29.26},
                 'cores-2-4': {'gpu': 51.12, 'npu': 48.07}, 'cores-2-5-7': {'gpu': 51.09, 'npu': 53.01}}
PHASES = 5
TICKS_PER_US = 24
NAMES = ('gpu-alone', 'npu-alone', 'gpu-npu', 'gpu-npu-stream', 'pll-cycles')
DEVICES = (1, 2, 3, 7, 15)
BUDGET_US = {'gpu': 1000.0, 'npu': 1000.0}
# The sweep's streaming sets (bit w: the worker on core 2, 3, 4, 5, 7 for w = 0..4).
SWEEP_NAMES = ('none', 'core-2', 'cores-2-4', 'cores-2-5-7', 'core-7')
SWEEP_MASKS = (0x00, 0x01, 0x07, 0x1f, 0x10)
# The ceiling for the arbitration rule: the most DRAM traffic one stream has moved, one A76 copy reading and writing 11.7 GB/s
# (docs/evidence/latency.jsonl, the bandwidth records the stage-1b hypothesis cites).
CEILING_GB_PER_SECOND = 11.7


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
    if len(raw) == LARGE_BYTES:
        return decode_large(raw)
    if len(raw) == COUNTERS_BYTES:
        return decode_counters(raw)
    if len(raw) not in (BYTES, SWEEP_BYTES):
        raise ValueError('The result must hold exactly %d or %d bytes (a failure record, or another image).' %
                         (BYTES, SWEEP_BYTES))
    magic, version, size, phases, clock0, clock1 = struct.unpack_from('<4I2Q', raw, 0)
    sweep = len(raw) == SWEEP_BYTES
    if (magic, version, size, phases) != (MAGIC, SWEEP_VERSION if sweep else VERSION, len(raw), PHASES):
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
        expected = (3 | (4 if SWEEP_MASKS[i] else 0)) if sweep else DEVICES[i]
        if devices != expected:
            raise ValueError('Phase %d ran devices 0x%x, not 0x%x.' % (i, devices, expected))
        phase = {'name': SWEEP_NAMES[i] if sweep else NAMES[i], 'devices': devices, 'throttled': throttled, 'gpu': device(raw, at + 8),
                 'npu': device(raw, at + 96), 'overlap': overlap, 'ms': round(ticks / TICKS_PER_US / 1000, 3),
                 'stream_sweeps': sweeps,
                 'stream_gb_per_second': round(sweeps * worker_bytes * 2 * TICKS_PER_US * 1e6 / stream_ticks / 1e9, 2)
                 if stream_ticks else None}
        result['phases'].append(phase)
    if sweep:
        return decode_sweep(raw, result)
    result['checks'] = checks(result)
    result['in_range'] = all(c['ok'] for c in result['checks'])
    return result


def decode_sweep(raw, result):
    (result['chase_lines'],) = struct.unpack_from('<Q', raw, 1200)
    for i, phase in enumerate(result['phases']):
        at = 1208 + 56 * i
        mask, per_sample = struct.unpack_from('<2I', raw, at)
        loads, ticks = struct.unpack_from('<2Q', raw, at + 40)
        if mask != SWEEP_MASKS[i]:
            raise ValueError('Phase %d streamed with set 0x%x, not 0x%x.' % (i, mask, SWEEP_MASKS[i]))
        chase = distribution(raw, at + 8)
        ns = lambda k: None if chase[k] is None or not per_sample else round(chase[k] * 1000 / per_sample, 1)
        phase['stream_mask'] = mask
        phase['chase'] = {'samples': chase['count'], 'overflow': chase['overflow'], 'loads_per_sample': per_sample,
                          'loads': loads, 'p50_ns': ns('p50_us'), 'p99_ns': ns('p99_us'), 'max_ns': ns('max_us'),
                          'mean_ns': round(ticks * 1000 / TICKS_PER_US / loads, 1) if loads else None}
        if not mask:
            phase['stream_gb_per_second'] = 0.0
    result['sweep'] = sweep_ratios(result['phases'])
    result['checks'] = sweep_checks(result)
    result['in_range'] = all(c['ok'] for c in result['checks'])
    result['decision'] = sweep_decision(result)
    return result


def sweep_ratios(p):
    """Each phase against the phase without streaming: the job-time ratio of each device (the rate without streaming over the
    rate in the phase), its loss in percent, and the chase median's ratio."""
    out = []
    base = p[0]
    for ph in p:
        row = {'name': ph['name'], 'stream_gb_per_second': ph['stream_gb_per_second']}
        for d in ('gpu', 'npu'):
            b, r = base[d]['jobs_per_second'], ph[d]['jobs_per_second']
            row[d + '_job_time_ratio'] = round(b / r, 3) if b and r else None
            row[d + '_loss_percent'] = round(100 * (1 - r / b), 2) if b and r is not None else None
        b, c = base['chase']['p50_ns'], ph['chase']['p50_ns']
        row['chase_ratio'] = round(c / b, 3) if b and c else None
        for d in ('gpu', 'npu'):
            j = row[d + '_job_time_ratio']
            # How far the device's slowdown is from the loaded DRAM latency's: 0 when it tracks the chase exactly.
            row[d + '_over_chase'] = round(j / row['chase_ratio'] - 1, 3) if j and row['chase_ratio'] else None
        out.append(row)
    return out


def sweep_checks(r):
    """The ranges of the stage-1b hypothesis (docs/designs/concurrent-accel.md, "Stage 1b")."""
    out = []

    def check(name, value, low=None, high=None):
        ok = value is not None and (low is None or value >= low) and (high is None or value <= high)
        out.append({'name': name, 'value': value, 'low': low, 'high': high, 'ok': ok})

    p, s = r['phases'], r['sweep']
    for i, low, high in ((1, 3, 5), (2, 5, 7), (3, 6.0, 7.0), (4, 7, 10)):
        check('stream gb/s %s' % p[i]['name'], p[i]['stream_gb_per_second'], low, high)
    for i, low, high in ((1, 10, 30), (2, 35, 55), (3, 48, 60), (4, 40, 65)):
        check('npu loss percent %s' % p[i]['name'], s[i]['npu_loss_percent'], low, high)
        g, n = s[i]['gpu_loss_percent'], s[i]['npu_loss_percent']
        check('gpu loss minus npu loss points %s' % p[i]['name'], None if g is None or n is None else round(g - n, 2), -15, 15)
    # The chase median rises with the streaming bandwidth: in bandwidth order, each streaming phase's median is above the
    # previous one's, the first above the phase without streaming.
    order = sorted(range(1, PHASES), key=lambda i: p[i]['stream_gb_per_second'] or 0)
    medians = [p[0]['chase']['p50_ns']] + [p[i]['chase']['p50_ns'] for i in order]
    check('chase median rises with bandwidth', None if None in medians else
          int(all(a < b for a, b in zip(medians, medians[1:]))), 1, 1)
    for i in range(1, PHASES):
        check('npu job time over chase ratio %s' % p[i]['name'], s[i]['npu_over_chase'], -0.25, 0.25)
    for i in range(PHASES):
        check('chase samples %s' % p[i]['name'], p[i]['chase']['samples'], 1000)
        check('chase overflow %s' % p[i]['name'], p[i]['chase']['overflow'], 0, 0)
        check('npu completions with a gpu job in flight %s' % p[i]['name'], p[i]['overlap'], 1000)
        for d in ('gpu', 'npu'):
            check('%s pipelined jobs %s' % (d, p[i]['name']), p[i][d]['pipelined_jobs'], 1000)
    check('chase cycle lines', r['chase_lines'], 262144, 262144)
    check('phase longest ms', max(ph['ms'] for ph in p), None, 3000)
    check('mismatches', sum(ph[d]['mismatches'] for ph in p for d in ('gpu', 'npu')), 0, 0)
    check('pll_npu unlocked reads', r['pll']['unlocked'], 0, 0)
    check('pll_npu first differing site', r['pll']['first_site'], 0, 0)
    check('throttled phases', sum(1 for ph in p if ph['throttled']), 0, 0)
    check('top point at start and end', r['initial_top'] + r['final_top'], 2, 2)
    return out


def sweep_decision(r):
    """The stage-1b decision rule. Saturation: the cores 2-4 and stage-1 streaming totals are within 15% of the larger, and
    every streaming phase's job time ratio (both devices) is within 25% of its chase ratio. Arbitration: a device's job time
    ratio is more than 1.5 times its chase ratio in a phase, or core 2 alone costs a device more than 30% while it streams
    less than half the ceiling."""
    p, s = r['phases'], r['sweep']
    a, b = p[2]['stream_gb_per_second'], p[3]['stream_gb_per_second']
    totals_close = bool(a and b and abs(a - b) <= 0.15 * max(a, b))
    tracks = all(s[i][d + '_over_chase'] is not None and abs(s[i][d + '_over_chase']) <= 0.25
                 for i in range(1, PHASES) for d in ('gpu', 'npu'))
    exceeds = [(p[i]['name'], d) for i in range(1, PHASES) for d in ('gpu', 'npu')
               if s[i][d + '_over_chase'] is not None and s[i][d + '_over_chase'] > 0.5]
    core2 = [d for d in ('gpu', 'npu') if (s[1][d + '_loss_percent'] or 0) > 30 and
             (p[1]['stream_gb_per_second'] or 0) < CEILING_GB_PER_SECOND / 2]
    verdict = 'arbitration' if exceeds or core2 else 'saturation' if totals_close and tracks else 'neither'
    return {'verdict': verdict, 'totals_within_15_percent': totals_close, 'all_track_chase_within_25_percent': tracks,
            'over_chase_by_more_than_50_percent': exceeds, 'core_2_costs_over_30_percent': core2,
            'ceiling_gb_per_second': CEILING_GB_PER_SECOND}


def decode_large(raw, counters=False):
    """The larger-job sweep (struct accel_overlap_large_result), or the first part of the counter calibration."""
    magic, version, size, phases, clock0, clock1 = struct.unpack_from('<4I2Q', raw, 0)
    if counters:
        if (magic, version, size, phases) != (MAGIC, COUNTERS_VERSION, COUNTERS_BYTES, len(COUNTER_NAMES)):
            raise ValueError('The result header differs (a failure record, or another image).')
    elif (magic, size, phases) != (MAGIC, LARGE_BYTES, LARGE_PHASES) or version not in (LARGE_VERSION, WIDTH_VERSION):
        raise ValueError('The result header differs (a failure record, or another image).')
    width = version == WIDTH_VERSION
    names, devices_expected, modes_expected = ((COUNTER_NAMES, COUNTER_DEVICES, COUNTER_MODES) if counters else
                                               (LARGE_NAMES, LARGE_DEVICES, LARGE_MODES))
    initial_top, final_top, workers, worker_bytes = struct.unpack_from('<4I', raw, 32)
    (reference, diff, last, unlocked, writes, in_flight, completions, first_site, first_word, cycles, cycle_mismatches,
     close_ticks, open_ticks) = struct.unpack_from('<4I3Q4I2Q', raw, 48)
    (chase_lines,) = struct.unpack_from('<Q', raw, 120)
    gpu_elements, npu_bytes = struct.unpack_from('<2I', raw, 128)
    r = {'clock': {'cpu_l_mhz': clock0 & 0xffff, 'cpu_b_mhz': clock0 >> 16 & 0xffff, 'cpu_l_mv': clock1 & 0xffff,
                   'cpu_b_mv': clock1 >> 16 & 0xffff, 'dsu_mhz': clock1 >> 32 & 0xffff},
         'initial_top': initial_top, 'final_top': final_top, 'stream_workers': workers,
         'stream_bytes_per_worker': worker_bytes, 'chase_lines': chase_lines, 'gpu_elements': gpu_elements,
         'npu_tensor_bytes': npu_bytes,
         'pll': {'reference': '0x%08x' % reference, 'last': '0x%08x' % last, 'unlocked': unlocked, 'writes': writes,
                 'first_site': first_site, 'first_word': '0x%08x' % first_word}, 'phases': []}
    for i in range(1 if width else len(names)):
        at = 136 + 312 * i
        if counters and not any(raw[at:at + 312]):
            break   # a counter call failed in an earlier phase, so this one did not run
        devices, throttled = struct.unpack_from('<2I', raw, at)
        overlap, ticks, sweeps, stream_ticks = struct.unpack_from('<4Q', raw, at + 184)
        mask, per_sample = struct.unpack_from('<2I', raw, at + 216)
        loads, chase_ticks = struct.unpack_from('<2Q', raw, at + 256)
        mode, slot_jobs = struct.unpack_from('<2I', raw, at + 272)
        plain_sum, plain_max, synced_sum, synced_max = struct.unpack_from('<4Q', raw, at + 280)
        if devices != devices_expected[i] or mode != modes_expected[i] or mask != (mode & 0xff):
            raise ValueError('Phase %d ran devices 0x%x with streaming 0x%x, not 0x%x with 0x%x.' %
                             (i, devices, mode, devices_expected[i], modes_expected[i]))
        chase = distribution(raw, at + 224)
        ns = lambda k: None if chase[k] is None or not per_sample else round(chase[k] * 1000 / per_sample, 1)
        phase = {'name': names[i], 'devices': devices, 'stream_mode': mode, 'throttled': throttled,
                 'gpu': device(raw, at + 8), 'npu': device(raw, at + 96), 'overlap': overlap,
                 'ms': round(ticks / TICKS_PER_US / 1000, 3), 'stream_sweeps': sweeps,
                 'stream_gb_per_second': round(sweeps * worker_bytes * 2 * TICKS_PER_US * 1e6 / stream_ticks / 1e9, 2)
                 if stream_ticks else 0.0,
                 'chase': {'samples': chase['count'], 'overflow': chase['overflow'], 'p50_ns': ns('p50_us'),
                           'p99_ns': ns('p99_us'), 'mean_ns': round(chase_ticks * 1000 / TICKS_PER_US / loads, 1) if loads else None},
                 'slot_jobs': slot_jobs}
        if slot_jobs:
            phase['gpu_slot_job_us'] = round(plain_sum / slot_jobs / TICKS_PER_US, 1)
            phase['gpu_slot_job_synced_us'] = round(synced_sum / slot_jobs / TICKS_PER_US, 1)
            phase['gpu_output_maintenance_us'] = round((synced_sum - plain_sum) / slot_jobs / TICKS_PER_US, 1)
            phase['gpu_output_maintenance_max_us'] = round((synced_max - plain_max) / TICKS_PER_US, 1)
        r['phases'].append(phase)
    if counters:
        if any(raw[136 + 312 * len(names):LARGE_BYTES]):
            raise ValueError('The counter run wrote larger-job phases after its fourth.')
        return r
    if width:
        if any(raw[136 + 312:]):
            raise ValueError('The width run wrote phases after the GPU-alone phase.')
        r['checks'] = width_checks(r)
        r['in_range'] = all(c['ok'] for c in r['checks'])
        return r
    r['losses'] = large_losses(r['phases'])
    r['checks'] = large_checks(r)
    r['in_range'] = all(c['ok'] for c in r['checks'])
    r['decision'] = large_decision(r)
    return r


def window_model(samples, window_ticks):
    """What the counters did across period ends, from one probe's samples of the data counts (DT_RD + DT_WR):
    'zero' (never counted), 'latched' (held still between steps about one period apart: each step shows the last
    period's count), 'restarts' (rose at almost every sample and fell back at each period end: each period's count is
    the peak before a fall), 'stopped' (rose, then held still for more than one and a half periods) or 'free' (rose
    through every period end)."""
    v = [s['count'][2] + s['count'][3] for s in samples]
    t = [s['ticks'] for s in samples]
    out = {'model': None, 'samples': len(v), 'period_ms': None, 'period_units': None, 'period_counts': None}
    if len(v) < 8:
        return out
    if not any(v):
        out['model'] = 'zero'
        return out
    falls = [i for i in range(1, len(v)) if v[i] < v[i - 1]]
    changes = [i for i in range(1, len(v)) if v[i] != v[i - 1]]
    spacing = lambda idx: sorted(t[b] - t[a] for a, b in zip(idx, idx[1:]))
    median = lambda xs: xs[len(xs) // 2] if xs else None
    # A latched count changes only at period ends, a few times in the probe and several samples apart, and may rise or
    # fall at each change; a restarting count changes at almost every sample and falls at each period end; a stopped
    # count changes at consecutive samples and then not at all.
    sample_gap = median(sorted(b - a for a, b in zip(t, t[1:])))
    if len(changes) <= len(v) // 4 and (len(changes) < 2 or median(spacing(changes)) >= 4 * sample_gap):
        out['model'] = 'latched'
        gaps = spacing(changes)
        ends = changes
    elif falls:
        out['model'] = 'restarts'
        gaps = spacing(falls)
        peaks = [i - 1 for i in falls[1:]]   # the first period started at the probe's begin, after the clear
        ends = peaks or [falls[0] - 1]
    else:
        flat = len(v) - 1 - changes[-1]
        if t[-1] - t[changes[-1]] > 1.5 * window_ticks and flat:
            out['model'] = 'stopped'
            out['period_ms'] = round(t[changes[-1]] / TICKS_PER_US / 1000, 1)
            return out
        out['model'] = 'free'
        return out
    gap = median(gaps) if gaps else t[(falls or changes)[0]]   # one period end seen: the first period began at 0
    out['period_ms'] = round(gap / TICKS_PER_US / 1000, 1) if gap else None
    pick = lambda w: sorted(samples[i]['count'][w] for i in ends)
    out['period_counts'] = {w: median(pick(k)) for k, w in enumerate(('rq_rd', 'rq_wr', 'dt_rd', 'dt_wr'))}
    out['period_units'] = median(sorted(v[i] for i in ends))
    return out


def decode_counters(raw):
    """The NSI counter calibration (struct accel_overlap_counter_result). A record whose counter call failed decodes too:
    the phases after the failure did not run, and the counter record still names the status and the failing address."""
    r = decode_large(raw, counters=True)
    at = LARGE_BYTES
    open_status, close_status, fail_address, fail_value = struct.unpack_from('<4I', raw, at)
    reset = struct.unpack_from('<18I', raw, at + 16)
    r['counters'] = {'open_status': open_status, 'close_status': close_status, 'fail_address': '0x%08x' % fail_address,
                     'fail_value': '0x%08x' % fail_value, 'phases_run': len(r['phases']),
                     'reset': {'port_%d' % port: ['0x%08x' % w for w in reset[9 * b:9 * b + 9]]
                               for b, port in enumerate(COUNTER_PORTS)}}
    for i, phase in enumerate(r['phases'][:COUNTED_PHASES]):
        base = at + 16 + 72 + 184 * i
        status, clear0, clear1, _ = struct.unpack_from('<4I', raw, base)
        live = struct.unpack_from('<12I', raw, base + 16)
        value = struct.unpack_from('<12I', raw, base + 64)
        held = struct.unpack_from('<12I', raw, base + 112)
        begin, live_at, end = struct.unpack_from('<3Q', raw, base + 160)
        c = {'status': status, 'clear_readback': ['0x%08x' % clear0, '0x%08x' % clear1],
             'ms': round((end - begin) / TICKS_PER_US / 1000, 3) if end > begin else None,
             'live_at_fraction': round((live_at - begin) / (end - begin), 3) if live_at and end > begin else None}
        for b, port in enumerate(COUNTER_PORTS):
            c['port_%d' % port] = {
                'value': dict(zip(COUNTER_WORDS, value[6 * b:6 * b + 6])),
                'live': dict(zip(COUNTER_WORDS, live[6 * b:6 * b + 6])),
                'held': dict(zip(COUNTER_WORDS, held[6 * b:6 * b + 6]))}
        phase['counters'] = c
    for k, phase in enumerate(r['phases'][COUNTED_PHASES:]):
        base = at + 16 + 72 + 184 * COUNTED_PHASES + 3160 * k
        status, bank, cycle, count, clear, _ = struct.unpack_from('<6I', raw, base)
        begin, end = struct.unpack_from('<2Q', raw, base + 24)
        value = struct.unpack_from('<6I', raw, base + 40)
        held = struct.unpack_from('<6I', raw, base + 64)
        samples = [{'ticks': x[0], 'jobs': x[1], 'count': x[2:6]}
                   for x in (struct.unpack_from('<6I', raw, base + 88 + 24 * n) for n in range(min(count, 128)))]
        window_ticks = cycle * TICKS_PER_US / 600 if cycle else None
        q = {'status': status, 'port': COUNTER_PORTS[bank] if bank < 2 else None, 'cycle': cycle,
             'clear_readback': '0x%08x' % clear, 'ms': round((end - begin) / TICKS_PER_US / 1000, 3) if end > begin else None,
             'value': dict(zip(COUNTER_WORDS, value)), 'held': dict(zip(COUNTER_WORDS, held)),
             'series': [[s['ticks'], s['jobs']] + list(s['count']) for s in samples]}
        q.update(window_model(samples, window_ticks or 2400000))
        if len(samples) >= 2 and samples[-1]['ticks'] > samples[0]['ticks']:
            q['jobs_per_second'] = round((samples[-1]['jobs'] - samples[0]['jobs']) * TICKS_PER_US * 1e6 /
                                         (samples[-1]['ticks'] - samples[0]['ticks']), 1)
        if q['period_counts'] and q['period_ms'] and q.get('jobs_per_second'):
            jobs = q['jobs_per_second'] * q['period_ms'] / 1000
            q['per_job'] = {w: round(n / jobs, 1) for w, n in q['period_counts'].items()}
        phase['probe'] = q
    r['calibration'] = calibration(r)
    r['checks'] = counter_checks(r)
    r['in_range'] = all(c['ok'] for c in r['checks'])
    return r


def calibration(r):
    """Per-job counts. With a model in which a count can be read before its period ends ('restarts' or 'free'), the
    counted phases give them over one second; with 'latched', the probes give them from one period's count."""
    p = r['phases']
    probes = [ph['probe'] for ph in p[COUNTED_PHASES:]]
    models = sorted({q['model'] for q in probes if q['model']})
    out = {'probe_models': models, 'source': None,
           'gpu_known_read_units_per_job': r['gpu_elements'] * 8 // DATA_UNIT,
           'gpu_known_write_units_per_job': r['gpu_elements'] * 4 // DATA_UNIT,
           'npu_known_units_per_job': r['npu_tensor_bytes'] // DATA_UNIT}
    per = lambda v, jobs: round(v / jobs, 1) if jobs else None
    if len(p) >= 3 and models and set(models) <= {'restarts', 'free'}:
        gpu, npu = p[1], p[2]
        gpu_jobs = gpu['gpu']['pipelined_jobs'] + 2 * gpu['slot_jobs']   # each slot ran a plain and a synced job
        npu_jobs = npu['npu']['pipelined_jobs']
        g, n = gpu['counters']['port_6']['value'], npu['counters']['port_18']['value']
        out.update(source='counted phases', gpu_jobs=gpu_jobs, npu_jobs=npu_jobs,
                   gpu={w: per(g[w], gpu_jobs) for w in COUNTER_WORDS[:4]},
                   npu={w: per(n[w], npu_jobs) for w in COUNTER_WORDS[:4]},
                   gpu_la_rd_per_request=round(g['la_rd'] / g['rq_rd'], 2) if g['rq_rd'] else None,
                   npu_la_rd_per_request=round(n['la_rd'] / n['rq_rd'], 2) if n['rq_rd'] else None)
    elif len(probes) == 2 and models == ['latched'] and all(q.get('per_job') for q in probes):
        out.update(source='window probes', gpu=probes[0]['per_job'], npu=probes[1]['per_job'])
    if out.get('gpu'):
        q = out['gpu']['rq_rd'] + out['gpu']['rq_wr'] if None not in (out['gpu']['rq_rd'], out['gpu']['rq_wr']) else None
        out['gpu_requests_per_job'] = q
        out['gpu_discriminator'] = (None if q is None else 'line requests: few resident tasks' if q < 100000 else
                                    'per-lane requests: a per-request limit' if q > 200000 else 'undecided')
    return out


def counter_checks(r):
    """The ranges of the NSI counter hypothesis (docs/designs/concurrent-accel.md, "NSI counters")."""
    out = []

    def check(name, value, low=None, high=None):
        ok = value is not None and (low is None or value >= low) and (high is None or value <= high)
        out.append({'name': name, 'value': value, 'low': low, 'high': high, 'ok': ok})

    k, p, cal = r['counters'], r['phases'], r['calibration']
    check('counter open status', k['open_status'], 0, 0)
    check('phases run', k['phases_run'], len(COUNTER_NAMES), len(COUNTER_NAMES))
    check('counter close status', k['close_status'], 0, 0)
    check('counter phase and probe status', sum(1 for ph in p if (ph.get('counters') or ph.get('probe'))['status']), 0, 0)
    check('reset enable bits set', sum(int(k['reset']['port_%d' % port][0], 16) & 1 for port in COUNTER_PORTS), 0, 0)
    if len(p) < len(COUNTER_NAMES):
        return out
    probes = [ph['probe'] for ph in p[COUNTED_PHASES:]]
    check('probe samples', min(q['samples'] for q in probes), 100, 128)
    check('probes agree on one model', len(cal['probe_models']) == 1 or None)
    check('probe model counts', cal['probe_models'][0] in ('restarts', 'latched', 'free') if cal['probe_models'] else None)
    if cal['probe_models'] and cal['probe_models'][0] in ('restarts', 'latched'):
        for q in probes:   # the period follows CYCLE at the 600 MHz NSI clock: 100 ms
            check('port %d probe period ms' % q['port'], q['period_ms'], 90, 110)
    counted = cal['source'] == 'counted phases'
    if counted:
        # The counters hold after the disable.
        check('counts that moved after the disable', sum(
            1 for ph in p[:COUNTED_PHASES] for port in COUNTER_PORTS for w in COUNTER_WORDS
            if ph['counters']['port_%d' % port]['held'][w] != ph['counters']['port_%d' % port]['value'][w]), 0, 0)
        # Live counting: the data sample at half the phase, on the port of the phase's device.
        for i, port in ((1, 6), (2, 18)):
            c = p[i]['counters']['port_%d' % port]
            total = c['value']['dt_rd'] + c['value']['dt_wr']
            check('%s port %d live share at half' % (p[i]['name'], port),
                  round((c['live']['dt_rd'] + c['live']['dt_wr']) / total, 3) if total else None, 0.35, 0.65)
        # Controls. Idle for 100 ms: under 10,000 data units (160 KB) on either port, 0.1% of the GPU phase's traffic in
        # 100 ms. The core-2 stream reaches DRAM through the CPU ports: under 100,000 units on either port, 0.05% of its
        # bytes.
        for i, limit in ((0, 9999), (3, 99999)):
            for port in COUNTER_PORTS:
                v = p[i]['counters']['port_%d' % port]['value']
                check('%s port %d data units' % (p[i]['name'], port), v['dt_rd'] + v['dt_wr'], 0, limit)
        # Each accelerator's traffic stays off the other's port: below 1% of its own count.
        g6, g18 = p[1]['counters']['port_6']['value'], p[1]['counters']['port_18']['value']
        n6, n18 = p[2]['counters']['port_6']['value'], p[2]['counters']['port_18']['value']
        check('gpu-alone port 18 share', round((g18['dt_rd'] + g18['dt_wr']) / max(1, g6['dt_rd'] + g6['dt_wr']), 4), 0, 0.01)
        check('npu-alone port 6 share', round((n6['dt_rd'] + n6['dt_wr']) / max(1, n18['dt_rd'] + n18['dt_wr']), 4), 0, 0.01)
        check('counted phase longest ms', max(ph['counters']['ms'] or 0 for ph in p[:COUNTED_PHASES]), None, 1200)
    # Calibration on the known bytes per job (16-byte units), from whichever source the model allows.
    g, n = cal.get('gpu') or {}, cal.get('npu') or {}
    rd, wr, nb = cal['gpu_known_read_units_per_job'], cal['gpu_known_write_units_per_job'], cal['npu_known_units_per_job']
    slack = 1.0 if counted else 1.1   # a probe's period holds about 68 GPU jobs, so its per-job count carries about 2% more
    check('gpu dt_rd per job', g.get('dt_rd'), round(rd / slack), round(rd * 1.04 * slack))
    check('gpu dt_wr per job', g.get('dt_wr'), round(wr / slack), round(2 * wr * 1.04 * slack))
    check('gpu requests per job decide', cal.get('gpu_discriminator') in ('line requests: few resident tasks',
                                                                          'per-lane requests: a per-request limit') or None)
    check('npu dt_rd per job', n.get('dt_rd'), round(nb / slack), round(nb * 1.05 * slack))
    check('npu dt_wr per job', n.get('dt_wr'), round(nb / slack), round(nb * 1.02 * slack))
    # The accelerators run as in the width run (675.9 jobs/s) and stage 1c (10,183.9 jobs/s), within 10%.
    check('gpu alone jobs/s', p[1]['gpu']['jobs_per_second'], 608, 744)
    check('npu alone jobs/s', p[2]['npu']['jobs_per_second'], 9165, 11202)
    check('gpu checked slot jobs', p[1]['slot_jobs'] + p[4]['slot_jobs'], 8, 8)
    check('core-2 stream sweeps', p[3]['stream_sweeps'], 1)
    check('mismatches', sum(ph[d]['mismatches'] for ph in p for d in ('gpu', 'npu')), 0, 0)
    check('chase overflow', sum(ph['chase']['overflow'] for ph in p), 0, 0)
    check('pll_npu unlocked reads', r['pll']['unlocked'], 0, 0)
    check('pll_npu first differing site', r['pll']['first_site'], 0, 0)
    check('throttled phases', sum(1 for ph in p if ph['throttled']), 0, 0)
    check('top point at start and end', r['initial_top'] + r['final_top'], 2, 2)
    return out


def width_checks(r):
    """The ranges of the GPU-width hypothesis (docs/designs/concurrent-accel.md, "The GPU add waits on memory")."""
    out = []

    def check(name, value, low=None, high=None):
        ok = value is not None and (low is None or value >= low) and (high is None or value <= high)
        out.append({'name': name, 'value': value, 'low': low, 'high': high, 'ok': ok})

    p = r['phases'][0]
    check('gpu alone jobs/s', p['gpu']['jobs_per_second'], 550, 850)
    check('gpu serial slot job us', p.get('gpu_slot_job_us'), 1200, 1800)
    check('gpu checked slot jobs', p['slot_jobs'], 4, 4)
    check('gpu pipelined jobs', p['gpu']['pipelined_jobs'], 200)
    check('mismatches', p['gpu']['mismatches'], 0, 0)
    check('chase overflow', p['chase']['overflow'], 0, 0)
    check('gpu elements per job', r['gpu_elements'], 262144, 262144)
    check('pll_npu unlocked reads', r['pll']['unlocked'], 0, 0)
    check('pll_npu first differing site', r['pll']['first_site'], 0, 0)
    check('throttled', p['throttled'], 0, 0)
    check('top point at start and end', r['initial_top'] + r['final_top'], 2, 2)
    return out


def large_losses(p):
    """Each phase with both devices against phase 2 (both, no streaming), and phase 2 against each device alone."""
    out = {}
    for ph in p[2:]:
        row = {}
        for d in ('gpu', 'npu'):
            b, v = p[2][d]['jobs_per_second'], ph[d]['jobs_per_second']
            row[d] = round(100 * (1 - v / b), 2) if b and v is not None else None
        out[ph['name']] = row
    out['together-over-alone-percent'] = {
        d: round(100 * p[2][d]['jobs_per_second'] / p[k][d]['jobs_per_second'], 1)
        if p[2][d]['jobs_per_second'] and p[k][d]['jobs_per_second'] else None for d, k in (('gpu', 0), ('npu', 1))}
    return out


def large_checks(r):
    """The ranges of the stage-1c hypothesis (docs/designs/concurrent-accel.md, "Stage 1c")."""
    out = []

    def check(name, value, low=None, high=None):
        ok = value is not None and (low is None or value >= low) and (high is None or value <= high)
        out.append({'name': name, 'value': value, 'low': low, 'high': high, 'ok': ok})

    p, loss = r['phases'], r['losses']
    check('gpu alone jobs/s', p[0]['gpu']['jobs_per_second'], 300, 2000)
    check('npu alone jobs/s', p[1]['npu']['jobs_per_second'], 4000, 11000)
    for d in ('gpu', 'npu'):
        check('%s together over alone percent' % d, loss['together-over-alone-percent'][d], 80, 100)
    for name, gpu, npu in (('core-2', (10, 40), (0, 26)), ('core-7', (18, 48), (14, 44)),
                           ('cores-2-4', (36, 66), (33, 63)), ('cores-2-5-7', (36, 66), (38, 68))):
        check('gpu loss percent %s' % name, loss[name]['gpu'], *gpu)
        check('npu loss percent %s' % name, loss[name]['npu'], *npu)
    for x3, single in (('core-2-x3', 'core-2'), ('core-7-x3', 'core-7')):
        for d in ('gpu', 'npu'):
            a, b = loss[x3][d], loss[single][d]
            check('%s loss %s minus %s points' % (d, x3, single), None if a is None or b is None else round(a - b, 2), -10, 10)
    for i, ph in enumerate(p):
        for d, bit in (('gpu', 1), ('npu', 2)):
            if ph['devices'] & bit:
                check('%s pipelined jobs %s' % (d, ph['name']), ph[d]['pipelined_jobs'], 200)
        if ph['devices'] & 3 == 3:
            check('npu completions with a gpu job in flight %s' % ph['name'], ph['overlap'], 200)
        if ph['devices'] & 1:
            check('gpu checked slot jobs %s' % ph['name'], ph['slot_jobs'], 4, 4)
        check('chase samples %s' % ph['name'], ph['chase']['samples'], 1000)
        check('chase overflow %s' % ph['name'], ph['chase']['overflow'], 0, 0)
    check('chase cycle lines', r['chase_lines'], 262144, 262144)
    check('gpu elements per job', r['gpu_elements'], 262144, 262144)
    check('npu tensor bytes', r['npu_tensor_bytes'], 360448, 360448)
    check('phase longest ms', max(ph['ms'] for ph in p), None, 3000)
    check('mismatches', sum(ph[d]['mismatches'] for ph in p for d in ('gpu', 'npu')), 0, 0)
    check('pll_npu unlocked reads', r['pll']['unlocked'], 0, 0)
    check('pll_npu first differing site', r['pll']['first_site'], 0, 0)
    check('throttled phases', sum(1 for ph in p if ph['throttled']), 0, 0)
    check('top point at start and end', r['initial_top'] + r['final_top'], 2, 2)
    return out


def large_decision(r):
    """The stage-1c decision rules: validity, job structure at the stage-1 set, and stream count (one direction)."""
    loss, out = r['losses'], {}
    for d in ('gpu', 'npu'):
        together = loss['together-over-alone-percent'][d]
        now, before = loss['cores-2-5-7'][d], STAGE_1B_LOSS['cores-2-5-7'][d]
        if now is None:
            verdict = 'unknown'
        elif now <= before / 2:
            verdict = 'lever'
        elif abs(now - before) <= 15:
            verdict = 'not a lever'
        elif now > before:
            verdict = 'worse'  # larger jobs lose more than the small ones: not a lever
        else:
            verdict = 'partial'
        out[d] = {'confounded': together is None or together < 80, 'stage_1c_loss': now, 'stage_1b_loss': before,
                  'job_structure': verdict}
    rows = {}
    for x3, single in (('core-2-x3', 'core-2'), ('core-7-x3', 'core-7')):
        rows[x3] = {d: ('requesting cores, not row interleaving'
                        if loss[x3][d] is not None and loss[single][d] is not None and abs(loss[x3][d] - loss[single][d]) <= 10
                        else 'not separated') for d in ('gpu', 'npu')}
    out['stream_count'] = rows
    return out


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
    # Wake samples: a GPU job (tens of microseconds) outlasts the step from submission to the blocking wait, so nearly every
    # serial GPU job blocks; a relu job (about 5.5 us) can finish before the producer blocks, so fewer NPU jobs give a sample.
    for i in range(4):
        for d, share in (('gpu', 0.95), ('npu', 0.5)):
            if p[i]['devices'] & (1 if d == 'gpu' else 2):
                check('%s wake samples %s' % (d, p[i]['name']), p[i][d]['wake']['count'],
                      int(share * p[i][d]['serial_jobs']), p[i][d]['serial_jobs'])
    check('phase longest ms', max(ph['ms'] for ph in p), None, 3000)
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
