#!/usr/bin/env python3
"""Request the previous boot's fault report from a claimed integrated OS device.

USB0 operation 5 of the integrated OS image answers with the 1,160-byte report
of docs/fault-capture.md and no frame header. fetch() runs that one exchange
on a device that a supervised launcher has already claimed; this module opens
no device and has no command line. Decode the result with decode_report() of
checks/fault-decode.py, which defines the report layout.
"""
import struct
import time

FAULT_OP = 5
REPORT_BYTES = 1160
REQUEST = struct.pack('<BBHIQ', FAULT_OP, 0, 0, 0, 0)


def fetch(device, same, deadline, retain, now=time.monotonic):
    """Send one operation 5 request and return the raw report as a read-only view.

    A short copy of device_exchange() in apps/mimus-board/host.py: same() runs
    before every transfer, and each transfer's timeout is the time left before
    the deadline, capped at 500 ms and at least 1 ms. The fragments are appended
    to one buffer, and retain() and the caller receive the same read-only view
    of it, so each report byte is copied once on the host. retain(raw) must save
    the report durably. An empty fragment, more than REPORT_BYTES, an expired
    deadline or a transport error raises; there is no retry, because a second
    request after a partial response would mix two frames.
    """
    def timeout():
        remaining = deadline - now()
        if remaining <= 0:
            raise ValueError('Fault report deadline expired.')
        return min(500, max(1, int(remaining * 1000)))

    same()
    if device.write(0x02, REQUEST, timeout=timeout()) != len(REQUEST):
        raise ValueError('Incomplete fault report request.')
    report = bytearray()
    while len(report) < REPORT_BYTES:
        same()
        part = device.read(0x81, (REPORT_BYTES - len(report) + 511) // 512 * 512, timeout=timeout())
        if not part:
            raise ValueError('Empty fault report fragment.')
        report += part
        if len(report) > REPORT_BYTES:
            raise ValueError('Fault report too large.')
    same()
    if now() >= deadline:
        raise ValueError('Fault report expired.')
    raw = memoryview(report).toreadonly()
    retain(raw)
    return raw
