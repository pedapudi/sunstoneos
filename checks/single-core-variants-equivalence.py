#!/usr/bin/env python3
"""Compare a boot-CPU-only image built from two source trees.

Use it when a build recipe, shared source or patch of a single-core image
changes in a way that should not change the generated code. Each side is a
configured and built CMake directory with its source tree. The check pairs the
target's object files by source path, with each tree's root written as SRC,
and requires equal contents and relocations for every allocated section.
--rename BEFORE=AFTER pairs a unit that moved, for example a patched copy in
the build directory that became a shared source. Every other object must exist
on both sides. The check then compares the linked images: the size of every
allocated section and of every named function and object, and, unless
--layout-may-differ is given, the contents of every allocated section with
the build ID bytes masked.
"""
import argparse
import importlib.util
import json
from pathlib import Path

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('harness', ROOT / 'checks/harness.py')
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)

SHF_ALLOC = 2


def objects(build, source, target):
    """Map each object's source path, with the tree root written as SRC, to its file."""
    directory = build / 'CMakeFiles' / f'{target}.dir'
    prefix = str(source.resolve()).lstrip('/') + '/'
    found = {}
    for path in directory.rglob('*.obj'):
        key = str(path.relative_to(directory))[:-len('.obj')]
        if key.startswith(prefix):
            key = 'SRC/' + key[len(prefix):]
        found[key] = path
    assert found, f'no objects for {target} in {build}'
    return found


def sections(path):
    """Allocated section bytes and relocations of one object file."""
    with open(path, 'rb') as stream:
        elf = ELFFile(stream)
        symbols = elf.get_section_by_name('.symtab')
        names = {}
        for index, section in enumerate(elf.iter_sections()):
            names[index] = section.name
        result = {}
        for section in elf.iter_sections():
            if section['sh_flags'] & SHF_ALLOC:
                data = b'' if section['sh_type'] == 'SHT_NOBITS' else section.data()
                result[section.name] = [section['sh_size'], data.hex(), []]
        for section in elf.iter_sections():
            if not isinstance(section, RelocationSection):
                continue
            applied = names[section['sh_info']]
            if applied not in result:
                continue
            for relocation in section.iter_relocations():
                symbol = symbols.get_symbol(relocation['r_info_sym'])
                name = symbol.name or names.get(symbol['st_shndx'], str(symbol['st_shndx']))
                result[applied][2].append((relocation['r_offset'], relocation['r_info_type'], name,
                                           relocation['r_addend'] if relocation.is_RELA() else 0))
        return result


def image(path):
    """Allocated section sizes and data, symbol sizes and build ID range."""
    with open(path, 'rb') as stream:
        elf = ELFFile(stream)
        symbols = {}
        build_id = None
        for symbol in elf.get_section_by_name('.symtab').iter_symbols():
            if symbol.name and symbol['st_info']['type'] in ('STT_FUNC', 'STT_OBJECT'):
                symbols.setdefault(symbol.name, set()).add(symbol['st_size'])
            if symbol.name == 'a733_smp_build_id':
                build_id = (symbol['st_value'], symbol['st_size'])
        assert build_id and build_id[1] == 32, build_id
        alloc = {}
        for section in elf.iter_sections():
            if section['sh_flags'] & SHF_ALLOC:
                data = b'' if section['sh_type'] == 'SHT_NOBITS' else bytearray(section.data())
                start = build_id[0] - section['sh_addr']
                if data and 0 <= start < len(data):
                    data[start:start + 32] = bytes(32)
                alloc[section.name] = (section['sh_addr'], section['sh_size'], bytes(data))
        return alloc, symbols


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True, help='built CMake directory of the reference tree')
    parser.add_argument('--before-source', type=Path, required=True, help='source root of the reference tree')
    parser.add_argument('--after', type=Path, required=True, help='built CMake directory of the changed tree')
    parser.add_argument('--after-source', type=Path, required=True, help='source root of the changed tree')
    parser.add_argument('--target', required=True, help='single-core target name')
    parser.add_argument('--rename', action='append', default=[], help='BEFORE=AFTER object source keys')
    parser.add_argument('--layout-may-differ', action='store_true',
                        help='link order changed; compare section and symbol sizes but not linked bytes')
    parser.add_argument('--output', type=Path, required=True, help='results.json to write')
    args = parser.parse_args()

    renames = dict(item.split('=', 1) for item in args.rename)
    before = {renames.get(k, k): v for k, v in objects(args.before, args.before_source, args.target).items()}
    after = objects(args.after, args.after_source, args.target)
    assert set(renames.values()) <= after.keys(), sorted(set(renames.values()) - after.keys())
    assert before.keys() == after.keys(), f'unpaired objects: {sorted(before.keys() ^ after.keys())}'
    compared = 0
    for name in sorted(before):
        if name.endswith('/build-id.c'):
            continue
        assert sections(before[name]) == sections(after[name]), f'object code differs: {name}'
        compared += 1

    image_before, symbols_before = image(args.before / f'{args.target}.elf')
    image_after, symbols_after = image(args.after / f'{args.target}.elf')
    assert {k: v[1] for k, v in image_before.items()} == {k: v[1] for k, v in image_after.items()}, \
        'allocated section sizes differ'
    assert symbols_before == symbols_after, \
        sorted(set(symbols_before.items()) ^ set((k, frozenset(v)) for k, v in symbols_after.items()))[:20]
    linked = 'sizes'
    if not args.layout_may_differ:
        assert image_before == image_after, 'linked allocated sections differ'
        linked = 'bytes'
    results = {'target': args.target, 'objects_compared': compared, 'renamed': renames,
               'linked_comparison': linked, 'sections': len(image_after), 'symbols': len(symbols_after),
               'passed': True}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    harness.save(args.output, results)
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
