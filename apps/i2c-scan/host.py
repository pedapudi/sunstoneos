#!/usr/bin/env python3
"""Host side of i2c-scan: decode the result record and list what departs from an empty bus.

The record is the 32-byte header and one 152-byte record per (bus, rate) that apps/i2c-scan/main.c writes.
decode() returns the fields; failures() names every departure from the prediction that no device is attached: each
of the 128 probes answers with an address NACK, the lines read high after open and close, and nothing else happens.
The count of starts that found the previous STOP still in flight (busy_starts, cumulative per bus) is reported, not judged: the start is hardware-ordered after the STOP.
"""
import struct

HEADER = struct.Struct('<QIIIIQ')
RECORD = struct.Struct('<26I6Q')
MAGIC = 0x3143325f5343414e
BUSES = ('twi0', 'twi1', 'twi2', 'twi3', 's_twi2')
STATUS = {1: 'ok', 2: 'nack_address', 3: 'nack_data', 4: 'arbitration', 5: 'bus_error', 6: 'timeout', 7: 'unexpected'}
FIELDS = ('bus', 'rate_request', 'rate', 'input_hz', 'clock_word', 'open_status', 'close_status', 'scanned',
          'nack', 'ack', 'other', 'status_mask', 'first_other', 'first_other_status', 'hardware_status_mask', 'reserved0',
          'lines_open', 'lines_close', 'interrupts', 'spurious', 'strays', 'idle_starts', 'busy_starts', 'fault',
          'fault_detail', 'reserved1', 'open_ticks', 'scan_ticks', 'close_ticks', 'min_ticks', 'max_ticks', 'sum_ticks')
TICKS_PER_US = 24


def decode(payload):
    if len(payload) != HEADER.size + 10 * RECORD.size:
        raise ValueError(f'i2c-scan result is {len(payload)} bytes, not {HEADER.size + 10 * RECORD.size}.')
    magic, version, count, expected, failed, reserved = HEADER.unpack_from(payload)
    if magic != MAGIC or version != 1 or reserved:
        raise ValueError('i2c-scan result has an unknown header.')
    records = []
    for i in range(min(count, 10)):
        values = dict(zip(FIELDS, RECORD.unpack_from(payload, HEADER.size + i * RECORD.size)))
        values['bus_name'] = BUSES[values['bus']] if values['bus'] < len(BUSES) else None
        values['statuses'] = [STATUS[b] for b in range(1, 8) if values['status_mask'] >> b & 1]
        if values['scanned']:
            values['scan_us'] = values['scan_ticks'] / TICKS_PER_US
            values['mean_probe_us'] = values['sum_ticks'] / values['scanned'] / TICKS_PER_US
            values['min_probe_us'] = values['min_ticks'] / TICKS_PER_US
            values['max_probe_us'] = values['max_ticks'] / TICKS_PER_US
        values['open_us'] = values['open_ticks'] / TICKS_PER_US
        values['close_us'] = values['close_ticks'] / TICKS_PER_US
        records.append(values)
    return {'records': records, 'count': count, 'expected': expected, 'failed': failed}


def failures(decoded):
    """Departures from the empty-bus prediction, as strings; empty when the run matches."""
    out = []
    if decoded['count'] != decoded['expected'] or decoded['failed']:
        out.append(f"scanned {decoded['count']} of {decoded['expected']} bus and rate pairs, failed flag {decoded['failed']}")
    for r in decoded['records']:
        tag = f"{r['bus_name']}@{r['rate_request']}"
        if r['open_status'] != 1 or r['close_status'] != 1:
            out.append(f"{tag}: open {r['open_status']} close {r['close_status']} fault {r['fault']}/{r['fault_detail']:#x}")
            continue
        if r['scanned'] != 128 or r['nack'] != 128 or r['ack'] or r['other']:
            out.append(f"{tag}: scanned {r['scanned']} nack {r['nack']} ack {r['ack']} other {r['other']} "
                       f"(first other {r['first_other']:#x} status {r['first_other_status']:#x}, statuses {r['statuses']})")
        if r['hardware_status_mask'] != 1 << (0x20 >> 1):
            out.append(f"{tag}: hardware status mask {r['hardware_status_mask']:#x}, expected only 0x20")
        if r['lines_open'] != 1 or r['lines_close'] != 1:
            out.append(f"{tag}: lines after open {r['lines_open']:#x}, after close {r['lines_close']:#x}")
        if r['spurious'] or r['strays'] or r['interrupts'] != 256:
            out.append(f"{tag}: interrupts {r['interrupts']} spurious {r['spurious']} strays {r['strays']}")
        if r['rate'] > r['rate_request'] or r['rate'] * 10 < r['rate_request'] * 9:
            out.append(f"{tag}: programmed rate {r['rate']} Hz")
    return out
