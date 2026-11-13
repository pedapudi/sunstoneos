#!/usr/bin/env python3
"""Run the upload digest helper's own bytes under Unicorn and compare its digests with hashlib.

Usage: upload-digest.py --output DIR

tools/board_images.py builds boot/a733/upload-digest.S exactly as the runner does for a board run. The same binary runs
at its link address in the launcher SRAM and at an address in DRAM, on an Armv8 AArch32 CPU model with the SHA-256
instructions, with the request block the runner writes (tools/board_images.py digest_request and digest_reply). The
cases cover lengths around every padding boundary, a range larger than the helper's tail buffer, the largest range
count the request block holds and each refusal status. Memory hooks hold the helper to its contract: it reads only its
own bytes, the request block, the requested ranges and its stack, with every data access aligned to its size (an
unaligned access to Device memory faults on the board, where the MMU is off; a vector or multiple load is aligned when
each of its 4-byte elements is); it writes only the status, ID_ISAR5 and
digest fields and its stack; it returns to the caller with r4-r11, sp, CPACR and FPEXC as it found them. A Cortex-A15
model, which has no SHA-256 instructions (ID_ISAR5 0), must refuse with the no-SHA-256 status before any SIMD
instruction. The instruction counts are an emulator figure for the runner's time prediction, not board timing.
No hardware access.
"""
import argparse
import hashlib
import json
from pathlib import Path
import random
import struct
import sys

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import (UC_CPU_ARM_MAX, UC_CPU_ARM_CORTEX_A15, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
                               UC_ARM_REG_CP_REG, UC_ARM_REG_FPEXC, UC_ARM_REG_R4)

if not __debug__:
    raise RuntimeError('Checks require assertions.')
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import board_images as images

CPACR = (15, 0, 0, 1, 0, 0, 2)
DRAM, DATA, STACK, STOP = 0x40000000, 0x40400000, 0x40080000, 0x40000000
SRAM_PAGE = (0x50000, 0x10000)


def run(helper, base, ranges, data, cpu=UC_CPU_ARM_MAX, request=None):
    """Run HELPER at BASE with a request for RANGES over DATA (mapped at DATA). Return the outcome and the accesses."""
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.ctl_set_cpu_model(cpu)
    uc.mem_map(*SRAM_PAGE)
    uc.mem_map(DRAM, 0x01000000)
    uc.mem_write(base, helper)
    uc.mem_write(DATA, data)
    request = request if request is not None else images.digest_request(ranges)
    block = base + images.DIGEST_PARAMS
    uc.mem_write(block, request)
    callee = [0x0badc0de + 0x1111 * n for n in range(8)]
    for n, value in enumerate(callee):
        uc.reg_write(UC_ARM_REG_R4 + n, value)
    uc.reg_write(UC_ARM_REG_SP, STACK)
    uc.reg_write(UC_ARM_REG_LR, STOP)
    reads, writes, faults = [], [], []
    stats = {'instructions': 0}

    def access(kind, store):
        def hook(uc, access_type, address, size, value, user):
            store.append((address, size))
            # The emulator reports a VLD1.32 or LDM as accesses of up to 8 bytes; the architecture checks the alignment
            # of each 4-byte element (Arm ARM, VLD1 multiple single elements and LDM), which is what faults on Device memory.
            if address % min(size, 4):
                faults.append(f'unaligned {kind} of {size} bytes at {address:#x}')
        return hook

    def count(uc, address, size, user):
        stats['instructions'] += 1
    uc.hook_add(UC_HOOK_MEM_READ, access('read', reads))
    uc.hook_add(UC_HOOK_MEM_WRITE, access('write', writes))
    uc.hook_add(UC_HOOK_CODE, count)
    cpacr = uc.reg_read(UC_ARM_REG_CP_REG, CPACR)
    uc.emu_start(base, STOP, count=50_000_000)
    assert uc.reg_read(UC_ARM_REG_PC) == STOP, 'The helper did not return to its caller.'
    reply = bytes(uc.mem_read(block, len(request)))
    preserved = ([uc.reg_read(UC_ARM_REG_R4 + n) for n in range(8)] == callee and uc.reg_read(UC_ARM_REG_SP) == STACK and
                 uc.reg_read(UC_ARM_REG_CP_REG, CPACR) == cpacr)
    if cpacr & (0xf << 20):
        preserved &= uc.reg_read(UC_ARM_REG_FPEXC) & (1 << 30) == 0
    return {'reply': reply, 'request': request, 'reads': reads, 'writes': writes, 'faults': faults,
            'preserved': preserved, 'instructions': stats['instructions']}


def inside(address, size, spans):
    return any(low <= address and address + size <= low + length for low, length in spans)


