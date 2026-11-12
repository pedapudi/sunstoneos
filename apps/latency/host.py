"""Decode the native CPU latency report without accessing a device."""
import struct

RESULT_BYTES = 63872
PMU_BYTES = 1216  # optional tail (padded to the 64-byte report alignment): event masks, then counts inside the kind-0 sites (SUNSTONE_LATENCY_PMU)
PHASES = ('initialization', 'idle', 'busy')
SITE_KINDS = ('irq_masked', 'task_lock_hold')
SITE_METRICS = ('irq_masked', 'lock1_hold')
SITE_SLOTS = 3
QUANTILES = 'nearest-rank logarithmic histogram upper endpoints; sixteen bins per power of two; exact maximum'
MAGIC = 0x53534C4154454E31
SCHED_MAGIC = 0x5353534348545231
METRICS = ('timer_irq', 'irq_masked', 'lock0_wait', 'lock0_hold', 'lock1_wait',
           'lock1_hold', 'wake_idle', 'wake_busy', 'dma', 'gpu', 'npu')


def histogram_upper(value):
    if value < 16:
        return value
    shift = value.bit_length() - 5
    return min((1 << 64) - 1, ((value >> shift) + 1) * (1 << shift) - 1)

def inputs():
    return []


def decode_scheduler_trace(payload, frequency):
    """Decode the diagnostic-only tail; all durations retain raw counter ticks."""
    if len(payload) < 32 or not frequency:
        raise ValueError('scheduler trace header or frequency is missing')
    magic, version, waits, tick_capacity = struct.unpack_from('<4Q', payload)
    if magic != SCHED_MAGIC or version not in (1, 2) or not waits or not tick_capacity:
        raise ValueError('scheduler trace schema or declared capacities differ')
    records = 32  # two drains, each bounded by SS_OWNER_BATCH = 16
    wait_words = 12 if version == 1 else 16
    core_words = 9 + 20 + 11 + 4 + records * 7 + (waits + 1) * wait_words
    expected = (4 + 8 * core_words + 16 + 2 + tick_capacity * 2) * 8
    padded = (expected + 63) // 64 * 64
    if len(payload) != padded or any(payload[expected:]):
        raise ValueError('scheduler trace length or alignment padding differs')
    offset = 32

    def take(count):
        nonlocal offset
        values = struct.unpack_from(f'<{count}Q', payload, offset)
        offset += count * 8
        return values

    def ordered(stamps):
        observed = [v for v in stamps if v]
        if observed != sorted(observed):
            raise ValueError('scheduler trace timestamps are out of order')

    def command():
        kind, slot, flags, *stamp = take(10)
        ordered(stamp)
        def duration(first, last):
            return stamp[last] - stamp[first] if stamp[first] and stamp[last] else 0
        return {'command': kind, 'slot': slot, 'flags': flags, 'stamp': stamp,
                'ticks': duration(0, 6),
                'steps_ticks': dict(zip(('reference', 'publish', 'drain', 'slice'),
                    (duration(0, 1), duration(1, 2), duration(3, 4), duration(4, 5))))}

    def wait_record():
        fields = ('start', 'end', 'site', 'lock', 'serving', 'next', 'flags',
                  'holder_core', 'holder_start', 'holder_end', 'holder_site', 'holder_generation')
        if version == 2:
            fields += ('fifo_start', 'fifo_end', 'fifo_serving', 'fifo_ticket')
        value = dict(zip(fields, take(wait_words)))
        if value['flags'] & ~(15 if version == 1 else 31):
            raise ValueError('scheduler lock wait flags differ from its version')
        if value['end'] < value['start']:
            raise ValueError('scheduler lock wait has negative duration')
        value['ticks'] = value['end'] - value['start']
        if value['flags'] & 3 == 3 and (value['holder_core'] >= 8 or not value['holder_start']):
            raise ValueError('scheduler lock holder identity is invalid')
        if value['flags'] & 4 and not value['holder_start'] <= value['holder_end'] <= value['end']:
            raise ValueError('scheduler lock holder completion is invalid')
        if version == 2:
            actual = value['flags'] & 16
            fifo_fields = tuple(value[name] for name in ('fifo_start', 'fifo_end', 'fifo_serving', 'fifo_ticket'))
            if actual:
                if value['lock'] != 0 or not value['fifo_start'] or not (
                        value['start'] <= value['fifo_start'] <= value['fifo_end'] <= value['end']):
                    raise ValueError('scheduler actual FIFO sample lies outside its global lock wait')
                if value['fifo_serving'] > 0xffff or value['fifo_ticket'] > 0xffff:
                    raise ValueError('scheduler actual FIFO ticket exceeds its 16-bit field')
                value['fifo_ticks'] = value['fifo_end'] - value['fifo_start']
                value['wrapper_residual_ticks'] = value['ticks'] - value['fifo_ticks']
            else:
                if any(fifo_fields):
                    raise ValueError('scheduler FIFO fields lack an actual-sample flag')
                value['fifo_ticks'] = value['wrapper_residual_ticks'] = None
        return value

    cores = []
    for core in range(8):
        count, *scalars = take(9)
        row = {'core': core, 'command_count': count,
               'command_stage_max_ticks': dict(zip(('reference', 'publish', 'drain', 'slice'), scalars[:4])),
               'irq_count': scalars[4], 'drain_count': scalars[5],
               'wait_count': scalars[6], 'wait_dropped': scalars[7],
               'command': command(), 'self_suspend': command()}
        values = take(11)
        ordered(values[:4])
        row['irq'] = {'stamp': list(values[:4]),
                      'service_ticks': dict(zip(('control', 'incoming', 'pull', 'expiry', 'slice'), values[4:9])),
                      'passes': values[9], 'result': values[10]}
        start, end, count, dropped = take(4)
        if end < start or count > records or dropped:
            raise ValueError('scheduler drain is incomplete or invalid')
        entries = []
        for index in range(records):
            slot, phase, *stamp = take(7)
            if index < count:
                ordered(stamp)
                if not start <= stamp[0] <= stamp[-1] <= end:
                    raise ValueError('scheduler control record lies outside its drain')
                entries.append({'slot': slot, 'phase': phase, 'stamp': stamp})
        row['drain'] = {'start': start, 'end': end, 'count': count, 'dropped': dropped, 'records': entries}
        row['wait_max'] = wait_record()
        entries = [wait_record() for _ in range(waits)]
        if row['wait_count'] > waits or row['wait_dropped'] and row['wait_count'] != waits:
            raise ValueError('scheduler wait log counters differ from capacity')
        row['waits'] = entries[:row['wait_count']]
        if any(value['ticks'] > row['wait_max']['ticks'] for value in row['waits']):
            raise ValueError('scheduler wait maximum omits a retained wait')
        cores.append(row)
    startup = [dict(zip(('scheduler_start', 'first_restore'), take(2))) for _ in range(8)]
    for markers in startup:
        if markers['scheduler_start'] and markers['first_restore'] and markers['first_restore'] < markers['scheduler_start']:
            raise ValueError('scheduler startup timestamps are out of order')
    tick_count, tick_dropped = take(2)
    if tick_count > tick_capacity or tick_dropped and tick_count != tick_capacity:
        raise ValueError('scheduler tick log counters differ from capacity')
    ticks = []
    for index in range(tick_capacity):
        start, end = take(2)
        if index < tick_count:
            if not start or end < start or ticks and start < ticks[-1]['end']:
                raise ValueError('scheduler timer intervals overlap or are invalid')
            ticks.append({'start': start, 'end': end})
    for row in cores:
        for value in [row['wait_max'], *row['waits']]:
            value['cause'] = 'unclassified'
            value['holder_tick_overlap_ticks'] = 0
            actual = version == 2 and value['flags'] & 16
            if actual and value['fifo_ticket'] == value['fifo_serving']:
                # Reservation sees no predecessor. The remaining wrapper
                # duration has no attribution to a particular mechanism.
                value['cause'] = 'uncontended_FIFO'
                continue
            if not value['ticks'] or value['flags'] & 15 != 15:
                continue
            begin, end = max(value['start'], value['holder_start']), value['holder_end']
            if actual:
                # Keep the early holder snapshot separate from the actual
                # reservation. A changed predecessor or intervening waiter
                # prevents a claim about the complete wait's cause.
                if ((value['fifo_ticket'] - value['fifo_serving']) & 0xffff) != 1 or \
                        value['fifo_serving'] != value['serving'] or value['fifo_ticket'] != value['next']:
                    continue
                begin = max(value['fifo_start'], value['holder_start'])
                end = min(value['fifo_end'], value['holder_end'])
            if end <= begin:
                continue
            # More than the observed holder can precede this waiter. Preserve
            # the overlap without attributing the complete wait to one holder.
            ticket_mask = 0xffff if value['lock'] == 0 else 0xffffffff
            if (value['next'] - value['serving']) & ticket_mask != 1:
                continue
            if value['holder_core'] == 0:
                overlap = sum(max(0, min(end, tick['end']) - max(begin, tick['start'])) for tick in ticks)
                value['holder_tick_overlap_ticks'] = overlap
                if overlap:
                    value['cause'] = 'holder_tick_overlap'
                    continue
                if tick_dropped and (not ticks or end > ticks[-1]['end']):
                    continue
            value['cause'] = 'observed_holder_contention'
    return {'version': version, 'waits_capacity': waits, 'ticks_capacity': tick_capacity,
            'cores': cores, 'startup': startup, 'tick_count': tick_count,
            'tick_dropped': tick_dropped, 'ticks': ticks}


