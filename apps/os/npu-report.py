#!/usr/bin/env python3
"""Request and decode the OS NPU client's report from a claimed device.

USB0 operation 6 of an integrated OS image built with SUNSTONE_A733_OS_NPU
answers with the 104-byte report of apps/os/npu.h and no frame header. Other
OS images stop USB0 on this operation. fetch() runs that one exchange on a
device that a supervised launcher has already claimed; this module opens no
device and has no command line.

classify() keeps the outcomes apart:

- 'pending': the client has not published its report yet;
- 'matched': every completed job's outputs equal the linked reference;
- 'mismatch': at least one completed job's outputs differ from the reference,
  with the bytes that kept the SUBMIT fill pattern in the last job reported;
- 'refused': a thermal stop or the lifetime ended the work early, with no
  mismatch among the completed jobs;
- 'retained': a runtime, transport or hardware failure kept every NPU resource.

A transport error during fetch() raises ValueError and yields no report.
"""
import struct
import time

REPORT_OP = 6
REPORT_BYTES = 104
MAGIC = 0x5255504e
PATTERN = 0xc35a3ca5
REQUEST = struct.pack('<BBHIQ', REPORT_OP, 0, 0, 0, 0)
FIELDS = ('magic', 'version', 'bytes', 'finished', 'epoch', 'generation', 'model', 'jobs_selected', 'jobs',
          'mismatches', 'refused', 'retained', 'stopped', 'output_bytes', 'pattern_bytes', 'pattern')
LAYOUT = '<4I2Q10I'


def fetch(device, same, deadline, retain, now=time.monotonic):
    """Send one operation 6 request and return the raw report as a read-only view.

    The exchange follows fetch() in tools/fault-report.py: same() runs before
    every transfer, each timeout is the time left before the deadline (at most
    500 ms, at least 1 ms), and retain(raw) must save the report durably. An
    empty fragment, more than REPORT_BYTES, an expired deadline or a transport
    error raises; there is no retry.
    """
    def timeout():
        remaining = deadline - now()
        if remaining <= 0:
            raise ValueError('NPU report deadline expired.')
        return min(500, max(1, int(remaining * 1000)))

    same()
    if device.write(0x02, REQUEST, timeout=timeout()) != len(REQUEST):
        raise ValueError('Incomplete NPU report request.')
    report = bytearray()
    while len(report) < REPORT_BYTES:
        same()
        part = device.read(0x81, 512, timeout=timeout())
        if not part:
            raise ValueError('Empty NPU report fragment.')
        report += part
        if len(report) > REPORT_BYTES:
            raise ValueError('NPU report too large.')
    same()
    if now() >= deadline:
        raise ValueError('NPU report expired.')
    raw = memoryview(report).toreadonly()
    retain(raw)
    return raw


def decode(raw, epoch, generation):
    """The report's fields; raises ValueError for another layout or session."""
    raw = bytes(raw)
    if len(raw) != REPORT_BYTES:
        raise ValueError('NPU report size differs.')
    r = dict(zip(FIELDS, struct.unpack_from(LAYOUT, raw)))
    r['last_digest'] = raw[72:104].hex()
    if (r['magic'], r['version'], r['bytes']) != (MAGIC, 1, REPORT_BYTES):
        raise ValueError('Not an NPU report.')
    if (r['epoch'], r['generation']) != (epoch, generation):
        raise ValueError('NPU report names another session.')
    flags = ('finished', 'refused', 'retained', 'stopped')
    if any(r[k] not in (0, 1) for k in flags):
        raise ValueError('Invalid NPU report flag.')
    if not r['finished']:
        if any(r[k] for k in FIELDS[6:]) or any(raw[72:]):
            raise ValueError('A pending NPU report carries results.')
        return r
    if (r['pattern'] != PATTERN or r['jobs'] > r['jobs_selected'] or r['mismatches'] > r['jobs'] or
            r['pattern_bytes'] > r['output_bytes'] or (r['jobs'] == 0) != (r['output_bytes'] == 0) or
            (r['jobs'] == 0 and (r['pattern_bytes'] or any(raw[72:])))):
        raise ValueError('Inconsistent NPU report.')
    if not r['retained'] and not r['stopped']:
        raise ValueError('An NPU report without retention must follow a proven stop.')
    return r


def classify(report):
    """One outcome per report; numerical mismatch never hides a retention."""
    if not report['finished']:
        return 'pending'
    if report['retained']:
        return 'retained'
    if report['mismatches']:
        return 'mismatch'
    if report['refused'] or report['jobs'] < report['jobs_selected']:
        return 'refused'
    return 'matched'
