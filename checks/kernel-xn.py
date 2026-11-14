#!/usr/bin/env python3
"""Walk the EL1 tables that each OS image builds and refuse writable code and executable read-only data.

For each linked OS image, the check runs the image's own a733_cached_memory_prepare()
(platform/a733-smp-cached/memory.c) under Unicorn, with the machine model of
checks/cpu-table-walks.py, and walks the EL1 level-1 table and every table below it,
applying the hierarchical table bits. It then requires:

1. no page that EL1 or EL0 can write is executable at either level, and no physical
   page that one virtual address makes writable is executable through another;
2. every page of .rodata, the application manifest, the other allocated read-only
   sections and the sealed session (apps/os/sealed-link.patch) is read-only and
   execute-never at EL1 and EL0, every other allocated section that is not
   code is execute-never at both levels, and every code section is executable
   at EL1;
3. the image area 0x40000000..0x48000000 is mapped by level-2 block entries only,
   one per 2 MiB, with no level-3 table, so splitting read-only data from text
   changes no TLB reach: 63 valid blocks (the monitor block 0x42000000 stays
   invalid at EL1, docs/memory-protection.md).

Six mutations of the walked tables, each of which one rule must refuse, show that
the rules can fail: text made writable, text made execute-never, read-only data
made executable at EL1 and at EL0, the sealed session placed in the text block,
and the read-only data block split into 4 KiB pages with unchanged permissions.
No hardware is accessed.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import sys

from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
# Every OS image the gate builds.
GATE_RUNS = [['--image', '{elf:sup}', '--image', '{elf:headset}', '--image', '{elf:os-usb-ledger}', '--image',
               '{elf:resident-stop}', '--image', '{elf:card-profile}', '--image', '{elf:ret-npu-load}', '--image',
               '{elf:ret-gpu}', '--image', '{elf:ret-latency}', '--image', '{elf:ret-radio-scan}', '--image',
               '{elf:ret-walks-cached}', '--image', '{elf:ret-walks-uncached}', '--output', '{output}']]
sys.path.insert(0, str(ROOT / 'checks'))
import harness  # noqa: E402,F401  Refuses to run with assertions disabled.

spec = importlib.util.spec_from_file_location('cpu_table_walks', ROOT / 'checks/cpu-table-walks.py')
walks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(walks)
LAYOUT = walks.load('memory_layout', ROOT / 'tools/memory_layout.py').record()
CODE, RODATA = tuple(LAYOUT['image']['code']), tuple(LAYOUT['image']['os_rodata'])
HEAD = (0x40000000, 0x48000000)          # level-1 slot 1's first 128 MiB: the image, the monitor and the EL0 window
HEAD_BLOCKS = 63                         # every 2 MiB of HEAD except the monitor block
BIT = lambda n: 1 << n                   # noqa: E731
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 1, 2, 4


class Image:
    """Load segments and symbols: what the machine model needs, without DWARF."""

    def __init__(self, path):
        with open(path, 'rb') as f:
            elf = ELFFile(f)
            self.parts = [(int(p['p_vaddr']), p.data()) for p in elf.iter_segments() if p['p_type'] == 'PT_LOAD']
            self.symbols = {}
            for s in elf.get_section_by_name('.symtab').iter_symbols():
                self.symbols.setdefault(s.name, set()).add(int(s['st_value']))
            self.sections = [(s.name, int(s['sh_addr']), int(s['sh_size']), int(s['sh_flags']))
                             for s in elf.iter_sections()
                             if s['sh_flags'] & SHF_ALLOC and s['sh_size'] and HEAD[0] <= s['sh_addr'] < HEAD[1]]

    def address(self, name):
        values = self.symbols.get(name, set()) - {0}
        assert len(values) == 1, name
        return next(iter(values))


class Tables:
    """The EL1 translation tables as words, with an overlay for mutations."""

    def __init__(self, machine, root):
        self.machine, self.root, self.overlay, self.pages = machine, root, {}, {}

    def word(self, table, index):
        address = table + 8 * index
        if address in self.overlay:
            return self.overlay[address]
        page = address & ~0xfff
        if page not in self.pages:
            self.pages[page] = bytes(self.machine.uc.mem_read(page, 4096))
        return int.from_bytes(self.pages[page][address - page:address - page + 8], 'little')

    def leaves(self):
        """(va, size, descriptor, level, inherited) for every valid leaf; inherited ORs the table bits 59..62 above it."""
        def walk(table, level, base, inherited):
            shift = 39 - 9 * level
            for i in range(512):
                d = self.word(table, i)
                if not d & 1:
                    continue
                va = base + (i << shift)
                if level < 3 and d & 2:
                    yield from walk(d & 0xfffffffff000, level + 1, va, inherited | (d & (0xf << 59)))
                elif level == 3 and not d & 2:
                    continue                            # reserved at level 3
                else:
                    yield va, 1 << shift, d, level, inherited
        yield from walk(self.root, 1, 0, 0)

    def head_entries(self):
        """Level-2 descriptors of HEAD, from the level-1 entry of slot 1."""
        d = self.word(self.root, 1)
        assert d & 3 == 3, 'level-1 slot 1 is not a table'
        table = d & 0xfffffffff000
        return [self.word(table, i) for i in range((HEAD[0] - 0x40000000) >> 21, (HEAD[1] - 0x40000000) >> 21)]


def access(d, inherited):
    """(el1_write, el0_read, el0_write, el1_exec, el0_exec) of a leaf descriptor under its table bits."""
    ap = (d >> 6) & 3
    el0 = bool(ap & 1) and not inherited & BIT(61)
    write = not ap & 2 and not inherited & BIT(62)
    return (write, el0, el0 and write, not (d & BIT(53) or inherited & BIT(59)), not (d & BIT(54) or inherited & BIT(60)))


def violations(image, tables):
    """Every rule failure, as (rule, detail)."""
    found = []
    pages = []
    for va, size, d, level, inherited in tables.leaves():
        el1_write, el0_read, el0_write, el1_exec, el0_exec = access(d, inherited)
        pa = d & 0xfffffffff000 & ~(size - 1)
        pages.append((va, size, pa, el1_write or el0_write, el1_exec or el0_exec, el0_read, el1_exec, el0_exec))
        if (el1_write or el0_write) and (el1_exec or el0_exec):
            found.append(('writable-executable', '%#x+%#x' % (va, size)))
    # A physical page writable through one address and executable through another is W+X too.
    writable = [(pa, pa + size) for _, size, pa, w, _, _, _, _ in pages if w]
    for va, size, pa, w, x, _, _, _ in pages:
        if x and not w and any(lo < pa + size and pa < hi for lo, hi in writable):
            found.append(('writable-alias-of-code', '%#x+%#x' % (va, size)))
    for name, low, size, flags in image.sections:
        read_only = name == '.sealed' or not flags & SHF_WRITE
        code = flags & SHF_EXECINSTR and name != '.sealed'
        for va, page, pa, w, x, el0_read, el1_exec, el0_exec in pages:
            if not (va < low + size and low < va + page):
                continue
            if not code and (el1_exec or el0_exec):
                found.append(('executable-data' if not read_only else 'executable-rodata', '%s at %#x' % (name, va)))
            if read_only and w:
                found.append(('writable-rodata', '%s at %#x' % (name, va)))
            if code and not el1_exec:
                found.append(('code-not-executable', '%s at %#x' % (name, va)))
    entries = tables.head_entries()
    blocks = sum(1 for d in entries if d & 3 == 1)
    split = [HEAD[0] + (i << 21) for i, d in enumerate(entries) if d & 3 == 3]
    if split or blocks != HEAD_BLOCKS:
        found.append(('image-block-count', '%d blocks, tables at %s' % (blocks, [hex(a) for a in split])))
    return found


def summary(image, tables):
    runs = []
    for va, size, d, level, inherited in tables.leaves():
        if not HEAD[0] <= va < HEAD[1]:
            continue
        el1_write, el0_read, el0_write, el1_exec, el0_exec = access(d, inherited)
        key = ('rw' if el1_write else 'ro', ('rw' if el0_write else 'ro') if el0_read else 'none',
               ''.join(('EL1 ' if el1_exec else '', 'EL0' if el0_exec else '')).strip() or 'never', (d >> 2) & 7)
        if runs and runs[-1]['end'] == va and runs[-1]['key'] == key:
            runs[-1]['end'] = va + size
        else:
            runs.append({'start': va, 'end': va + size, 'key': key})
    rows = []
    for r in runs:
        names = sorted({n for n, low, size, _ in image.sections if low < r['end'] and r['start'] < low + size})
        rows.append({'range': '%#x-%#x' % (r['start'], r['end']), 'el1': r['key'][0], 'el0': r['key'][1],
                     'execute': r['key'][2], 'attr_index': r['key'][3], 'sections': names})
    return rows


def mutations(image, tables):
    """Each mutation and the rule it must trip; the overlay is cleared after each."""
    l2 = tables.word(tables.root, 1) & 0xfffffffff000
    text, rodata = l2 + 8 * ((CODE[0] - 0x40000000) >> 21), l2 + 8 * ((RODATA[0] - 0x40000000) >> 21)
    text_word, rodata_word = tables.word(l2, (CODE[0] - 0x40000000) >> 21), tables.word(l2, (RODATA[0] - 0x40000000) >> 21)
    results = {}

    def trial(name, rule, overlay, sections=None):
        tables.overlay = overlay
        saved = image.sections
        if sections is not None:
            image.sections = sections
        try:
            rules = {r for r, _ in violations(image, tables)}
        finally:
            tables.overlay, image.sections = {}, saved
        assert rule in rules, (name, rule, rules)
        results[name] = sorted(rules)

    trial('text-writable', 'writable-executable', {text: text_word & ~(3 << 6) | (1 << 6)})
    trial('text-execute-never', 'code-not-executable', {text: text_word | BIT(53)})
    trial('rodata-el1-executable', 'executable-rodata', {rodata: rodata_word & ~BIT(53)})
    trial('rodata-el0-executable', 'executable-rodata', {rodata: rodata_word & ~BIT(54)})
    sealed = [s for s in image.sections if s[0] == '.sealed']
    assert len(sealed) == 1, 'the image has no sealed session'
    moved = [s if s[0] != '.sealed' else ('.sealed', CODE[1] - s[2] - (CODE[1] - s[2]) % 4096, s[2], s[3])
             for s in image.sections]
    trial('sealed-in-text', 'executable-rodata', {}, moved)
    # Split the read-only data block into 512 pages with its own permissions: nothing but the block count changes.
    page = 0x47ffe000
    split = {page + 8 * i: (rodata_word & ~0x1fffff & 0xfffffffff000) + (i << 12) | (rodata_word & ~0xfffffffff000) | 3
             for i in range(512)}
    split[rodata] = page | 3
    tables.overlay = split
    try:
        rules = {r for r, _ in violations(image, tables)}
    finally:
        tables.overlay = {}
    assert rules == {'image-block-count'}, ('rodata-split-into-pages', rules)
    results['rodata-split-into-pages'] = sorted(rules)
    return results


def check(path):
    image = Image(path)
    machine = walks.Machine(image)
    assert machine.call('a733_cached_memory_prepare') == 1, 'table preparation refused'
    tables = Tables(machine, image.address('a733_cached_el1_l1'))
    found = violations(image, tables)
    assert not found, found
    assert image.address('__os_rodata_block') == RODATA[0], 'the OS link script places RODATA elsewhere'
    return {'tables': summary(image, tables), 'mutations': mutations(image, tables)}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--image', type=Path, action='append', required=True, help='a linked OS image')
    parser.add_argument('--output', type=Path, required=True, help='new directory for the result')
    args = parser.parse_args()
    out = args.output.resolve()
    assert ROOT not in [out, *out.parents]
    out.mkdir(parents=True, exist_ok=False)
    result = {}
    for path in args.image:
        result[str(path)] = check(path.resolve())
        print('PASS %s: no writable executable page, read-only data execute-never, %d image blocks' % (path, HEAD_BLOCKS))
    (out / 'result.json').write_text(json.dumps(result, indent=1) + '\n')
    print('PASS kernel-xn: %d images, 6 mutations each refused' % len(args.image))


if __name__ == '__main__':
    main()
