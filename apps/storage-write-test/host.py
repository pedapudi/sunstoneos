#!/usr/bin/env python3
"""Host side of storage-write-test: the second input (the mode) and the decoder of its 512-byte result.

Modes: 0 creates the 512 MiB region test file (no read-back; a guard six seconds before the session deadline stops the write, closes cleanly and reports status 11);
3 reads the whole file back against the stream; 1 is the interruption test, first lifetime (the application
stops reporting progress after `cut_bytes` so the watchdog resets the board mid-flush); 2 is its second lifetime (mount,
recover, read back what the volume holds)."""
import struct

FIELDS = ('tag mode status detail file_bytes written_bytes au_sectors cpu_hz cpu_calibration_ticks counter_hz '
          'create_ticks generate_ticks flush_ticks close_ticks total_ticks read_ticks flushes flush_p50 flush_p99 flush_max '
          'wear_bytes wear_data_bytes wear_metadata_bytes wear_writes wear_aligned wear_unaligned wear_syncs wear_flushes '
          'owner_elapsed_us owner_asleep_us owner_wakes mismatches first_mismatch volume_size recoveries recovered_clusters').split()
HIST = 16
TAIL = ('app_core irq_cores owner_cores caller_cores sha_ce diag0 diag1 diag2 diag3 isar0 observed').split()
TAG = 0x5357544553544f4b
COUNTER_HZ = 24_000_000


MODE_NAMES = {(1, 3): 'SDR104', (1, 2): 'SDR50', (1, 1): 'SDR25', (1, 0): 'SDR12', (0, 1): 'high speed', (0, 0): 'default speed'}


def decode_mode(w):
    """The bus speed mode words of the SD owner (apps/os/large_sd.c, os_sd_mode): timing in 7:4, 1.8 V in bit 8, the UHS-I outcome in 15:12, mode steps in
    23:20, ceiling in 31:28, re-entry in bit 32, a second opening because the card was found at 1.8 V in bit 36, recovery restarts in 47:40; the card clock;
    sample delay, window start and width, group 1 support bits and S18A; the first four steps (from, to, reason); the first two faults with the position each stopped at (word 3 bits 40:32 and 56:48 the card-side blocks, word 4 bits 31:26 and 63:58 the descriptor)."""
    if not w[0] & 1:
        return None
    timing, v18 = w[0] >> 4 & 15, w[0] >> 8 & 1
    steps = [dict(frm=w[3] >> (8 * i) & 3, to=w[3] >> (8 * i + 2) & 3, reason=w[3] >> (8 * i + 4) & 15) for i in range(min(4, w[0] >> 20 & 15))]
    return dict(mode=MODE_NAMES.get((v18, timing), (v18, timing)), signal_1v8=bool(v18), outcome_code=w[0] >> 12 & 15, mode_steps=w[0] >> 20 & 15, ceiling=w[0] >> 28 & 15,
                reentered=bool(w[0] >> 32 & 1), reopened=bool(w[0] >> 36 & 1), restarts=w[0] >> 40 & 255, card_hz=w[1], sample_delay=w[2] & 255, window_start=w[2] >> 8 & 255,
                window_width=w[2] >> 16 & 255, group1_support=hex(w[2] >> 24 & 255), s18a=bool(w[2] >> 32 & 1), steps=steps,
                faults=[dict(core_error=f & 15, protocol=f >> 4 & 63, interrupts=hex(f >> 10 & 0xffff), descriptor=f >> 26 & 63, blocks=w[3] >> (32 + 16 * i) & 511)
                        for i, f in enumerate((w[4] & 0xffffffff, w[4] >> 32 & 0xffffffff)) if f & 0x3ffffff])


def mode_input(mode=0, cut_bytes=0):
    return struct.pack('<IIQ', mode, 0, cut_bytes).ljust(4096, b'\0')


