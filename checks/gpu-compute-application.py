#!/usr/bin/env python3
"""Check copied numerical USB frames without accessing a device."""
import importlib.util
from pathlib import Path
import struct

path = Path(__file__).resolve().parents[1] / 'apps/gpu-render/observe.py'
spec = importlib.util.spec_from_file_location('gpu_observe', path)
observe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(observe)
epoch = 0x123456789abcdef0
expected = tuple(((17 + (i + 1) * 3) ^ (0x9e3779b9 + (i << 16))) & 0xffffffff
                 for i in range(32))


def frame(submission, state=1):
    header = struct.pack('<7IQI', 0x43555047, 1, state, submission,
                         32 if state == 1 else 0, 2,
                         submission + 1 if state == 1 else 0, epoch,
                         64 if state == 1 else 0)
    return header + (struct.pack('<32I', *expected) if state == 1 else b'')


def refuses(data, submission=1, seen_epoch=epoch):
    try:
        observe.decode_compute(data, submission, seen_epoch)
    except ValueError:
        return
    raise AssertionError('malformed or mismatched frame accepted')


for submission in range(2):
    for state in range(3):
        assert observe.decode_compute(frame(submission, state), submission, epoch) == state
refuses(frame(0))
refuses(frame(1), seen_epoch=epoch + 1)
for offset in (0, 4, 8, 12, 16, 20, 24, 36, 40, 167):
    damaged = bytearray(frame(1))
    damaged[offset] ^= 1
    refuses(bytes(damaged))
refuses(frame(1)[:-1])
refuses(frame(1, 0) + b'\0')
render = struct.pack('<7IQI', 0x52555047, 1, 1, 1, 64, 64, 64, epoch, 4096)
refuses(render + bytes.fromhex('2080c0ff') * 64)
try:
    observe.decode(frame(1)[:40], 1, epoch)
except ValueError:
    pass
else:
    raise AssertionError('render decoder accepted compute frame')
print('PASS GPU compute USB frames: two copied submissions, pending/failure, identity, sequence, arithmetic and render separation')
