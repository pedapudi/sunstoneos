#!/usr/bin/env python3
"""Check ring-field packing against the pinned SDK HAL's isolated transpose encoder.

Synthetic parameter records establish bit locations and byte-to-unit conversion;
they are not complete executable workloads or evidence of device access limits.
"""
import argparse
import ctypes as c
import hashlib
import io
import json
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2
from unicorn.arm64_const import UC_ARM64_REG_X30, UC_ARM64_REG_SP, UC_ARM64_REG_PC

HAL_SHA = '3ed5357b26bd6c4fb68fbdc0b21d637227dda711d1fe67b9e28accdc48bb11f2'
BASE, RAM, STOP = 0x100000, 0x200000, 0x300000


class Patch(c.Structure):
    _fields_ = [('source_bytes', c.c_uint64), ('referenced_bytes', c.c_uint64),
                ('source_device', c.c_uint32), ('source_offset', c.c_uint32),
                ('destination_offset', c.c_uint32), ('encoding', c.c_int)]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('hal', type=Path)
    ap.add_argument('native', type=Path)
    ap.add_argument('output', type=Path)
    args = ap.parse_args()
    blob = args.hal.read_bytes()
    assert hashlib.sha256(blob).hexdigest() == HAL_SHA
    args.output.mkdir(parents=True)
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    elf = ELFFile(io.BytesIO(blob))
    segments = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
    size = max(s['p_vaddr'] + s['p_memsz'] for s in segments)
    uc.mem_map(BASE, (size + 4095) & -4096)
    for s in segments:
        uc.mem_write(BASE + s['p_vaddr'], s.data())
    uc.mem_map(RAM, 0x10000); uc.mem_map(STOP, 0x1000)

    def instruction(machine, address, size, user):
        if not BASE + 0x4620 <= address < BASE + 0x4980:
            raise RuntimeError(f'unexpected SDK instruction {address - BASE:#x}')

    uc.hook_add(UC_HOOK_CODE, instruction)
    native = c.CDLL(str(args.native.resolve()))
    native.nbg_relocate.argtypes = [c.c_void_p, c.c_size_t, c.POINTER(Patch), c.c_size_t]
    cases = []
    for base in (0x400000, 0x30000000, 0xe0000000, 0xfffff000):
        for length in (64, 128, 4032, 0x7e800):
            if base + length > 0xffffffff:
                continue
            # Tensor pointers need not be at the beginning of the circular range.
            start = base + length // 2
            end = base + length
            params = bytearray(184)
            for offset, value in ((0x20, start), (0x48, start),
                                  (0x88, length), (0x8c, end),
                                  (0x90, length), (0x94, end)):
                struct.pack_into('<I', params, offset, value)
            uc.mem_write(RAM, bytes(params))
            uc.mem_write(RAM + 0x1000, b'\xa5' * 320)
            out = RAM + 0x1040
            for reg, value in ((UC_ARM64_REG_X0, RAM), (UC_ARM64_REG_X1, out),
                               (UC_ARM64_REG_X2, RAM + 0x2000),
                               (UC_ARM64_REG_X30, STOP), (UC_ARM64_REG_SP, RAM + 0xfff0)):
                uc.reg_write(reg, value)
            uc.emu_start(BASE + 0x4620, STOP, timeout=1_000_000, count=512)
            assert uc.reg_read(UC_ARM64_REG_PC) == STOP and uc.reg_read(UC_ARM64_REG_X0) == 0
            assert struct.unpack('<I', uc.mem_read(RAM + 0x2000, 4)) == (192,)
            assert bytes(uc.mem_read(RAM + 0x1000, 64)) == b'\xa5' * 64
            assert bytes(uc.mem_read(out + 192, 64)) == b'\xa5' * 64
            expected = bytes(uc.mem_read(out, 192))
            words = struct.unpack('<48I', expected)
            assert words[6] == words[7] == start
            assert words[18] == words[20] == length >> 6
            assert words[19] == words[21] == end >> 6
            assert words[31] & 2  # The HAL transpose encoder sets this selector.
            before = bytearray(expected)
            for offset in (24, 28, 76, 84):
                struct.pack_into('<I', before, offset, 0)
            target = c.create_string_buffer(bytes(before), len(before))
            patches = (Patch * 4)(Patch(length, 1, base, length // 2, 24, 0),
                                  Patch(length, 1, base, length // 2, 28, 0),
                                  Patch(length, length, base, 0, 76, 3),
                                  Patch(length, length, base, 0, 84, 3))
            assert native.nbg_relocate(target, len(before), patches, len(patches)) == 1
            assert target.raw == expected
            (args.output/f'descriptor-{len(cases)}.bin').write_bytes(expected)
            cases.append(dict(base=hex(base), bytes=length, tensor=hex(start), exclusive_end=hex(end)))
    report = dict(result='pass', cases=cases, comparisons=len(cases),
                  hal_sha256=HAL_SHA, native_sha256=hashlib.sha256(args.native.read_bytes()).hexdigest(),
                  allowed_code=['0x4620', '0x4980'], parameter_bytes=184, descriptor_bytes=192,
                  execution_semantics_verified=False, physical_access=False)
    (args.output/'comparison.json').write_text(json.dumps(report, indent=2)+'\n')
    print(f'PASS ring packing: {len(cases)} isolated HAL comparisons; no device execution')


if __name__ == '__main__':
    main()