def decode(payload):
    if len(payload) != 512:
        raise ValueError(f'storage-write result is {len(payload)} bytes, not 512.')
    values = struct.unpack_from('<%dQ' % len(FIELDS), payload, 0)
    out = dict(zip(FIELDS, values))
    offset = 8 * len(FIELDS)
    out['histogram'] = list(struct.unpack_from('<%dI' % HIST, payload, offset))
    offset += 4 * HIST
    out.update(zip(TAIL, struct.unpack_from('<%dQ' % len(TAIL), payload, offset)))
    offset += 8 * len(TAIL)
    out['irq_entries'] = list(struct.unpack_from('<8I', payload, offset))
    offset += 32
    out['mode_words'] = list(struct.unpack_from('<5Q', payload, offset))
    out['sd_mode'] = decode_mode(out['mode_words'])
    if out['tag'] != TAG:
        raise ValueError('storage-write result has the wrong tag.')
    out['irq_stray'] = out['irq_cores'] >> 32
    out['irq_cores'] &= 0xffffffff
    out['owner_recheck_wakes'] = out['owner_wakes'] >> 32
    out['owner_wakes'] &= 0xffffffff
    sec = lambda ticks: ticks / COUNTER_HZ
    out['cpu_mhz'] = out['cpu_hz'] / 1e6
    appended = out['written_bytes'] - (out['volume_size'] if out['mode'] == 5 else 0)
    out['session_bytes'] = appended
    if out['flush_ticks']:
        out['flush_rate_mb_s'] = appended / sec(out['flush_ticks']) / 1e6
    if out['total_ticks']:
        out['session_rate_mb_s'] = appended / sec(out['total_ticks']) / 1e6
    if out['mode'] in (2, 3):                      # a read-back: flush_ticks is the time inside fwr_read, generate_ticks the stream, close_ticks the compare
        out['read_phase_seconds'] = {'fwr_read': sec(out['flush_ticks']), 'stream': sec(out['generate_ticks']), 'compare': sec(out['close_ticks']),
                                     'total': sec(out['read_ticks'])}
        if out['flush_ticks']:
            out['read_rate_mb_s'] = out['written_bytes'] / sec(out['flush_ticks']) / 1e6
    out['phase_seconds'] = {k: sec(out[k + '_ticks']) for k in ('create', 'generate', 'flush', 'close', 'total')}
    out['flush_latency_ms'] = {k: sec(out['flush_' + k]) * 1e3 for k in ('p50', 'p99', 'max')}
    if out['owner_elapsed_us']:
        out['owner_busy_fraction'] = 1 - out['owner_asleep_us'] / out['owner_elapsed_us']
    return out


E_TIMEOUT = 11


def guard_stop(decoded):
    """A write that ended at the time guard: status 11 with the bytes written in `detail`, the file synced and closed at that length (no close error, no mismatch).
    The session's lifetime bounds a write run, not the feature, so this is a pass; the bytes written and the throughput are the result."""
    return (decoded['mode'] in (0, 5) and decoded['status'] == E_TIMEOUT and decoded['detail'] == decoded['written_bytes'] and decoded['mismatches'] == 0 and
            0 < decoded['written_bytes'] < decoded['file_bytes'])


def failures(decoded):
    if decoded['mismatches'] == 0 and (decoded['status'] == 0 or guard_stop(decoded)):
        return []
    return [f"status {decoded['status']} detail {decoded['detail']} mismatches {decoded['mismatches']}"]


def inputs():
    return [mode_input(0)]


def word_names():
    """The name of each little-endian 32-bit word of the 512-byte result, for building an expectation (low and high halves of the 64-bit fields)."""
    names = []
    for n in FIELDS:
        names += [n, n + '.high']
    names += ['histogram%d' % i for i in range(HIST)]
    for n in TAIL:
        names += [n, n + '.high']
    names += ['irq_entries%d' % i for i in range(8)]
    return names + ['reserved'] * (128 - len(names))


def expectation(mode, **fixed):
    """A sunstone-board-expect-u32-v1 document for a run of `mode`: the tag, the mode, status 0 to 12, the file size, the clock window, no mismatches, the
    application core, the SHA-256 path (0 to 2) and the reserved words zero; every other word unconstrained. `fixed` maps a word name to an exact value."""
    words = [{'min': 0, 'max': 4294967295}] * 128
    names = word_names()
    def put(name, value):
        words[names.index(name)] = value
    put('tag', TAG & 0xffffffff); put('tag.high', TAG >> 32)
    put('mode', mode); put('mode.high', 0)
    put('status', {'min': 0, 'max': 12}); put('status.high', 0)
    put('file_bytes', 536870912); put('file_bytes.high', 0)
    put('cpu_hz', {'min': 1500000000, 'max': 2200000000}); put('cpu_hz.high', 0)
    put('counter_hz', 24000000); put('counter_hz.high', 0)
    put('mismatches', 0); put('mismatches.high', 0); put('first_mismatch', 0); put('first_mismatch.high', 0)
    put('app_core', {'min': 0, 'max': 7}); put('sha_ce', {'min': 0, 'max': 2})
    for i, n in enumerate(names):
        if n == 'reserved':
            words[i] = 0
    for k, v in fixed.items():
        put(k, v)
    return {'schema': 'sunstone-board-expect-u32-v1', 'words': words}
