#!/usr/bin/env python3
"""Inspect isolated stride instructions in the pinned compiler simulator.

This checks decoder arithmetic with saved descriptors and synthetic object
state. It does not run inference or establish hardware access bounds.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE
from unicorn.x86_const import (
    UC_X86_REG_RAX, UC_X86_REG_RBX, UC_X86_REG_RCX, UC_X86_REG_RDX,
    UC_X86_REG_RDI, UC_X86_REG_RSI, UC_X86_REG_R8, UC_X86_REG_RSP,
    UC_X86_REG_RIP,
)

SIMULATOR_SHA = '2d102725286968ffa4b011dfb7f92ca9f4434a71fd19920beaf4045923bbac6a'
OLD_SHA = 'fc0d50bdc863e4dfc74eef6dad9d34655c560e759a2f426979a01c505fa9b95f'
CONV_SHA = 'db42d815100417f4b8913896462ed031beafae35fb4ee1c28569ac950a9e8fca'
OBJECT, STACK, STOP = 0x1000000, 0x1100000, 0x1200000
RAW = OBJECT + 0x1bb40


def pinned(path, expected):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError(f'unexpected input hash: {path}')
    return data


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('simulator', type=Path)
    ap.add_argument('older_model', type=Path)
    ap.add_argument('convolution', type=Path)
    ap.add_argument('output', type=Path)
    args = ap.parse_args()
    blob = pinned(args.simulator, SIMULATOR_SHA)
    older = pinned(args.older_model, OLD_SHA)
    conv = pinned(args.convolution, CONV_SHA)
    args.output.mkdir(parents=True)
    uc = Uc(UC_ARCH_X86, UC_MODE_64)
    elf = ELFFile(io.BytesIO(blob))
    segments = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
    size = max(s['p_vaddr'] + s['p_memsz'] for s in segments)
    uc.mem_map(0, (size + 4095) & -4096)
    for segment in segments:
        uc.mem_write(segment['p_vaddr'], segment.data())
    uc.mem_map(OBJECT, 0x30000)
    uc.mem_map(STACK, 0x10000)
    uc.mem_map(STOP, 0x1000)
    permitted = [0, 0]

    def instruction(machine, address, length, user):
        if not permitted[0] <= address < permitted[1]:
            raise RuntimeError(f'unexpected simulator instruction: {address:#x}')

    uc.hook_add(UC_HOOK_CODE, instruction)

    def put32(address, value):
        uc.mem_write(address, struct.pack('<I', value))

    def get32(address):
        return struct.unpack('<I', uc.mem_read(address, 4))[0]

    def run(start, end, stop=STOP, registers=(), limit=1000):
        permitted[:] = start, end
        uc.mem_write(STACK + 0xfff0, struct.pack('<Q', STOP))
        uc.reg_write(UC_X86_REG_RSP, STACK + 0xfff0)
        for reg, value in registers:
            uc.reg_write(reg, value)
        uc.emu_start(start, stop, timeout=1_000_000, count=limit)
        assert uc.reg_read(UC_X86_REG_RIP) == stop, 'instruction bound exhausted'
        return uc.reg_read(UC_X86_REG_RAX)

    # The target identity is present in both the compiler and board driver.
    identity = [0x9000, 0x9202, 0x05090009, 0x08000000, 0x1000003b]
    regs = [UC_X86_REG_RDI, UC_X86_REG_RSI, UC_X86_REG_RDX,
            UC_X86_REG_RCX, UC_X86_REG_R8]
    entry = run(0xc99c6, 0xc9a7f, registers=zip(regs, identity), limit=20000)
    assert entry != 0
    assert list(struct.unpack('<5I', uc.mem_read(entry, 20))) == identity
    # Resolve only the featureDB GOT entry used by the isolated predicates.
    uc.mem_write(0x4109b0, struct.pack('<Q', 0x558270))
    uc.mem_write(0x558270, struct.pack('<Q', entry))
    assert run(0xc9e24, 0xc9e51) & 0xff == 1
    assert run(0xc9e52, 0xc9e8e) & 0xff == 0
    input_address_feature = get32(entry + 0x120)

    old_offset, old_bytes = struct.unpack_from('<II', older, 376 + 6 * 8)
    assert old_bytes == 125 * 192
    cases = [
        ('older-first-descriptor', older[old_offset:old_offset + 192]),
        ('convolution', conv[1244:1436]),
    ]
    # Distinct high halves catch accidental multiplication or field swapping.
    for name, in_stride, out_stride in [
            ('high-channel-strides', 0x12345678, 0x9abcdef0),
            ('maximum-channel-strides', 0xffffffff, 0xffffffff)]:
        raw = bytearray(192)
        struct.pack_into('<H', raw, 66, in_stride & 0xffff)
        struct.pack_into('<H', raw, 34, out_stride & 0xffff)
        struct.pack_into('<I', raw, 104, (in_stride >> 16) | (out_stride & 0xffff0000))
        cases.append((name, bytes(raw)))
    results = []
    for name, raw in cases:
        uc.mem_write(OBJECT, bytes(0x30000))
        uc.mem_write(RAW, raw)
        words = struct.unpack('<48I', raw)
        expected_in = (words[16] >> 16) | ((words[26] & 0xffff) << 16)
        expected_out = (words[8] >> 16) | (words[26] & 0xffff0000)
        run(0x1591ee, 0x159220, stop=0x159220,
            registers=[(UC_X86_REG_RBX, OBJECT)])
        assert get32(OBJECT + 0x17a8) == expected_in
        assert get32(OBJECT + 0x17ac) == expected_out
        result = dict(name=name, input_channel_stride=expected_in,
                      output_channel_stride=expected_out)
        if name in ('older-first-descriptor', 'convolution'):
            # Only low row-stride decoding is exercised here. Extension
            # fields, signed image offsets, pooling and interleave are omitted.
            assert words[45] == 0
            run(0x1587c7, 0x1587e9, stop=0x1587e9,
                registers=[(UC_X86_REG_RBX, OBJECT)])
            in_row, out_row = words[16] & 0xffff, words[17] & 0xffff
            assert get32(OBJECT + 0x1bd24) == in_row
            assert get32(OBJECT + 0x1bd20) == out_row
            width, height = (words[1] >> 6) & 0x1fff, words[1] >> 19
            channels = (words[0] >> 6) & 0x3fff
            # Select the stride branch synthetically. The target database
            # field is zero, so this is not evidence of its active input path.
            put32(OBJECT + 0x146c, 1)
            put32(OBJECT + 0x1bd08, width)
            put32(OBJECT + 0x1bd0c, height)
            put32(OBJECT + 0x1774, 0x10000000)
            # Stop before the external-memory helper; capture its address.
            run(0x15985c, 0x159918, stop=0x6c790, registers=[
                (UC_X86_REG_RDI, OBJECT), (UC_X86_REG_RSI, width - 1),
                (UC_X86_REG_RDX, height - 1), (UC_X86_REG_RCX, channels - 1)])
            last_offset = (channels - 1) * expected_in + (height - 1) * in_row + width - 1
            address = uc.reg_read(UC_X86_REG_RSI)
            assert address == 0x10000000 + last_offset
            put32(OBJECT + 0x1778, 0x20000000)
            uc.mem_write(OBJECT + 0x26408, b'\x01')
            # x=1,y=1 with one channel stride isolates output arithmetic
            # without asserting pooled dimensions or output-ring behavior.
            address_out = run(0x16bb66, 0x16bb91, registers=[
                (UC_X86_REG_RDI, OBJECT), (UC_X86_REG_RSI, 1),
                (UC_X86_REG_RDX, 1), (UC_X86_REG_RCX, expected_out)])
            assert address_out == 0x20000000 + expected_out + out_row + 1
            result.update(input_row_stride=in_row, output_row_stride=out_row,
                          input_last_offset_with_synthetic_state=last_offset,
                          output_sample_offset_with_synthetic_state=address_out - 0x20000000)
        results.append(result)
    report = dict(result='pass', simulator_sha256=SIMULATOR_SHA,
                  older_model_sha256=OLD_SHA, convolution_sha256=CONV_SHA,
                  identity=[hex(x) for x in identity], feature_record=hex(entry),
                  decoder='cmoNeuralNetV92', input_address_feature=input_address_feature,
                  cases=results,
                  synthetic_object_state=True, full_descriptor_decoding=False,
                  inference_executed=False, physical_access=False,
                  hardware_access_bounds_verified=False)
    (args.output / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS target decoder selection and four isolated channel-stride cases; '
          'two address-arithmetic samples; no inference or device access')


if __name__ == '__main__':
    main()
