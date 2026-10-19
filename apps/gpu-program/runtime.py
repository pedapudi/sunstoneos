#!/usr/bin/env python3
"""Replace input and copy results within an already running trusted GPU session."""
import struct
import math
import time

MAGIC = 0x49525047
READ, SUBMIT, CLOSE = 1, 2, 3

def frame(operation, sequence, epoch, first=0, data=b''):
    if not 0 < epoch < 2**64 or not 0 <= sequence < 2**32 or len(data) > 4096:
        raise ValueError('GPU request bounds')
    return struct.pack('<4IQ2I', MAGIC, 2, operation, sequence, epoch, first, len(data)) + data

def decode(raw, epoch, total, chunk=0):
    if len(raw) < 40:
        raise ValueError('Short GPU reply')
    magic, version, result, first, count, words, sequence, lo, hi, acquired = struct.unpack_from('<10I', raw)
    if (magic, version, first, words, lo | hi << 32) != (0x50555047, 2, chunk, total, epoch):
        raise ValueError('GPU response identity')
    if result not in (0, 1, 2, 3, 4) or len(raw) != 40 + 4 * count:
        raise ValueError('GPU response bounds')
    if result == 1:
        expected = min(32, total - chunk * 32)
        if not sequence or count != expected or acquired != total:
            raise ValueError('GPU result is incomplete')
    elif count or acquired:
        raise ValueError('Unacquired GPU output')
    if result in (2, 4):
        raise ValueError('GPU session failed or request refused')
    return result, sequence, raw[40:]

def sequence(exchange, epoch, total, input_data, timeout=20):
    """exchange receives one complete frame and returns one copied response."""
    if not 0 < total <= 992 or not 4 <= len(input_data) <= 4096 or timeout <= 0 or not math.isfinite(timeout):
        raise ValueError('GPU input bounds')
    deadline = time.monotonic() + timeout
    def transaction(op, seq, first=0, data=b''):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('GPU runtime deadline')
        raw = exchange(frame(op, seq, epoch, first, data), remaining)
        if time.monotonic() > deadline:
            raise TimeoutError('GPU runtime deadline after response')
        return decode(raw, epoch, total, first // 32)
    def output(seq):
        result = bytearray()
        for first in range(0, total, 32):
            while True:
                state, actual, data = transaction(READ, seq, first)
                if state == 1:
                    if actual != seq:
                        raise ValueError('GPU output sequence changed')
                    result += data
                    break
                if state != 0:
                    raise ValueError('GPU closed before output')
                time.sleep(.001)
        return bytes(result)
    before = output(1)
    state, actual, _ = transaction(SUBMIT, 1, data=input_data)
    if state != 0 or actual != 1:
        raise ValueError('GPU replacement acknowledgment differs')
    after = output(2)
    state, actual, _ = transaction(CLOSE, 2)
    if state != 0 or actual != 2:
        raise ValueError('GPU close acknowledgment differs')
    while True:
        state, actual, _ = transaction(READ, 2)
        if state == 3 and actual == 2:
            break
        if state != 0:
            raise ValueError('GPU shutdown incomplete')
        time.sleep(.001)
    return before, after

def capture(device, epoch, words, input_data, timeout=20):
    """Use a coordinator-owned, identity-validated and locked device handle.

    This consumer never discovers, opens, resets or launches a device. The
    enclosing board coordinator retains its existing session and port lock.
    """
    deadline = time.monotonic() + timeout
    def exchange(request, remaining):
        remaining = min(remaining, deadline - time.monotonic())
        if remaining <= 0:
            raise TimeoutError('GPU runtime deadline')
        if device.write(0x02, request, timeout=max(1, int(remaining * 1000))) != len(request):
            raise ValueError('Short GPU input transfer')
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('GPU runtime deadline after write')
        response = bytes(device.read(0x81, 168, timeout=max(1, int(remaining * 1000))))
        if time.monotonic() > deadline:
            raise TimeoutError('GPU runtime deadline after read')
        return response
    return sequence(exchange, epoch, words, input_data, timeout)