def contract(outcome, helper, base, ranges):
    """The helper touched only what it may; return the names of the violations."""
    block = base + images.DIGEST_PARAMS
    count = len(ranges)
    stack = (STACK - 256, 256)
    readable = [(base, len(helper)), (block, len(outcome['request'])), stack] + [(a, n) for a, n in ranges]
    writable = [(block + 8, 8), (block + 16 + 8 * count, 32 * count), stack]
    problems = list(outcome['faults'])
    problems += [f'read of {s} at {a:#x}' for a, s in outcome['reads'] if not inside(a, s, readable)]
    problems += [f'write of {s} at {a:#x}' for a, s in outcome['writes'] if not inside(a, s, writable)]
    if not outcome['preserved']:
        problems.append('r4-r11, sp, CPACR or FPEXC changed')
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    a = parser.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    helper = images.build_upload_digest(a.output / 'build').read_bytes()
    rnd = random.Random(20261112)
    data = bytes(rnd.getrandbits(8) for _ in range(1 << 20))
    lengths = [0, 1, 3, 55, 56, 57, 63, 64, 65, 119, 120, 121, 127, 128, 129, 1000, 4096, 65536, 300001]
    ranges = [(DATA + 4 * rnd.randrange(0, 1000), n) for n in lengths]
    cases = {}
    for name, base in (('launcher-sram', images.LAUNCHER_SRAM[0]), ('dram', DRAM + 0x10000)):
        outcome = run(helper, base, ranges, data)
        status, isar5, digests = images.digest_reply(outcome['request'], outcome['reply'])
        assert status == images.DIGEST_DONE and isar5 >> 12 & 15, (name, hex(status), hex(isar5))
        expected = [hashlib.sha256(data[a - DATA:a - DATA + n]).digest() for a, n in ranges]
        assert digests == expected, (name, [n for (_, n), d, e in zip(ranges, digests, expected) if d != e])
        problems = contract(outcome, helper, base, ranges)
        assert not problems, (name, problems[:5])
        blocks = sum(n // 64 + (1 if n % 64 < 56 else 2) for n in lengths)
        cases[name] = {'ranges': len(ranges), 'bytes': sum(lengths), 'instructions': outcome['instructions'],
                       'instructions_per_block': round(outcome['instructions'] / blocks, 1)}
        print(f'PASS {name}: {len(ranges)} digests equal hashlib, contract held')
    # The largest request the block holds, and one range more.
    many = [(DATA + 64 * n, 64 + n) for n in range(images.DIGEST_MAX_RANGES)]
    outcome = run(helper, images.LAUNCHER_SRAM[0], many, data)
    status, _, digests = images.digest_reply(outcome['request'], outcome['reply'])
    assert status == images.DIGEST_DONE and digests == [hashlib.sha256(data[a - DATA:a - DATA + n]).digest() for a, n in many]
    assert not contract(outcome, helper, images.LAUNCHER_SRAM[0], many)
    end = images.LAUNCHER_SRAM[0] + images.DIGEST_PARAMS + len(outcome['request'])
    assert end <= images.LAUNCHER_SRAM[1] < end + 40, 'The request block does not fill the reservation to within one range.'
    print(f'PASS largest request: {images.DIGEST_MAX_RANGES} ranges inside the launcher reservation')
    refusals = {}
    over = struct.pack('<4I', images.DIGEST_MAGIC, images.DIGEST_MAX_RANGES + 1, 0, 0) + bytes(40 * (images.DIGEST_MAX_RANGES + 1))
    bad_magic = bytearray(images.digest_request(ranges[:2]))
    bad_magic[0] ^= 1
    for name, request, rr, expected in (
            ('count', over, [], 0xe0000003),
            ('magic', bytes(bad_magic), [], 0xe0000001),
            ('alignment', images.digest_request([(DATA, 64), (DATA + 2, 64)]), [(DATA, 64)], 0xe0000004)):
        outcome = run(helper, images.LAUNCHER_SRAM[0], rr, data, request=request)
        status = struct.unpack_from('<I', outcome['reply'], 8)[0]
        assert status == expected, (name, hex(status))
        assert outcome['preserved'] and not outcome['faults'], name
        refusals[name] = hex(status)
    outcome = run(helper, images.LAUNCHER_SRAM[0], ranges[:2], data, cpu=UC_CPU_ARM_CORTEX_A15)
    status, isar5, _ = images.digest_reply(outcome['request'], outcome['reply'])
    assert status == 0xe0000002 and not isar5 >> 12 & 15 and outcome['preserved'], (hex(status), hex(isar5))
    refusals['no-sha2'] = hex(status)
    print('PASS refusals: count, magic, alignment and a core without SHA-256 return their status and restore state')
    result = {'helper_sha256': hashlib.sha256(helper).hexdigest(), 'helper_bytes': len(helper), 'cases': cases,
              'max_ranges': images.DIGEST_MAX_RANGES, 'refusals': refusals}
    (a.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS upload digest helper against hashlib under Unicorn:', json.dumps(result))


if __name__ == '__main__':
    try:
        main()
    except UcError as error:
        raise SystemExit(f'FAIL emulator stopped: {error}')
