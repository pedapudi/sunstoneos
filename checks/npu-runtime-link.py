#!/usr/bin/env python3
"""Check the real offline tensor-link routine and assembler on admitted models.

The production C routine and its private helpers are compiled unchanged for
the host. Platform scheduling and cache publication are modeled. CRC builtins
use a portable CRC32C implementation; no accelerator or timing is modeled.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def function(source, name):
    match = re.search(r'^.*\b' + re.escape(name) + r'\(', source, re.M)
    assert match, name
    start = source.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--catalogue', type=Path, required=True, help='Catalogue containing both pinned models')
    p.add_argument('--producer', type=Path, required=True)
    p.add_argument('--consumer', type=Path, required=True)
    p.add_argument('--freertos', type=Path, default=CACHE / 'sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2')
    a = p.parse_args()
    output = a.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / 'tmp').mkdir()
    inputs = [ROOT / 'checks/npu-runtime-link.c', Path(__file__).resolve(), a.catalogue, a.producer, a.consumer,
              ROOT / 'drivers/npu/a733_npu_runtime.c', ROOT / 'drivers/npu/a733_npu_runtime_interim.c',
              ROOT / 'tools/npu-session-catalogue.py']
    catalogue = json.loads(a.catalogue.read_text())['models']
    entries = [next(m for m in catalogue if m['model']['sha256'] == sha(path) and m['model']['bytes'] == path.stat().st_size)
               for path in (a.producer, a.consumer)]
    spec = importlib.util.spec_from_file_location('link_catalogue', ROOT / 'tools/npu-session-catalogue.py')
    catalogue_tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(catalogue_tool)
    table = output / 'admitted.c'
    table.write_text(catalogue_tool.table(entries))
    runtime = (ROOT / 'drivers/npu/a733_npu_runtime.c').read_text()
    names = ('fault', 'good', 'state', 'admit', 'pages', 'whole_groups', 'need', 'place', 'sized',
             'npu_runtime_requirements', 'fill_end', 'maintain', 'same_plan', 'npu_runtime_link_tensor')
    routines = output / 'link-routines.c'
    crc = (ROOT / 'drivers/npu/a733_npu_runtime_interim.c').read_text()
    crc = crc.replace('__attribute__((target("crc")))', '').replace('__builtin_arm_crc32cb', 'fixture_crc_byte').replace('__builtin_arm_crc32cd', 'fixture_crc_word')
    end_pair = re.search(r'^#define END_PAIR .*$', runtime, re.M)[0]
    page = re.search(r'^#define PAGE .*$', runtime, re.M)[0]
    structures = ''.join(re.search(r'struct ' + name + r' \{.*?\};', runtime, re.S)[0] + '\n'
                         for name in ('sizes', 'placement'))
    routines.write_text(end_pair + '\n' + page + '\n' + structures + ''.join(function(runtime, n) for n in names) + crc)
    units = ['checks/npu-runtime-link.c', 'drivers/npu/nbg_model.c', 'drivers/npu/nbg_assembly.c',
             'drivers/npu/nbg_relocation.c', 'drivers/npu/nbg_checksum.c', 'drivers/npu/nbg_sh_nt.c',
             'drivers/npu/a733_npu_mmu.c', 'drivers/storage/storage_digest.c']
    inputs += [ROOT / name for name in units]
    executable = output / 'link-check'
    command = ['/usr/bin/clang-21', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-ffunction-sections', '-fdata-sections',
               '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'apps/npu-convolution-single-core'),
               '-I' + str(a.freertos / 'include'), '-I' + str(a.freertos / 'portable/GCC/ARM_AARCH64_SRE'),
               '-DSUNSTONE_LINK_ROUTINES="' + str(routines) + '"',
               *[str(ROOT / name) for name in units], str(table), '-Wl,--gc-sections', '-o', str(executable)]
    env = dict(os.environ, TMPDIR=str(output / 'tmp'))
    built = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True)
    (output / 'build.log').write_text(built.stdout + built.stderr)
    (output / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
    built.check_returncode()
    dependency_commands = []
    common = command[:command.index(str(ROOT / units[0]))]
    for index, unit in enumerate([*[ROOT / name for name in units], table]):
        dependency_command = [*common, '-MM', str(unit)]
        dependency_commands.append(dependency_command)
        dependency = subprocess.run(dependency_command, cwd=ROOT, env=env, capture_output=True, text=True, check=True).stdout
        (output / f'dependency-{index}.d').write_text(dependency)
        inputs += [Path(name) for name in shlex.split(dependency.replace('\\\n', ' ').split(':', 1)[1])]
    rows = []
    for case, label in enumerate(('linked-relocation-and-crc', 'scale-refusal', 'shape-refusal',
                                  'allocation-refusal', 'bound-refusal', 'overlapping-placement-refusal')):
        result = subprocess.run([str(executable), str(a.producer), str(a.consumer), str(case)], env=env,
                                capture_output=True, text=True)
        (output / (label + '.log')).write_text(result.stdout + result.stderr)
        assert result.returncode == 0, (label, result.stdout + result.stderr)
        rows.append({'case': label, 'passed': True})
    record = {'passed': True, 'physical_access': False, 'cases': rows,
              'scope': 'actual admitted model decoding, assembler begin/next, production tensor link and integrity checks; modeled scheduler/cache callbacks; portable CRC32C instructions',
              'inputs': {str(path.resolve()): sha(path) for path in sorted(set(inputs))},
              'tools': {str(path.resolve()): sha(path) for path in (Path(command[0]), Path(sys.executable))},
              'dependency_commands': dependency_commands,
              'generated': {path.name: sha(path) for path in (table, routines, executable)}}
    (output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print('PASS', len(rows), 'actual tensor-link/assembler and refusal cases')


if __name__ == '__main__':
    main()
