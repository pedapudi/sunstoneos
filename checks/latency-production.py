#!/usr/bin/env python3
"""Compare normal OS ELFs built before and after optional latency instrumentation.

Require identical executable bytes and addresses, unchanged writable storage,
and no latency symbols. Preserve complete disassembly and every section's hash.
Build identity data may differ, so whole-file equality is reported separately.
"""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import subprocess

from elftools.elf.elffile import ELFFile


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def inspect(path):
    payload = path.read_bytes()
    sections, objects, latency = {}, [], []
    with path.open('rb') as stream:
        elf = ELFFile(stream)
        if elf['e_machine'] != 'EM_AARCH64' or elf['e_type'] != 'ET_EXEC':
            raise ValueError(f'{path}: expected an AArch64 executable')
        for section in elf.iter_sections():
            if not section['sh_flags'] & 2:  # SHF_ALLOC
                continue
            sections[section.name] = {
                'address': section['sh_addr'], 'size': section['sh_size'],
                'flags': section['sh_flags'], 'type': section['sh_type'],
                'alignment': section['sh_addralign'],
                'sha256': sha256(section.data()),
            }
        symbols = elf.get_section_by_name('.symtab')
        if symbols is None:
            raise ValueError(f'{path}: symbol table required to check storage')
        for symbol in symbols.iter_symbols():
            if 'latency' in symbol.name.lower():
                latency.append(symbol.name)
            index = symbol['st_shndx']
            if symbol['st_info']['type'] == 'STT_OBJECT' and isinstance(index, int):
                section = elf.get_section(index)
                if section['sh_flags'] & 2:
                    objects.append([symbol.name, section.name, symbol['st_value'], symbol['st_size']])
    return {'path': str(path), 'sha256': sha256(payload), 'sections': sections,
            'objects': sorted(objects), 'latency_symbols': sorted(set(latency))}


def cache(path):
    values = {}
    for line in path.read_text().splitlines():
        if line and not line.startswith(('#', '//')) and '=' in line:
            key, value = line.split('=', 1)
            values[key.split(':', 1)[0]] = value
    expected = {'SUNSTONE_A733_OS': 'ON', 'SUNSTONE_LATENCY': 'OFF',
                'SUNSTONE_APPLICATION': '', 'SUNSTONE_EXTERNAL_APPLICATIONS': '',
                'MIMUS_DIR': ''}
    for key, value in expected.items():
        if values.get(key) != value:
            raise ValueError(f'{path}: {key} must be {value!r}, got {values.get(key)!r}')
    return {'path': str(path), 'sha256': sha256(path.read_bytes()),
            'selection': {key: values[key] for key in expected}}


def disassembly(objdump, path):
    result = subprocess.check_output([objdump, '-d', str(path)], text=True)
    # Only the input filename is incidental; retain addresses and instruction bytes.
    return result.replace(str(path) + ':\tfile format', 'IMAGE:\tfile format', 1)


def compare(reference, current):
    rows, failures = [], []
    names = sorted(reference['sections'].keys() | current['sections'].keys())
    for name in names:
        old, new = reference['sections'].get(name), current['sections'].get(name)
        executable = bool(((old or {}).get('flags', 0) | (new or {}).get('flags', 0)) & 4)
        writable = bool(((old or {}).get('flags', 0) | (new or {}).get('flags', 0)) & 1)
        equal = old == new
        rows.append({'name': name, 'executable': executable, 'writable': writable,
                     'identical': equal, 'reference': old, 'current': new})
        if (executable or writable) and not equal:
            failures.append(f'{name}: {"executable" if executable else "writable"} section changed')
    if not any(row['executable'] and row['reference'] and row['reference']['size'] for row in rows):
        failures.append('no nonempty executable section')
    if reference['objects'] != current['objects']:
        failures.append('allocated object symbol names, addresses or sizes changed')
    if reference['latency_symbols'] or current['latency_symbols']:
        failures.append('latency symbols present')
    return rows, failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--current', type=Path, required=True)
    parser.add_argument('--reference-cache', type=Path, required=True)
    parser.add_argument('--current-cache', type=Path, required=True)
    parser.add_argument('--objdump', required=True)
    parser.add_argument('--output', type=Path, required=True, help='new evidence directory')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    reference, current = inspect(args.reference), inspect(args.current)
    configurations = [cache(args.reference_cache), cache(args.current_cache)]
    rows, failures = compare(reference, current)
    old, new = [disassembly(args.objdump, path) for path in (args.reference, args.current)]
    for name, content in [('reference', old), ('current', new)]:
        (args.output / (name + '.disassembly.txt')).write_text(content)
    difference = ''.join(difflib.unified_diff(old.splitlines(keepends=True), new.splitlines(keepends=True),
                                           fromfile='reference', tofile='current'))
    (args.output / 'disassembly.diff').write_text(difference)
    if difference:
        failures.append('disassembly changed')
    report = {'passed': not failures, 'failures': failures, 'configurations': configurations,
              'reference': reference, 'current': current, 'section_comparison': rows,
              'disassembly_identical': old == new,
              'whole_elf_identical': reference['sha256'] == current['sha256']}
    (args.output / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print(('FAIL' if failures else 'PASS') + ': production executable bytes, storage and latency symbol exclusion')
    for failure in failures:
        print(f'  {failure}')
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
