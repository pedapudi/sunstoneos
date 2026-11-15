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
CONTROL_MAGIC = 0x53534354524C3031
CONTROL_BYTES = 17536
SCHED_PMU_MAGIC = 0x5353504D55303031
SCHED_PMU_BYTES = 11712
SCHED_PMU_KINDS = ('select_self', 'select_other', 'control', 'tick', 'blank')
SCHED_PMU_EVENTS = ((0x03, 0x01, 0x17, 0x2a, 0x34, 0x35),
                    (0x03, 0x01, 0x17, 0x24, 0x34, 0x35))
SCHED_PMU_STATUS = ('ok', 'unarmed', 'not_el1', 'wrong_core', 'nested',
                    'not_active', 'wrong_cookie', 'no_counters',
                    'config_mismatch', 'not_prepared', 'frozen', 'overflow')
CAUSE_MAGIC = 0x5353434155534531
CAUSE_BYTES = 17328
CAUSE_EVENTS = ('l1i_refill', 'l1d_refill', 'l2d_refill', 'l3d_refill', 'stall_backend')
CAUSE_EVENT_TYPES = (0x01, 0x03, 0x17, 0x2a, 0x24)
CAUSE_FOOTER_MAGIC = 0x5353434155534631
CAUSE_PING_MAGIC = 0x535350494e473031
CAUSE_PING_BYTES = 3392
CAUSE_CASES = ('blank', 'isb', 'dsb_ish', 'dsb_sy', 'store', 'store_dsb_ish',
               'store_dsb_sy', 'sgi', 'sgi_dsb_ish', 'sgi_dsb_sy', 'pmr_same',
               'sgi_isb', 'sgi_isb_dsb_ish', 'sgi_isb_dsb_sy', 'pmr_same_dsb_sy')
SERVICE_MAGIC = 0x5353535643594331
SERVICE_FOOTER_MAGIC = 0x5353535643594631
BAND_MAGIC = 0x535342414e445331
BAND_FOOTER_MAGIC = 0x535342414e444631
BAND_BYTES = 1872
BAND_PROBES = 2
BAND_BINS = 8
WAKE_MAGIC = 0x5353574b53454731
AXI_MAGIC = 0x5353415849434131  # stage 3a AXI2HIF cap section (apps/latency/axi-cap.h)
AXI_FOOTER_MAGIC = 0x5353415849434631
AXI_HEADER = '<3Q4I14I4I'
AXI_WINDOW = '<2Q8Q8Q'
AXI_SWITCH = '<3Q2I3I1I14I'
AXI_SWITCHES = 16
AXI_SUBPHASES = 4
AXI_BYTES = struct.calcsize(AXI_HEADER) + (2 + AXI_SUBPHASES) * struct.calcsize(AXI_WINDOW) + AXI_SWITCHES * struct.calcsize(AXI_SWITCH)
AXI_OPS = {1: 'entry', 2: 'set_nonbinding', 3: 'set_b', 4: 'clear', 5: 'restore'}
AXI_STATUS = ('ok', 'gate_closed', 'readback', 'entry_set', 'bad_op')
AXI_WORDS = (0x30, 0x48, 0x4c, 0x50, 0x54, 0x58, 0x5c)
AXI_SWEEP_BYTES = 2 * 1048576  # each sweep reads and rewrites 1 MiB, as the traffic words count it
WAKE_FOOTER_MAGIC = 0x5353574b53454631
WAKE_POINTS = ('irq_entry', 'wrapper_return', 'exit_start', 'work_start', 'work_done', 'mask_restored',
               'switch_begin', 'switch_end', 'running')
WAKE_SEGMENTS = tuple(f'{a}_to_{b}' for a, b in zip(WAKE_POINTS, WAKE_POINTS[1:]))
WAKE_PARTS = ('drain', 'incoming', 'pull', 'expiry', 'slice')
WAKE_VALUES = 1 + len(CAUSE_EVENTS)
WAKE_SPANS = len(WAKE_SEGMENTS) + len(WAKE_PARTS)
WAKE_WORST = 3
WAKE_TICK_WORDS = 4 + 2 * len(CAUSE_EVENTS) + 8 + 8 * len(CAUSE_EVENTS)


def wake_layout(version):
    """Words per sample, cell and core, and the report's bytes. Version 3 adds each
    sample's deadline, each cell's release ticks and per-span ranks, and keeps the
    three largest wakes per setting where versions 1 and 2 kept one."""
    v3 = version >= 3
    sample = (5 if v3 else 4) + WAKE_SPANS * WAKE_VALUES
    cell = (4 if v3 else 3) + WAKE_SPANS * WAKE_VALUES + (WAKE_SPANS * (WAKE_WORST + 1) if v3 else 0)
    core = 8 + 2 * cell + (1 + 2 * (WAKE_WORST if v3 else 1)) * sample
    return sample, cell, core, 128 + 8 * 8 * core + 4 * 8 * WAKE_TICK_WORDS


WAKE_SAMPLE_WORDS, WAKE_CELL_WORDS, WAKE_CORE_WORDS, WAKE_BYTES = wake_layout(3)
WAKE_BYTES_BY_VERSION = {version: wake_layout(version)[3] for version in (1, 2, 3)}
BAND_CELL_FIELDS = ('windows', 'cycles', 'foreign', 'foreign_irq', 'foreign_max', 'foreign_sum', 'own', 'own_max')
SERVICE_CELL = 464
SERVICE_BINS = 105
SERVICE_BYTES = 128 + 2 * 8 * 2 * 2 * SERVICE_CELL
SERVICE_PHASES = ('idle', 'busy')
SERVICE_KINDS = ('service', 'drain')
SERVICE_STATES = ('empty_return_off', 'empty_return_on')
CONTROL_BINS = 192
CONTROL_BIN_TICKS = 4
CONTROL_MINIMUM = 10000
METRICS = ('timer_irq', 'irq_masked', 'lock0_wait', 'lock0_hold', 'lock1_wait',
           'lock1_hold', 'wake_idle', 'wake_busy', 'dma', 'gpu', 'npu')


def histogram_upper(value):
    if value < 16:
        return value
    shift = value.bit_length() - 5
    return min((1 << 64) - 1, ((value >> shift) + 1) * (1 << shift) - 1)

def inputs():
    return []


