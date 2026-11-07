#!/usr/bin/env python3
"""The unaligned memory routines (runtime/memory_unaligned.c) against a byte reference, on an AArch64 core model.

The relaxed units of an OS image call ss_memcpy_unaligned, ss_memmove_unaligned and ss_memset_unaligned, which load and store
unaligned 8-byte words and 16-byte-aligned pairs. The routines are built for AArch64 with -mno-strict-align as the relaxed
units are and run in Unicorn. Each case compares the destination with a reference built a byte at a time, with a canary of
64 bytes on each side that must stay intact and the source left unchanged:

- memcpy of every size from 0 to 130 bytes and of 255 to 65536 bytes around the powers of two, over source and destination
  offsets that take every alignment from 0 to 15 (all 256 pairs below 70 bytes, a spread of pairs above);
- memmove with the destination from 70 bytes below to 70 bytes above the source, sizes up to 200 bytes and a few larger;
- memset over the same sizes and destination offsets.

It also counts the instructions each routine executes for 4096 bytes, aligned and relatively misaligned, against the strict
routines the pre-MMU code links (runtime/memory.c, memmove.c): the relaxed routine does not fall to a byte loop when the
alignments differ. Unicorn does not fault on an unaligned access; whether the A55 and A76 allow one is the SCTLR_EL1.A check
that every core makes before its first task (platform/a733-smp-cached/continue.c).
"""
import argparse
import json
from pathlib import Path
import random
import struct
import subprocess

from unicorn import UC_HOOK_BLOCK, Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ('runtime/memory_unaligned.c', 'runtime/memory.c', 'runtime/memmove.c', 'checks/memory-unaligned.c',
                'checks/memory-unaligned.py', 'include/sunstone_relaxed_memory.h')
GATE_RUNS = [['--output', '{output}']]
CODE, STACK, REQUEST, DATA = 0x100000, 0x300000, 0x200000, 0x1000000
CANARY = 64
DEAD = 0xdead0000

