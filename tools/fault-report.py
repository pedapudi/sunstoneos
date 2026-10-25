#!/usr/bin/env python3
"""Request the previous boot's fault report from a claimed integrated OS device.

USB0 operation 5 of the integrated OS image answers with the 1,160-byte report
of docs/fault-capture.md and no frame header. fetch() runs that one exchange
on a device that a supervised launcher has already claimed; this module opens
no device and has no command line. Decode the result with decode_report() of
checks/fault-decode.py, which defines the report layout. The same exchange
reads the other fixed report without a frame header, the 104-byte NPU report
of operation 6 (apps/os/npu-report.py).
"""
import struct
import time

FAULT_OP = 5
REPORT_BYTES = 1160
REQUEST = struct.pack('<BBHIQ', FAULT_OP, 0, 0, 0, 0)


def fetch(device, same, deadline, retain, now=time.monotonic, *, op=FAULT_OP, size=REPORT_BYTES,
          name='fault report'):
    """Send one operation op request and return the raw size-byte report as a read-only view.

    A short copy of device_exchange() in apps/mimus-board/host.py: same() runs
    before every transfer, and each transfer's timeout is the time left before
    the deadline, capped at 500 ms and at least 1 ms. The fragments are appended
    to one buffer, and retain() and the caller receive the same read-only view
    of it, so each report byte is copied once on the host. retain(raw) must save
    the report durably. An empty fragment, more than size bytes, an expired
    deadline or a transport error raises; there is no retry, because a second
    request after a partial response would mix two frames. Error messages
    call the report name.
    """
    request = struct.pack('<BBHIQ', op, 0, 0, 0, 0)
    title = name[:1].upper() + name[1:]

    def timeout():
        remaining = deadline - now()
        if remaining <= 0:
            raise ValueError(title + ' deadline expired.')
        return min(500, max(1, int(remaining * 1000)))

    same()
    if device.write(0x02, request, timeout=timeout()) != len(request):
        raise ValueError('Incomplete ' + name + ' request.')
    report = bytearray()
    while len(report) < size:
        same()
        part = device.read(0x81, (size - len(report) + 511) // 512 * 512, timeout=timeout())
        if not part:
            raise ValueError('Empty ' + name + ' fragment.')
        report += part
        if len(report) > size:
            raise ValueError(title + ' too large.')
    same()
    if now() >= deadline:
        raise ValueError(title + ' expired.')
    raw = memoryview(report).toreadonly()
    retain(raw)
    return raw
