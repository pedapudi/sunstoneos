#!/usr/bin/env python3
"""Compare native address encoding with isolated instructions in the pinned SDK.

No dynamic loader, system calls, HAL imports, or device access are provided.
Only the pool/SRAM patch routines and their bit-position helper may execute.
The test supplies synthetic allocations; it does not establish access sizes
or destinations for execution of the compiled network.
"""
import argparse
import ctypes
import hashlib
import io
import json
import random
import struct
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2
from unicorn.arm64_const import UC_ARM64_REG_X3, UC_ARM64_REG_X30, UC_ARM64_REG_SP
from unicorn.arm64_const import UC_ARM64_REG_PC

SDK_SHA = "82f049b0ed0065dd4d443e37eeb1edfcbef24457c9c24e36170d64d5b748ca66"
MODEL_SHA = "fc0d50bdc863e4dfc74eef6dad9d34655c560e759a2f426979a01c505fa9b95f"
BASE, RAM, STOP = 0x100000, 0x200000, 0x300000


class Patch(ctypes.Structure):
    _fields_ = [("source_bytes", ctypes.c_uint64), ("referenced_bytes", ctypes.c_uint64),
                ("source_device", ctypes.c_uint32), ("source_offset", ctypes.c_uint32),
                ("destination_offset", ctypes.c_uint32), ("encoding", ctypes.c_int)]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("sdk", type=Path)
    ap.add_argument("model", type=Path)
    ap.add_argument("native", type=Path, help="host shared library built from nbg_relocation.c")
    args = ap.parse_args()
    sdk, model = args.sdk.read_bytes(), args.model.read_bytes()
    if hashlib.sha256(sdk).hexdigest() != SDK_SHA or hashlib.sha256(model).hexdigest() != MODEL_SHA:
        raise SystemExit("reference identity mismatch")
    native = ctypes.CDLL(str(args.native.resolve()))
    native.nbg_relocate.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(Patch), ctypes.c_size_t]
    native.nbg_relocate.restype = ctypes.c_int
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    elf = ELFFile(io.BytesIO(sdk))
    segments = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
    size = max(s['p_vaddr'] + s['p_memsz'] for s in segments)
    uc.mem_map(BASE, (size + 4095) & ~4095)
    for s in segments:
        uc.mem_write(BASE + s['p_vaddr'], s.data())
    uc.mem_map(RAM, 0x10000)
    uc.mem_map(STOP, 0x1000)

    def check_pc(machine, address, size, user):
        pc = address - BASE
        if not any(lo <= pc < hi for lo, hi in
                   [(0xb040, 0xb0b8), (0xb830, 0xb914), (0xb920, 0xba84)]):
            raise RuntimeError(f"unexpected reference instruction at {pc:#x}")

    uc.hook_add(UC_HOOK_CODE, check_pc)
    rng = random.Random(733)
    comparisons = 0

    def compare(source, address, source_bytes, offset, dest, encoding):
        nonlocal comparisons
        # SDK runtime patch: 24-byte linked-list metadata followed by wire fields.
        record = struct.pack('<10I', 1, dest, source, 0xffffffff,
                             offset, encoding, 0, 0, 0, 0)
        uc.mem_write(RAM, bytes(0x10000))
        uc.mem_write(RAM + 0x20, struct.pack('<I', 0x1001e))
        uc.mem_write(RAM + 0x3a8, struct.pack('<Q', address))
        uc.mem_write(RAM + 8, struct.pack('<Q', RAM + 0x1000))
        uc.mem_write(RAM + 0x1018, struct.pack('<I', address))
        uc.mem_write(RAM + 0x2000 + 24, record)
        before = bytearray(rng.randbytes(256))
        old = struct.unpack_from('<I', before, dest)[0]
        struct.pack_into('<I', before, dest, (old & 0x03ffffff) | ((comparisons % 64) << 26))
        before = bytes(before)
        uc.mem_write(RAM + 0x3000, before)
        for reg, value in [(UC_ARM64_REG_X0, RAM), (UC_ARM64_REG_X1, RAM + 0x3000),
                           (UC_ARM64_REG_X2, RAM + 0x2000), (UC_ARM64_REG_X3, source),
                           (UC_ARM64_REG_X30, STOP), (UC_ARM64_REG_SP, RAM + 0xfff0)]:
            uc.reg_write(reg, value)
        uc.emu_start(BASE + (0xb830 if source == 3 else 0xb920), STOP, count=512)
        if uc.reg_read(UC_ARM64_REG_PC) != STOP or uc.reg_read(UC_ARM64_REG_X0) != 0:
            raise RuntimeError("reference did not return successfully within instruction limit")
        expected = bytes(uc.mem_read(RAM + 0x3000, 256))
        result = ctypes.create_string_buffer(before, len(before))
        patch = Patch(source_bytes, 1, address, offset, dest, encoding)
        if native.nbg_relocate(result, len(before), ctypes.byref(patch), 1) != 1:
            raise RuntimeError("native refused supported reference case")
        if result.raw != expected:
            raise RuntimeError(f"encoding mismatch: {(source, address, offset, dest, encoding)}")
        # Full-buffer comparison includes every untouched byte.
        comparisons += 1

    patch_offset, patch_bytes = struct.unpack_from('<II', model, 376 + 9 * 8)
    counts = {3: 0, 8: 0}
    for record in struct.iter_unpack('<10I', model[patch_offset:patch_offset + patch_bytes]):
        kind, dest, source, index, offset, encoding, name, high_dest, high_mask, dest_base = record
        if source not in counts:
            continue
        if kind != 1 or high_dest or high_mask or dest_base:
            raise RuntimeError("unsupported fixture relocation")
        allocation_bytes = struct.unpack_from('<I', model, 348 if source == 3 else 372)[0]
        compare(source, 0x10000000 if source == 3 else 0x400000,
                allocation_bytes, offset, dest, encoding)
        counts[source] += 1
    fixture_comparisons = comparisons
    # All 64 preserved-bit patterns, low-address bits, address carry, and last
    # device byte are covered without extending this check to unsupported modes.
    for source in (3, 8):
        for encoding in (0, 1):
            for offset in range(64):
                compare(source, 0x10000000, 4096, offset * (64 if encoding else 1), 20, encoding)
            compare(source, 0xfffff000, 4096, 4032 if encoding else 4095, 84, encoding)
            compare(source, 0x0fffffc0, 4096, 64, 24, encoding)
    print(json.dumps({"result": "pass", "sdk_sha256": SDK_SHA,
                      "model_sha256": MODEL_SHA, "fixture_pool_records": counts[3],
                      "fixture_sram_records": counts[8], "fixture_comparisons": fixture_comparisons,
                      "boundary_comparisons": comparisons - fixture_comparisons,
                      "native_sha256": hashlib.sha256(args.native.read_bytes()).hexdigest(),
                      "board_access": False}, indent=2))


if __name__ == '__main__':
    main()