def decode(payload, dma=False):
    tail = payload[RESULT_BYTES:]
    pmu_tail, scheduler_tail = b'', b''
    if len(tail) >= 8 and struct.unpack_from('<Q', tail)[0] == SCHED_MAGIC:
        scheduler_tail = tail
    elif len(tail) >= PMU_BYTES + 8 and struct.unpack_from('<Q', tail, PMU_BYTES)[0] == SCHED_MAGIC:
        pmu_tail, scheduler_tail = tail[:PMU_BYTES], tail[PMU_BYTES:]
    elif len(tail) == PMU_BYTES:
        pmu_tail = tail
    elif tail or len(payload) != RESULT_BYTES:
        raise ValueError(f'latency result must contain {RESULT_BYTES} bytes')
    payload = payload[:RESULT_BYTES]
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
                    if kind == 0 and pmu_tail and p in (2, 3):
                        row = 0 if p == 2 else 1
                        counts = struct.unpack_from('<6I', pmu_tail, 32 + ((row * 8 + core) * SITE_SLOTS + slot) * 24)
                        entries[-1]['pmu'] = dict(zip(('instructions', 'cycles', 'stall_backend', 'mem_access',
                                                       'bus_access', 'l1d_refills'), counts))
                kinds[kind_name] = entries
            sites[phase_name] = kinds
        cores.append({'core': core, 'sweeps': sweeps, 'bytes_read': read,
                      'bytes_written': written, 'digest': digest, 'metrics': samples, 'phases': phases,
                      'lock_acquisitions_including_recursive': dict(zip(('isr', 'task'), acquisitions)),
                      'phase_lock_acquisitions': dict(zip(PHASES, phase_acquisitions)),
                      'wake_site': site, 'raw_wakes': raw, 'sites': sites})
    result = {'quantile_method': QUANTILES, 'phase_attribution': 'named phases contain whole intervals; aggregate metrics and sites include gaps and crossings', 'model': False, 'frequency': frequency, 'elapsed_ticks': elapsed,
            'phase_bounds': dict(zip(PHASES, bounds)), 'core_mask': mask, 'timer_irq_core_mask': irq_mask, 'dma_jobs': dma_jobs, 'cores': cores}
    if scheduler_tail:
        result['scheduler_trace'] = decode_scheduler_trace(scheduler_tail, frequency)
    return result
