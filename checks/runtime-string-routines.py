#!/usr/bin/env python3
"""Differential and access check of runtime memcpy, memmove and memset.

runtime/memory.c and runtime/memmove.c are compiled for AArch64 with the
project's freestanding flags, both with and without -mgeneral-regs-only, and
executed under instruction emulation. Every source and destination alignment
modulo 16 and a range of lengths are compared with a byte-wise reference, and
memmove is also run over overlapping ranges in both directions. Each data
access must be naturally aligned (8-byte alignment for an X-register pair) and
must stay inside the caller's source or destination range. The linked routines
must also pass the SIMD/DC ZVA scan of checks/elf-memory-hazards.py.
"""
import argparse
import importlib.util
import json
import random
import subprocess
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_MEM_WRITE
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_LR,
                                 UC_ARM64_REG_SP)

ROOT = Path(__file__).resolve().parents[1]
CODE, RETURN, DATA, STACK = 0x100000, 0x0ff000, 0x200000, 0x300000
DATA_BYTES = 0x10000
SRC, DST = DATA + 0x1000, DATA + 0x8000
LENGTHS = list(range(41)) + [47, 63, 64, 65, 100, 127, 128, 129, 255, 256, 300]
FLAGS = ['-march=armv8-a', '-mstrict-align', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
         '-ffunction-sections', '-fdata-sections', '-O2', '-Wall', '-Wextra', '-Werror']


def build(clang, lld, output, general):
    objects = []
    for source in ('memory.c', 'memmove.c'):
        target = output / f'{Path(source).stem}{"-general" if general else ""}.o'
        subprocess.check_call([clang, '--target=aarch64-none-elf', f'-I{ROOT}/runtime/include', *FLAGS,
                               *(['-mgeneral-regs-only'] if general else []), '-c',
                               str(ROOT / 'runtime' / source), '-o', str(target)])
        objects.append(str(target))
    image = output / f'routines{"-general" if general else ""}.elf'
    subprocess.check_call([lld, '-static', '--no-gc-sections', f'--image-base={CODE:#x}', f'-Ttext={CODE:#x}', '-e', 'memcpy',
                           *objects, '-o', str(image)])
    return image


class Routines:
    def __init__(self, image):
        with image.open('rb') as handle:
            elf = ELFFile(handle)
            self.symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
            segments = [(s['p_vaddr'], s.data()) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        self.u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.u.mem_map(RETURN, 0x1000)
        self.u.mem_map(CODE, 0x10000)
        self.u.mem_map(DATA, DATA_BYTES)
        self.u.mem_map(STACK - 0x1000, 0x2000)
        for address, data in segments:
            self.u.mem_write(address, data)
        self.accesses = []
        self.u.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, self.access, begin=DATA, end=DATA + DATA_BYTES - 1)

    def access(self, unused, kind, address, size, value, data):
        self.accesses.append((kind == UC_MEM_WRITE, address, size))

    def call(self, name, *arguments):
        self.accesses = []
        for register, value in zip((UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2), arguments):
            self.u.reg_write(register, value)
        self.u.reg_write(UC_ARM64_REG_LR, RETURN)
        self.u.reg_write(UC_ARM64_REG_SP, STACK)
        self.u.emu_start(self.symbols[name], RETURN, count=200000)
        return self.u.reg_read(UC_ARM64_REG_X0)


def verify(routines, name, arguments, before, expected, reads, writes):
    result = routines.call(name, *arguments)
    after = bytes(routines.u.mem_read(DATA, DATA_BYTES))
    case = f'{name}{tuple(hex(a) for a in arguments)}'
    assert result == arguments[0], (case, 'return value', hex(result))
    assert after == expected, (case, 'contents', next(i for i in range(DATA_BYTES) if after[i] != expected[i]))
    for write, address, size in routines.accesses:
        assert size in (1, 2, 4, 8, 16) and address % min(size, 8) == 0, (case, 'misaligned', hex(address), size)
        low, high = writes if write else reads
        assert low <= address and address + size <= high, (case, 'outside range', write, hex(address), size)
    return len(routines.accesses)


def run(routines, rng):
    counts = {'memcpy': 0, 'memset': 0, 'memmove': 0}
    accesses = {'memcpy': 0, 'memset': 0, 'memmove': 0}
    base = bytes(rng.getrandbits(8) for _ in range(DATA_BYTES))
    for src_offset in range(16):
        for dst_offset in range(16):
            for length in LENGTHS:
                routines.u.mem_write(DATA, base)
                src, dst = SRC + src_offset, DST + dst_offset
                expected = bytearray(base)
                expected[dst - DATA:dst - DATA + length] = base[src - DATA:src - DATA + length]
                accesses['memcpy'] += verify(routines, 'memcpy', (dst, src, length), base, bytes(expected),
                                             (src, src + length), (dst, dst + length))
                counts['memcpy'] += 1
    for dst_offset in range(16):
        for length in LENGTHS:
            for value in (0, 0xa5, 0x1ff, -1):
                routines.u.mem_write(DATA, base)
                dst = DST + dst_offset
                expected = bytearray(base)
                expected[dst - DATA:dst - DATA + length] = bytes([value & 0xff]) * length
                accesses['memset'] += verify(routines, 'memset', (dst, value & (2**64 - 1), length), base,
                                             bytes(expected), (0, 0), (dst, dst + length))
                counts['memset'] += 1
    for src_offset in range(16):
        for delta in list(range(-24, 25)) + [-300, 300]:
            for length in LENGTHS:
                routines.u.mem_write(DATA, base)
                src = SRC + 0x400 + src_offset
                dst = src + delta
                expected = bytearray(base)
                expected[dst - DATA:dst - DATA + length] = base[src - DATA:src - DATA + length]
                accesses['memmove'] += verify(routines, 'memmove', (dst, src, length), base, bytes(expected),
                                              (src, src + length), (dst, dst + length))
                counts['memmove'] += 1
    return counts, accesses


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--clang', default='/usr/lib/llvm-21/bin/clang')
    parser.add_argument('--lld', default=str(Path.home() / '.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'))
    parser.add_argument('--objdump', default='/usr/lib/llvm-21/bin/llvm-objdump')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    hazards = module('elf_memory_hazards', ROOT / 'checks/elf-memory-hazards.py')
    results = []
    for general in (False, True):
        image = build(args.clang, args.lld, args.output, general)
        report = hazards.check(image, args.objdump, r'^$')
        assert not report['failures'], report['failures']
        assert all(report['string_routines'][name] == 'clean' for name in hazards.STRING_ROUTINES), report
        counts, accesses = run(Routines(image), random.Random(0x5eed))
        results.append({'general_regs_only': general, 'image': str(image), 'cases': counts,
                        'checked_accesses': accesses})
        print(f'PASS runtime string routines general-regs-only={general}: {counts}')
    (args.output / 'result.json').write_text(json.dumps(results, indent=1) + '\n')


if __name__ == '__main__':
    main()
