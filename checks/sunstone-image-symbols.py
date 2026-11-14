#!/usr/bin/env python3
"""Admit the public breadcrumb's lifetime word without exposing sealed inputs."""
import argparse
import copy
import importlib.util
import json
import shutil
from pathlib import Path
import struct
import subprocess
from harness import CLANG, environment, refused

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--image', '{elf:sup}', '--output', '{output}']]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('image_symbols', ROOT / 'tools/sunstone-image-link.py')
    link = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(link)
    source = args.output / 'crumb.c'
    source.write_text('#define SUNSTONE_CRUMBS 1\n#define SUNSTONE_A733_OS 1\n'
                      '#include "sunstone_crumb.h"\n'
                      'void progress(void){sunstone_crumb(SUNSTONE_CRUMB_RECOGNITION_END,0);}\n')
    obj = args.output / 'crumb.o'
    subprocess.run([str(CLANG), '--target=aarch64-none-elf', '-O2', '-ffreestanding',
                    '-I', str(ROOT / 'include'), '-c', str(source), '-o', str(obj)],
                   check=True, env=environment(), capture_output=True)
    elf = link.Elf(args.image.read_bytes())
    def check(image, object_path=obj):
        return link.application_symbols(image, [object_path], [], [], '/usr/bin/llvm-nm-21')
    assert not any(section[5] for name, section in zip(elf.names, elf.sections)
                   if name == 'sunstone_manifest'), 'fixture must be an OS image without an application'
    result = check(elf)
    assert result['region_initializers'] == 0
    cases = ['empty-os-without-application-manifest', 'public-header-lifetime-reference']
    # With no application, an initializer has no valid region index even when
    # the object inventory and immutable section bounds agree with its bytes.
    initializer = args.output / 'initializer.c'
    initializer.write_text('__attribute__((used,section("sunstone_region_initializers"),aligned(8)))\n'
                           'const struct {unsigned index,reserved;unsigned long size;} init={0,0,8};\n')
    initializer_obj = args.output / 'initializer.o'
    subprocess.run([str(CLANG), '--target=aarch64-none-elf', '-O2', '-ffreestanding',
                    '-c', str(initializer), '-o', str(initializer_obj)], check=True, capture_output=True)
    changed = copy.deepcopy(elf)
    section_index = changed.names.index('sunstone_region_initializers')
    section = list(changed.sections[section_index])
    section[4], section[5] = len(changed.data), 16
    changed.sections[section_index] = tuple(section)
    changed.data += struct.pack('<IIQ', 0, 0, 8)
    stop = next(s for s in changed.symbols if s['name'] == '__stop_sunstone_region_initializers')
    stop['value'] = section[3] + 16
    refused(lambda: check(changed, initializer_obj), 'region initializer index', kind=AssertionError)
    cases.append('initializer-without-manifest-refused')
    sealed = next(s['value'] for s in elf.symbols if s['name'] == 'os_sealed')
    for name, field, value in (
            ('session-word', 'value', sealed),
            ('ready-word', 'value', sealed + 4096),
            ('past-owner', 'value', sealed + 4104),
            ('inherited-object-size', 'size', 4104),
            ('function-alias', 'type', 2), ('absolute-alias', 'shndx', 0xfff1),
            ('weak-definition', 'bind', 2)):
        changed = copy.deepcopy(elf)
        alias = next(s for s in changed.symbols if s['name'] == 'os_crumb_lifetime')
        alias[field] = value
        refused(lambda: check(changed), 'breadcrumb lifetime', kind=AssertionError)
        cases.append(name)
    duplicate = copy.deepcopy(elf)
    duplicate.symbols.append(copy.deepcopy(next(s for s in elf.symbols if s['name'] == 'os_crumb_lifetime')))
    refused(lambda: check(duplicate), 'breadcrumb lifetime', kind=AssertionError)
    cases.append('duplicate-alias')
    private = args.output / 'private.c'
    private.write_text('extern unsigned os_sealed[];unsigned private_read(void){return os_sealed[0];}\n')
    private_obj = args.output / 'private.o'
    subprocess.run([str(CLANG), '--target=aarch64-none-elf', '-O2', '-ffreestanding',
                    '-c', str(private), '-o', str(private_obj)], check=True, capture_output=True)
    refused(lambda: check(elf, private_obj), 'private OS symbols: os_sealed', kind=AssertionError)
    cases.append('sealed-input-reference-refused')
    # The public calls are the kit's ss_ and gpu_queue_ declarations plus one named measurement call. A sunstone_
    # declaration in a kit header stays private: a copy of the tool reads a kit whose sunstone_app.h declares one.
    tree = args.output / 'kit-tree'
    (tree / 'tools').mkdir(parents=True)
    shutil.copytree(ROOT / 'include', tree / 'include')
    shutil.copyfile(ROOT / 'tools/sunstone-image-link.py', tree / 'tools/sunstone-image-link.py')
    with (tree / 'include/sunstone_app.h').open('a') as stream:
        stream.write('\nint sunstone_internal_probe(void);\n')
    spec = importlib.util.spec_from_file_location('image_symbols_kit', tree / 'tools/sunstone-image-link.py')
    kit_link = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(kit_link)
    def kit_check(name, call):
        source, output = args.output / (name + '.c'), args.output / (name + '.o')
        source.write_text(call)
        subprocess.run([str(CLANG), '--target=aarch64-none-elf', '-O2', '-ffreestanding',
                        '-c', str(source), '-o', str(output)], check=True, capture_output=True)
        return lambda: kit_link.application_symbols(elf, [output], [], [], '/usr/bin/llvm-nm-21')
    refused(kit_check('kit-private', 'int sunstone_internal_probe(void);int f(void){return sunstone_internal_probe();}\n'),
            'private OS symbols: sunstone_internal_probe', kind=AssertionError)
    cases.append('sunstone-prefixed-kit-declaration-refused')
    kit_check('kit-tlbi', 'struct s;int sunstone_tlbi_sample(struct s *);int f(void){return sunstone_tlbi_sample(0);}\n')()
    cases.append('named-measurement-call-admitted')
    (args.output / 'result.json').write_text(json.dumps({'passed': True, 'cases': cases}, indent=2) + '\n')
    print(f'PASS application breadcrumb symbol: {len(cases)} cases')


if __name__ == '__main__':
    main()
