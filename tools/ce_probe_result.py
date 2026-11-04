#!/usr/bin/env python3
"""Decode the CE probe's record (include/a733_ce_probe.h). The record holds no random byte."""
import struct
import sys

MAGIC = 0x3152504345333337
SIZE = 96
STAGES = ['new', 'entry-read', 'clocked', 'buffers', 'configured', 'requested', 'done']


def decode(data):
    """The record as plain data; ValueError if it is not a CE probe record."""
    if len(data) != SIZE:
        raise ValueError('CE probe record is %d bytes, not %d' % (len(data), SIZE))
    (magic, version, size, stage, failed, failure_stage, flags, entry_clk, entry_bgr, entry_mbus, clocked_clk,
     clocked_bgr, clocked_mbus, status, error, lpc, returned, task_us, limit_us, descriptor, output) = \
        struct.unpack('<QII4I3I3I3I3IQQ', data)
    if magic != MAGIC or version != 1 or size != SIZE or stage >= len(STAGES) or failure_stage > len(STAGES):
        raise ValueError('not a CE probe record')
    return {
        'stage': STAGES[stage], 'failed': bool(failed),
        'failed_at': STAGES[failure_stage] if failed and failure_stage < len(STAGES) else None,
        'health_tests_at_hardware_defaults_not_programmed': bool(flags & 1),
        'found_enabled_and_cycled': bool(flags & 2), 'task_completed': bool(flags & 4),
        'block_not_all_zero': bool(flags & 8), 'double_entropy_source_set': bool(flags & 16),
        'entry': {'ce_clk': entry_clk, 'ce_bgr': entry_bgr, 'mbus': entry_mbus},
        'clocked': {'ce_clk': clocked_clk, 'ce_bgr': clocked_bgr, 'mbus': clocked_mbus},
        'task': {'status': status, 'error': error, 'lpc': lpc}, 'bytes_returned': returned,
        'task_us': task_us, 'step_limit_us': limit_us,
        'descriptor_physical': descriptor, 'output_physical': output,
    }


if __name__ == '__main__':
    import json
    print(json.dumps(decode(open(sys.argv[1], 'rb').read()), indent=2))
