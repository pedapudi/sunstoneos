#!/usr/bin/env python3
"""Host side of rtc-alarm: decode the 288-byte result and compare it with the prediction.

The RTC runs from the internal RC oscillator. The decoder states the measured RTC second (system-counter time per RTC
second over the alarm interval) and does not assert 32.768 kHz accuracy. The retention datum is `initial`, the calendar
the RTC held when the run started: after an earlier run set day 20770 04:00:00, it must be that calendar plus the
elapsed real time divided by the measured RTC second; retention() compares two decoded runs."""
import struct

HEAD = struct.Struct('<QIIIIII')
TIME = struct.Struct('<7I')
INFO = struct.Struct('<7I5IIxxxxQ')
MAGIC = 0x315f4d4c4152435f
TICKS = 24e6
SET_DAY, SET_HMS = 20770, 4 << 16


def t(v):
    d, h, m, s, y, mo, da = v
    return {'day': d, 'hour': h, 'minute': m, 'second': s, 'date': '%04d-%02d-%02d' % (y, mo, da), 'seconds': d * 86400 + h * 3600 + m * 60 + s}


def decode(p):
    if len(p) != 288:
        raise ValueError(f'rtc-alarm result is {len(p)} bytes, not 288.')
    magic, version, failed, step, sd, sh, _ = HEAD.unpack_from(p, 0)
    if magic != MAGIC or version != 1:
        raise ValueError('rtc-alarm result has an unknown header.')
    i = INFO.unpack_from(p, 32)
    after = t(TIME.unpack_from(p, 96))
    after_ticks, = struct.unpack_from('<Q', p, 128)
    a = struct.unpack_from('<iI7I7IIIQQQII', p, 136)
    alarm = {'status': a[0], 'seconds': a[1], 'armed': t(a[2:9]), 'terminal': t(a[9:16]), 'target_day': a[16], 'target_hms': a[17],
             'arm_ticks': a[18], 'irq_ticks': a[19], 'done_ticks': a[20], 'irq_count': a[21]}
    final = t(TIME.unpack_from(p, 240))
    final_ticks, set_ticks = struct.unpack_from('<QQ', p, 272)
    out = {'failed': failed, 'step': step, 'initial': t(i[0:7]), 'clock_control': i[7], 'clock_status': i[8], 'prescaler': i[9],
           'calibration': i[10], 'oscillator': i[11], 'source': i[12], 'opened_ticks': i[13], 'after_set': after, 'after_set_ticks': after_ticks,
           'alarm': alarm, 'final': final, 'final_ticks': final_ticks, 'set_done_ticks': set_ticks}
    if alarm['irq_ticks'] > alarm['arm_ticks'] and alarm['status'] == 1:
        interval = alarm['target_day'] * 86400 + (alarm['target_hms'] >> 16) * 3600 + ((alarm['target_hms'] >> 8) & 63) * 60 + (alarm['target_hms'] & 63) - alarm['armed']['seconds']
        out['alarm_latency_ms'] = (alarm['done_ticks'] - alarm['irq_ticks']) / TICKS * 1000
        # Board result: the alarm interrupt arrives when the calendar reaches the target second plus one (the terminal
        # calendar reads target + 1 s), and the calendar read at arm lies somewhere in its RTC second. The wait is
        # therefore between `seconds` and `seconds + 1` RTC seconds.
        wait = (alarm['irq_ticks'] - alarm['arm_ticks']) / TICKS
        out['rtc_second_min_s'] = wait / (interval + 1)
        out['rtc_second_max_s'] = wait / interval
        out['alarm_wait_s'] = (alarm['irq_ticks'] - alarm['arm_ticks']) / TICKS
    out['note'] = 'RC oscillator: the RTC second is measured (rtc_second_s), not asserted; no 32.768 kHz accuracy is claimed.'
    return out


def failures(d):
    out = []
    if d['failed']:
        out.append(f"step {d['failed']} failed")
    if d['source'] != 2:
        out.append(f"source {d['source']}, expected the normal RC (2)")
    if d['after_set']['seconds'] - (SET_DAY * 86400 + SET_HMS // 65536 * 3600) not in (0, 1, 2):
        out.append('calendar after the set differs from the request by more than 2 s')
    a = d['alarm']
    if a['status'] != 1 or a['irq_count'] != 1:
        out.append(f"alarm status {a['status']} interrupts {a['irq_count']}")
    elif not 2.5 <= d['alarm_wait_s'] <= 6.0:
        out.append(f"alarm after {d['alarm_wait_s']:.2f} s of real time for 3 RTC seconds")
    elif a['terminal']['seconds'] < a['target_day'] * 86400 + (a['target_hms'] >> 16) * 3600 + ((a['target_hms'] >> 8) & 63) * 60 + (a['target_hms'] & 63):
        out.append('terminal calendar before the alarm target')
    return out


def retention(first, second, real_s, rtc_second_s=1.19):
    """Does the calendar of the later run's start continue the one the earlier run set? Returns (expected, actual, ok)."""
    set_at = first['alarm']['terminal']['seconds']
    expected = set_at + real_s / rtc_second_s
    actual = second['initial']['seconds']
    return expected, actual, abs(actual - expected) <= max(5, 0.1 * real_s / rtc_second_s)
