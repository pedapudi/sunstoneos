#!/usr/bin/env python3
"""Attribute CPU0 tick cache lines from a QEMU footprint trace to structures.

Input: an ELF with DWARF and a trace written by tools/qemu-tick-footprint.c
with marks at the latency cause hooks (release begin/end, deadline-scan
begin/end). Output: JSON with, per segment, every 64-byte data and instruction
line, the variable and field each access resolves to, and the code location
that touched it first. Diagnostic code (the cause hooks, the latency recorder
and its IRQ wrapper) is reported separately from production code.
"""
import argparse
import bisect
import collections
import json
import subprocess
import sys
from pathlib import Path

from elftools.elf.elffile import ELFFile

LINE = 64
FIXTURE_DIR = 'apps/latency/'
RANK = ('os', 'stack', 'fixture', 'diagnostic')
DIAGNOSTIC_FILES = ('kernel/scheduler/latency_cause.c', 'runtime/latency.c',
                    'runtime/latency_irq.c', 'kernel/scheduler/latency_trace',
                    'kernel/scheduler/latency_pmu.c')


def strip(die):
    while die is not None and die.tag in ('DW_TAG_typedef', 'DW_TAG_const_type',
                                          'DW_TAG_volatile_type', 'DW_TAG_atomic_type',
                                          'DW_TAG_restrict_type'):
        if 'DW_AT_type' not in die.attributes:
            return None
        die = die.get_DIE_from_attribute('DW_AT_type')
    return die


def size_of(die):
    die = strip(die)
    if die is None:
        return 0
    if 'DW_AT_byte_size' in die.attributes:
        return die.attributes['DW_AT_byte_size'].value
    if die.tag == 'DW_TAG_array_type':
        return element_count(die) * size_of(die.get_DIE_from_attribute('DW_AT_type'))
    if die.tag == 'DW_TAG_pointer_type':
        return 8
    return 0


def dims(die):
    out = []
    for child in die.iter_children():
        if child.tag != 'DW_TAG_subrange_type':
            continue
        if 'DW_AT_count' in child.attributes:
            out.append(child.attributes['DW_AT_count'].value)
        elif 'DW_AT_upper_bound' in child.attributes:
            out.append(child.attributes['DW_AT_upper_bound'].value + 1)
        else:
            out.append(0)
    return out


def element_count(die):
    n = 1
    for d in dims(die):
        n *= d
    return n


def field_path(die, offset, depth=0):
    die = strip(die)
    if die is None or depth > 12:
        return ''
    if die.tag == 'DW_TAG_array_type':
        elem = die.get_DIE_from_attribute('DW_AT_type')
        esize = size_of(elem)
        if not esize:
            return ''
        shape = dims(die)
        index = offset // esize
        parts = []
        for extent in reversed(shape):
            if extent:
                parts.append(index % extent)
                index //= extent
        text = ''.join(f'[{i}]' for i in reversed(parts))
        return text + field_path(elem, offset % esize, depth + 1)
    if die.tag in ('DW_TAG_structure_type', 'DW_TAG_union_type'):
        best = None
        for member in die.iter_children():
            if member.tag != 'DW_TAG_member':
                continue
            loc = member.attributes.get('DW_AT_data_member_location')
            start = loc.value if loc is not None and isinstance(loc.value, int) else 0
            mtype = member.get_DIE_from_attribute('DW_AT_type')
            msize = size_of(mtype) or 1
            if start <= offset < start + msize:
                name = member.attributes.get('DW_AT_name')
                label = name.value.decode() if name else '<anon>'
                best = '.' + label + field_path(mtype, offset - start, depth + 1)
                if die.tag == 'DW_TAG_structure_type':
                    break
        return best or f'+{offset}'
    return ''


