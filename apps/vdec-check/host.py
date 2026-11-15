#!/usr/bin/env python3
"""Host side of vdec-check: decode the 768-byte result record and list departures from the stage-1 prediction.

The record is the 32-byte header and struct ss_video_decode_info (include/sunstone_video_decode.h) that
apps/vdec-check/main.c returns. decode() returns the fields. failures() names every departure from the stage-1
prediction in docs/video-decode-os.md: the open and close succeed, the entry words match the port 3-3 FEL dump where
the binding depends on them, the 0x5e4 open reads back 0x40306, PLL_VE0 reads 0xf9204d00 once started and 0x01204d00
after the close, the module word is 0x81000000 started and 0x01000000 closed, reset and bus gate are released only
while open, the identity is one the binding lists, and domain 2 answers ON either adopted or after one measured
command. The entry words that the binding does not depend on are reported, not judged.
"""
import struct

HEADER = struct.Struct('<8I')
WORDS = ('pll_ref', 'rtc_osc', 'ahb', 'nsi', 'mbus', 'pll_ve0', 'pll_ve0_p0', 'pll_ve0_p1', 'pll_gates', 'pll_status',
         'auto', 'module', 'bgr', 'ahb_gates', 'mbus_gates', 'mclk_gates', 'rccu_pck', 'gicd_enable', 'gicd_pending',
         'gicd_active')
INFO = struct.Struct('<2I' + '20I' * 3 + '9I7I4I' + '6Q2Q3Q')
TRANSITION = struct.Struct('<12I4Q')
TRANSITION_FIELDS = ('domain', 'policy_before', 'status_before', 'policy_written', 'first_word', 'pending_word',
                     'reached_word', 'all_and', 'all_or', 'reads', 'spin_reads', 'commands', 'command_at', 'first_at',
                     'pending_at', 'reached_at')
DOMAINS = 4
SIZE = HEADER.size + INFO.size + DOMAINS * TRANSITION.size
CHECK_MAGIC = 0x4b434456
INFO_MAGIC = 0x49444556
STATUS = {1: 'opened-and-closed', 2: 'open-refused', 3: 'close-failed'}
ROW = {0: 'idle', 1: 'pending', 2: 'on', 3: 'off', 4: 'refused', 5: 'failed'}
STEPS = {0: 'none', 1: 'rail', 2: 'clock', 3: 'pck-lease', 4: 'supply', 5: 'config', 6: 'entry-words', 7: 'entry',
         8: 'row', 9: 'row-timeout', 17: 'cold', 18: 'started-words', 19: 'identity', 32: 'close', 33: 'closed-words'}
VERSIONS = (0x0003332100021321, 0x0003331000021320, 0x0003052000012011)
TICKS_PER_US = 24
INTID_81 = 1 << 17


def decode(payload):
    if len(payload) != SIZE:
        raise ValueError(f'vdec-check result is {len(payload)} bytes, not {SIZE}.')
    (magic, stage, status, open_result, close_result, fault_step, fault_value, reserved) = HEADER.unpack_from(payload)
    if magic != CHECK_MAGIC or stage != 1 or reserved:
        raise ValueError('vdec-check result has an unknown header.')
    v = INFO.unpack_from(payload, HEADER.size)
    signed = lambda x: x - (1 << 32) if x >> 31 else x
    out = {'stage': stage, 'status': STATUS.get(status, status), 'open_result': signed(open_result),
           'close_result': signed(close_result), 'fault_step': STEPS.get(fault_step, fault_step),
           'fault_value': fault_value, 'info_magic': v[0], 'info_bytes': v[1]}
    for k, name in enumerate(('entry', 'started', 'closed')):
        out[name] = dict(zip(WORDS, v[2 + 20 * k:22 + 20 * k]))
    i = 62
    out['gates_entry'], out['gates_a1'] = list(v[i:i + 3]), list(v[i + 3:i + 6])
    out['mclk_written'], out['mclk_readback'], out['lock_reads'] = v[i + 6:i + 9]
    i += 9
    (out['feature'], out['identity_high'], out['identity_low'], high, low, out['cold_phase'],
     out['failed']) = v[i:i + 7]
    out['version'] = high << 32 | low
    i += 7
    row_state, out['authority'], out['measured'], _ = v[i:i + 4]
    out['row_state'] = ROW.get(row_state, row_state)
    i += 4
    times = dict(zip(('entry_at', 'entered_at', 'on_at', 'hold_a_at', 'locked_at', 'hold_b_at', 'row_request_us',
                      'row_on_us', 'requested_ticks', 'started_ticks', 'closed_ticks'), v[i:i + 11]))
    out['times'] = times
    out['transitions'] = [dict(zip(TRANSITION_FIELDS, TRANSITION.unpack_from(
        payload, HEADER.size + INFO.size + k * TRANSITION.size))) for k in range(DOMAINS)]
    # Derived durations in microseconds; zero where a phase was not reached.
    span = lambda a, b: times[b] - times[a] if times[a] and times[b] >= times[a] else None
    out['durations_us'] = {
        'entry_hold': span('entry_at', 'entered_at'), 'row_wait': span('entered_at', 'on_at'),
        'hold_a': span('on_at', 'hold_a_at'), 'lock_wait': span('hold_a_at', 'locked_at'),
        'hold_b': span('locked_at', 'hold_b_at'), 'owner_row_wait': span('row_request_us', 'row_on_us'),
        'open': (times['started_ticks'] - times['requested_ticks']) / TICKS_PER_US if times['started_ticks'] else None,
        'close': (times['closed_ticks'] - times['started_ticks']) / TICKS_PER_US if times['closed_ticks'] else None}
    domain2 = [t for t in out['transitions'] if t['domain'] == 2]
    if domain2 and domain2[0]['commands'] and domain2[0]['reached_at'] >= domain2[0]['command_at']:
        out['durations_us']['domain2_to_on'] = domain2[0]['reached_at'] - domain2[0]['command_at']
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
    e, s, c = d['entry'], d['started'], d['closed']
    want(e['pll_ve0'] == 0x41305a00, f"PLL_VE0 at entry {e['pll_ve0']:#010x}, predicted 0x41305a00 (FEL)")
    want(e['module'] == 0 and e['bgr'] & 0x40004 == 0, f"module {e['module']:#x} or reset/bus {e['bgr']:#x} at entry")
    want(e['auto'] & 0x30001 == 0x10000, f"0x1b10 at entry {e['auto']:#x}")
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
    want(d['cold_phase'] == 5 and d['failed'] == 0, f"cold phase {d['cold_phase']}, failed {d['failed']}")
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
        want(c['auto'] & 0x30001 == 0x10000, f"0x1b10 closed {c['auto']:#x} (model assumption 0x10000)")
    want(d['row_state'] == 'on' and d['measured'] == 1, f"row {d['row_state']}, measured {d['measured']}")
    t = [x for x in d['transitions'] if x['domain'] == 2]
    if not t:
        out.append('no domain-2 transition slot')
    else:
        t = t[0]
        adopted = t['commands'] == 0 and t['policy_before'] & 0xf == 8 and t['status_before'] == 8
        measured = (t['commands'] == 1 and t['policy_before'] & 0xf == 0 and t['status_before'] == 0 and
                    t['policy_written'] & 0xf == 8 and t['reached_word'] == 8 and t['reads'] == t['spin_reads'] and
                    t['all_or'] & ~8 == 0 and t['reached_at'] - t['command_at'] <= 10000)
        want(adopted or measured, f"domain 2 neither adopted ON nor one measured OFF-to-ON command: {t}")
    return out
