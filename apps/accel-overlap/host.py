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
COUNTERS_BYTES = 3768
COUNTER_NAMES = ('idle', 'gpu-alone', 'npu-alone', 'core-2')
COUNTER_DEVICES = (0, 1, 2, 4)
COUNTER_MODES = (0, 0, 0, 0x01)
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


def decode_counters(raw):
    """The NSI counter calibration (struct accel_overlap_counter_result)."""
    r = decode_large(raw, counters=True)
    at = LARGE_BYTES
    open_status, close_status, fail_address, fail_value = struct.unpack_from('<4I', raw, at)
    reset = struct.unpack_from('<18I', raw, at + 16)
    r['counters'] = {'open_status': open_status, 'close_status': close_status, 'fail_address': '0x%08x' % fail_address,
                     'fail_value': '0x%08x' % fail_value,
                     'reset': {'port_%d' % port: ['0x%08x' % w for w in reset[9 * b:9 * b + 9]]
                               for b, port in enumerate(COUNTER_PORTS)}}
    for i, phase in enumerate(r['phases']):
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
    gpu, npu = r['phases'][1], r['phases'][2]
    gpu_jobs = gpu['gpu']['pipelined_jobs'] + 2 * gpu['slot_jobs']   # each slot ran a plain and a synced job
    npu_jobs = npu['npu']['pipelined_jobs']
    g, n = gpu['counters']['port_6']['value'], npu['counters']['port_18']['value']
    per = lambda v, jobs: round(v / jobs, 1) if jobs else None
    r['calibration'] = {
        'gpu_jobs': gpu_jobs, 'npu_jobs': npu_jobs,
        'gpu_known_read_units_per_job': r['gpu_elements'] * 8 // DATA_UNIT,
        'gpu_known_write_units_per_job': r['gpu_elements'] * 4 // DATA_UNIT,
        'npu_known_units_per_job': r['npu_tensor_bytes'] // DATA_UNIT,
        'gpu_dt_rd_per_job': per(g['dt_rd'], gpu_jobs), 'gpu_dt_wr_per_job': per(g['dt_wr'], gpu_jobs),
        'gpu_requests_per_job': per(g['rq_rd'] + g['rq_wr'], gpu_jobs),
        'gpu_rq_rd_per_job': per(g['rq_rd'], gpu_jobs), 'gpu_rq_wr_per_job': per(g['rq_wr'], gpu_jobs),
        'npu_dt_rd_per_job': per(n['dt_rd'], npu_jobs), 'npu_dt_wr_per_job': per(n['dt_wr'], npu_jobs),
        'npu_requests_per_job': per(n['rq_rd'] + n['rq_wr'], npu_jobs),
        'gpu_la_rd_per_request': round(g['la_rd'] / g['rq_rd'], 2) if g['rq_rd'] else None,
        'npu_la_rd_per_request': round(n['la_rd'] / n['rq_rd'], 2) if n['rq_rd'] else None}
    q = r['calibration']['gpu_requests_per_job']
    r['calibration']['gpu_discriminator'] = (None if q is None else 'line requests: few resident tasks' if q < 100000 else
                                            'per-lane requests: a per-request limit' if q > 200000 else 'undecided')
    r['checks'] = counter_checks(r)
    r['in_range'] = all(c['ok'] for c in r['checks'])
    return r


def counter_checks(r):
    """The ranges of the NSI counter hypothesis (docs/designs/concurrent-accel.md, "NSI counters")."""
    out = []

    def check(name, value, low=None, high=None):
        ok = value is not None and (low is None or value >= low) and (high is None or value <= high)
        out.append({'name': name, 'value': value, 'low': low, 'high': high, 'ok': ok})

    k, p, cal = r['counters'], r['phases'], r['calibration']
    check('counter open status', k['open_status'], 0, 0)
    check('counter close status', k['close_status'], 0, 0)
    check('counter phase status', sum(1 for ph in p if ph['counters']['status']), 0, 0)
    check('reset enable bits set', sum(int(k['reset']['port_%d' % port][0], 16) & 1 for port in COUNTER_PORTS), 0, 0)
    # The counters hold after the disable.
    check('counts that moved after the disable', sum(
        1 for ph in p for port in COUNTER_PORTS for w in COUNTER_WORDS
        if ph['counters']['port_%d' % port]['held'][w] != ph['counters']['port_%d' % port]['value'][w]), 0, 0)
    # Live counting: the data sample at half the phase, on the port of the phase's device.
    for i, port in ((1, 6), (2, 18)):
        c = p[i]['counters']['port_%d' % port]
        total = c['value']['dt_rd'] + c['value']['dt_wr']
        check('%s port %d live share at half' % (p[i]['name'], port),
              round((c['live']['dt_rd'] + c['live']['dt_wr']) / total, 3) if total else None, 0.35, 0.65)
    # Controls: no accelerator job, or only the core-2 stream, moves under 1,000 data units on either port.
    for i in (0, 3):
        for port in COUNTER_PORTS:
            v = p[i]['counters']['port_%d' % port]['value']
            check('%s port %d data units' % (p[i]['name'], port), v['dt_rd'] + v['dt_wr'], 0, 999)
    # Each accelerator's traffic stays off the other's port: below 1% of its own count.
    g6 = p[1]['counters']['port_6']['value']; g18 = p[1]['counters']['port_18']['value']
    n6 = p[2]['counters']['port_6']['value']; n18 = p[2]['counters']['port_18']['value']
    check('gpu-alone port 18 share', round((g18['dt_rd'] + g18['dt_wr']) / max(1, g6['dt_rd'] + g6['dt_wr']), 4), 0, 0.01)
    check('npu-alone port 6 share', round((n6['dt_rd'] + n6['dt_wr']) / max(1, n18['dt_rd'] + n18['dt_wr']), 4), 0, 0.01)
    # Calibration on the known bytes per job (16-byte units).
    rd, wr, nb = cal['gpu_known_read_units_per_job'], cal['gpu_known_write_units_per_job'], cal['npu_known_units_per_job']
    check('gpu dt_rd per job', cal['gpu_dt_rd_per_job'], rd, round(rd * 1.04))
    check('gpu dt_wr per job', cal['gpu_dt_wr_per_job'], wr, round(2 * wr * 1.04))
    check('gpu requests per job decide', cal['gpu_discriminator'] in ('line requests: few resident tasks',
                                                                      'per-lane requests: a per-request limit') or None)
    check('npu dt_rd per job', cal['npu_dt_rd_per_job'], nb, round(nb * 1.05))
    check('npu dt_wr per job', cal['npu_dt_wr_per_job'], nb, round(nb * 1.02))
    # The counted phases end well inside the 7.16 s period of CYCLE 0xffffffff at the 600 MHz NSI clock.
    check('counted phase longest ms', max(ph['counters']['ms'] or 0 for ph in p), None, 1200)
    # The accelerators run as in the width run (675.9 jobs/s) and stage 1c (10,183.9 jobs/s), within 10%.
    check('gpu alone jobs/s', p[1]['gpu']['jobs_per_second'], 608, 744)
    check('npu alone jobs/s', p[2]['npu']['jobs_per_second'], 9165, 11202)
    check('gpu checked slot jobs', p[1]['slot_jobs'], 4, 4)
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
