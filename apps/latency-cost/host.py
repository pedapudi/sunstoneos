"""Decode complete task-notification round trips from a native cost report."""
import struct

MAGIC = 0x53534C434F535431
SAMPLES, ROUNDTRIPS = 48, 32


def inputs():
    return []


def decode(payload):
    if len(payload) != 7784:
        raise ValueError('latency cost result must contain 7784 bytes')
    magic, frequency, enabled, samples, trips, mask, counter_min = struct.unpack_from('<2Q4IQ', payload)
    if (magic, samples, trips, mask) != (MAGIC, SAMPLES, ROUNDTRIPS, 255) or enabled not in (0, 1) or not frequency:
        raise ValueError('latency cost report header differs')
    cores = []
    for c in range(8):
        offset = 40 + c * (SAMPLES * 16 + 8)
        ticks = struct.unpack_from(f'<{SAMPLES}Q', payload, offset)
        switches = struct.unpack_from(f'<{SAMPLES}Q', payload, offset + SAMPLES * 8)
        timer_mask, = struct.unpack_from('<Q', payload, offset + SAMPLES * 16)
        if timer_mask >> SAMPLES or any(s < 2 * ROUNDTRIPS for s in switches) or not all(ticks):
            raise ValueError(f'core {c} has invalid switch coverage or timer flags')
        selected = [i for i in range(SAMPLES) if switches[i] == 2 * ROUNDTRIPS and not (timer_mask >> i & 1)]
        def summary(indices):
            values = [ticks[i] * 1e9 / frequency / (2 * ROUNDTRIPS) for i in indices]
            return dict(batches=len(indices), minimum_ns=min(values), mean_ns=sum(values) / len(values),
                        maximum_ns=max(values)) if values else None
        cores.append(dict(core=c, ticks=ticks, switches=switches, timer_mask=timer_mask,
                          all_batches=summary(range(SAMPLES)), no_tick_or_extra_switch=summary(selected)))
    irq_ticks = struct.unpack_from('<96Q', payload, 6248)
    irq_os_ticks = struct.unpack_from('<96Q', payload, 6248 + 96 * 8)
    if not all(irq_ticks) or any(t > 3 for t in irq_os_ticks):
        raise ValueError('timer handler duration or catch-up coverage differs')
    irq = {}
    for os_ticks in sorted(set(irq_os_ticks)):
        values = [ticks * 1e9 / frequency for ticks, n in zip(irq_ticks, irq_os_ticks) if n == os_ticks]
        irq[str(os_ticks)] = dict(samples=len(values), minimum_ns=min(values),
                                 mean_ns=sum(values) / len(values), maximum_ns=max(values))
    return dict(enabled=bool(enabled), frequency=frequency, samples=samples, roundtrips=trips,
                counter_pair_min_ticks=counter_min, cores=cores,
                irq_ticks=irq_ticks, irq_os_ticks=irq_os_ticks, timer_handler_by_tick_count=irq,
                interpretation='Complete notification round-trip time divided by two switches; includes notification API and timing overhead. Timer flags observe OS tick changes, not every possible IRQ. IRQ samples time the complete application IRQ handler and its recorder wrapper; common exception entry/return and subsequent context restore are outside the interval.')
