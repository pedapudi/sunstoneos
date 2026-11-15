#!/usr/bin/env python3
"""Host side of vdec-check: decode the result record and list departures from the stage-1 prediction.

The record is the 32-byte header and struct vdec_check_record (apps/vdec-check/record.h) that
apps/vdec-check/main.c returns. decode() returns the fields. failures() names every departure from the stage-1
prediction in docs/video-decode-os.md: the open and close succeed, the entry words match the port 3-3 FEL dump where
the binding depends on them, the 0x5e4 open reads back 0x40306, PLL_VE0 reads 0xf9204d00 once started and 0x01204d00
after the close, the module word is 0x81000000 started and 0x01000000 closed, reset and bus gate are released only
while open, the identity is one the binding lists, and domain 2 answers ON either adopted or after one measured
command. The entry words that the binding does not depend on, the status bits outside the low nibble, 0x1b10
after the close and the two VE+0x804 words of the VCU pulse are reported, not judged.

A fault after the first write closes the decoder and still returns the record: failures() then names the fault step,
the unit's failure flags and the address and word of the failing access, and judges the words after that close
against the close prediction (module gate, reset and bus gate closed, PLL_VE0 disabled).
"""
import struct

HEADER = struct.Struct('<8I')
WORDS = ('pll_ref', 'rtc_osc', 'ahb', 'nsi', 'mbus', 'pll_ve0', 'pll_ve0_p0', 'pll_ve0_p1', 'pll_gates', 'pll_status',
         'auto', 'module', 'bgr', 'ahb_gates', 'mbus_gates', 'mclk_gates', 'rccu_pck', 'gicd_enable', 'gicd_pending',
         'gicd_active')
# The record's structures as apps/vdec-check/record.h declares them: (member, format, count) in order, a nested
# structure given by its member list. layout() places them as the AArch64 C ABI does, each member at its natural
# alignment and each structure padded to its largest; checks/os-vdec-fault-return.py compares the offsets with the
# image's DWARF.
COLD = (('entry_gates', 'I', 3), ('gates', 'I', 3), ('mclk_written', 'I', 1), ('mclk_readback', 'I', 1),
        ('entry_at', 'Q', 1), ('entered_at', 'Q', 1), ('on_at', 'Q', 1), ('hold_a_at', 'Q', 1),
        ('locked_at', 'Q', 1), ('hold_b_at', 'Q', 1), ('close_at', 'Q', 1), ('closed_at', 'Q', 1),
        ('lock_reads', 'I', 1), ('vcu', 'I', 2))
TRANSITION = (('policy_before', 'I', 1), ('status_before', 'I', 1), ('policy_written', 'I', 1),
              ('first_word', 'I', 1), ('pending_word', 'I', 1), ('reached_word', 'I', 1), ('all_and', 'I', 1),
              ('all_or', 'I', 1), ('reads', 'I', 1), ('spin_reads', 'I', 1), ('commands', 'I', 1),
              ('command_at', 'Q', 1), ('first_at', 'Q', 1), ('pending_at', 'Q', 1), ('reached_at', 'Q', 1))
RECORD = (('magic', 'I', 1), ('bytes', 'I', 1), ('entry', 'I', 20), ('started', 'I', 20), ('closed', 'I', 20),
          ('cold', COLD, 1), ('feature', 'I', 1), ('identity_high', 'I', 1), ('identity_low', 'I', 1),
          ('version_high', 'I', 1), ('version_low', 'I', 1), ('cold_phase', 'I', 1), ('failed', 'I', 1),
          ('row_state', 'I', 1), ('authority', 'I', 1), ('measured', 'I', 1), ('fault_closed', 'I', 1),
          ('fault_step', 'I', 1), ('fault_value', 'I', 1), ('refused_address', 'I', 1), ('refused_word', 'I', 1),
          ('row_request_us', 'Q', 1), ('row_seen_us', 'Q', 1), ('owner_started_us', 'Q', 1),
          ('owner_verified_us', 'Q', 1), ('requested_ticks', 'Q', 1),
          ('started_ticks', 'Q', 1), ('closed_ticks', 'Q', 1), ('transition', TRANSITION, 1))


