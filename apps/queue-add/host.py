#!/usr/bin/env python3
"""Validate the complete queue-add binary result emitted by ss_finish.

Completion times are bounded by the application's 200000-microsecond job
budget. --expect-result on tools/dev-run.py selects the exact expected job
count while the copied result.bin preserves every measured timing value.
"""
import struct


def decode(payload):
    if len(payload) != 48:
        raise ValueError('queue-add result must contain 48 bytes.')
    jobs, elements, checksum, sequence, *times = struct.unpack('<12I', payload)
    if not 1 <= jobs <= 8 or (elements, checksum, sequence) != (65, 8840, jobs):
        raise ValueError('queue-add jobs, elements, checksum or sequence differs.')
    if any(value >= 200000 for value in times[:jobs]) or any(times[jobs:]):
        raise ValueError('queue-add active timing exceeds the job budget or unused timing is nonzero.')
    return {'jobs': jobs, 'elements': elements, 'checksum': checksum, 'sequence': sequence}
