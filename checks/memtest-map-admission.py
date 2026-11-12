#!/usr/bin/env python3
"""Check complete memory-test mapping admission and optional native refusal before RAM writes."""
import argparse
import ctypes
import importlib.util
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'checks'))
import harness


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


host = module('memtest_host', ROOT / 'apps/memtest/check.py')


class MMU(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint64) for name in ('ttbr0', 'tcr', 'mair', 'sctlr')]


TCR = module('memtest_tcr', ROOT / 'tools/a733_tcr.py').CACHED_WALK
ATTR = (1 << 10) | (3 << 8) | 5
XN = (1 << 53) | (1 << 54)
ROOT_TABLE, CHILD = 0x41100000, 0x41101000
FIRST, END = 0x44000000, 0x340000000
READER = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint64,
                         ctypes.POINTER(ctypes.c_uint64))


def golden():
    words = {ROOT_TABLE + 8: CHILD | 3}
    words.update({ROOT_TABLE + 8 * i: (i << 30) | ATTR | XN for i in range(2, 13)})
    words.update({CHILD + 8 * i: (0x40000000 + (i << 21)) | ATTR | XN for i in range(32, 512)})
    return words


def pure(out):
    lib = host.library(out)
    lib.memtest_mapping_fault.argtypes = [ctypes.POINTER(host.Geometry), ctypes.POINTER(MMU), READER,
                                         ctypes.c_void_p]
    lib.memtest_mapping_fault.restype = ctypes.c_uint64
    words, reads, invalid_reads = golden(), [], []
    failed_read = None

    @READER
    def read(unused, address, value):
        if not (0x40000000 <= address <= FIRST - 8 and not address & 7):
            invalid_reads.append(hex(address))
            return 0
        reads.append(address)
        if address == failed_read:
            return 0
        value[0] = words.get(address, 0)
        return 1

    def run(mmu=None, geometry=None):
        reads.clear()
        mmu = mmu or MMU(ROOT_TABLE, TCR, 0x44ff00, 0x30d01805)
        geometry = geometry or host.board_geometry()
        result = lib.memtest_mapping_fault(ctypes.byref(geometry), ctypes.byref(mmu), read, None)
        assert not invalid_reads, invalid_reads
        return result

    assert run() == 0 and len(reads) == 492 and len(set(reads)) == 492
    tested = list(reads)
    mutations = 0
    # Every leaf, including all blocks above 4 GiB, must be admitted.
    for location in tested:
        if location == ROOT_TABLE + 8:
            continue
        original = words[location]
        for bit in (0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 12, 32, 34, 51, 52, 55, 63):
            words[location] = original ^ (1 << bit)
            assert run() != 0, (hex(location), bit)
            mutations += 1
        for xn in (0, 1 << 53, 1 << 54):
            words[location] = (original & ~XN) | xn
            assert run() == 0
        words[location] = original
        failed_read = location
        assert run() != 0
        failed_read = None
    # Upper table restrictions, reserved flags and unprotected child addresses
    # must fail before a child dereference.
    for value in (0, 1, 2, 3, 0x3ffff003, FIRST | 3, (1 << 64) - 1,
                  *((CHILD | 3) ^ (1 << bit) for bit in (2, 10, 48, 59, 60, 61, 62, 63))):
        words[ROOT_TABLE + 8] = value
        assert run() == FIRST and reads == [ROOT_TABLE + 8], hex(value)
    words = golden()
    # Canonical control fields are checked before any table read.
    for field in ('ttbr0', 'tcr', 'mair'):
        for bit in range(64):
            if field == 'ttbr0' and 12 <= bit < 26:
                continue  # A different protected root is valid to inspect.
            mmu = MMU(ROOT_TABLE, TCR, 0x44ff00, 0x30d01805)
            setattr(mmu, field, getattr(mmu, field) ^ (1 << bit))
            assert run(mmu) == FIRST and not reads, (field, bit)
    for bit in (0, 2, 12):
        assert run(MMU(ROOT_TABLE, TCR, 0x44ff00, 0x30d01805 ^ (1 << bit))) == FIRST
        assert not reads
    assert run(MMU(ROOT_TABLE, TCR, 0x44ff00, 0x30d01805 | (1 << 25))) == FIRST
    assert not reads  # Big-endian EL1 table loads are unsupported.
    for first, end in ((0, 6144), (32, 32), (32, 6145), (6144, 32)):
        geometry = host.board_geometry()
        geometry.first_block, geometry.end_block = first, end
        assert run(geometry=geometry) != 0 and not reads
    assert run() == 0
    return {'table_reads': len(reads), 'leaf_mutations_refused': mutations,
            'first_tested': hex(FIRST), 'end_tested': hex(END), 'physical_access': False}


def native(elf):
    model = module('memtest_emulate', ROOT / 'checks/memtest-emulate.py')
    from unicorn import UC_HOOK_MEM_WRITE
    counts = {}
    # Enter the real task's admission with its reduced map. Faults are in a
    # middle leaf the original three translation probes do not cover.
    for name, mutation in (('alias', 1 << 21), ('EL0', 1 << 6), ('readonly', 1 << 7),
                           ('not-accessed', 1 << 10), ('device', 1 << 2),
                           ('child-outside', None), ('bad-tcr', None), ('table-alias', None)):
        run = model.Run(elf, mask=0)
        m = run.m
        if name == 'bad-tcr':
            m.sys['tcr_el1'] ^= 1 << 7  # EPD0 disables TTBR0 walks.
        elif name == 'child-outside':
            m.write(m.address('level1') + 8, FIRST | 3)
        elif name == 'table-alias':
            slot = (m.address('level1') >> 21) & 511
            address = m.address('level2') + slot * 8
            m.write(address, m.read(address) ^ (1 << 21))
        else:
            address = m.address('level2') + 40 * 8
            m.write(address, m.read(address) ^ mutation)
        writes = []
        m.uc.hook_add(UC_HOOK_MEM_WRITE, lambda uc, access, at, size, value, data: writes.append(at),
                     begin=model.MAP_START, end=model.MAP_END - 1)
        record = run.go()
        assert record['status'] == 'refused' and record['refusal'] == 'mapping', (name, record)
        assert not writes, (name, writes)
        counts[name] = {'tested_memory_writes': len(writes), 'progress': record['progress']}
    run = model.Run(elf, mask=0)
    record = run.go()
    assert record['status'] == 'pass', record
    assert run.m.counts.get('at s1e1r') == 2, run.m.counts
    assert run.m.counts.get('at s1e1w') == 3, run.m.counts
    assert run.m.counts.get('ccsidr_el1') == 3, run.m.counts
    assert record['dcache_bytes'] == (32 + 256 + 2048) * 1024
    counts['valid-reduced-map'] = {'table_page_translations': run.m.counts['at s1e1r'],
                                   'write_translations': run.m.counts['at s1e1w'],
                                   'cache_geometry_reads': run.m.counts['ccsidr_el1']}
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--elf', type=Path, help='also enter admission in this native memory-test ELF')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    result = {'host': pure(args.output)}
    if args.elf:
        result['native'] = native(args.elf)
    harness.save(args.output / 'result.json', result)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
