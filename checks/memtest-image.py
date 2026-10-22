#!/usr/bin/env python3
"""Layout checks of the linked memory qualification image.

- Recovery region: the monitor's vector, code, state and stack pages sit at
  0x4100_1000-0x4100_4fff with code or data below offset 0x7c0 (0x800 for the
  vector table) and a beacon, the word's address XOR K, in every word from
  0x7c0 to 0xfff, in the ELF and in the uploaded binary.
- Every relocation applied to the recovery pages targets the recovery pages.
- The deepest EL3 call path, entry included, fits the 0x7c0-byte stack.
- The run contract is zero in the binary at its linker-placed address.
- The record and walk layouts in the image's DWARF match the host mirrors.
- Translation tables: platform_init runs in Unicorn and the check reports the
  DRAM mapped Normal write-back and non-cacheable and the table memory, for
  the image and, with --baseline, for an image of the tree before this work.
  Every table word equals either the current platform's (128 MiB) or the
  memtest platform's golden word for the 12,288 MiB map.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'checks'))
import harness  # noqa: E402
from elftools.elf.elffile import ELFFile  # noqa: E402
from elftools.elf.relocation import RelocationSection  # noqa: E402


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


machine = module('memtest_machine', ROOT / 'checks/memtest-machine.py')
host = module('memtest_host', ROOT / 'apps/memtest/check.py')
REGION, REGION_END = 0x41001000, 0x41005000
STACK_BYTES = 0x7c0
OBJDUMP = 'llvm-objdump-21'


def symbols(elf):
    out = {}
    for s in elf.get_section_by_name('.symtab').iter_symbols():
        if s.name:
            out.setdefault(s.name, []).append(s)
    return out


def one(table, name):
    values = {int(s['st_value']) for s in table[name]}
    assert len(values) == 1, (name, values)
    return values.pop()


def layout(elf, table):
    assert one(table, 'monitor_entry') == 0x41000000
    assert one(table, '__memtest_recovery_start') == REGION and one(table, '__memtest_recovery_end') == REGION_END
    assert one(table, 'memtest_contract') == REGION_END
    assert one(table, '__monitor_stack_top') == 0x410047c0
    pages = {'monitor_vectors': 0x41001000, 'monitor_sync': 0x41002000, 'monitor_reset': 0x41002000,
             'monitor_exception': 0x41002000, 'clean_data_cache': 0x41002000, 'invalidate_data_cache': 0x41002000,
             'saved': 0x41003000}
    placed = {}
    for name, page in pages.items():
        for s in table[name]:
            start, size = int(s['st_value']), int(s['st_size'])
            limit = 0x800 if name == 'monitor_vectors' else 0x7c0
            assert page <= start and start + size <= page + limit, (name, hex(start), size)
            placed[name] = hex(start)
    for name in ('monitor_init', 'wait_clear'):
        for s in table.get(name, []):
            assert not REGION <= int(s['st_value']) < REGION_END, name
    return placed


def loaded(elf):
    return [(int(p['p_vaddr']), p.data()) for p in elf.iter_segments() if p['p_type'] == 'PT_LOAD']


def word_at(parts, address):
    for base, data in parts:
        if base <= address and address + 8 <= base + len(data):
            return int.from_bytes(data[address - base:address - base + 8], 'little')
    raise AssertionError(f'{address:#x} is not loaded')


def beacons(parts, binary):
    count = 0
    for page in range(REGION, REGION_END, 4096):
        for address in range(page + 0x7c0, page + 0x1000, 8):
            assert word_at(parts, address) == address ^ host.K, hex(address)
            if binary is not None:
                offset = address - 0x41000000
                assert int.from_bytes(binary[offset:offset + 8], 'little') == address ^ host.K
            count += 1
    # The vector table's last slot: one MOV and one branch, then padding.
    assert all(word_at(parts, 0x41001788 + 8 * i) == 0 for i in range(7))
    return count


def relocations(elf, table):
    """Targets of every relocation applied to the recovery pages."""
    symtab = elf.get_section_by_name('.symtab')
    checked = 0
    for section in elf.iter_sections():
        if not isinstance(section, RelocationSection):
            continue
        target = elf.get_section(section['sh_info'])
        if not REGION <= target['sh_addr'] < REGION_END or not target['sh_size']:
            continue
        for r in section.iter_relocations():
            symbol = symtab.get_symbol(r['r_info_sym'])
            destination = int(symbol['st_value']) + r['r_addend']
            assert REGION <= destination < REGION_END, (target.name, symbol.name, hex(destination))
            checked += 1
    assert checked, 'the image was linked without --emit-relocs'
    return checked


def stack_depth(path, table):
    """Deepest EL3 stack use over the monitor's call graph, from disassembly."""
    text = subprocess.check_output([OBJDUMP, '-d', '--no-show-raw-insn', str(path)], text=True)
    functions, current = {}, None
    for line in text.splitlines():
        header = re.match(r'^[0-9a-f]+ <([^>]+)>:$', line)
        if header:
            current = functions.setdefault(header[1], {'frame': 0, 'calls': set()})
            continue
        if current is None or not line.strip():
            continue
        frame = re.search(r'\bsub\s+sp, sp, #(0x[0-9a-f]+|\d+)', line)
        push = re.search(r'\[sp, #-(0x[0-9a-f]+|\d+)\]!', line)
        for m in (frame, push):
            if m:
                current['frame'] += int(m[1], 0)
        call = re.search(r'\b(bl|b)\s+0x[0-9a-f]+ <([^>+]+)>', line)
        if call and call[2] in table:
            current['calls'].add(call[2])
    roots = ['monitor_entry', 'monitor_exception', 'monitor_sync']
    def depth(name, seen):
        f = functions.get(name)
        if not f or name in seen:
            return 0
        return f['frame'] + max([depth(c, seen | {name}) for c in f['calls'] if c != name] or [0])
    deepest = {root: depth(root, set()) for root in roots}
    assert max(deepest.values()) <= STACK_BYTES, deepest
    return deepest


