#!/usr/bin/env python3
"""Host side of rtc-crystal: decode the 544-byte result and judge the crystal selection against its prediction.

Prediction: the open reads the RC (source 2); five RTC seconds on the RC take about 1.19 s each; the selection ends with
source 1 (status & 7 == 1, control & 0x193 == 0x11) and four healthy samples; the calendar read before and after differs
by 0 or 1 s (the switch takes milliseconds); ten RTC seconds on the crystal take 10.000000 s of system counter time to
within 50 ppm. The measurement resolution is the 20 us poll step plus the wake and read latency at each of the two
edges, under 100 us in all, so under 10 ppm over ten seconds; the reference is the 24 MHz system counter, whose own
error is not known to better than the oscillator tolerance, which the 50 ppm allows for."""
import struct

TICKS = 24e6
MAGIC = 0x315f5359525f4354
TIME = struct.Struct('<7I')


def t(b, o):
    d, h, m, s, y, mo, da = TIME.unpack_from(b, o)
    return {'day': d, 'seconds': d * 86400 + h * 3600 + m * 60 + s, 'date': '%04d-%02d-%02d' % (y, mo, da), 'clock': '%02d:%02d:%02d' % (h, m, s)}


def rate(b, o):
    status, seconds, source, polls, a, bb = struct.unpack_from('<4IQQ', b, o)
    r = {'status': status, 'seconds': seconds, 'source': source, 'polls': polls, 'edge_a_ticks': a, 'edge_b_ticks': bb}
    if status == 1 and bb > a:
        r['rtc_second_s'] = (bb - a) / TICKS / seconds
        r['error_ppm'] = ((bb - a) / TICKS / seconds - 1.0) * 1e6
    return r


def decode(p):
    if len(p) != 544:
        raise ValueError(f'rtc-crystal result is {len(p)} bytes, not 544.')
    magic, version, failed, step, _ = struct.unpack_from('<QIIII', p, 0)
    if magic != MAGIC or version != 1:
        raise ValueError('rtc-crystal result has an unknown header.')
    info = {'initial': t(p, 24), 'clock_control': struct.unpack_from('<I', p, 52)[0], 'clock_status': struct.unpack_from('<I', p, 56)[0],
            'source': struct.unpack_from('<I', p, 72)[0]}
    sw = struct.unpack_from('<8I', p, 224)
    crystal = {'result': sw[0], 'control_before': sw[1], 'status_before': sw[2], 'control_after': sw[3], 'status_after': sw[4],
               'samples': sw[5], 'healthy_run': sw[6], 'writes': sw[7], 'before': t(p, 256), 'after': t(p, 284)}
    a = struct.unpack_from('<iI', p, 400)
    alarm = {'status': a[0], 'seconds': a[1], 'armed': t(p, 408), 'terminal': t(p, 436)}
    ar = struct.unpack_from('<IIQQQI', p, 464)
    alarm.update({'target_day': ar[0], 'target_hms': ar[1], 'arm_ticks': ar[2], 'irq_ticks': ar[3], 'done_ticks': ar[4], 'irq_count': ar[5]})
    return {'failed': failed, 'step': step, 'info': info, 'rc_rate': rate(p, 192), 'crystal': crystal, 'crystal_rate': rate(p, 328),
            'after_set': t(p, 360), 'alarm': alarm, 'final': t(p, 504)}


def failures(d):
    out = []
    if d['failed']:
        out.append(f"step {d['failed']} failed")
    if d['info']['source'] != 2:
        out.append(f"opened on source {d['info']['source']}, expected the RC (2); the board may already be on the crystal")
    c = d['crystal']
    if c['result'] != 1:
        out.append(f"selection result {c['result']} (2 is the fallback to the RC): control {c['control_after']:#x} status {c['status_after']:#x} healthy run {c['healthy_run']} of {c['samples']} samples")
    else:
        if (c['control_after'] & 0x193) != 0x11 or (c['status_after'] & 7) != 1 or c['healthy_run'] != 4:
            out.append('external state not as expected after the switch')
        jump = c['after']['seconds'] - c['before']['seconds']
        if jump not in (0, 1):
            out.append(f'calendar jumped {jump} s across the switch')
    r = d['rc_rate']
    if r.get('status') != 1 or not 1.0 <= r.get('rtc_second_s', 0) <= 1.4:
        out.append(f"RC second {r.get('rtc_second_s')} outside 1.0 to 1.4 s")
    x = d['crystal_rate']
    if c['result'] == 1 and (x.get('status') != 1 or abs(x.get('error_ppm', 1e9)) > 50):
        out.append(f"crystal second error {x.get('error_ppm')} ppm, expected within 50 ppm")
    a = d['alarm']
    if a['status'] != 1 or a['irq_count'] != 1:
        out.append(f"alarm status {a['status']} interrupts {a['irq_count']}")
    return out
