"""Decode the returning scheduler wait fixture's version-one result."""
import struct

MAGIC = 0x3153544941575645
FORMAT = struct.Struct('<7Q6I')
TASKS, TLBI_CORE, RUN_US, PHASE_US = 24, 5, 3000000, 600000


def inputs():
    return []


def decode(payload):
    if len(payload) != FORMAT.size:
        raise ValueError('event wait result must contain 80 bytes')
    (magic, elapsed, stopping, bursts1, bursts2, busy1, busy2,
     version, waits, stopped, wakes, issued, core) = FORMAT.unpack(payload)
    if (magic, version, stopped, issued, core) != (MAGIC, 1, TASKS, 0, TLBI_CORE):
        raise ValueError('event wait identity, worker count or control form differs')
    if not all((bursts1, bursts2, busy1, busy2, waits, wakes)):
        raise ValueError('event wait result has an empty phase or observation')
    if not 2 * PHASE_US <= elapsed <= RUN_US or stopping > elapsed - 2 * PHASE_US:
        raise ValueError('event wait duration exceeds its declared phases or run limit')
    return dict(version=version, elapsed_us=elapsed, stopping_us=stopping,
                application_waits=waits, stopped_workers=stopped,
                phases=[dict(bursts=bursts1, busy_us=busy1), dict(bursts=bursts2, busy_us=busy2)],
                tlbi=dict(wakes=wakes, issued=issued, core=core,
                          observed_interval_us=elapsed / wakes,
                          interpretation='Elapsed workload time divided by observed control wakes; sampling overhead is included. Missed periods may be skipped.'))