if not __debug__:
    raise RuntimeError('checks require assertions')

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--clang', default='/usr/bin/clang-21')
p.add_argument('--lld', type=Path, default=Path.home() / '.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)

objects = []
for name, flags in (('checks/memory-unaligned.c', ['-mno-strict-align']), ('runtime/memory_unaligned.c', ['-mno-strict-align']),
                    ('runtime/memory.c', ['-mstrict-align']), ('runtime/memmove.c', ['-mstrict-align'])):
    obj = a.output / (Path(name).stem + '.o')
    subprocess.run([a.clang, '-target', 'aarch64-none-elf', '-ffreestanding', '-nostdlib', '-fno-builtin', '-O2',
                    '-mgeneral-regs-only', '-march=armv8-a', *flags, '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'runtime/include'),
                    '-c', str(ROOT / name), '-o', str(obj)], check=True)
    objects.append(obj)
image = a.output / 'memory.bin'
subprocess.run([str(a.lld), '-Ttext=%#x' % CODE, '--image-base=0', '-e', 'check_entry', '--oformat=binary', '-o', str(image),
                *map(str, objects)], check=True)
code = image.read_bytes()

uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
uc.mem_map(CODE, 0x100000)
uc.mem_write(CODE, code)
uc.mem_map(REQUEST, 0x1000)
uc.mem_map(STACK, 0x10000)
uc.mem_map(DATA, 0x400000)
executed = [0]
uc.hook_add(UC_HOOK_BLOCK, lambda u, address, size, _: executed.__setitem__(0, executed[0] + size // 4))


def run(op, d, s, n):
    executed[0] = 0
    uc.mem_write(REQUEST, struct.pack('<4Q', op, d, s, n))
    uc.reg_write(UC_ARM64_REG_X0, REQUEST)
    uc.reg_write(UC_ARM64_REG_SP, STACK + 0x8000)
    uc.reg_write(UC_ARM64_REG_LR, DEAD)
    uc.emu_start(CODE, DEAD, count=50_000_000)
    assert uc.reg_read(UC_ARM64_REG_PC) == DEAD, 'routine did not return (op %d, n %d)' % (op, n)
    return executed[0]


rng = random.Random(1107)
SRC, DST = DATA + 0x1000, DATA + 0x200000
cases = 0


def copy_case(op, n, so, do):
    global cases
    src = bytes(rng.randrange(256) for _ in range(min(n, 4096))) * (n // 4096 + 1)
    src = src[:n]
    uc.mem_write(SRC + so - CANARY, b'\x5a' * CANARY + src + b'\x5a' * CANARY)
    uc.mem_write(DST + do - CANARY, b'\xa5' * CANARY + b'\x00' * n + b'\xa5' * CANARY)
    run(op, DST + do, SRC + so, n)
    got = bytes(uc.mem_read(DST + do - CANARY, n + 2 * CANARY))
    assert got == b'\xa5' * CANARY + src + b'\xa5' * CANARY, ('memcpy', op, n, so, do)
    assert bytes(uc.mem_read(SRC + so - CANARY, n + 2 * CANARY)) == b'\x5a' * CANARY + src + b'\x5a' * CANARY, ('source', n, so, do)
    cases += 1


for op in (0, 1):
    for n in range(0, 131):
        pairs = [(s, d) for s in range(16) for d in range(16)] if n < 70 else [(0, 0), (1, 0), (0, 3), (7, 9), (15, 1), (8, 8), (5, 13)]
        for so, do in pairs:
            copy_case(op, n, so, do)
    for n in (255, 256, 257, 511, 512, 513, 1023, 1024, 1025, 4095, 4096, 4097, 65535, 65536):
        for so, do in ((0, 0), (1, 0), (0, 3), (7, 9), (15, 1)):
            copy_case(op, n, so, do)

# memmove with overlap: the destination below and above the source.
for n in list(range(1, 201)) + [1000, 4096, 5000, 65536]:
    for shift in list(range(-70, 71)) if n < 120 else (-65, -33, -32, -17, -16, -9, -8, -1, 1, 7, 8, 15, 16, 17, 31, 32, 33, 64, 65):
        base = DATA + 0x400
        so = 100
        data = bytes(rng.randrange(256) for _ in range(min(n, 4096))) * (n // 4096 + 1)
        data = data[:n]
        window = bytearray(b'\x33' * (n + 400))
        window[so:so + n] = data
        uc.mem_write(base, bytes(window))
        reference = bytearray(window)
        dst = so + shift
        reference[dst:dst + n] = window[so:so + n]
        run(1, base + dst, base + so, n)
        got = bytes(uc.mem_read(base, n + 400))
        assert got == bytes(reference), ('memmove', n, shift)
        cases += 1

# memset.
for n in list(range(0, 131)) + [255, 256, 257, 1023, 1024, 1025, 4095, 4096, 4097, 65535, 65536]:
    for do in range(16) if n < 70 else (0, 1, 3, 7, 9, 15):
        value = rng.randrange(256)
        uc.mem_write(DST + do - CANARY, b'\xa5' * CANARY + b'\x00' * n + b'\xa5' * CANARY)
        run(2, DST + do, value, n)
        got = bytes(uc.mem_read(DST + do - CANARY, n + 2 * CANARY))
        assert got == b'\xa5' * CANARY + bytes([value]) * n + b'\xa5' * CANARY, ('memset', n, do)
        cases += 1

# Instructions for 4096 bytes: aligned, and with the source and destination at different alignments.
cost = {}
for name, op in (('relaxed memcpy', 0), ('strict memcpy', 3)):
    for label, so, do in (('aligned', 0, 0), ('misaligned', 3, 0), ('both unaligned', 5, 11)):
        uc.mem_write(SRC + so, bytes(4096))
        cost['%s, %s' % (name, label)] = round(run(op, DST + do, SRC + so, 4096) / 4096, 3)
for name, op in (('relaxed memset', 2), ('strict memset', 5)):
    cost['%s, unaligned destination' % name] = round(run(op, DST + 3, 0x5a, 4096) / 4096, 3)
assert cost['relaxed memcpy, misaligned'] < 1.0 and cost['relaxed memcpy, both unaligned'] < 1.0, cost
assert cost['strict memcpy, misaligned'] > 3.0, cost
(a.output / 'result.json').write_text(json.dumps({'cases': cases, 'instructions_per_byte': cost}, indent=1))
print('PASS unaligned memory routines: %d cases match the byte reference; instructions per byte %s' % (cases, cost))
