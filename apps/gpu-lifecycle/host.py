#!/usr/bin/env python3
"""Validate the complete GPU lifecycle result copied by the returning runner."""
import struct


def decode(payload):
    if len(payload) != 88:
        raise ValueError('GPU lifecycle result must contain 88 bytes.')
    magic, mode, opens, closes, jobs, sequence, checksum, fault, detail, recoveries, peer_jobs, elements, *times = struct.unpack('<22I', payload)
    if magic != 0x314c5047 or elements != 65 or checksum != 8840:
        raise ValueError('GPU lifecycle format, elements or checksum differs.')
    if mode == 1:
        if (opens, closes, jobs, sequence, fault, detail, recoveries, peer_jobs) != (10, 10, 10, 10, 0, 0, 0, 1):
            raise ValueError('GPU lifecycle cycle counts or peer completion differs.')
    elif mode == 2:
        if (opens, closes, jobs, sequence, fault, detail, recoveries, peer_jobs) != (2, 1, 1, 2, 4, 11, 1, 1):
            raise ValueError('GPU lifecycle fault, recovery or new-job result differs.')
    else:
        raise ValueError('GPU lifecycle mode differs.')
    if any(value >= 200000 for value in times[:jobs]) or any(times[jobs:]):
        raise ValueError('GPU lifecycle timing exceeds the job budget or unused timing is nonzero.')
    return {'mode': mode, 'opens': opens, 'closes': closes, 'jobs': jobs,
            'sequence': sequence, 'checksum': checksum, 'recoveries': recoveries,
            'peer_jobs': peer_jobs, 'elements': elements}