def decode_scheduler_trace(payload, frequency, *, restore_spans=None):
    """Decode the tail, with optional build metadata for its startup endpoint."""
    if restore_spans is not None and not isinstance(restore_spans, bool):
        raise ValueError('scheduler restore-span metadata must be Boolean or absent')
    if len(payload) < 32 or not frequency:
        raise ValueError('scheduler trace header or frequency is missing')
    magic, version, waits, tick_capacity = struct.unpack_from('<4Q', payload)
    if version == 5:
        configuration = tick_capacity & ~0xffffffff
        tick_capacity &= 0xffffffff
        if configuration & ~(3 << 62) or configuration == 1 << 62:
            raise ValueError('scheduler build metadata has reserved bits or no validity flag')
        if configuration & (1 << 63):
            embedded_restore = bool(configuration & (1 << 62))
            if restore_spans is not None and restore_spans != embedded_restore:
                raise ValueError('scheduler and control restore-span metadata disagree')
            restore_spans = embedded_restore
    if magic != SCHED_MAGIC or version not in (1, 2, 3, 4, 5) or not waits or not tick_capacity:
        raise ValueError('scheduler trace schema or declared capacities differ')
    records = 32  # two drains, each bounded by SS_OWNER_BATCH = 16
    wait_words = 12 if version == 1 else 16
    core_words = 9 + 20 + 11 + 4 + records * 7 + (waits + 1) * wait_words
    expected = (4 + 8 * core_words + 16 + 2 + tick_capacity * 2) * 8
    if version >= 3:
        expected += 8 * 72 * 8
    if version >= 4:
        history_offset = expected
        expected += (1 + 8 * 10) * 8
        if version == 5:
            if len(payload) < expected:
                raise ValueError('scheduler history header is missing')
            capacity, = struct.unpack_from('<Q', payload, history_offset)
            expected += (4 + 11 * capacity) * 8
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
        direct = bool(flags & 64)
        if direct and (version != 5 or kind != 2 or flags not in (69, 85, 93, 197) or not all(stamp)):
            raise ValueError('scheduler direct suspension flags or timestamps differ')
        no_selection = direct and bool(flags & 16)
        if no_selection and len(set(stamp[4:])) != 1:
            raise ValueError('scheduler atomic no-selection suspension claims saved-frame stages')
        result = {'command': kind, 'slot': slot, 'flags': flags, 'stamp': stamp,
                  'ticks': duration(0, 6), 'direct_self_suspend': direct,
                  'steps_ticks': dict(zip(('reference', 'publish', 'drain', 'slice'),
                      (duration(0, 1), duration(1, 2), duration(3, 4), duration(4, 5))))}
        if direct:
            result['steps_ticks'] = dict(zip(('reference', 'publish', 'local_commit'),
                (duration(0, 1), duration(1, 2), duration(3, 4))))
            if not no_selection:
                result['steps_ticks'].update(save_frame=duration(4, 5), select=duration(5, 6))
            result['selection_recorded'] = not no_selection
            result['self_suspend_outcome'] = ('deferred_without_selection' if flags & 8 else
                'completed_before_local_selection' if no_selection else
                'parked_at_exception_exit' if flags & 128 else 'selected_replacement')
            if flags & 128:
                result['steps_ticks'] = dict(zip(('reference', 'publish_until_exit', 'save_frame', 'select'),
                    (duration(0, 1), duration(1, 3), duration(4, 5), duration(5, 6))))
            result['masked_c_ticks'] = duration(3, 6)
            result['reference_publication_scope'] = 'before_local_guard' if flags & 8 else 'before_the_API_mask'
        return result

    def wait_record():
        fields = ('start', 'end', 'site', 'lock', 'serving', 'next', 'flags',
                  'holder_core', 'holder_start', 'holder_end', 'holder_site', 'holder_generation')
        if version >= 2:
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
        if version >= 2:
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
               'command_stage_max_legacy_only': ['drain', 'slice'] if version == 5 else [],
               'self_suspend_selection': ('largest atomic self local guarded C interval; legacy command interval until an atomic sample exists'
                   if version == 5 else 'largest command interval'),
               'irq_count': scalars[4], 'drain_count': scalars[5],
               'wait_count': scalars[6], 'wait_dropped': scalars[7],
               'command': command(), 'self_suspend': command()}
        values = take(11)
        ordered(values[:4])
        row['irq'] = {'stamp': list(values[:4]),
                      'service_ticks': dict(zip(('control', 'incoming', 'pull', 'expiry', 'slice'), values[4:9])),
                      'passes': values[9], 'result': values[10]}
        if version >= 3:
            if values[10] not in (0, 1, 3):
                raise ValueError('scheduler IRQ result is invalid')
            if values[10] == 3 and (len(set(values[1:4])) != 1 or any(values[4:10])):
                raise ValueError('scheduler fast IRQ contains mask or service work')
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
                record = {'slot': slot, 'phase': phase, 'stamp': stamp}
                if version == 5:
                    record['record_type'] = 'locked'
                if phase == 1 << 63:
                    if version != 5 or stamp[2] != stamp[3] or stamp[3] != stamp[4]:
                        raise ValueError('scheduler busy FIFO record differs from its version or shape')
                    record.update(record_type='fifo_busy', fifo_try_ticks=stamp[1] - stamp[0],
                                  park_ticks=stamp[2] - stamp[1])
                elif phase > 3:
                    raise ValueError('scheduler control transfer phase is invalid')
                entries.append(record)
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
    for core, markers in enumerate(startup):
        if markers['scheduler_start'] and markers['first_restore'] and markers['first_restore'] < markers['scheduler_start']:
            raise ValueError('scheduler startup timestamps are out of order')
        markers['start_kind'] = (None if not markers['scheduler_start'] else
            'primary_running_flag_boundary' if version >= 3 and core == 0 else 'port_start_entry')
        markers['restore_kind'] = 'pre_eret' if version >= 3 else 'restore_entry'
        if version == 5:
            if restore_spans is True:
                markers['restore_kind'] = 'final_software_return_point'
                markers['restore_scope'] = ('includes return-observer bookkeeping; excludes its '
                    'timestamp store, final temporary GP restore and ERET')
            elif restore_spans is False:
                markers['restore_scope'] = 'port restore marker; excludes the remaining assembly restore and ERET'
            else:
                markers['restore_kind'] = 'build_dependent_pre_eret'
                markers['restore_scope'] = ('restore-span build metadata unavailable: ON includes return-observer '
                    'bookkeeping, OFF records the port restore marker; both exclude the remaining assembly restore and ERET')
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
            actual = version >= 2 and value['flags'] & 16
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
    wake_cores = []
    history_capacity = None
    histories = []
    if version >= 4:
        history_offset = offset + 8 * 72 * 8
        history_capacity, = struct.unpack_from('<Q', payload, history_offset)
        if not history_capacity:
            raise ValueError('scheduler history capacity is missing')
        for core in range(8):
            histories.append(dict(zip(('latest_start', 'latest_end', 'latest_work', 'latest_result', 'latest_sequence',
                                       'trailing_count', 'trailing_work', 'irq_overwritten', 'switch_overwritten', 'future_irqs'),
                                      struct.unpack_from('<10Q', payload, history_offset + 8 + core * 80))))
    if version >= 3:
        names = ('fast', 'slow', 'unmatched')
        limits = (10, 25, 50, 80, 100)

        def summary():
            count, total, maximum, *over = take(8)
            if not count and (total or maximum or any(over)):
                raise ValueError('scheduler empty wake summary contains measurements')
            if total < maximum or total > count * maximum or any(v > count for v in over) or over != sorted(over, reverse=True):
                raise ValueError('scheduler wake summary counts or durations are inconsistent')
            for limit, number in zip(limits, over):
                if bool(number) != (maximum > frequency * limit // 1_000_000):
                    raise ValueError('scheduler wake summary threshold differs from maximum')
            return {'count': count, 'sum_ticks': total, 'max_ticks': maximum,
                    'over_us': dict(zip(map(str, limits), over))}

        for core in range(8):
            ready, running, flags, running_slot = take(4)
            stamps = list(take(5))
            service = list(take(5))
            passes, result, irq_sequence, previous_irq_end = take(4)
            begin, end, selected_slot, switch_sequence, previous_switch_end, switch_irq_sequence = take(6)
            if running < ready or flags & ~(127 if version == 5 else 63 if version == 4 else 31) or running_slot >= 128:
                raise ValueError('scheduler wake endpoints, flags or probe slot are invalid')
            if irq_sequence:
                if not all(stamps) or stamps != sorted(stamps) or previous_irq_end > stamps[0] or result not in (0, 1, 3):
                    raise ValueError('scheduler wake IRQ timing is invalid')
                if sum(service) > stamps[3] - stamps[2]:
                    raise ValueError('scheduler wake service exceeds owner work interval')
                if result == 3 and (len(set(stamps[1:])) != 1 or passes or any(service)):
                    raise ValueError('scheduler wake fast IRQ contains service work')
            elif any(stamps + service + [passes, result, previous_irq_end]):
                raise ValueError('scheduler wake IRQ fields lack a sequence')
            if switch_sequence:
                if not begin or end < begin or previous_switch_end > begin or selected_slot >= 128:
                    raise ValueError('scheduler wake switch timing or identity is invalid')
            elif any((begin, end, selected_slot, previous_switch_end, switch_irq_sequence)):
                raise ValueError('scheduler wake switch fields lack a sequence')
            history = histories[core] if histories else None
            lost = bool(flags & 32)
            snapshot_incomplete = bool(flags & 64) if version == 5 else False
            trailing = history['trailing_count'] if history else 0
            if history:
                if not ready and any(history.values()):
                    raise ValueError('scheduler empty wake contains history observations')
                if history['future_irqs'] > history_capacity:
                    raise ValueError('scheduler future IRQ count exceeds retained history')
                for sequence, overwritten in ((irq_sequence, history['irq_overwritten']),
                                               (history['latest_sequence'], history['irq_overwritten']),
                                               (switch_sequence, history['switch_overwritten'])):
                    if sequence and not overwritten < sequence <= overwritten + history_capacity:
                        raise ValueError('scheduler retained sequence is outside its history window')
                if not snapshot_incomplete and history['latest_sequence'] and history['irq_overwritten'] != max(
                        history['latest_sequence'] + history['future_irqs'] - history_capacity, 0):
                    raise ValueError('scheduler IRQ overwrite count differs from retained and future sequences')
                if lost and not (history['irq_overwritten'] or history['switch_overwritten']):
                    raise ValueError('scheduler history loss lacks overwritten records')
                expected_lost = bool((not switch_sequence and history['switch_overwritten']) or
                                     (switch_sequence and switch_irq_sequence and not irq_sequence) or
                                     (not history['latest_sequence'] and history['irq_overwritten']))
                if lost != expected_lost:
                    raise ValueError('scheduler history loss flag differs from missing required records')
                if history['latest_sequence']:
                    if not 0 < history['latest_start'] <= history['latest_end'] <= running or history['latest_result'] not in (0, 1, 3):
                        raise ValueError('scheduler latest IRQ is outside the measured interval')
                    if history['latest_work'] > history['latest_end'] - history['latest_start'] or history['latest_result'] == 3 and history['latest_work']:
                        raise ValueError('scheduler latest IRQ work interval is invalid')
                elif any(history[name] for name in ('latest_start', 'latest_end', 'latest_work', 'latest_result')):
                    raise ValueError('scheduler latest IRQ fields lack a sequence')
                if irq_sequence:
                    if history['latest_sequence'] < irq_sequence or trailing != history['latest_sequence'] - irq_sequence:
                        raise ValueError('scheduler trailing IRQ count differs from sequence distance')
                    if trailing:
                        if history['latest_start'] < stamps[4] or not history['latest_work'] <= history['trailing_work'] <= history['latest_end'] - stamps[4]:
                            raise ValueError('scheduler trailing IRQ work exceeds its interval')
                    elif (history['latest_start'], history['latest_end'], history['latest_work'], history['latest_result']) != (stamps[0], stamps[4], stamps[3] - stamps[2], result) or history['trailing_work']:
                        raise ValueError('scheduler latest IRQ differs from its selecting IRQ')
                elif trailing or history['trailing_work']:
                    raise ValueError('scheduler trailing IRQ work lacks a selecting IRQ')
            multiple = previous_irq_end > ready or previous_switch_end > ready or bool(trailing)
            linked = bool(irq_sequence and switch_sequence and not multiple and not lost and not snapshot_incomplete and
                          ready <= stamps[4] <= begin <= end <= running and
                          irq_sequence == switch_irq_sequence and selected_slot == running_slot and
                          result in ((0, 1, 3) if version >= 4 else (1, 3)))
            expected_flags = (1 if irq_sequence else 0) | (2 if switch_sequence else 0) | (8 if multiple else 0) | (32 if lost else 0)
            expected_flags |= 64 if snapshot_incomplete else 0
            expected_flags |= 4 if linked else 16
            if ready and flags != expected_flags:
                raise ValueError('scheduler wake linkage flags differ from its observations')
            if not ready and any([running, flags, running_slot, *stamps, *service, passes, result,
                                  irq_sequence, previous_irq_end, begin, end, selected_slot,
                                  switch_sequence, previous_switch_end, switch_irq_sequence]):
                raise ValueError('scheduler empty wake chain contains observations')
            kind = names[0 if result == 3 else 1] if linked else 'unmatched'
            summaries = {}
            for name in names:
                whole, tail = summary(), summary()
                if name == 'unmatched' and tail['count'] or name != 'unmatched' and tail['count'] != whole['count']:
                    raise ValueError('scheduler wake tail summary lacks matching classified samples')
                summaries[name] = {'whole_wake': whole, 'owner_exit_to_running': tail}
            total = sum(v['whole_wake']['count'] for v in summaries.values())
            maximum = max(v['whole_wake']['max_ticks'] for v in summaries.values())
            if bool(total) != bool(ready) or maximum != running - ready or ready and summaries[kind]['whole_wake']['max_ticks'] != running - ready:
                raise ValueError('scheduler retained wake maximum differs from its summaries')
            wake_cores.append({'core': core, 'scope': 'busy', 'count': total,
                'history': history,
                'classes': summaries, 'maximum': {
                    'ready': ready, 'running': running, 'flags': flags, 'running_slot': running_slot,
                    'class': kind, 'linked': linked, 'multiple_entries': multiple,
                    'snapshot_incomplete': snapshot_incomplete,
                    'whole_wake_ticks': running - ready,
                    'owner_exit_to_running_ticks': running - stamps[0] if linked else None,
                    'irq': {'stamp': stamps, 'service_ticks': dict(zip(('control', 'incoming', 'pull', 'expiry', 'slice'), service)),
                            'passes': passes, 'result': result, 'sequence': irq_sequence, 'previous_end': previous_irq_end},
                    'switch': {'begin': begin, 'end': end, 'selected_slot': selected_slot,
                               'sequence': switch_sequence, 'previous_end': previous_switch_end,
                               'irq_sequence': switch_irq_sequence}}})
    cpu0_window = None
    if version == 5:
        window_offset = offset + (1 + 8 * 10) * 8
        irq_count, switch_count, window_flags, capacity = struct.unpack_from('<4Q', payload, window_offset)
        window_offset += 32
        if capacity != history_capacity or max(irq_count, switch_count) > capacity or window_flags & ~15:
            raise ValueError('scheduler CPU0 history window header differs')
        if window_flags & 8 and irq_count == capacity:
            raise ValueError('scheduler CPU0 window claims an overrun but retains every row')
        maximum = wake_cores[0]['maximum']
        ready, running = maximum['ready'], maximum['running']
        irq_rows, switch_rows, owner_sequences = [], [], set()
        for index in range(capacity):
            sequence, descriptor, begin, end, pc, owner_begin, owner_end = \
                struct.unpack_from('<7Q', payload, window_offset)
            window_offset += 56
            if index >= irq_count:
                if any((sequence, descriptor, begin, end, pc, owner_begin, owner_end)):
                    raise ValueError('scheduler unused CPU0 IRQ row contains data')
                continue
            interrupt, spsr = descriptor & 0xffff, descriptor >> 16
            if (not 0 < begin <= end <= running or not pc or interrupt >= 1020 or spsr > 0xffffffff or
                    spsr & 31 not in (0, 4, 5) or irq_rows and begin < irq_rows[-1]['dispatch_begin']):
                raise ValueError('scheduler CPU0 IRQ identity or dispatch timing is invalid')
            if sequence:
                if sequence in owner_sequences or not end <= owner_begin <= owner_end <= running:
                    raise ValueError('scheduler CPU0 owner interval or sequence is invalid')
                owner_sequences.add(sequence)
            elif owner_begin or owner_end:
                raise ValueError('scheduler guarded CPU0 IRQ contains owner work')
            irq_rows.append({'sequence': sequence, 'interrupt': interrupt, 'spsr': spsr,
                'pc': pc, 'dispatch_begin': begin, 'dispatch_end': end,
                'dispatch_ticks': end - begin, 'owner_begin': owner_begin, 'owner_end': owner_end,
                'owner_ticks': owner_end - owner_begin,
                'overlaps_wake': end > ready or owner_end > ready})
        for index in range(capacity):
            begin, end, irq_sequence, slot = struct.unpack_from('<4Q', payload, window_offset)
            window_offset += 32
            if index >= switch_count:
                if any((begin, end, irq_sequence, slot)):
                    raise ValueError('scheduler unused CPU0 selection row contains data')
                continue
            if (not 0 < begin <= end <= running or slot >= 128 or
                    switch_rows and begin < switch_rows[-1]['end']):
                raise ValueError('scheduler CPU0 selection timing or task is invalid')
            if irq_sequence:
                origin = next((row for row in irq_rows if row['sequence'] == irq_sequence), None)
                if origin:
                    if origin['owner_end'] > begin:
                        raise ValueError('scheduler CPU0 selection precedes its owner exit')
                elif not window_flags & 13:
                    raise ValueError('scheduler CPU0 selection origin is missing without history loss')
            switch_rows.append({'begin': begin, 'end': end, 'irq_sequence': irq_sequence,
                'selected_slot': slot, 'origin': 'irq' if irq_sequence else 'svc',
                'ticks': end - begin, 'overlaps_wake': end > ready})
        if not ready and (irq_count or switch_count or window_flags):
            raise ValueError('scheduler empty CPU0 wake contains an event window')
        selected = maximum['switch']
        if switch_rows and selected['sequence'] and (switch_rows[-1]['begin'], switch_rows[-1]['end'],
                switch_rows[-1]['irq_sequence'], switch_rows[-1]['selected_slot']) != (
                selected['begin'], selected['end'], selected['irq_sequence'], selected['selected_slot']):
            raise ValueError('scheduler CPU0 window omits the latest selecting event')
        latest = histories[0]['latest_sequence']
        if latest and latest not in owner_sequences and not window_flags & 13:
            raise ValueError('scheduler CPU0 window omits the latest owner exit')
        cpu0_window = {'capacity': capacity, 'flags': window_flags,
            'irq_prefix_lost': bool(window_flags & 1), 'switch_prefix_lost': bool(window_flags & 2),
            'incomplete': bool(window_flags & 4), 'irq_overrun': bool(window_flags & 8),
            'ready': ready, 'running': running,
            'scope': 'outer C dispatch and ordinary owner exits',
            'irqs': irq_rows, 'selections': switch_rows}
    return {'version': version, 'waits_capacity': waits, 'ticks_capacity': tick_capacity,
            **({'restore_spans': restore_spans} if version == 5 else {}),
            'cores': cores, 'startup': startup, 'tick_count': tick_count,
            'primary_scheduler_start': startup[0]['scheduler_start'] if version >= 3 else None,
            'startup_scope': ('scheduler_start_to_final_software_return_point' if version == 5 and restore_spans is True else
                'scheduler_start_to_build_dependent_pre_eret_marker' if version == 5 and restore_spans is None else
                'scheduler_start_to_pre_eret_markers' if version >= 3 else 'port_start_to_restore_entry_lower_bound'),
            'tick_dropped': tick_dropped, 'ticks': ticks, 'wake_chains': wake_cores, 'history_capacity': history_capacity, 'cpu0_wake_window': cpu0_window}


def decode_control(payload, frequency):
    """Validate the stopped control recorder and report measurement qualification.

    Counts, errors and missed deadlines remain visible even when they prevent
    qualification. Inconsistent fields or an unknown binary layout are rejected.
    """
    if len(payload) != CONTROL_BYTES:
        raise ValueError('control qualification report length differs')
    magic, version, hz, minimum, requested, mode, ready, quiescent, parked, errors, complete, flags = \
        struct.unpack_from('<4Q8I', payload)
    if (magic != CONTROL_MAGIC or version != 1 or not 0 < hz <= 0xffffffff or hz != frequency or
            minimum != CONTROL_MINIMUM or requested != 0xfe or mode not in (0, 1) or
            flags not in (1, 3, 5, 7) or errors & ~15 or parked not in (0, 1) or
            any(mask & ~requested for mask in (ready, quiescent, complete))):
        raise ValueError('control qualification header differs')
    reasons = []
    if ready != requested:
        reasons.append('not_all_cores_armed')
    if quiescent != requested or not parked:
        reasons.append('writers_not_quiescent')
    rows, actual_complete, actual_errors = [], 0, 0
    stats_fields = ('path', 'core', 'rate_hz', 'count', 'consumed', 'gaps', 'dropped', 'missed',
                    'latency_max_ns', 'latency_p9999_ns', 'jitter_max_ns', 'jitter_p9999_ns')
    rates = (1000, 2000, 4000, 8000, 10000, 4000, 4000) if mode == 0 else (10000,) * 7

    def tick_range(ns, error_flags):
        if ns == 0xffffffff and error_flags & 4:
            return ((ns * hz + 999999999) // 1000000000, 0xffffffff)
        low = (ns * hz + 999999999) // 1000000000
        high = ((ns + 1) * hz + 999999999) // 1000000000 - 1
        if low > high or high < 0 or low > 0xffffffff:
            raise ValueError('control metric cannot represent counter ticks')
        return low, min(high, 0xffffffff)

    def ns(ticks):
        return min(0xffffffff, ticks * 1000000000 // hz)

    def metric(bins, count, maximum, percentile, error_flags, *, raw_ticks=False):
        if sum(bins) != count:
            raise ValueError('control histogram count differs from its path')
        if not count:
            if maximum or percentile:
                raise ValueError('empty control histogram has timing data')
            return {'count': 0, 'max_ns': 0, 'p9999_upper_ns': 0,
                    'p9999_upper_ticks': 0, 'saturated_count': 0}
        last = max(b for b, value in enumerate(bins) if value)
        low, high = (maximum, maximum) if raw_ticks else tick_range(maximum, error_flags)
        low, high = max(low, last * CONTROL_BIN_TICKS), min(high,
            (last + 1) * CONTROL_BIN_TICKS - 1 if last < CONTROL_BINS - 1 else 0xffffffff)
        if low > high:
            raise ValueError('control histogram maximum differs from its highest bin')
        rank, cumulative, selected = count - count // 10000, 0, 0
        for selected, value in enumerate(bins):
            cumulative += value
            if cumulative >= rank:
                break
        # The recorder returns the exact maximum below its declared minimum.
        use_maximum = count < CONTROL_MINIMUM or selected == CONTROL_BINS - 1
        upper_ticks = high if use_maximum else (selected + 1) * CONTROL_BIN_TICKS
        expected = maximum if use_maximum else (upper_ticks if raw_ticks else ns(upper_ticks))
        if percentile != expected:
            raise ValueError('control percentile differs from nearest-rank histogram endpoint')
        return {'count': count, 'max_ns': ns(maximum) if raw_ticks else maximum,
                'max_ticks_interval': [low, high], 'p9999_upper_ticks': upper_ticks,
                'p9999_upper_ns': ns(percentile) if raw_ticks else percentile,
                'saturated_count': bins[-1]}

    for index, rate in enumerate(rates):
        offset = 64 + index * 2496
        core, rate_hz, paths, error_flags = struct.unpack_from('<4I', payload, offset)
        if (core != index + 1 or rate_hz != rate or paths != (3 if mode == 0 else 1) or
                error_flags & ~15 or hz < rate):
            raise ValueError('control core, rate, paths or error flags differ')
        stats = [dict(zip(stats_fields, struct.unpack_from('<3I4x5Q4I', payload, offset + 16 + path * 72)))
                 for path in range(2)]
        entry_max, gap_max, entry_p9999, gap_p9999, published_control, published_task = \
            struct.unpack_from('<4I2Q', payload, offset + 160)
        histograms = [struct.unpack_from(f'<{CONTROL_BINS}I', payload,
                      offset + 192 + h * CONTROL_BINS * 4) for h in range(3)]
        for path, row in enumerate(stats):
            if not paths & (1 << path):
                if any(row.values()):
                    raise ValueError('unselected control path contains statistics')
                continue
            if (row['path'], row['core'], row['rate_hz']) != (path, core, rate):
                raise ValueError('control statistics identity differs')
            if row['count'] != (published_task if path else published_control):
                raise ValueError('control published count differs from final statistics')
            if row['consumed'] != (row['count'] if path else 0):
                raise ValueError('control histogram-only consumption count differs')
            for name in ('latency', 'jitter'):
                count = row['count'] if name == 'latency' else max(0, row['count'] - 1)
                maximum, percentile = row[name + '_max_ns'], row[name + '_p9999_ns']
                if not count and (maximum or percentile):
                    raise ValueError('empty control metric has statistics')
                if count:
                    low, high = tick_range(maximum, error_flags)
                    endpoints = {ns((b + 1) * CONTROL_BIN_TICKS)
                                 for b in range(min(high // CONTROL_BIN_TICKS + 1, CONTROL_BINS - 1))}
                    if high >= (CONTROL_BINS - 1) * CONTROL_BIN_TICKS:
                        endpoints.add(maximum)
                    if (count < CONTROL_MINIMUM and percentile != maximum or
                            count >= CONTROL_MINIMUM and percentile not in endpoints):
                        raise ValueError('control statistics percentile exceeds its maximum bin')
            for name in ('gaps', 'dropped', 'missed'):
                if row[name]:
                    reasons.append(f'core{core}_{"task" if path else "control"}_{name}')
        if error_flags:
            reasons.append(f'core{core}_runtime_errors')
        actual_errors |= error_flags
        if published_control > minimum and (mode == 1 or published_task >= minimum):
            actual_complete |= 1 << core
        else:
            reasons.append(f'core{core}_insufficient_samples')
        task_latency = metric(histograms[0], stats[1]['count'], stats[1]['latency_max_ns'],
                              stats[1]['latency_p9999_ns'], error_flags)
        task_gap = metric(histograms[1], stats[1]['count'], gap_max, gap_p9999, error_flags, raw_ticks=True)
        control_jitter = metric(histograms[2], max(0, stats[0]['count'] - 1),
                                stats[0]['jitter_max_ns'], stats[0]['jitter_p9999_ns'], error_flags)
        if mode == 1 and (published_task or entry_max or entry_p9999 or gap_max or gap_p9999):
            raise ValueError('idle control-only measurement contains task data')
        entry_upper = ((entry_max // CONTROL_BIN_TICKS + 1) * CONTROL_BIN_TICKS
                       if entry_max < (CONTROL_BINS - 1) * CONTROL_BIN_TICKS else entry_max)
        if entry_p9999 > entry_upper:
            raise ValueError('task interrupt-entry percentile exceeds its maximum bin')
        rows.append({'core': core, 'rate_hz': rate, 'period_ticks': hz // rate,
            'paths': paths, 'error_flags': error_flags,
            'stats': stats, 'published_control': published_control, 'published_task': published_task,
            'task_latency': task_latency, 'task_gap': task_gap, 'control_jitter': control_jitter,
            'task_entry': {'max_ticks': entry_max, 'p9999_upper_ticks': entry_p9999},
            'histograms': dict(zip(('task_latency', 'task_gap', 'control_jitter'), histograms))})
    if actual_errors != errors or actual_complete != complete:
        raise ValueError('control error or completion summary differs from its cores')
    task_target = all(row['task_latency']['p9999_upper_ticks'] * 1000000 <= 10 * hz for row in rows) if mode == 0 else None
    return {'version': version, 'frequency_hz': hz, 'minimum_samples': minimum,
        'mode': 'busy' if mode == 0 else 'idle', 'tickless': bool(flags & 2),
        'restore_spans': bool(flags & 4), 'flags': flags,
        'requested_mask': requested, 'ready_mask': ready, 'quiescent_mask': quiescent,
        'consumer_parked': parked, 'complete_mask': complete, 'errors': errors,
        'quantile_method': 'nearest-rank 99.99th percentile; four-tick bin upper endpoints; exact maximum in saturated bin or below 10000 samples',
        'measurement_qualified': not reasons, 'unqualified_reasons': reasons,
        'task_wake_target': {'interval': 'deadline_to_task_running', 'target_us': 10,
                             'met': task_target, 'qualified': not reasons and task_target is True},
        'cores': rows}


def decode_scheduler_pmu(payload, frequency):
    """Decode separate event banks without joining counts from different spans."""
    if len(payload) != SCHED_PMU_BYTES or not frequency:
        raise ValueError('scheduler PMU report length or frequency differs')
    header = struct.unpack_from('<8Q', payload)
    if header[:7] != (SCHED_PMU_MAGIC, 1, SCHED_PMU_BYTES, 8, 5, 2, 6) or header[7] not in (6, 14):
        raise ValueError('scheduler PMU header or raw-count flags differ')
    if header[7] & 8:
        return {'version': 1, 'raw_counts': True, 'snapshot_complete': False, 'cores': [],
                'nested_rejection_count_available': False, 'reason': 'captures_not_quiescent'}
    cores = []
    for core in range(8):
        words = struct.unpack_from('<182Q', payload, 64 + core * 1456)
        status, midr, counters, ceid0, ceid1, support0, support1, configured, attempts, completed, *reserved = words[:12]
        if (status >= len(SCHED_PMU_STATUS) or midr > 0xffffffff or counters > 31 or
                configured & ~3 or any(reserved) or
                completed > attempts or (support0 | support1) & ~63):
            raise ValueError('scheduler PMU core metadata differs')
        supports = (support0, support1)
        if status == 0:
            if not midr or counters < 6:
                raise ValueError('scheduler PMU configured core has no identity or counters')
            for bank, events in enumerate(SCHED_PMU_EVENTS):
                expected = sum(1 << index for index, event in enumerate(events)
                    if index < counters and ((ceid0 >> event) & 1 if event < 32 else
                                             (ceid1 >> (event - 32)) & 1))
                if supports[bank] != expected:
                    raise ValueError('scheduler PMU support mask differs from core capabilities')
        rows, observed = [], 0
        for kind, name in enumerate(SCHED_PMU_KINDS):
            for bank in range(2):
                data = words[12 + (kind * 2 + bank) * 17:12 + (kind * 2 + bank + 1) * 17]
                count, nested, row_status, begin, end, cycles, *rest = data
                events, supported, overflow, slot, flags, cookie = rest[:6], *rest[6:]
                row = {'kind': name, 'bank': bank, 'count': count}
                if not count:
                    if any(data):
                        raise ValueError('scheduler PMU empty bank contains a sample')
                    row.update(observed=False, reason='no_sample_in_this_bank')
                    rows.append(row)
                    continue
                if (nested or row_status >= len(SCHED_PMU_STATUS) or supported != supports[bank] or
                        overflow & ~0x8000003f or flags != (7 if kind == 0 else 6) or
                        not cookie or end < begin or any(value > 0xffffffff for value in events) or
                        not configured & (1 << bank)):
                    raise ValueError('scheduler PMU sample identity, flags or counters differ')
                if kind < 3 and slot >= 128 or kind == 3 and core:
                    raise ValueError('scheduler PMU sample has an invalid task slot or timer core')
                if row_status == 0 and (not begin or (end == begin and kind != 4) or not cycles or overflow):
                    raise ValueError('scheduler PMU successful sample has no interval or overflow')
                if any(value for index, value in enumerate(events) if not supported & (1 << index)):
                    raise ValueError('scheduler PMU unsupported event has a count')
                names = ('l1d_refill', 'l1i_refill', 'l2_refill',
                         'l3_refill' if bank == 0 else 'stall_backend', 'dtlb_walk', 'itlb_walk')
                valid = row_status == 0 and not overflow
                elapsed = end - begin
                mhz = cycles * frequency / (elapsed * 1000000) if valid and elapsed else None
                expected_mhz = 1794 if core < 6 else 2002
                row.update(observed=True, status=SCHED_PMU_STATUS[row_status], supported_mask=supported,
                    overflow_mask=overflow, slot=slot, flags=flags, cookie=cookie,
                    begin_tick=begin, end_tick=end, elapsed_ticks=elapsed,
                    time_us=elapsed * 1000000 / frequency, cycles=cycles,
                    below_one_counter_tick=not elapsed,
                    event_codes=list(SCHED_PMU_EVENTS[bank]), raw_events=list(events),
                    events={event_name: value if valid and supported & (1 << index) else None
                            for index, (event_name, value) in enumerate(zip(names, events))},
                    effective_mhz=mhz, expected_mhz=expected_mhz,
                    top_clock_consistent=abs(mhz - expected_mhz) <= expected_mhz * .1 if mhz is not None else None,
                    nested_rejection_count=None,
                    event_scope='one retained invocation, not totals over count')
                rows.append(row)
                observed += count
        if observed != completed:
            raise ValueError('scheduler PMU completed count differs from captured rows')
        cores.append({'core': core, 'status': SCHED_PMU_STATUS[status], 'midr': midr,
                      'counter_count': counters, 'pmceid0': ceid0, 'pmceid1': ceid1,
                      'supported': list(supports), 'configured_banks': configured,
                      'attempts': attempts, 'completed': completed, 'rows': rows})
    return {'version': 1, 'raw_counts': True, 'snapshot_complete': not bool(header[7] & 8),
            'nested_rejection_count_available': False, 'cores': cores,
            'bank_scope': 'each bank is a separate invocation; event counts are not joined',
            'retention': 'maximum-cycle invocation per core, kind and bank, or an error sample; count includes every completed invocation',
            'clock_scope': 'cycle/counter ratio includes boundary skew; ten-percent top-clock comparison',
            'recorder_scope': 'PMU publication follows counter freeze; existing latency diagnostics remain in the measured path'}


def cycle_distribution(values):
    ordered = sorted(values)
    if not ordered or ordered[0] <= 0:
        raise ValueError('cause samples must contain positive raw cycles')
    return {'count': len(ordered), 'minimum': ordered[0],
            'median': ordered[(len(ordered) - 1) // 2],
            'p99': ordered[(99 * len(ordered) + 99) // 100 - 1],
            'maximum': ordered[-1], 'raw_cycles': list(values)}


def wake_masks(payload):
    """The release and scan masks a wake segment report names (version 2 and later), or all eight cores (version 1)."""
    if not payload:
        return 0xff, 0xff
    version, release, scan = (struct.unpack_from('<Q', payload, 8)[0], *struct.unpack_from('<2Q', payload, 64))
    return (release, scan) if version >= 2 else (0xff, 0xff)


def timer_core_mask(irq_mask, wake_tail):
    """The cores that may take timer samples: the header's mask, plus, in a cause build, the cores
    CPU0's tick does not scan, which arm their own one-shot timers (configSS_LOCAL_TIMERS)."""
    return irq_mask | (0xff & ~wake_masks(wake_tail)[1])


def decode_latency_cause(payload, frequency, userenr=4, release_mask=0xff, scan_mask=0xff):
    if len(payload) != CAUSE_BYTES:
        raise ValueError('latency cause report has the wrong size')
    words = struct.unpack('<' + 'Q' * (CAUSE_BYTES // 8), payload)
    offset = 0

    def take(names):
        nonlocal offset
        fields = names.split()
        result = dict(zip(fields, words[offset:offset + len(fields)]))
        offset += len(fields)
        return result

    header = take('magic version bytes cores phases cases samples flags')
    if tuple(header.values())[:7] != (CAUSE_MAGIC, 4, CAUSE_BYTES, 8, 2, 15, 64) or header['flags'] != 13:
        raise ValueError('latency cause header is invalid or its snapshot is incomplete')
    cores = []
    for core in range(8):
        row = take('status midr mpidr pmcr userenr filter enabled overflow irq_count ready frozen '
                   'icc_ctlr pmr sgi_irq_count cycle_begin tick_begin cycle_end tick_end')
        if (row['status'] or row['ready'] != 1 or row['frozen'] != 1 or row['overflow'] or
                row['pmcr'] & 0x4f != 0x41 or row['userenr'] != userenr or row['filter'] or
                row['enabled'] != (1 << 31) | 0x1f or row['cycle_end'] <= row['cycle_begin'] or
                row['tick_end'] <= row['tick_begin']):
            raise ValueError(f'cause core {core} configuration or clock interval is invalid')
        row.update(core=core, pmhe=bool(row['icc_ctlr'] & (1 << 6)),
                   wall_cycle_ratio_mhz=(row['cycle_end'] - row['cycle_begin']) * frequency /
                       (row['tick_end'] - row['tick_begin']) / 1000000)
        cores.append(row)
    sends = []
    for source in range(8):
        for target in range(8):
            row = take('count total_cycles max_cycles interrupted')
            if row['max_cycles'] > row['total_cycles'] or (not row['count'] and any(row.values())):
                raise ValueError('cause SGI aggregate is inconsistent')
            if row['count']:
                row.update(source=source, target=target)
                sends.append(row)
    phases = {}
    for phase in ('idle', 'busy'):
        rows = []
        for target in range(8):
            row = take('release_count release_total release_max release_sgi_total release_sgi_max release_sends '
                       'state_count state_total state_max state_sgi_total state_sgi_max state_sends '
                       'mask_tail_total mask_tail_max')
            if (row['release_max'] > row['release_total'] or row['state_max'] > row['state_total'] or
                    row['release_sgi_total'] > row['release_total'] or
                    row['state_sgi_total'] > row['state_total'] or
                    row['mask_tail_total'] > row['release_total']):
                raise ValueError('cause target aggregate is inconsistent')
            row['target'] = target
            rows.append(row)
        phases[phase] = rows
    tick = take('count begin_tick end_tick begin_cycles end_cycles release_mask state_mask interrupted')
    # A complete tick released every core of the release mask and read the deadlines of every core of the scan mask;
    # with local timers CPU0's tick reads its own deadlines only (kernel/scheduler/freertos.c, ssKernelTick).
    if (not tick['count'] or tick['release_mask'] != release_mask or tick['state_mask'] != scan_mask or
            tick['end_cycles'] <= tick['begin_cycles'] or tick['end_tick'] <= tick['begin_tick']):
        raise ValueError('cause report lacks a complete correlated tick for its release and scan masks')
    targets = []
    for target in range(8):
        row = take('release_begin release_end release_sgi_cycles release_sends '
                   'state_begin state_end state_sgi_cycles state_sends deadline_state deadline '
                   'slice_deadline due mask_tail_begin mask_tail_end')
        for part, mask in (('release', release_mask), ('state', scan_mask)):
            begin, end = row[part + '_begin'], row[part + '_end']
            if mask >> target & 1:
                if not tick['begin_cycles'] <= begin < end <= tick['end_cycles']:
                    raise ValueError('cause component lies outside its retained tick')
            elif begin or end or row[part + '_sgi_cycles'] or row[part + '_sends']:
                raise ValueError('cause tick holds a component for a core outside its masks')
        release = row['release_end'] - row['release_begin']
        if row['mask_tail_begin'] or row['mask_tail_end']:
            if not row['release_begin'] <= row['mask_tail_begin'] < row['mask_tail_end'] <= row['release_end']:
                raise ValueError('cause mask tail lies outside its release')
        mask_tail = row['mask_tail_end'] - row['mask_tail_begin']
        if row['release_sgi_cycles'] + mask_tail > release:
            raise ValueError('cause SGI and mask-tail components exceed their release')
        row.update(target=target, release_cycles=release, mask_tail_cycles=mask_tail,
                   release_residual_cycles=release - row['release_sgi_cycles'] - mask_tail,
                   deadline_scan_cycles=row['state_end'] - row['state_begin'])
        targets.append(row)
    tick['targets'] = targets
    tick['cycles'] = tick['end_cycles'] - tick['begin_cycles']
    tick['time_us'] = (tick['end_tick'] - tick['begin_tick']) * 1000000 / frequency
    tick['effective_mhz'] = tick['cycles'] / tick['time_us']
    tick['top_clock_consistent'] = abs(tick['effective_mhz'] - 1794) <= 179.4
    tick['outside_release_and_deadline_cycles'] = tick['cycles'] - sum(
        row['release_cycles'] + row['deadline_scan_cycles'] for row in targets)
    if tick['outside_release_and_deadline_cycles'] < 0:
        raise ValueError('cause component intervals exceed their tick')
    bench = take('requested done status sgi_sends sgi_observed delivery_wait_ticks pending '
                 'observer_count observer_min observer_median observer_max')
    counts = words[offset:offset + 15]
    offset += 15
    if (bench['requested'] != 1 or bench['done'] != 1 or bench['status'] or bench['pending'] or
            tuple(counts) != (64,) * 15 or bench['sgi_sends'] != 384 or
            bench['observer_count'] != 64 or
            not 0 < bench['observer_min'] <= bench['observer_median'] <= bench['observer_max'] or
            bench['sgi_observed'] < bench['sgi_sends']):
        raise ValueError('cause barrier samples or SGI completion coverage are incomplete')
    cases = {}
    for name, count in zip(CAUSE_CASES, counts):
        cases[name] = cycle_distribution(words[offset:offset + count])
        offset += 64
    bench['cases'] = cases

    def grid(*shape):
        nonlocal offset
        count = 1
        for extent in shape:
            count *= extent
        flat = list(words[offset:offset + count])
        offset += count
        for extent in reversed(shape[1:]):
            flat = [flat[i:i + extent] for i in range(0, len(flat), extent)]
        return flat

    types = grid(5)
    if tuple(types) != CAUSE_EVENT_TYPES:
        raise ValueError('cause event types differ from the declared counters')
    pmceid0 = grid(8)
    # PMCEID0_EL0 bits [31:0] advertise common events 0x00-0x1f; events 0x20-0x3f
    # (L3D refill 0x2a, backend stall 0x24) are in PMCEID1_EL0, which this report
    # does not hold. The scheduler PMU record 1794497365941804086-44d410dc read
    # both registers and found all six of its events, these two included, on every core.
    for core, value in enumerate(pmceid0):
        missing = [name for name, event in zip(CAUSE_EVENTS, CAUSE_EVENT_TYPES)
                   if event < 32 and not value >> event & 1]
        if missing:
            raise ValueError(f'cause core {core} does not implement {missing}')
    ev_tick, ev_release, ev_scan = grid(5), grid(8, 5), grid(8, 5)
    phase_ticks, phase_cycles, phase_outside_cycles = grid(2), grid(2), grid(2)
    phase_events, phase_outside = grid(2, 5), grid(2, 5)
    phase_release_cycles, phase_scan_cycles = grid(2, 8), grid(2, 8)
    phase_release, phase_scan = grid(2, 8, 5), grid(2, 8, 5)
    dsu_fields = ('captured', 'clusteridr', 'clustercfr', 'clusterpartcr', 'clusterthreadsid', 'actlr_el3',
                  'clusterpwrstat', 'clusteracpsid', 'clusterstashsid', 'clusterthreadsidovr',
                  'threadsid_set', 'threadsid_after', 'partition', 'partcr_after')
    dsu_rows = [dict(zip(dsu_fields, row)) for row in grid(8, len(dsu_fields))]
    if offset != len(words):
        raise ValueError('cause decoder and wire report disagree')

    def named(values, divisor=1):
        return {name: value / divisor for name, value in zip(CAUSE_EVENTS, values)}
    for target, row in enumerate(targets):
        row['release_events'] = named(ev_release[target])
        row['deadline_scan_events'] = named(ev_scan[target])
    outside_events = [ev_tick[i] - sum(ev_release[t][i] + ev_scan[t][i] for t in range(8)) for i in range(5)]
    if any(value < 0 for value in outside_events):
        raise ValueError('cause segment events exceed their tick')
    tick['events'] = named(ev_tick)
    tick['outside_events'] = named(outside_events)
    complete = {}
    for index, phase in enumerate(('idle', 'busy')):
        n = phase_ticks[index]
        if not n:
            complete[phase] = {'ticks': 0}
            continue
        complete[phase] = {
            'ticks': n, 'mean_cycles': phase_cycles[index] / n,
            'mean_events': named(phase_events[index], n),
            'mean_outside_cycles': phase_outside_cycles[index] / n,
            'mean_outside_events': named(phase_outside[index], n),
            'targets': [{'target': t, 'mean_release_cycles': phase_release_cycles[index][t] / n,
                         'mean_release_events': named(phase_release[index][t], n),
                         'mean_scan_cycles': phase_scan_cycles[index][t] / n,
                         'mean_scan_events': named(phase_scan[index][t], n)} for t in range(8)]}
    events = {'types': dict(zip(CAUSE_EVENTS, types)), 'complete_ticks': complete,
              'scope': 'raw counts with hook bookkeeping included; L3D refill is core-attributable; '
                       'phase means cover ticks with every release and scan of the release and scan masks',
              'release_mask': release_mask, 'scan_mask': scan_mask}
    bench['extra_sgi_observations'] = bench['sgi_observed'] - bench['sgi_sends']
    # A missing boot capture is reported, not refused: the register values are
    # a record beside the measurement and never gate it. A partition write is
    # part of the measurement, so its record must be consistent.
    partition = dsu_rows[0]['partition']
    for core, row in enumerate(dsu_rows):
        written = (row['threadsid_set'], row['threadsid_after'], row['partition'], row['partcr_after'])
        if not partition:
            if any(written):
                raise ValueError(f'cause core {core} records a DSU write in an unpartitioned build')
        elif (row['captured'] != 1 or row['threadsid_set'] > 7 or
                row['threadsid_after'] & 7 != row['threadsid_set'] or
                row['partition'] != (partition if core == 0 else 0) or
                row['partcr_after'] != (partition if core == 0 else 0)):
            raise ValueError(f'cause core {core} L3 partition write or readback is inconsistent')
    cluster = ('clusteridr', 'clustercfr', 'clusterpartcr', 'clusterpwrstat', 'clusteracpsid', 'clusterstashsid')
    dsu = {'cores': [dict({k: hex(v) for k, v in row.items() if k != 'captured'},
                          core=core, captured=row['captured'] == 1)
                     for core, row in enumerate(dsu_rows)],
           'complete': all(row['captured'] == 1 for row in dsu_rows),
           'cluster_registers_agree': len({tuple(row[k] for k in cluster)
                                           for row in dsu_rows if row['captured'] == 1}) <= 1,
           'boot_partition_clear': all(row['clusterpartcr'] == 0 and row['clusterthreadsid'] == 0
                                       for row in dsu_rows if row['captured'] == 1),
           'partition': ({'clusterpartcr': hex(partition), 'readback': hex(dsu_rows[0]['partcr_after']),
                          'scheme_ids': [row['threadsid_after'] & 7 for row in dsu_rows]}
                         if partition else None),
           'scope': ('read once per core at EL3 during boot, before any write; a partitioned build then sets '
                     'each core\'s scheme ID and CPU0 the cluster partition, and any entry to EL3 clears both')}
    return {'header': header, 'cores': cores, 'sgi': sends, 'phases': phases,
            'tick': tick, 'benchmark': bench, 'events': events, 'dsu': dsu,
            'component_scope': 'raw instrumented cycles; hook bookkeeping and its interaction with DSB remain included',
            'benchmark_scope': 'one IRQ-masked leaf sample per tick; matching blank and common endpoint ISB included; stores follow endpoint',
            'sgi_scope': 'raw and context-synchronized arms differ; SGI-only does not establish target delivery',
            'clock_scope': 'active CPU0 tick gives its cycle frequency; first/latest IRQ ratios can include WFI and are wall-interval coverage only',
            'deadline_scope': 'state fields describe the production per-CPU deadline scan; release residual also contains API guards and existing observers'}


def decode_latency_cause_ping(payload, frequency, core_mhz):
    if len(payload) != CAUSE_PING_BYTES:
        raise ValueError('cause ping report has the wrong size')
    magic, version, size, samples, freq, begin, end, status = struct.unpack_from('<8Q', payload)
    if ((magic, version, size, samples) != (CAUSE_PING_MAGIC, 1, CAUSE_PING_BYTES, 256) or
            freq != frequency or status != 1 or end <= begin or end - begin > frequency):
        raise ValueError('cause ping header or bounded completion is invalid')
    ref = dict(zip(('core', 'group', 'status', 'completed', 'begin_tick', 'end_tick', 'irq_begin', 'irq_end'),
                   struct.unpack_from('<8Q', payload, 64)))
    if (ref['core'], ref['group'], ref['status'], ref['completed']) != (0, 0, 1, 256) or ref['irq_end'] < ref['irq_begin']:
        raise ValueError('cause ping local reference is incomplete')
    ref['cycles'] = cycle_distribution(struct.unpack_from('<256I', payload, 128))
    pairs = []
    for index, target in enumerate((1, 6)):
        offset = 1152 + index * 1120
        row = dict(zip(('source_core', 'target_core', 'source_group', 'target_group', 'status', 'completed',
                        'begin_tick', 'end_tick', 'source_irq_begin', 'source_irq_end', 'target_irq_begin', 'target_irq_end'),
                       struct.unpack_from('<12Q', payload, offset)))
        if ((row['source_core'], row['target_core'], row['source_group'], row['target_group'], row['status'], row['completed']) !=
                (0, target, 0, int(target == 6), 1, 256) or row['end_tick'] < row['begin_tick'] or
                row['source_irq_end'] < row['source_irq_begin'] or row['target_irq_end'] < row['target_irq_begin']):
            raise ValueError('cause ping pair identity, sequence or coverage is invalid')
        row['roundtrip_cycles'] = cycle_distribution(struct.unpack_from('<256I', payload, offset + 96))
        row['estimated_transfer_median_cycles'] = row['roundtrip_cycles']['median'] / 2
        row['estimated_transfer_median_us'] = row['estimated_transfer_median_cycles'] / core_mhz
        row['half_difference_of_medians_cycles'] = (row['roundtrip_cycles']['median'] - ref['cycles']['median']) / 2
        pairs.append(row)
    return {'status': 'ok', 'frequency': frequency, 'begin_tick': begin, 'end_tick': end,
            'reference': ref, 'pairs': pairs,
            'scope': 'CPU0 measures round trips on its own cycle counter; half RTT includes peer instructions, polling and IRQ interference',
            'topology_scope': 'CPU-L and CPU-B are clock/type groups sharing the documented DSU; no separate DSU hop is asserted'}


def service_bin_bounds(index):
    """Cycles [low, high] held by a service cycle bin (kernel/scheduler/service_cycles.h)."""
    if index < 8:
        return index * 8, index * 8 + 7
    if index == SERVICE_BINS - 1:
        return 1 << 18, None
    exponent, part = 6 + (index - 8) // 8, (index - 8) % 8
    step = 1 << (exponent - 3)
    low = (1 << exponent) + part * step
    return low, low + step - 1


def service_quantile(bins, count, fraction):
    """Upper bound of the bin holding the given quantile; None for an empty cell or the open last bin."""
    if not count:
        return None
    target, seen = max(1, -(-count * fraction // 1)), 0
    for index, value in enumerate(bins):
        seen += value
        if seen >= target:
            return service_bin_bounds(index)[1]
    raise ValueError('service cycle bins hold fewer spans than the count')


def peel_wake_segments(payload, result_size=None):
    """Split the wake segment section, which comes last unless the cap section follows it, from the result. Its footer
    records the whole result's size."""
    if len(payload) < 64 or struct.unpack_from('<Q', payload, len(payload) - 64)[0] != WAKE_FOOTER_MAGIC:
        return payload, b''
    _, version, size, at, length, *rest = struct.unpack_from('<8Q', payload, len(payload) - 64)
    if (version != 1 or size != (len(payload) if result_size is None else result_size) or
            length not in WAKE_BYTES_BY_VERSION.values() or at % 64 or
            at + length > len(payload) - 64 or any(rest) or any(payload[at + length:-64])):
        raise ValueError('wake segment footer offsets, padding or report size are invalid')
    return payload[:at], payload[at:at + length]


def peel_axi_cap(payload):
    """Split the stage 3a cap section, which comes last, from the result."""
    if len(payload) < 64 or struct.unpack_from('<Q', payload, len(payload) - 64)[0] != AXI_FOOTER_MAGIC:
        return payload, b''
    _, version, size, at, length, *rest = struct.unpack_from('<8Q', payload, len(payload) - 64)
    if (version != 1 or size != len(payload) or length != AXI_BYTES or at % 64 or
            at + length > len(payload) - 64 or any(rest) or any(payload[at + length:-64])):
        raise ValueError('cap footer offsets, padding or report size are invalid')
    return payload[:at], payload[at:at + length]


def decode_axi_cap(payload, frequency, stream_mask):
    """The AXI2HIF cap of stage 3a: the entry words, the warm-up windows, each busy sub-phase's stream rate and every
    bridge operation with its cost and read-back (docs/designs/concurrent-accel.md)."""
    words = struct.unpack_from(AXI_HEADER, payload)
    magic, version, size, bw_b, sat_b, bw_nb, sat_nb = words[:7]
    entry = [list(words[7 + 7 * b:14 + 7 * b]) for b in range(2)]
    subphase_wakes, capped_mask, switch_count, complete = words[21:25]
    if (magic, version, size) != (AXI_MAGIC, 1, AXI_BYTES) or switch_count > AXI_SWITCHES:
        raise ValueError('cap section header is invalid')
    if subphase_wakes * AXI_SUBPHASES != 128 or capped_mask != 0x6 or (bw_b, sat_b, bw_nb, sat_nb) != (753, 483, 4095, 1023):
        raise ValueError('cap section names other sub-phases or words than stage 3a')
    at = struct.calcsize(AXI_HEADER)

    def window():
        nonlocal at
        values = struct.unpack_from(AXI_WINDOW, payload, at)
        at += struct.calcsize(AXI_WINDOW)
        start, end, before, after = values[0], values[1], values[2:10], values[10:18]
        if not start or end <= start or any(b > a for b, a in zip(before, after)):
            raise ValueError('cap window stamps or sweep counts are out of order')
        seconds = (end - start) / frequency
        sweeps = [a - b for b, a in zip(before, after)]
        if any(sweeps[c] for c in range(8) if not stream_mask >> c & 1):
            raise ValueError('a core outside the streaming mask swept in a cap window')
        return {'start': start, 'end': end, 'seconds': seconds, 'sweeps': sweeps,
                'gb_per_second': sum(sweeps) * AXI_SWEEP_BYTES / seconds / 1e9}
    warm = [window() for _ in range(2)]
    sub = [window() for _ in range(AXI_SUBPHASES)]
    switches = []
    for index in range(AXI_SWITCHES):
        values = struct.unpack_from(AXI_SWITCH, payload, at)
        at += struct.calcsize(AXI_SWITCH)
        if index >= switch_count:
            if any(values):
                raise ValueError('cap switch record beyond the count is not empty')
            continue
        begin, end, access, op, status, g0, g1, g2, accesses = values[:9]
        readback = [list(values[9 + 7 * b:16 + 7 * b]) for b in range(2)]
        if op not in AXI_OPS or status >= len(AXI_STATUS) or end < begin or access > end - begin:
            raise ValueError('cap switch record is invalid')
        switches.append({'op': AXI_OPS[op], 'status': AXI_STATUS[status], 'us': (end - begin) * 1e6 / frequency,
                         'access_us': access * 1e6 / frequency, 'accesses': accesses, 'begin': begin, 'end': end,
                         'gates': ['0x%08x' % g for g in (g0, g1, g2)],
                         'readback': [{'0x%02x' % w: '0x%x' % v for w, v in zip(AXI_WORDS, row)} for row in readback]})
    ops = [s['op'] for s in switches]
    expected = ['entry', 'set_nonbinding', 'clear', 'set_b', 'clear', 'set_b', 'clear', 'restore']
    if complete != 1 or ops != expected or any(s['status'] != 'ok' for s in switches) or any(any(row) for row in entry):
        raise ValueError('cap run is incomplete, failed an operation or found a word set at entry: %s' %
                         [(s['op'], s['status']) for s in switches])
    if not all(sub[k]['start'] >= warm[1]['end'] for k in range(AXI_SUBPHASES)) or \
            any(sub[k]['start'] < sub[k - 1]['end'] for k in range(1, AXI_SUBPHASES)):
        raise ValueError('cap sub-phases overlap the warm-up or each other')
    for k, set_index, clear_index in ((1, 3, 4), (2, 5, 6)):
        if not switches[set_index]['end'] <= sub[k]['start'] < sub[k]['end'] <= switches[clear_index]['begin']:
            raise ValueError('a capped sub-phase is not enclosed by its set and clear')
    for k in (0, 3):
        if any(s['begin'] < sub[k]['end'] and s['end'] > sub[k]['start'] for s in switches if s['op'] != 'entry'):
            raise ValueError('a bridge operation overlaps an uncapped sub-phase')
    return {'bw_word': bw_b, 'sat_word': sat_b, 'subphase_wakes': subphase_wakes, 'capped': [bool(capped_mask >> k & 1)
            for k in range(AXI_SUBPHASES)], 'entry': entry, 'warm': warm,
            'warm_ratio': (warm[1]['gb_per_second'] / warm[0]['gb_per_second']) if warm[0]['gb_per_second'] else 0.0,
            'subphases': sub, 'switches': switches}


def axi_subphase_wakes(cores, axi, frequency):
    """Each core's busy wakes by sub-phase. The quota steps put busy wakes 32k to 32k + 31 in sub-phase k, each opened
    by its own wake 0. A wake whose ready stamp precedes the operation that opened its sub-phase is counted as early:
    its interval began before the cap state of the sub-phase was in place."""
    opened = [None, axi['switches'][3]['end'], axi['switches'][5]['end'], axi['switches'][6]['end']]
    out = []
    for core in cores:
        busy = [w for w in core['raw_wakes'] if w['metric'] == 'wake_busy']
        if len(busy) != axi['subphase_wakes'] * AXI_SUBPHASES:
            raise ValueError('core %d busy wakes do not fill the cap sub-phases' % core['core'])
        rows = []
        for k in range(AXI_SUBPHASES):
            part = busy[k * axi['subphase_wakes']:(k + 1) * axi['subphase_wakes']]
            us = [(w['running'] - w['ready']) * 1e6 / frequency for w in part]
            rows.append({'capped': axi['capped'][k], 'wake0_us': us[0], 'max_us': max(us), 'max_after_wake0_us': max(us[1:]),
                         'mean_us': sum(us) / len(us), 'mean_after_wake0_us': sum(us[1:]) / (len(us) - 1),
                         'early': sum(1 for w in part if opened[k] is not None and w['ready'] < opened[k]),
                         'us': us, 'ready': [w['ready'] for w in part]})
        out.append(rows)
    return out


def decode_wake_segments(payload, frequency):
    """Each busy wake's cycles and cache events by segment of its chain, from
    the interrupt that started it to the woken task's first instruction, and
    CPU0's busy complete ticks by band setting (kernel/scheduler/latency_cause.h)."""
    if len(payload) < 16:
        raise ValueError('wake segment header is invalid or incomplete')
    version = struct.unpack_from('<Q', payload, 8)[0]
    if version not in WAKE_BYTES_BY_VERSION or len(payload) != WAKE_BYTES_BY_VERSION[version]:
        raise ValueError('wake segment header is invalid or incomplete')
    size_bytes = WAKE_BYTES_BY_VERSION[version]
    v3 = version >= 3
    words = struct.unpack('<' + 'Q' * (size_bytes // 8), payload)
    header = words[:16]
    magic, version, size, cores, points, parts, shift, complete = header[:8]
    # Version 2 and later name the release and scan masks and count each core's local timer arms.
    masks = header[8:10] if version >= 2 else (0xff, 0xff)
    if ((magic, size, cores, points, parts, complete) !=
            (WAKE_MAGIC, size_bytes, 8, len(WAKE_POINTS), len(WAKE_PARTS), 1) or
            any(header[10:]) or (version == 1 and any(header[8:10])) or
            not 0 < masks[0] <= 0xff or not 0 < masks[1] <= 0xff):
        raise ValueError('wake segment header is invalid or incomplete')
    offset = 16

    def take(count):
        nonlocal offset
        values = words[offset:offset + count]
        offset += count
        return values

    def values(names):
        out = {}
        for name in names:
            v = take(WAKE_VALUES)
            out[name] = {'cycles': v[0], **dict(zip(CAUSE_EVENTS, v[1:]))}
        return out

    def scaled(table, count):
        return {name: {key: value / count for key, value in row.items()} for name, row in table.items()}

    def us(ticks):
        return ticks * 1e6 / frequency

    def sample(core):
        deadline = take(1)[0] if v3 else None
        release, entry, running, index = take(4)
        if deadline is None:
            deadline = release
        segments, part = values(WAKE_SEGMENTS), values(WAKE_PARTS)
        if not running:
            if (deadline or release or entry or index or
                    any(v for row in (*segments.values(), *part.values()) for v in row.values())):
                raise ValueError('empty wake segment sample holds values')
            return None
        # CPU0 releases its own task inside the interrupt that starts its chain.
        if not ((not v3 or deadline <= min(release, entry)) and
                (entry <= release <= running if core == 0 else release <= entry <= running)):
            raise ValueError('wake segment sample is out of order')
        # Versions 1 and 2 stamped the release only; their wake runs from it.
        return {'index': index & 0xffffffff, 'interrupted': bool(index >> 32 & 1), 'nested': bool(index >> 33 & 1),
                'wake_us': us(running - deadline),
                'release_to_running_us': us(running - release),
                'before_entry_us': us(entry - deadline) if v3 or core else 0.0,
                'chain_cycles': sum(row['cycles'] for row in segments.values()),
                'segments': segments, 'parts': part}

    result = {'version': version, 'release_mask': masks[0], 'scan_mask': masks[1],
              'switch_shift': shift, 'cores': [], 'ticks': {},
              'scope': ('a chain starts at the outermost interrupt entry and is kept only when every point '
                        'follows in order and it carried the release (CPU0: release inside the chain); a chain '
                        'completed before a later interrupt links with that interrupt in its last segment; busy samples after '
                        'the first are filed by the band setting at the wake; events are refills and backend stalls; '
                        + ('wakes run from the deadline, the compare value of the timer interrupt that released them'
                           if v3 else 'wakes run from the release stamp'))}
    for core in range(8):
        linked, unlinked, nested, interrupted, first_state, arms, arm_cycles, arm_max = take(8)
        if (nested > linked or interrupted > linked or (version == 1 and (arms or arm_cycles or arm_max)) or
                arm_max > arm_cycles or (not arms and arm_cycles)):
            raise ValueError('wake segment core counts or reserved words are invalid')
        cells = {}
        for setting in ('off', 'on'):
            count, pre, whole = take(3)
            release_ticks = take(1)[0] if v3 else None
            segments, part = values(WAKE_SEGMENTS), values(WAKE_PARTS)
            ranks = None
            if v3:
                top = [take(WAKE_WORST) for _ in range(WAKE_SPANS)]
                least = take(WAKE_SPANS)
                if (any(list(row) != sorted(row, reverse=True) for row in top) or
                        any(row[min(count, WAKE_WORST):] != (0,) * (WAKE_WORST - min(count, WAKE_WORST))
                            for row in top) or
                        (count and any(least[k] > top[k][0] for k in range(WAKE_SPANS)))):
                    raise ValueError('wake segment cell ranks are invalid')
                names = (*WAKE_SEGMENTS, *WAKE_PARTS)
                ranks = {name: {'top_cycles': list(top[k][:min(count, WAKE_WORST)]),
                                'least_cycles': least[k] if count else None}
                         for k, name in enumerate(names)}
            if not count and (pre or whole or release_ticks or (ranks and any(least))):
                raise ValueError('empty wake segment cell holds values')
            cells[setting] = {'count': count,
                'mean_wake_us': us(whole) / count if count else None,
                'mean_release_to_running_us': us(release_ticks) / count if count and v3 else None,
                'mean_before_entry_us': us(pre) / count if count else None,
                'mean_segments': scaled(segments, count) if count else None,
                'mean_parts': scaled(part, count) if count else None,
                **({'ranks': ranks} if v3 else {})}
        first = sample(core)
        if v3:
            worst = {}
            for setting in ('off', 'on'):
                rows = [sample(core) for _ in range(WAKE_WORST)]
                kept = [row for row in rows if row is not None]
                if (rows[len(kept):] != [None] * (WAKE_WORST - len(kept)) or
                        [row['wake_us'] for row in kept] != sorted((row['wake_us'] for row in kept), reverse=True) or
                        len(kept) != min(cells[setting]['count'], WAKE_WORST)):
                    raise ValueError('wake segment worst samples are out of order or miscounted')
                worst[setting] = kept
            maxima = {setting: rows[0] if rows else None for setting, rows in worst.items()}
        else:
            maxima = {setting: sample(core) for setting in ('off', 'on')}
            worst = {setting: [row] if row else [] for setting, row in maxima.items()}
        if sum(cell['count'] for cell in cells.values()) + (first is not None) > linked:
            raise ValueError('wake segment cells exceed the linked count')
        result['cores'].append({'core': core, 'linked': linked, 'unlinked': unlinked, 'nested': nested,
                                'interrupted': interrupted,
                                'timer_arms': {'count': arms, 'mean_cycles': arm_cycles / arms if arms else None,
                                               'max_cycles': arm_max,
                                               'scope': 'local timer register writes at EL1 while measuring, '
                                                        'net of an empty bracket; the arm call itself is outside'},
                                'first_state': {'seen': bool(first_state >> 16 & 1), 'linked': bool(first_state >> 8 & 1),
                                                'through_held_chain': bool(first_state >> 9 & 1),
                                                'current_chain_stage': first_state & 0xff},
                                **cells, 'first': first, 'maximum': maxima, 'worst': worst})

    def tick():
        count, cycles, outside, crossed = take(4)
        events, outside_events = take(len(CAUSE_EVENTS)), take(len(CAUSE_EVENTS))
        release_cycles = take(8)
        release_events = [take(len(CAUSE_EVENTS)) for _ in range(8)]
        if crossed > count or (not count and (cycles or outside or any(events) or any(release_cycles))):
            raise ValueError('wake segment tick cell is inconsistent')
        n = count or 1
        return {'count': count, 'crossed': crossed, 'mean_cycles': cycles / n if count else None,
                'mean_outside_cycles': outside / n if count else None,
                'mean_events': dict(zip(CAUSE_EVENTS, (v / n for v in events))) if count else None,
                'mean_outside_events': dict(zip(CAUSE_EVENTS, (v / n for v in outside_events))) if count else None,
                'mean_release': [{'core': x, 'cycles': release_cycles[x] / n,
                                  **dict(zip(CAUSE_EVENTS, (v / n for v in release_events[x])))}
                                 for x in range(8)] if count else None}
    for name in ('off', 'on', 'first', 'second'):
        result['ticks'][name] = tick()
    if offset != len(words):
        raise ValueError('wake segment report has unread words')
    return result


def peel_bands(payload, result_size=None):
    """Split the band probe section, which follows the service footer, from the result. Its footer
    records the whole result's size, which exceeds the payload when a wake segment section followed it."""
    if len(payload) < 64 or struct.unpack_from('<Q', payload, len(payload) - 64)[0] != BAND_FOOTER_MAGIC:
        return payload, b''
    _, version, size, at, length, *rest = struct.unpack_from('<8Q', payload, len(payload) - 64)
    if (version != 1 or size != (len(payload) if result_size is None else result_size) or
            length != BAND_BYTES or at % 64 or
            at + length > len(payload) - 64 or any(rest) or any(payload[at + length:-64])):
        raise ValueError('band footer offsets, padding or report size are invalid')
    return payload[:at], payload[at:at + length]


def decode_bands(payload):
    """The band probes on CPUs 1 and 2, by band setting, the setting of each
    wake sample, and CPU0's PMR change benchmark (apps/latency/workload.h)."""
    words = struct.unpack('<' + 'Q' * (BAND_BYTES // 8), payload)
    (magic, version, size, gap, margin, shift, notified, timeouts), rest = words[:8], words[8:]
    discarded, runs, unfinished = rest[0:2], rest[2:4], rest[4:6]
    complete, reserved = rest[6], rest[7]
    if (magic, version, size, complete, reserved) != (BAND_MAGIC, 1, BAND_BYTES, 1, 0) or not gap or not margin:
        raise ValueError('band report header is invalid or incomplete')
    offset = 16
    wake_on = []
    for core in range(8):
        bits = words[offset:offset + 3]
        offset += 3
        wake_on.append([bits[i // 64] >> (i % 64) & 1 for i in range(192)])
    probes = []
    for probe in range(BAND_PROBES):
        settings = {}
        for setting in ('off', 'on'):
            cell = dict(zip(BAND_CELL_FIELDS, words[offset:offset + 8]))
            bins = list(words[offset + 8:offset + 16])
            offset += 16
            if (sum(bins) != cell['foreign'] or cell['foreign_irq'] > cell['foreign'] or
                    (cell['foreign'] and not gap < cell['foreign_max'] <= cell['foreign_sum']) or
                    (not cell['foreign'] and (cell['foreign_max'] or cell['foreign_sum'])) or
                    (not cell['own'] and cell['own_max'])):
                raise ValueError('band probe cell is inconsistent')
            cell['bins'] = bins
            cell['foreign_mean'] = cell['foreign_sum'] / cell['foreign'] if cell['foreign'] else None
            settings[setting] = cell
        probes.append({'core': 1 + probe, 'discarded_windows': discarded[probe],
                       'unfinished_windows': unfinished[probe], 'background_runs': runs[probe], **settings})
    pmr = {}
    counts = words[offset:offset + 2]
    offset += 2
    for name, count in zip(('pmr_change', 'pmr_change_dsb_sy_isb'), counts):
        if count != 64:
            raise ValueError('band PMR change samples are incomplete')
        pmr[name] = cycle_distribution(words[offset:offset + 64])
        offset += 64
    return {'gap_cycles': gap, 'margin_cycles': margin, 'switch_shift': shift, 'notified': notified,
            'wait_timeouts': timeouts, 'probes': probes, 'wake_on': wake_on, 'pmr_benchmark': pmr,
            'scope': ('cycles from each probe core\'s cycle counter; a gap is foreign when the probe\'s own wake '
                      'task did not complete in it; windows filed by the band setting in force at their start')}


def peel_service_cycles(payload, result_size=None):
    """Split the service cycle section, which follows the cause footer, from the result. Its footer
    records the whole result's size, which exceeds the payload when a band section followed it."""
    if len(payload) < 64 or struct.unpack_from('<Q', payload, len(payload) - 64)[0] != SERVICE_FOOTER_MAGIC:
        return payload, b''
    _, version, size, at, length, *rest = struct.unpack_from('<8Q', payload, len(payload) - 64)
    if (version != 1 or size != (len(payload) if result_size is None else result_size) or
            length != SERVICE_BYTES or at % 64 or
            at + length > len(payload) - 64 or any(rest) or any(payload[at + length:-64])):
        raise ValueError('service cycle footer offsets, padding or report size are invalid')
    return payload[:at], payload[at:at + length]


def decode_service_cycles(payload):
    header = struct.unpack_from('<16Q', payload, 0)
    magic, version, size, cores, phases, kinds, states, bins, shift, complete = header[:10]
    if (magic, version, size, cores, phases, kinds, states, bins) != (
            SERVICE_MAGIC, 1, SERVICE_BYTES, 8, 2, 2, 2, SERVICE_BINS) or complete != 1 or any(header[10:]):
        raise ValueError('service cycle report header is invalid or incomplete')
    result = {'switch_shift': shift,
              'scope': ('cycles from the CPU cycle counter around each service pass and control drain, both '
                        'settings interleaved by counter bit; spans that took an interrupt are counted apart'),
              'phases': {}}
    offset = 128
    for phase in SERVICE_PHASES:
        cores_out = []
        for core in range(8):
            row = {'core': core}
            for kind in SERVICE_KINDS:
                for state in SERVICE_STATES:
                    count, total, maximum, interrupted, interrupted_sum = struct.unpack_from('<5Q', payload, offset)
                    values = struct.unpack_from(f'<{SERVICE_BINS}I', payload, offset + 40)
                    if sum(values) != count or (count and maximum * count < total) or (not count and (total or maximum)):
                        raise ValueError('service cycle cell is inconsistent')
                    row.setdefault(kind, {})[state] = {
                        'count': count, 'mean_cycles': total / count if count else None, 'max_cycles': maximum,
                        'p50_upper_cycles': service_quantile(values, count, 0.5),
                        'p99_upper_cycles': service_quantile(values, count, 0.99),
                        'interrupted': interrupted, 'interrupted_sum_cycles': interrupted_sum,
                        'bins': list(values)}
                    offset += SERVICE_CELL
            cores_out.append(row)
        result['phases'][phase] = cores_out
    return result


def peel_latency_cause(payload, result_size=None):
    """Split the cause and ping reports from the result. Footer version 1
    streams on every core; version 2 names the streaming cores in its last word.
    The footer records the whole result's size, which exceeds the payload when
    a service cycle section followed it (result_size)."""
    cause_tail, ping_tail, stream_mask = b'', b'', 0xff
    if len(payload) >= 64 and struct.unpack_from('<Q', payload, len(payload) - 64)[0] == CAUSE_FOOTER_MAGIC:
        footer = struct.unpack_from('<8Q', payload, len(payload) - 64)
        _, version, size, cause_at, cause_size, ping_at, ping_size, last = footer
        if version == 2 and 0 < last <= 0xff:
            stream_mask = last
        elif version != 1 or last:
            raise ValueError('cause footer offsets, padding or report sizes are invalid')
        if (size != (len(payload) if result_size is None else result_size) or cause_size != CAUSE_BYTES or
                ping_size != CAUSE_PING_BYTES or cause_at < RESULT_BYTES or
                cause_at % 64 or ping_at != cause_at + cause_size or
                ping_at + ping_size > len(payload) - 64 or
                any(payload[ping_at + ping_size:-64])):
            raise ValueError('cause footer offsets, padding or report sizes are invalid')
        cause_tail = payload[cause_at:ping_at]
        ping_tail = payload[ping_at:ping_at + ping_size]
        payload = payload[:cause_at]
    return payload, cause_tail, ping_tail, stream_mask


def decode(payload, dma=False):
    if len(payload) < RESULT_BYTES:
        raise ValueError(f'latency result must contain {RESULT_BYTES} bytes')
    result_size = len(payload)
    payload, axi_tail = peel_axi_cap(payload)
    payload, wake_tail = peel_wake_segments(payload, result_size)
    payload, band_tail = peel_bands(payload, result_size)
    payload, service_tail = peel_service_cycles(payload, result_size)
    payload, cause_tail, ping_tail, stream_mask = peel_latency_cause(payload, result_size)
    tail = payload[RESULT_BYTES:]
    scheduler_pmu_tail = b''
    if len(tail) >= SCHED_PMU_BYTES and struct.unpack_from('<Q', tail, len(tail) - SCHED_PMU_BYTES)[0] == SCHED_PMU_MAGIC:
        tail, scheduler_pmu_tail = tail[:-SCHED_PMU_BYTES], tail[-SCHED_PMU_BYTES:]
    control_tail = b''
    if len(tail) >= CONTROL_BYTES and struct.unpack_from('<Q', tail, len(tail) - CONTROL_BYTES)[0] == CONTROL_MAGIC:
        tail, control_tail = tail[:-CONTROL_BYTES], tail[-CONTROL_BYTES:]
    pmu_tail, scheduler_tail = b'', b''
    if len(tail) >= 8 and struct.unpack_from('<Q', tail)[0] == SCHED_MAGIC:
        scheduler_tail = tail
    elif len(tail) >= PMU_BYTES + 8 and struct.unpack_from('<Q', tail, PMU_BYTES)[0] == SCHED_MAGIC:
        pmu_tail, scheduler_tail = tail[:PMU_BYTES], tail[PMU_BYTES:]
    elif len(tail) == PMU_BYTES:
        pmu_tail = tail
    elif tail:
        raise ValueError(f'latency result must contain {RESULT_BYTES} bytes')
    payload = payload[:RESULT_BYTES]
    magic, frequency, elapsed, mask, model, dma_jobs, irq_mask = struct.unpack_from('<3Q4I', payload)
    if magic != MAGIC or mask != 0xff or model or irq_mask not in (1, 0xff) or \
            (dma_jobs < 2 if dma else dma_jobs != 0):
        raise ValueError('native CPU latency header differs')
    if not frequency or not 0 < elapsed <= (15000 if control_tail else 10000):
        raise ValueError('latency counter frequency or run interval is invalid')
    timer_cores = timer_core_mask(irq_mask, wake_tail)
    cores = []
    fields = ('count', 'max_ticks', 'max_start', 'max_site', 'first_over_start',
              'first_over_ticks', 'first_over_site')
    for core in range(8):
        sweeps, read, written, digest = struct.unpack_from('<4Q', payload, 40 + core * 32)
        if stream_mask >> core & 1:
            if sweeps < 2 or read != sweeps * 1048576 or written != read:
                raise ValueError(f'core {core} memory traffic is incomplete')
        elif sweeps or read or written or digest:
            raise ValueError(f'core {core} streams outside the named streaming cores')
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
        # CPU0 always takes the scheduler tick; with local timers CPUs 1-7 take their own.
        if (core == 0 and not samples['timer_irq']['count']) or \
                (samples['timer_irq']['count'] and not timer_cores >> core & 1):
            raise ValueError('timer IRQ samples on a core outside the timer core mask')
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
            'phase_bounds': dict(zip(PHASES, bounds)), 'core_mask': mask, 'stream_mask': stream_mask, 'timer_irq_core_mask': irq_mask, 'dma_jobs': dma_jobs, 'cores': cores}
    if scheduler_pmu_tail:
        if pmu_tail or control_tail or not scheduler_tail:
            raise ValueError('scheduler PMU report requires its exclusive traced CPU profile')
        result['scheduler_pmu'] = decode_scheduler_pmu(scheduler_pmu_tail, frequency)
        for core in result['scheduler_pmu']['cores']:
            for row in core['rows']:
                if row['observed']:
                    row['phase'] = next((name for name, (begin, end) in result['phase_bounds'].items()
                        if begin <= row['begin_tick'] < end and row['end_tick'] <= end),
                        'unassigned_or_crossing')
    control = decode_control(control_tail, frequency) if control_tail else None
    if scheduler_tail:
        result['scheduler_trace'] = decode_scheduler_trace(scheduler_tail, frequency,
            restore_spans=control['restore_spans'] if control is not None else None)
        for row in result['scheduler_trace']['wake_chains']:
            raw = [w for w in cores[row['core']]['raw_wakes'] if w['metric'] == 'wake_busy']
            maximum = row['maximum']
            if row['count'] != len(raw) or not any(w['ready'] == maximum['ready'] and w['running'] == maximum['running'] for w in raw):
                raise ValueError('scheduler busy-wake chain differs from the named workload samples')
            if maximum['whole_wake_ticks'] != max(w['running'] - w['ready'] for w in raw):
                raise ValueError('scheduler busy-wake maximum differs from the named workload samples')
    if control_tail:
        result['control'] = control
        targets = {}
        for label, metric_name in (('task_lock_wait', 'lock1_wait'), ('interrupts_masked', 'irq_masked')):
            count = sum(row['metrics'][metric_name]['count'] for row in cores)
            maximum = max(row['metrics'][metric_name]['max_ticks'] for row in cores)
            targets[label] = {'target_us': 25, 'max_ticks': maximum,
                'max_us': maximum * 1000000 / frequency,
                'met': maximum * 1000000 <= 25 * frequency if count else None}
        targets['task_wake'] = result['control']['task_wake_target']
        result['qualification'] = {'targets': targets,
            'scope': 'deadline-to-task p9999, task-lock wait maximum and recorded mask maximum',
            'recorded_targets_met': result['control']['measurement_qualified'] and
                                    all(value['met'] is True for value in targets.values()),
            'mask_coverage': ('recorded software exception interval'
                if result['control']['restore_spans'] else 'fast and switch mask-span coverage unqualified'),
            'limitations': ['startup masking requires separate qualification',
                            'three final-image runs are required for repeatability',
                            'exception mask timing excludes entry-save work and post-timestamp observer, restore and ERET work']}
    if cause_tail:
        if dma or pmu_tail or control_tail or scheduler_pmu_tail or not scheduler_tail:
            raise ValueError('cause report requires its exclusive traced CPU profile')
        # The wake segment build lets the woken task read the event counters (PMUSERENR.ER).
        release_mask, scan_mask = wake_masks(wake_tail)
        result['latency_cause'] = decode_latency_cause(cause_tail, frequency, 12 if wake_tail else 4,
                                                       release_mask, scan_mask)
        result['latency_cause_ping'] = decode_latency_cause_ping(ping_tail, frequency,
            result['latency_cause']['tick']['effective_mhz'])
    if service_tail:
        if not cause_tail:
            raise ValueError('service cycle report requires the cause profile')
        result['service_cycles'] = decode_service_cycles(service_tail)
    if band_tail:
        if not cause_tail:
            raise ValueError('band probe report requires the cause profile')
        result['bands'] = decode_bands(band_tail)
    if wake_tail:
        if not cause_tail:
            raise ValueError('wake segment report requires the cause profile')
        result['wake_segments'] = decode_wake_segments(wake_tail, frequency)
    if axi_tail:
        result['axi_cap'] = decode_axi_cap(axi_tail, frequency, stream_mask)
        result['axi_cap']['wakes'] = axi_subphase_wakes(result['cores'], result['axi_cap'], frequency)
    return result
