#!/usr/bin/env python3
"""Admit the public breadcrumb's lifetime word without exposing sealed inputs."""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
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
    check(elf)
    cases = ['public-header-lifetime-reference']
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
    (args.output / 'result.json').write_text(json.dumps({'passed': True, 'cases': cases}, indent=2) + '\n')
    print(f'PASS application breadcrumb symbol: {len(cases)} cases')


if __name__ == '__main__':
    main()
