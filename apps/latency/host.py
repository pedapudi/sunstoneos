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
        if direct and (version != 5 or kind != 2 or flags not in (69, 85, 93) or not all(stamp)):
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
                'completed_before_local_selection' if no_selection else 'selected_replacement')
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
        if capacity != history_capacity or max(irq_count, switch_count) > capacity or window_flags & ~7:
            raise ValueError('scheduler CPU0 history window header differs')
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
                elif not window_flags & 5:
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
        if latest and latest not in owner_sequences and not window_flags & 5:
            raise ValueError('scheduler CPU0 window omits the latest owner exit')
        cpu0_window = {'capacity': capacity, 'flags': window_flags,
            'irq_prefix_lost': bool(window_flags & 1), 'switch_prefix_lost': bool(window_flags & 2),
            'incomplete': bool(window_flags & 4), 'ready': ready, 'running': running,
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


def decode(payload, dma=False):
    if len(payload) < RESULT_BYTES:
        raise ValueError(f'latency result must contain {RESULT_BYTES} bytes')
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
    if magic != MAGIC or mask != 0xff or model or irq_mask != 1 or \
            (dma_jobs < 2 if dma else dma_jobs != 0):
        raise ValueError('native CPU latency header differs')
    if not frequency or not 0 < elapsed <= (15000 if control_tail else 10000):
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
    if scheduler_pmu_tail:
        if pmu_tail or control_tail or not scheduler_tail:
            raise ValueError('scheduler PMU report requires its exclusive traced CPU profile')
        result['scheduler_pmu'] = decode_scheduler_pmu(scheduler_pmu_tail, frequency)
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
    return result
