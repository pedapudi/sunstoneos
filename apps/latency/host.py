"""Decode the native CPU latency report without accessing a device."""
import struct

RESULT_BYTES = 63872
PHASES = ('initialization', 'idle', 'busy')
SITE_KINDS = ('irq_masked', 'task_lock_hold')
SITE_METRICS = ('irq_masked', 'lock1_hold')
SITE_SLOTS = 3
QUANTILES = 'nearest-rank logarithmic histogram upper endpoints; sixteen bins per power of two; exact maximum'
MAGIC = 0x53534C4154454E31
METRICS = ('timer_irq', 'irq_masked', 'lock0_wait', 'lock0_hold', 'lock1_wait',
           'lock1_hold', 'wake_idle', 'wake_busy', 'dma', 'gpu', 'npu')


def histogram_upper(value):
    if value < 16:
        return value
    shift = value.bit_length() - 5
    return min((1 << 64) - 1, ((value >> shift) + 1) * (1 << shift) - 1)

def inputs():
    return []


def decode(payload, dma=False):
    if len(payload) != RESULT_BYTES:
        raise ValueError(f'latency result must contain {RESULT_BYTES} bytes')
    magic, frequency, elapsed, mask, model, dma_jobs, irq_mask = struct.unpack_from('<3Q4I', payload)
    if magic != MAGIC or mask != 0xff or model or irq_mask != 1 or \
            (dma_jobs < 2 if dma else dma_jobs != 0):
        raise ValueError('native CPU latency header differs')
    if not frequency or not 0 < elapsed <= 10000:
        raise ValueError('latency counter frequency or run interval is invalid')
    cores = []
    fields = ('count', 'max_ticks', 'max_start', 'max_site', 'first_over_start',
              'first_over_ticks', 'first_over_site')
    for core in range(8):
        sweeps, read, written, digest = struct.unpack_from('<4Q', payload, 40 + core * 32)
        if sweeps < 2 or read != sweeps * 1048576 or written != read:
            raise ValueError(f'core {core} memory traffic is incomplete')
        samples = {}
        for metric, name in enumerate(METRICS):
            words = struct.unpack_from('<7Q', payload, 320 + core * 640 + metric * 56)
            samples[name] = dict(zip(fields, words))
            count, maximum, start, site, over_start, over_ticks, over_site = words
            if not count and any(words):
                raise ValueError(f'core {core} {name} has data without samples')
            if over_ticks > maximum or (not over_ticks and (over_start or over_site)):
                raise ValueError(f'core {core} {name} overrun context differs')
        if samples['wake_idle']['count'] != 64 or samples['wake_busy']['count'] != 128:
            raise ValueError(f'core {core} wake samples are incomplete')
        if any(samples[name]['count'] for name in ('gpu', 'npu')) or \
                (not dma and samples['dma']['count']) or \
                (dma and bool(samples['dma']['count']) != (core == 0)):
            raise ValueError('native CPU image cannot report accelerator or DMA samples')
        if dma and core == 0 and samples['dma']['count'] < 2:
            raise ValueError('native DMA completion coverage is incomplete')
        if bool(samples['timer_irq']['count']) != (core == 0):
            raise ValueError('timer IRQ coverage differs from CPU0-only delivery')
        phases = {}
        distribution_fields = ('count', 'p50_upper_ticks', 'p99_upper_ticks',
                               'max_ticks', 'max_start', 'max_site', 'overflow_bin_count')
        for phase, phase_name in enumerate(PHASES):
            distributions = {}
            for metric, name in enumerate(METRICS):
                values = struct.unpack_from('<7Q', payload,
                    5440 + (phase * 8 + core) * 616 + metric * 56)
                count, p50, p99, maximum, start, site, overflow = values
                if (not count and any(values)) or p50 > p99 or p99 > histogram_upper(maximum) or maximum > samples[name]['max_ticks']:
                    raise ValueError(f'core {core} {phase_name} {name} distribution differs')
                if overflow > count or (overflow and maximum < (31 << 59)):
                    raise ValueError('invalid overflow-bin count')
                if count and p99 < maximum and count < 100:
                    raise ValueError(f'core {core} {phase_name} {name} quantile differs')
                expected_wake = 64 if metric == 6 and phase == 1 else 128 if metric == 7 and phase == 2 else 0
                if metric in (6, 7) and count != expected_wake:
                    raise ValueError(f'core {core} phase wake samples are incomplete')
                distributions[name] = dict(zip(distribution_fields, values))
            phases[phase_name] = distributions
        for name in METRICS:
            if sum(phases[p][name]['count'] for p in PHASES) > samples[name]['count']:
                raise ValueError(f'core {core} {name} phase count exceeds aggregate')
        acquisitions = struct.unpack_from('<2Q', payload, 20224 + core * 16)
        if any(acquisitions[lock] < samples[f'lock{lock}_wait']['count'] for lock in range(2)):
            raise ValueError(f'core {core} acquisition counts differ')
        phase_acquisitions = [struct.unpack_from('<2Q', payload, 20352 + (p * 8 + core) * 16)
                              for p in range(3)]
        if any(sum(v[lock] for v in phase_acquisitions) > acquisitions[lock] for lock in range(2)):
            raise ValueError('phase acquisition count exceeds aggregate')
        bounds = [struct.unpack_from('<2Q', payload, 20736 + p * 16) for p in range(3)]
        if bounds[0][0] or any(not end or end < begin for begin, end in bounds) or any(
                bounds[p][1] > bounds[p + 1][0] for p in range(2)):
            raise ValueError('phase bounds overlap or are incomplete')
        for p, phase_name in enumerate(PHASES):
            begin, end = bounds[p]
            for distribution in phases[phase_name].values():
                if distribution['count'] and not (begin <= distribution['max_start'] < end and
                        distribution['max_ticks'] <= end - distribution['max_start']):
                    raise ValueError('phase maximum crosses a phase boundary')
        site, = struct.unpack_from('<Q', payload, 20784 + core * 8)
        raw = []
        for index in range(192):
            ready, running, metric, flags = struct.unpack_from('<2Q2I', payload,
                20848 + (core * 192 + index) * 24)
            phase_index = 1 if index < 64 else 2
            if metric != (6 if index < 64 else 7) or flags != (1 if index < 64 else 2) or not site:
                raise ValueError('raw wake workload flags or site differ')
            begin, end = bounds[phase_index]
            if not begin <= ready <= running <= end or ready >= end:
                raise ValueError('raw wake crosses a phase boundary')
            raw.append(dict(ready=ready, running=running, metric=METRICS[metric], workload_flags=flags))
        for phase_index, first, last, name in ((1, 0, 64, 'wake_idle'), (2, 64, 192, 'wake_busy')):
            durations = sorted(v['running'] - v['ready'] for v in raw[first:last])
            distribution = phases[PHASES[phase_index]][name]
            aggregate_wake = samples[name]
            if aggregate_wake['max_ticks'] != durations[-1] or aggregate_wake['max_site'] != site or not any(
                    v['ready'] == aggregate_wake['max_start'] and v['running'] - v['ready'] == durations[-1]
                    for v in raw[first:last]):
                raise ValueError('raw wake aggregate maximum provenance differs')
            n = len(durations)
            if distribution['max_site'] != site or not any(
                    v['ready'] == distribution['max_start'] and v['running'] - v['ready'] == distribution['max_ticks']
                    for v in raw[first:last]):
                raise ValueError('raw wake maximum provenance differs')
            if (distribution['p50_upper_ticks'], distribution['p99_upper_ticks'], distribution['max_ticks']) != (
                    histogram_upper(durations[(n + 1) // 2 - 1]), histogram_upper(durations[n - n // 100 - 1]), durations[-1]):
                raise ValueError('raw wake quantiles differ from distribution')
        sites = {}
        for p, phase_name in enumerate((*PHASES, 'aggregate')):
            kinds = {}
            for kind, (kind_name, metric_name) in enumerate(zip(SITE_KINDS, SITE_METRICS)):
                entries = []
                previous = (1 << 64) - 1
                empty = False
                for slot in range(SITE_SLOTS):
                    values = struct.unpack_from('<4Q', payload,
                        57728 + (((p * 8 + core) * 2 + kind) * SITE_SLOTS + slot) * 32)
                    ticks, start, enter_site, exit_site = values
                    if not ticks:
                        if any(values):
                            raise ValueError('inactive attribution slot has context')
                        empty = True
                        continue
                    if empty or ticks > previous:
                        raise ValueError('attribution slots are not packed in descending order')
                    if not enter_site or not exit_site:
                        raise ValueError('active attribution slot lacks enter or exit site')
                    if ticks > samples[metric_name]['max_ticks']:
                        raise ValueError('attribution duration exceeds aggregate metric maximum')
                    if p < len(PHASES):
                        begin, end = bounds[p]
                        if not begin <= start < end or ticks > end - start:
                            raise ValueError('attribution interval crosses a phase boundary')
                    previous = ticks
                    entries.append(dict(zip(('ticks', 'start', 'enter_site', 'exit_site'), values)))
                kinds[kind_name] = entries
            sites[phase_name] = kinds
        cores.append({'core': core, 'sweeps': sweeps, 'bytes_read': read,
                      'bytes_written': written, 'digest': digest, 'metrics': samples, 'phases': phases,
                      'lock_acquisitions_including_recursive': dict(zip(('isr', 'task'), acquisitions)),
                      'phase_lock_acquisitions': dict(zip(PHASES, phase_acquisitions)),
                      'wake_site': site, 'raw_wakes': raw, 'sites': sites})
    return {'quantile_method': QUANTILES, 'phase_attribution': 'named phases contain whole intervals; aggregate metrics and sites include gaps and crossings', 'model': False, 'frequency': frequency, 'elapsed_ticks': elapsed,
            'phase_bounds': dict(zip(PHASES, bounds)), 'core_mask': mask, 'timer_irq_core_mask': irq_mask, 'dma_jobs': dma_jobs, 'cores': cores}