def load_variables(path):
    variables = []
    with open(path, 'rb') as f:
        elf = ELFFile(f)
        dwarf = elf.get_dwarf_info()
        for cu in dwarf.iter_CUs():
            top = cu.get_top_DIE()
            unit = top.attributes.get('DW_AT_name')
            unit = unit.value.decode() if unit else '?'
            for die in cu.iter_DIEs():
                if die.tag != 'DW_TAG_variable' or 'DW_AT_location' not in die.attributes:
                    continue
                loc = die.attributes['DW_AT_location'].value
                if not isinstance(loc, list) or not loc:
                    continue
                if loc[0] == 0x03 and len(loc) == 9:  # DW_OP_addr
                    addr = int.from_bytes(bytes(loc[1:9]), 'little')
                elif loc[0] == 0xa1:  # DW_OP_addrx with a ULEB128 index
                    index, shift, i = 0, 0, 1
                    while True:
                        index |= (loc[i] & 0x7f) << shift
                        shift += 7
                        i += 1
                        if not loc[i - 1] & 0x80:
                            break
                    if i != len(loc):
                        continue
                    addr = dwarf.get_addr(cu, index)
                else:
                    continue
                decl = die
                if 'DW_AT_specification' in die.attributes:
                    decl = die.get_DIE_from_attribute('DW_AT_specification')
                if 'DW_AT_type' not in decl.attributes:
                    continue
                vtype = decl.get_DIE_from_attribute('DW_AT_type')
                name = decl.attributes.get('DW_AT_name')
                name = name.value.decode() if name else '?'
                variables.append((addr, size_of(vtype), name, unit, vtype))
    variables.sort(key=lambda v: v[0])
    return variables


def symbol(elf, name):
    with open(elf, 'rb') as f:
        table = ELFFile(f).get_section_by_name('.symtab')
        found = table.get_symbol_by_name(name) if table else None
        return found[0]['st_value'] if found else None


def symbolize(elf, pcs):
    if not pcs:
        return {}
    text = '\n'.join(hex(pc) for pc in pcs) + '\n'
    out = subprocess.run(['/usr/bin/llvm-symbolizer-21', '--obj', str(elf), '--functions=linkage',
                          '--inlining=false'],
                         input=text, capture_output=True, text=True, check=True).stdout
    blocks = [b.splitlines() for b in out.strip().split('\n\n')]
    result = {}
    for pc, block in zip(pcs, blocks):
        func = block[0] if block else '?'
        loc = block[1] if len(block) > 1 else '?'
        result[pc] = (func, loc)
    return result


def parse(trace):
    windows, current = [], None
    for line in open(trace):
        f = line.split()
        if not f or f[0] == '#':
            continue
        if f[0] == 'W' and len(f) == 2:
            current = []
        elif f[0] == 'E':
            windows.append(current)
            current = None
        elif current is not None:
            if f[0] == 'M':
                current.append(('M', int(f[1], 16)))
            elif f[0] == 'I':
                current.append(('I', int(f[1], 16)))
            else:
                current.append((f[0], int(f[1], 16), int(f[2], 16), int(f[3])))
    return windows