def dwarf_layouts(path):
    image = harness.Image(path, layout_names={'memtest_record', 'memtest_walk', 'memtest_contract'})
    record = image.layouts['memtest_record']
    for name, _ in host.Record._fields_:
        assert record[name] == getattr(host.Record, name).offset, name
    contract = image.layouts['memtest_contract']
    for name, _ in host.Contract._fields_:
        assert contract[name] == getattr(host.Contract, name).offset, name
    walk = image.layouts['memtest_walk']
    header = (ROOT / 'apps/memtest/memtest.h').read_text()
    for name, offset in re.findall(r'#define MEMTEST_WALK_([A-Z_]+) (\d+)', header):
        if name.lower() in walk:
            assert walk[name.lower()] == int(offset), name
    return len(record)


def contract_zero(parts, table):
    address = one(table, 'memtest_contract')
    assert all(word_at(parts, address + 8 * i) == 0 for i in range(16))


def tables(path, model=True):
    """Run platform_init in Unicorn and classify every table word."""
    m = machine.Machine(path)
    level1, level2 = m.platform_tables(golden=False)
    words1 = [m.read(level1 + 8 * i) for i in range(512)]
    words2 = [m.read(level2 + 8 * i) for i in range(512)]
    golden1 = {i: (i << 30) | machine.GOLDEN_ATTRIBUTES for i in range(2, 13)}
    golden2 = {i: (0x40000000 + i * 0x200000) | machine.GOLDEN_ATTRIBUTES for i in range(64, 512)}
    golden = all(words1[i] == v for i, v in golden1.items()) and all(words2[i] == v for i, v in golden2.items())
    current = all(words1[i] == 0 for i in golden1) and all(words2[i] == 0 for i in golden2)
    assert golden or current, 'tested-map entries are neither absent nor the golden word'
    assert all(words1[i] == 0 for i in range(13, 512))
    summary = m.table_summary()
    summary['table_bytes'] = 2 * 4096
    summary['tested_map_entries'] = 'golden' if golden else 'absent'
    if model and not golden:
        # The memtest platform's entries, as the emulation checks model them.
        modeled = machine.Machine(path)
        modeled.platform_tables(golden=True)
        summary['with_memtest_platform_entries'] = modeled.table_summary()
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--binary', type=Path)
    parser.add_argument('--baseline', type=Path, help='returning-platform image of the tree before this work')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    with args.elf.open('rb') as f:
        elf = ELFFile(f)
        table = symbols(elf)
        parts = loaded(elf)
        binary = args.binary.read_bytes() if args.binary else None
        if binary is not None:
            for base, data in parts:
                if data and base < 0x41000000 + len(binary):
                    assert binary[base - 0x41000000:base - 0x41000000 + len(data)] == data, hex(base)
        result = {'placement': layout(elf, table), 'beacon_words': beacons(parts, binary),
                  'recovery_relocations': relocations(elf, table)}
        contract_zero(parts, table)
    result['stack_depth'] = stack_depth(args.elf, table)
    result['record_fields'] = dwarf_layouts(args.elf)
    result['tables'] = tables(args.elf)
    if args.baseline:
        result['baseline_tables'] = tables(args.baseline, model=False)
    harness.save(args.out / 'memtest-image.json', result)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
