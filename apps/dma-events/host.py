"""Decode the native DMA owner's finite result and idle-wake observation."""
import struct

HEAD = struct.Struct('<6Q')
SNAPSHOT = struct.Struct('<7Q2I')
NAMES = ('waits', 'wakes', 'idle_us', 'idle_notifications', 'idle_timeouts', 'deadline_expiries', 'irqs', 'waiting', 'retired')


def decode(payload):
    if len(payload) != HEAD.size + 3 * SNAPSHOT.size + 32:
        raise ValueError('DMA diagnostic result size differs')
    magic, version, begin, end, jobs, compared = HEAD.unpack_from(payload)
    if (magic, version, jobs, compared) != (0x31545645414d444f, 1, 2, 8192) or end < begin or end - begin < 1000000:
        raise ValueError('DMA diagnostic identity, copy count or idle duration differs')
    records = [dict(zip(NAMES, SNAPSHOT.unpack_from(payload, HEAD.size + i * SNAPSHOT.size))) for i in range(3)]
    before, after, stopped = records
    per_job = struct.unpack_from('<4Q', payload, HEAD.size + 3 * SNAPSHOT.size)
    if (before['waiting'], after['waiting'], stopped['retired']) != (1, 1, 1):
        raise ValueError('DMA diagnostic lacks its idle wait or retirement proof')
    if any(after[name] != before[name] for name in ('wakes', 'idle_timeouts', 'irqs')):
        raise ValueError('DMA owner wakes without an event in the idle window')
    if stopped['irqs'] < after['irqs'] + jobs:
        raise ValueError('DMA job interrupts are missing')
    if any(v < 1 for v in per_job[:2]) or any(per_job[2:]):
        raise ValueError('DMA per-job interrupt or deadline receipt differs')
    return {'jobs': jobs, 'compared_bytes': compared, 'idle_us': end - begin,
            'idle_wakes': after['wakes'] - before['wakes'], 'idle_wakes_per_second': 0,
            'job_irqs': stopped['irqs'] - after['irqs'], 'per_job_irqs': list(per_job[:2]),
            'before': before, 'after': after, 'retired': stopped}
