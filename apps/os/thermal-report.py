#!/usr/bin/env python3
"""Decode the first recognition's immutable CPU-clock thermal window.

Operation 7 returns pages through the existing 64-byte USB response frame.
Only CPU-clock OS images answer it. Version 2 rows use a 125 ms schedule from the
thermal owner, in physical order CPUB, DDR, NPU, CPUL, GPU. The report survives
later recognitions; a critical board failure can prevent its USB delivery.
"""
import struct

OP, MAGIC = 7, 0x5448524d
LEGACY_HEADER_BYTES, HEADER_BYTES, ROW_BYTES, MAX_ROWS = 72, 80, 56, 602
MAX_BYTES = HEADER_BYTES + ROW_BYTES * MAX_ROWS
PERIOD_US = 125000
# configTICK_RATE_HZ=1000: one wakeup tick and one read/publication tick.
# Integer microseconds from the 24 MHz counter add at most 1 us. This is an
# acceptance budget, not a guarantee: longer lock/read/scheduler delays fail.
LATENESS_US = 2 * 1000 + 1
FIELDS = ('magic', 'version', 'bytes', 'rows', 'epoch', 'generation', 'recognition_generation',
          'begin_us', 'end_us', 'overflow', 'cpu_critical_millicelsius', 'row_bytes', 'recognition_ok')


def require(ok, message):
    if not ok:
        raise ValueError(message)


def header(raw, epoch, generation):
    require(len(raw) >= LEGACY_HEADER_BYTES, 'Short thermal header.')
    h = dict(zip(FIELDS, struct.unpack_from('<4I5Q4I', raw)))
    require(h['magic'] == MAGIC and h['version'] in (1, 2) and h['row_bytes'] == ROW_BYTES,
            'Thermal layout differs.')
    head_bytes = LEGACY_HEADER_BYTES if h['version'] == 1 else HEADER_BYTES
    require(len(raw) >= head_bytes, 'Short thermal header.')
    if h['version'] == 2:
        h['scheduled_first_us'] = struct.unpack_from('<Q', raw, LEGACY_HEADER_BYTES)[0]
    require((h['epoch'], h['generation']) == (epoch, generation), 'Thermal session differs.')
    require(h['rows'] <= MAX_ROWS and h['bytes'] == head_bytes + h['rows'] * ROW_BYTES,
            'Thermal extent differs.')
    require(h['overflow'] in (0, 1) and h['recognition_ok'] in (0, 1) and h['recognition_generation'] and
            0 < h['cpu_critical_millicelsius'] <= 110000 and h['begin_us'] <= h['end_us'],
            'Thermal window fields differ.')
    return h


def decode(raw, epoch, generation):
    h = header(raw, epoch, generation)
    require(len(raw) == h['bytes'], 'Thermal report incomplete.')
    rows = []
    for offset in range(LEGACY_HEADER_BYTES if h['version'] == 1 else HEADER_BYTES, len(raw), ROW_BYTES):
        values = struct.unpack_from('<Q5i6I4B', raw, offset)
        row = dict(time_us=values[0], millicelsius=list(values[1:6]), age_us=list(values[6:11]),
                   sequence=values[11], seen=values[12], stale=values[13], level=values[14], sensor=values[15])
        require(row['seen'] <= 31 and row['stale'] <= 31 and row['level'] <= 2 and row['sensor'] <= 1,
                'Invalid thermal row flags.')
        require(not rows or row['time_us'] > rows[-1]['time_us'] and row['sequence'] >= rows[-1]['sequence'],
                'Thermal rows replayed or out of order.')
        rows.append(row)
    h['readings'] = rows
    return h


def classify(report):
    """Evidence completeness, never physical qualification of an operating point."""
    rows = report['readings']
    if report['overflow']:
        return 'overflow'
    if not report['recognition_ok']:
        return 'recognition-failed'
    if any(r['level'] == 2 or r['sensor'] for r in rows):
        return 'thermal-stop'
    if (not rows or rows[0]['time_us'] > report['begin_us'] or rows[-1]['time_us'] > report['end_us'] or
            any(r['seen'] != 31 or r['stale'] or max(r['age_us']) > 1000000 for r in rows)):
        return 'incomplete'
    if 'scheduled_first_us' in report:
        first = report['scheduled_first_us']
        # One row must cover each scheduled slot in the recognition window.
        # Compare actual times to that fixed phase: adjacent gaps include both
        # samples' jitter and cannot distinguish drift from a missing sample.
        if (first > report['begin_us'] or report['begin_us'] > first + PERIOD_US + LATENESS_US or
                any(not 0 <= row['time_us'] - (first + i * PERIOD_US) <= LATENESS_US
                    for i, row in enumerate(rows)) or
                report['end_us'] > first + len(rows) * PERIOD_US + LATENESS_US):
            return 'incomplete'
    elif (report['end_us'] - rows[-1]['time_us'] > 125000 or
            any(b['time_us'] - a['time_us'] > 125000 for a, b in zip(rows, rows[1:]))):
        # Archived images publish no schedule origin and retain their legacy
        # evidence rule. Current producers are checked against their phase.
        return 'incomplete'
    return 'complete'


def capture(exchange, epoch, generation):
    """Header pages and at most 33 row pages. No retry or wait for a pending window."""
    def page(offset, count, expected=0, total=None):
        frame = exchange(struct.pack('<BBHIQ', OP, 0, count, offset, expected))
        require(len(frame) >= 64, 'Short thermal frame.')
        magic, version, state, size, extent, position, storage, op = struct.unpack_from('<8I', frame)
        require((magic, version, position, storage, op) == (0x424d494d, 1, offset, 0, OP) and
                struct.unpack_from('<2Q', frame, 32) == (epoch, generation) and not any(frame[48:64]),
                'Thermal frame identity differs.')
        require(state == 1 and size == count and len(frame) == 64 + count,
                'Thermal window pending, refused or incomplete.')
        require(extent <= MAX_BYTES and (total is None or extent == total), 'Thermal extent changed.')
        return extent, bytes(frame[64:])
    total, first = page(0, LEGACY_HEADER_BYTES)
    if struct.unpack_from('<I', first, 4)[0] == 2:
        _, extra = page(LEGACY_HEADER_BYTES, HEADER_BYTES - LEGACY_HEADER_BYTES,
                        struct.unpack_from('<Q', first, 32)[0], total)
        first += extra
    h = header(first, epoch, generation)
    require(total == h['bytes'], 'Thermal frame and header extents differ.')
    raw = bytearray(first)
    while len(raw) < total:
        _, data = page(len(raw), min(1024, total - len(raw)), h['recognition_generation'], total)
        raw.extend(data)
    return bytes(raw)