def layout(members):
    """(offsets, size, alignment): each member's byte offset and nested layout, and the padded size."""
    offsets, at, align = {}, 0, 1
    for name, kind, count in members:
        if isinstance(kind, tuple):
            inner, size, a = layout(kind)
        else:
            inner, size, a = None, struct.calcsize('<' + kind), struct.calcsize('<' + kind)
        at = -(-at // a) * a
        offsets[name] = (at, inner)
        at += size * count
        align = max(align, a)
    return offsets, -(-at // align) * align, align


def unpack(payload, base, members):
    offsets = layout(members)[0]
    out = {}
    for name, kind, count in members:
        at = offsets[name][0]
        if isinstance(kind, tuple):
            out[name] = unpack(payload, base + at, kind)
        else:
            v = struct.unpack_from(f'<{count}{kind}', payload, base + at)
            out[name] = list(v) if count > 1 else v[0]
    return out


SIZE = HEADER.size + layout(RECORD)[1]
CHECK_MAGIC = 0x4b434456
INFO_MAGIC = 0x49444556
STATUS = {1: 'opened-and-closed', 2: 'open-refused', 3: 'close-failed'}
ROW = {0: 'idle', 1: 'pending', 2: 'on', 3: 'off', 4: 'refused', 5: 'failed'}
STEPS = {0: 'none', 1: 'rail', 2: 'clock', 3: 'pck-lease', 4: 'supply', 5: 'config', 6: 'entry-words', 7: 'entry',
         8: 'row', 9: 'row-timeout', 17: 'cold', 18: 'started-words', 19: 'identity', 32: 'close', 33: 'closed-words'}
VERSIONS = (0x0003332100021321, 0x0003331000021320, 0x0003052000012011)
TICKS_PER_US = 24
INTID_81 = 1 << 17
FAILED_FLAGS = ('failed', 'mutex-held', 'uncertain', 'late')


def decode(payload):
    if len(payload) != SIZE:
        raise ValueError(f'vdec-check result is {len(payload)} bytes, not {SIZE}.')
    (magic, stage, status, open_result, close_result, fault_step, fault_value, reserved) = HEADER.unpack_from(payload)
    if magic != CHECK_MAGIC or stage != 1 or reserved:
        raise ValueError('vdec-check result has an unknown header.')
    signed = lambda x: x - (1 << 32) if x >> 31 else x
    out = {'stage': stage, 'status': STATUS.get(status, status), 'open_result': signed(open_result),
           'close_result': signed(close_result), 'fault_step': STEPS.get(fault_step, fault_step),
           'fault_value': fault_value}
    r = unpack(payload, HEADER.size, RECORD)
    out.update(info_magic=r['magic'], info_bytes=r['bytes'])
    for name in ('entry', 'started', 'closed'):
        out[name] = dict(zip(WORDS, r[name]))
    c = r['cold']
    out['gates_entry'], out['gates_a1'] = c['entry_gates'], c['gates']
    out['mclk_written'], out['mclk_readback'], out['lock_reads'] = c['mclk_written'], c['mclk_readback'], c['lock_reads']
    out['vcu_before'], out['vcu_set'] = c['vcu']
    for name in ('feature', 'identity_high', 'identity_low', 'cold_phase', 'failed', 'authority', 'measured',
                 'fault_closed', 'refused_address', 'refused_word'):
        out[name] = r[name]
    out['version'] = r['version_high'] << 32 | r['version_low']
    out['row_state'] = ROW.get(r['row_state'], r['row_state'])
    out['record_fault_step'] = STEPS.get(r['fault_step'], r['fault_step'])
    out['record_fault_value'] = r['fault_value']
    out['failed_flags'] = [name for k, name in enumerate(FAILED_FLAGS) if out['failed'] >> k & 1]
    times = {k: c[k] for k in ('entry_at', 'entered_at', 'on_at', 'hold_a_at', 'locked_at', 'hold_b_at', 'close_at',
                               'closed_at')}
    times.update({k: r[k] for k in ('row_request_us', 'row_seen_us', 'owner_started_us', 'owner_verified_us',
                                    'requested_ticks', 'started_ticks', 'closed_ticks')})
    out['times'] = times
    out['transition'] = r['transition']
    # Derived durations in microseconds; zero where a phase was not reached.
    span = lambda a, b: times[b] - times[a] if times[a] and times[b] >= times[a] else None
    out['durations_us'] = {
        'entry_hold': span('entry_at', 'entered_at'), 'row_wait': span('entered_at', 'on_at'),
        'hold_a': span('on_at', 'hold_a_at'), 'lock_wait': span('hold_a_at', 'locked_at'),
        'hold_b': span('locked_at', 'hold_b_at'), 'close_hold': span('close_at', 'closed_at'),
        'owner_verify': span('owner_started_us', 'owner_verified_us'),
        'request_after_owner_start': span('owner_started_us', 'row_request_us'),
        'open': (times['started_ticks'] - times['requested_ticks']) / TICKS_PER_US if times['started_ticks'] else None,
        'close': (times['closed_ticks'] - times['started_ticks']) / TICKS_PER_US if times['closed_ticks'] else None}
    # Reported, not judged: no reading shows 0x1b10 move, so the close is predicted to leave it at 0x20000.
    out['auto_closed_as_entry'] = out['closed']['auto'] & 0x30001 == 0x20000
    t = out['transition']
    if t['commands'] and t['reached_at'] >= t['command_at']:
        out['durations_us']['domain2_to_on'] = t['reached_at'] - t['command_at']
    return out


def failures(d):
    """Departures from the stage-1 prediction, as strings; empty when the run matches."""
    out = []
    def want(ok, text):
        if not ok:
            out.append(text)
    want(d['status'] == 'opened-and-closed' and d['open_result'] == 1 and d['close_result'] == 1,
         f"status {d['status']}, open {d['open_result']}, close {d['close_result']}, fault {d['fault_step']} "
         f"{d['fault_value']:#x}")
    want(d['info_magic'] == INFO_MAGIC and d['info_bytes'] == SIZE - HEADER.size, 'record header differs')
    if d['info_magic'] != INFO_MAGIC:
        return out
    if d['fault_closed']:
        c = d['closed']
        out.append(f"fault {d['record_fault_step']} {d['record_fault_value']:#x} closed by the binding: unit "
                   f"{'/'.join(d['failed_flags']) or 'not failed'}, last access {d['refused_address']:#010x} read "
                   f"{d['refused_word']:#010x}")
        want(c['module'] & 1 << 31 == 0 and c['bgr'] & 0x40004 == 0 and c['pll_ve0'] & 0xe8000000 == 0,
             f"after the fault close: module {c['module']:#x}, reset/bus {c['bgr']:#x}, PLL_VE0 {c['pll_ve0']:#010x}")
    e, s, c = d['entry'], d['started'], d['closed']
    want(e['pll_ve0'] == 0x41305a00, f"PLL_VE0 at entry {e['pll_ve0']:#010x}, predicted 0x41305a00 (FEL)")
    want(e['module'] == 0 and e['bgr'] & 0x40004 == 0, f"module {e['module']:#x} or reset/bus {e['bgr']:#x} at entry")
    want(e['auto'] & 0x30001 == 0x20000, f"0x1b10 at entry {e['auto']:#x}, predicted 0x20000")
    want(e['ahb_gates'] & 1 and e['mbus_gates'] & 1 << 14, f"master gates at entry {e['ahb_gates']:#x} "
         f"{e['mbus_gates']:#x}")
    want(e['mclk_gates'] == 0x4, f"0x5e4 at entry {e['mclk_gates']:#x}, predicted 0x4 (FEL)")
    want(e['rccu_pck'] & 1, f"R_CCU 0x1ac at entry {e['rccu_pck']:#x}")
    for name in ('entry', 'started', 'closed'):
        w = d[name]
        want(not (w['gicd_enable'] | w['gicd_pending'] | w['gicd_active']) & INTID_81, f'INTID 81 not quiet ({name})')
    want(d['gates_entry'] == [e['ahb_gates'], e['mbus_gates'], e['mclk_gates']], 'entry gates differ from entry words')
    want(d['mclk_written'] == 0x40306 and d['mclk_readback'] == 0x40306,
         f"0x5e4 written {d['mclk_written']:#x}, read back {d['mclk_readback']:#x}, predicted 0x40306 both")
    want(d['gates_a1'][2] & 1 << 18 and d['gates_a1'][:2] == d['gates_entry'][:2], f"A1 gates {d['gates_a1']}")
    want(3 <= d['lock_reads'] <= 100, f"lock reads {d['lock_reads']}")
    want(d['cold_phase'] == 5 and d['failed'] == 0,
         f"cold phase {d['cold_phase']}, failed {'/'.join(d['failed_flags']) or 0}")
    want(d['version'] in VERSIONS and d['feature'] in (0, 0x16670040),
         f"identity {d['version']:#018x}, feature {d['feature']:#x}")
    want(s['pll_ve0'] == 0xf9204d00, f"PLL_VE0 started {s['pll_ve0']:#010x}, predicted 0xf9204d00")
    want(s['module'] == 0x81000000, f"module started {s['module']:#x}")
    want(s['bgr'] == e['bgr'] | 0x40004, f"reset/bus started {s['bgr']:#x}")
    want(s['mclk_gates'] == d['mclk_readback'], f"0x5e4 started {s['mclk_gates']:#x}")
    if d['status'] == 'opened-and-closed':
        want(c['pll_ve0'] == 0x01204d00, f"PLL_VE0 closed {c['pll_ve0']:#010x}, predicted 0x01204d00")
        want(c['module'] == 0x01000000, f"module closed {c['module']:#x}")
        want(c['bgr'] == e['bgr'], f"reset/bus closed {c['bgr']:#x}")
        want(c['mclk_gates'] == s['mclk_gates'] and c['ahb_gates'] == e['ahb_gates'] and
             c['mbus_gates'] == e['mbus_gates'], 'critical gates changed by the close')
    want(d['row_state'] == 'on' and d['measured'] == 1, f"row {d['row_state']}, measured {d['measured']}")
    t = d['transition']
    if not any(t.values()):
        out.append('no domain-2 transition record')
    else:
        # The owner judges the low nibble only; bit 8 and the other status bits are reported, not judged.
        adopted = t['commands'] == 0 and t['policy_before'] & 0xf == 8 and t['status_before'] & 0xf == 8
        measured = (t['commands'] == 1 and t['policy_before'] & 0xf == 0 and t['status_before'] & 0xf == 0 and
                    t['policy_written'] & 0xf == 8 and t['reached_word'] & 0xf == 8 and t['reads'] == t['spin_reads'] and
                    t['reached_at'] - t['command_at'] <= 10000)
        want(adopted or measured, f"domain 2 neither adopted ON nor one measured OFF-to-ON command: {t}")
    return out
