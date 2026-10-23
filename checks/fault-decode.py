#!/usr/bin/env python3
"""Decode retained fault records (docs/fault-capture.md).

Reads the 1,160-byte previous-boot report that USB0 operation 5 of the
integrated OS image returns, or one 128-byte slot, and prints each record. The
module also formats the single-core log line exactly as a733_fault_line() in
include/a733_fault.h does; checks/fault-capture.py compares the two.

  fault-decode.py REPORT.bin       decode a saved report
  fault-decode.py --slot SLOT.bin  decode one slot
"""
import argparse
import json
import struct
import sys
from pathlib import Path

SLOT_MAGIC = 0x31544c4633333741   # "A733FLT1"
SLOT_OPEN = 0x30544c4633333741    # "A733FLT0"
REPORT_MAGIC = 0x31544c46         # "FLT1"
REPORT_BYTES = 1160
SLOT_BYTES = 128
KINDS = {1: 'vector fault', 2: 'deliberate exit'}
FIELDS = ('magic', 'crc', 'info', 'status', 'detail', 'counter', 'epoch',
          'esr', 'elr', 'far', 'spsr', 'tcb', 'task0', 'task1', 'reserved0', 'reserved1')
# Exception classes (ESR_ELx.EC, bits 31:26) that the A733 images can raise;
# names follow TF-A 0892ad4d include/arch/aarch64/arch.h.
EXCEPTION_CLASSES = {0x00: 'unknown', 0x01: 'WFI/WFE', 0x07: 'FP access', 0x15: 'SVC64',
                     0x16: 'HVC64', 0x17: 'SMC64', 0x18: 'system register', 0x20: 'instruction abort, lower EL',
                     0x21: 'instruction abort, same EL', 0x22: 'PC alignment', 0x24: 'data abort, lower EL',
                     0x25: 'data abort, same EL', 0x26: 'SP alignment', 0x2c: 'FP exception', 0x2f: 'SError',
                     0x3c: 'BRK64'}
MODES = {0x0: 'EL0t', 0x4: 'EL1t', 0x5: 'EL1h', 0x8: 'EL2t', 0x9: 'EL2h', 0xc: 'EL3t', 0xd: 'EL3h'}

_TABLE = []
for _n in range(256):
    _c = _n
    for _ in range(8):
        _c = (_c >> 1) ^ (0x82f63b78 if _c & 1 else 0)
    _TABLE.append(_c)


def crc32c(data, crc=0xffffffff):
    """Raw CRC32C register update, as the crc32cx instruction applies it."""
    for byte in data:
        crc = _TABLE[(crc ^ byte) & 0xff] ^ (crc >> 8)
    return crc


def slot_crc(words):
    """CRC word of a slot: CRC32C of words 2..15 with the usual final XOR."""
    return crc32c(struct.pack('<14Q', *words[2:16])) ^ 0xffffffff


def task_name(words):
    raw = struct.pack('<2Q', words[12], words[13])
    name = raw.split(b'\0', 1)[0]
    return ''.join(chr(c) if 0x20 <= c < 0x7f else '?' for c in name)


def decode_slot(data):
    words = struct.unpack('<16Q', data)
    record = dict(zip(FIELDS, words))
    if words[0] == SLOT_MAGIC:
        state = 'valid' if words[1] == slot_crc(words) else 'bad-crc'
    elif words[0] == SLOT_OPEN:
        state = 'incomplete'
    elif not any(words):
        state = 'empty'
    else:
        state = 'invalid'
    info = words[2]
    kind = info & 0xff
    record.update(state=state, kind=KINDS.get(kind, f'unknown {kind}'), core=(info >> 16) & 0xff,
                  task=task_name(words))
    if kind == 1:
        record.update(vector_slot=(info >> 8) & 0xff, exception_class=(words[7] >> 26) & 0x3f,
                      exception=EXCEPTION_CLASSES.get((words[7] >> 26) & 0x3f, 'other'),
                      mode=MODES.get(words[10] & 0xf, 'other'))
    return record


def decode_report(data):
    if len(data) != REPORT_BYTES:
        raise ValueError(f'report holds {len(data)} bytes, expected {REPORT_BYTES}')
    header = struct.unpack_from('<Q', data)[0]
    if header & 0xffffffff != REPORT_MAGIC:
        raise ValueError(f'report magic {header & 0xffffffff:#x}')
    valid, incomplete = (header >> 32) & 0xff, (header >> 40) & 0xff
    result = struct.unpack_from('<16Q', data, 8)
    report = dict(valid_mask=valid, incomplete_mask=incomplete, result_copied=bool(header >> 48 & 1),
                  previous_result=dict(zip(('magic', 'epoch', 'generation', 'stage', 'status', 'detail',
                                            'start_counter', 'completed_mask', 'end_counter'), result)),
                  slots=[])
    for core in range(8):
        record = decode_slot(data[136 + core * SLOT_BYTES:136 + (core + 1) * SLOT_BYTES])
        if valid >> core & 1 and record['state'] != 'valid':
            raise ValueError(f'core {core} is marked valid but decodes as {record["state"]}')
        if valid >> core & 1:
            report['slots'].append(record)
        elif incomplete >> core & 1:
            report['slots'].append(dict(core=core, state='incomplete'))
    report['slots'].sort(key=lambda r: r.get('counter', 0))
    return report


def fault_line(words):
    """The single-core log line of a733_fault_line()."""
    line = f'FAIL exception core={(words[2] >> 16) & 7}'
    for label, value in (('ESR', words[7]), ('ELR', words[8]), ('FAR', words[9]), ('SPSR', words[10])):
        line += f' {label}=0x{value:016x}'
    return line + f' task={task_name(words)}\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('path', type=Path)
    parser.add_argument('--slot', action='store_true', help='the file holds one 128-byte slot')
    args = parser.parse_args()
    data = args.path.read_bytes()
    out = decode_slot(data) if args.slot else decode_report(data)
    json.dump(out, sys.stdout, indent=2)
    print()


if __name__ == '__main__':
    main()
