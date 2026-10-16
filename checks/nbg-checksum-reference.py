#!/usr/bin/env python3
"""Compare whole-file NBG checksum coverage with zlib and the isolated SDK CRC.

The header-field coverage is inferred from two independently published models.
The isolated SDK routine establishes its CRC primitive, not loader enforcement.
No complete SDK loader session or device operation is run.
"""
import argparse
import ctypes as c
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import zlib
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X30
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_PC


def load_reference():
    path = Path(__file__).with_name('nbg-loader-reference.py')
    spec = importlib.util.spec_from_file_location('reference', path)
    result = importlib.util.module_from_spec(spec); spec.loader.exec_module(result)
    return result


def zero_crc_file():
    """Solve 32 linear CRC bit equations for a four-byte suffix; independent of C."""
    data = bytearray(592)
    data[:8] = struct.pack('<II', 0x4e4d5056, 0x1001e)
    baseline = zlib.crc32(data)
    columns = []
    for bit in range(32):
        sample = bytearray(data); sample[-4 + bit // 8] ^= 1 << (bit % 8)
        columns.append(zlib.crc32(sample) ^ baseline)
    pivots = {}
    for bit, value in enumerate(columns):
        combination = 1 << bit
        while value:
            pivot = value.bit_length() - 1
            if pivot not in pivots:
                pivots[pivot] = (value, combination); break
            v, mask = pivots[pivot]; value ^= v; combination ^= mask
    value, solution = baseline, 0
    while value:
        v, mask = pivots[value.bit_length() - 1]; value ^= v; solution ^= mask
    data[-4:] = struct.pack('<I', solution)
    assert zlib.crc32(data) == 0
    return bytes(data)


class Checksum(c.Structure):
    _fields_ = [('stored', c.c_uint32), ('computed', c.c_uint32)]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('sdk', type=Path)
    ap.add_argument('model', type=Path)
    ap.add_argument('second_model', type=Path)
    ap.add_argument('native', type=Path)
    ap.add_argument('output', type=Path)
    args = ap.parse_args()
    reference = load_reference()
    sdk = args.sdk.read_bytes()
    assert hashlib.sha256(sdk).hexdigest() == reference.SDK_SHA
    models = [args.model.read_bytes(), args.second_model.read_bytes()]
    assert hashlib.sha256(models[0]).hexdigest() == reference.MODEL_SHA
    assert hashlib.sha256(models[1]).hexdigest() == 'e898680018e90837ec6941c9f0fa5566bde8876ac29a366d9ee53469342cb276'
    assert len(models[1]) == 9152 and struct.unpack_from('<III', models[1], 0) == (0x4e4d5056, 0x1001e, 0xba)
    args.output.mkdir(parents=True)
    zero = zero_crc_file(); (args.output/'zero-crc.nb').write_bytes(zero)
    models.append(zero)
    ref = reference.Reference(sdk, 0x30000000)
    lib = c.CDLL(str(args.native.resolve()))
    lib.nbg_checksum_verify.argtypes = [c.c_void_p, c.c_size_t, c.POINTER(Checksum)]
    rows = []
    for model in models:
        normalized = model[:108] + bytes(4) + model[112:]
        pointer = ref.allocate(len(normalized))['cpu']; ref.uc.mem_write(pointer, normalized)
        ref.uc.reg_write(UC_ARM64_REG_X0, pointer)
        ref.uc.reg_write(UC_ARM64_REG_X1, len(normalized))
        ref.uc.reg_write(UC_ARM64_REG_SP, reference.STACK + 0xfff0)
        ref.uc.reg_write(UC_ARM64_REG_X30, reference.STOP)
        ref.uc.emu_start(reference.BASE + 0x12b70, reference.STOP,
                         timeout=5_000_000, count=20_000_000)
        assert ref.uc.reg_read(UC_ARM64_REG_PC) == reference.STOP, 'SDK CRC execution budget exceeded'
        sdk_sum = ref.uc.reg_read(UC_ARM64_REG_X0) & 0xffffffff
        expected = zlib.crc32(normalized)
        source = c.create_string_buffer(model, len(model)); result = Checksum()
        assert lib.nbg_checksum_verify(source, len(model), c.byref(result)) == 1
        assert result.stored == result.computed == sdk_sum == expected
        assert source.raw == model
        rows.append(dict(bytes=len(model), sha256=hashlib.sha256(model).hexdigest(),
                         stored=hex(result.stored), computed=hex(result.computed), sdk_crc=hex(sdk_sum)))
    report = dict(result='pass', models=rows, sdk_sha256=reference.SDK_SHA,
                  native_sha256=hashlib.sha256(args.native.read_bytes()).hexdigest(),
                  checksum_field_zeroed=[108, 112], coverage='entire supplied file',
                  sdk_routine='0x12b70', sdk_loader_enforcement_established=False,
                  sdk_loader_executed=False, physical_access=False)
    (args.output/'comparison.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
