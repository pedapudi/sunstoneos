#!/usr/bin/env python3
"""Host side of storage-write-test: the second input (the mode) and the decoder of its 512-byte result.

Modes: 0 creates the 512 MiB region test file (no read-back; a 40 s guard stops the write, closes cleanly and reports status 11);
3 reads the whole file back against the stream; 1 is the interruption test, first lifetime (the application
stops reporting progress after `cut_bytes` so the watchdog resets the board mid-flush); 2 is its second lifetime (mount,
recover, read back what the volume holds)."""
import struct

FIELDS = ('tag mode status detail file_bytes written_bytes au_sectors cpu_hz cpu_calibration_ticks counter_hz '
          'create_ticks generate_ticks flush_ticks close_ticks total_ticks read_ticks flushes flush_p50 flush_p99 flush_max '
          'wear_bytes wear_data_bytes wear_metadata_bytes wear_writes wear_aligned wear_unaligned wear_syncs wear_flushes '
          'owner_elapsed_us owner_asleep_us owner_wakes mismatches first_mismatch volume_size recoveries recovered_clusters').split()
HIST = 16
TAIL = ('app_core irq_cores owner_cores caller_cores sha_ce guard_us diag0 diag1 diag2 diag3 isar0 observed').split()
TAG = 0x5357544553544f4b
COUNTER_HZ = 24_000_000


def mode_input(mode=0, cut_bytes=0):
    return struct.pack('<IIQ', mode, 0, cut_bytes).ljust(4096, b'\0')


def decode(payload):
    if len(payload) != 512:
        raise ValueError(f'storage-write result is {len(payload)} bytes, not 512.')
    n = len(FIELDS) + HIST + len(TAIL)
    values = struct.unpack('<%dQ' % n, payload[:8 * n])
    out = dict(zip(FIELDS, values))
    out['histogram'] = list(values[len(FIELDS):len(FIELDS) + HIST])
    out.update(zip(TAIL, values[len(FIELDS) + HIST:]))
    if out['tag'] != TAG:
        raise ValueError('storage-write result has the wrong tag.')
    sec = lambda ticks: ticks / COUNTER_HZ
    out['cpu_mhz'] = out['cpu_hz'] / 1e6
    if out['flush_ticks']:
        out['flush_rate_mb_s'] = out['written_bytes'] / sec(out['flush_ticks']) / 1e6
    out['flush_latency_ms'] = {k: sec(out['flush_' + k]) * 1e3 for k in ('p50', 'p99', 'max')}
    if out['owner_elapsed_us']:
        out['owner_busy_fraction'] = 1 - out['owner_asleep_us'] / out['owner_elapsed_us']
    return out


def failures(decoded):
    return [] if decoded['status'] == 0 and decoded['mismatches'] == 0 else [f"status {decoded['status']} detail {decoded['detail']} mismatches {decoded['mismatches']}"]


def inputs():
    return [mode_input(0)]