def segment(window, marks):
    """Label each record with its segment: release.N, scan.N or outside."""
    labels, label = [], 'outside'
    releases = scans = 0
    for rec in window:
        if rec[0] == 'M':
            if rec[1] == marks['release_begin']:
                label = f'release.{releases}'
            elif rec[1] == marks['release_end']:
                releases += 1
                label = 'outside'
            elif rec[1] == marks['state_begin']:
                label = f'scan.{scans}'
            elif rec[1] == marks['state_end']:
                scans += 1
                label = 'outside'
            continue
        labels.append((label, rec))
    return labels, releases, scans


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('elf', type=Path)
    p.add_argument('trace', type=Path)
    p.add_argument('--mark', action='append', default=[], help='name=hexaddr')
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    marks = {k: int(v, 16) for k, v in (m.split('=') for m in a.mark)}
    windows = parse(a.trace)
    full = [w for w in windows if segment(w, marks)[1:] == (8, 8)]
    if not full:
        sys.exit('no window with eight releases and eight deadline scans')
    variables = load_variables(a.elf)
    starts = [v[0] for v in variables]

    def resolve(addr):
        i = bisect.bisect_right(starts, addr) - 1
        while i >= 0:
            base, size, name, unit, vtype = variables[i]
            if base <= addr < base + max(size, 1):
                return f'{name}{field_path(vtype, addr - base)}', unit
            if base + max(size, 1) <= addr and addr - base > 1 << 24:
                break
            i -= 1
        return '?', '?'

    pcs = sorted({r[1] for w in full for r in w if r[0] != 'M'})
    where = symbolize(a.elf, pcs)
    stacks = symbol(a.elf, '__exception_stacks')

    def code_class(pc):
        loc = where.get(pc, ('?', '?'))[1]
        if any(d in loc for d in DIAGNOSTIC_FILES):
            return 'diagnostic'
        return 'fixture' if FIXTURE_DIR in loc else 'os'

    def data_class(pc, addr, unit):
        if code_class(pc) == 'diagnostic' or any(d in unit for d in DIAGNOSTIC_FILES):
            return 'diagnostic'
        if stacks and addr >= stacks and unit == '?':
            return 'stack'
        return 'fixture' if FIXTURE_DIR in unit else 'os'

    per_window = []
    for w in full:
        labels, _, _ = segment(w, marks)
        segs = collections.defaultdict(lambda: {'data': collections.OrderedDict(), 'code': {},
                                                'insns': collections.Counter(),
                                                'first_data': collections.Counter(),
                                                'first_code': collections.Counter()})
        seen_data, seen_code = set(), set()
        for label, rec in labels:
            s = segs[label]
            if rec[0] == 'I':
                cls = code_class(rec[1])
                s['insns'][cls] += 1
                line = rec[1] // LINE * LINE
                s['code'][line] = min(s['code'].get(line, cls), cls, key=RANK.index)
                if cls != 'diagnostic' and line not in seen_code:
                    seen_code.add(line)
                    s['first_code'][cls] += 1
                continue
            kind, pc, addr, size = rec
            field, unit = resolve(addr)
            if field == '?' and stacks and addr >= stacks:
                field = f'exception stack +{addr - stacks:#x}'
            cls = data_class(pc, addr, unit)
            site = where.get(pc, ('?', '?'))
            for line in {addr // LINE * LINE, (addr + size - 1) // LINE * LINE}:
                entry = s['data'].setdefault(line, {'reads': 0, 'writes': 0, 'class': cls,
                                                    'fields': collections.OrderedDict()})
                entry['writes' if kind == 'W' else 'reads'] += 1
                entry['class'] = min(entry['class'], cls, key=RANK.index)
                entry['fields'].setdefault(field, f'{site[0]} {site[1].rsplit("/", 1)[-1]}')
                if cls != 'diagnostic' and line not in seen_data:
                    seen_data.add(line)
                    s['first_data'][cls] += 1
        per_window.append((segs, len(seen_data), len(seen_code)))

    order = sorted(range(len(per_window)), key=lambda i: per_window[i][1])
    rep, rep_data, rep_code = per_window[order[len(order) // 2]]
    report = {'windows': len(full),
              'classes': 'os: kernel, port and platform; stack: exception stack; fixture: latency '
                         'application; diagnostic: cause hooks and latency recorder (excluded from first-touch counts)',
              'tick_first_touch_data_lines': rep_data, 'tick_first_touch_code_lines': rep_code,
              'tick_first_touch_data_range': [per_window[order[0]][1], per_window[order[-1]][1]],
              'segments': {}}
    for label in sorted(rep, key=lambda x: (x.split('.')[0], int(x.split('.')[1]) if '.' in x else -1)):
        s = rep[label]
        lines = [{'line': hex(line), 'class': e['class'], 'reads': e['reads'], 'writes': e['writes'],
                  'fields': [{'field': k, 'first_site': v} for k, v in e['fields'].items()]}
                 for line, e in s['data'].items()]
        by_var = collections.Counter()
        for e in s['data'].values():
            if e['class'] != 'diagnostic':
                by_var[next(iter(e['fields'])).split('.')[0].split('[')[0].split(' +')[0]] += 1
        report['segments'][label] = {
            'instructions': dict(s['insns']),
            'data_lines': dict(collections.Counter(e['class'] for e in s['data'].values())),
            'code_lines': dict(collections.Counter(s['code'].values())),
            'first_touch_data_lines': dict(s['first_data']),
            'first_touch_code_lines': dict(s['first_code']),
            'non_diagnostic_lines_by_variable': dict(by_var.most_common()),
            'lines': lines}
    a.output.write_text(json.dumps(report, indent=1) + '\n')
    for label, seg in report['segments'].items():
        print(f"{label:10} insns {seg['instructions']} data {seg['data_lines']} code {seg['code_lines']} "
              f"first data {seg['first_touch_data_lines']} first code {seg['first_touch_code_lines']}")
    print('tick first-touch data', rep_data, 'code', rep_code, 'windows', len(full),
          'data range', report['tick_first_touch_data_range'])


if __name__ == '__main__':
    main()
