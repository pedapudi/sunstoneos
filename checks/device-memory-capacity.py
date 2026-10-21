#!/usr/bin/env python3
"""Check large-pool placement, actual native mappings and bounded pool publication."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--reuse-production', type=Path)
    for name in ('clang', 'ld', 'qemu', 'freertos'):
        p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    objects = out / 'objects'
    objects.mkdir()
    commands = []
    dependencies = {ROOT / 'checks/device-memory-capacity.py',
                    ROOT / 'platform/a733-smp-cached/link-devices.ld'}
    environment = os.environ.copy()
    environment['PATH'] = str(a.clang.resolve().parent) + os.pathsep + environment.get('PATH', '')
    qemu_lib = a.qemu.resolve().parent.parent / 'lib/x86_64-linux-gnu'
    if qemu_lib.exists():
        environment['LD_LIBRARY_PATH'] = str(qemu_lib) + ':' + environment.get('LD_LIBRARY_PATH', '')

    def run(name, command, timeout=60):
        commands.append(command)
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        result = subprocess.run(command, cwd=ROOT, env=environment, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
        (out / (name + '.log')).write_text(result.stdout)
        if result.returncode:
            raise RuntimeError(f'{name} failed: {result.stdout[-4000:]}')

    def compile_source(source, name, host=False, extended=True, extra=()):
        source = ROOT / source
        obj = objects / (name + '.o')
        cmd = [str(a.clang.resolve()), '-O2', '-g', '-Wall', '-Wextra', '-Werror',
               '-I' + str(ROOT / 'platform/a733-smp-cached'), '-I' + str(ROOT / 'include')]
        if source.suffix == '.c':
            cmd += ['-std=c11']
        if host:
            cmd += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        else:
            cmd += ['--target=aarch64-none-elf', '-march=armv8-a', '-mstrict-align',
                    '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables',
                    '-fno-asynchronous-unwind-tables', '-ffunction-sections', '-fdata-sections',
                    '-I' + str(ROOT / 'runtime/include'), '-I' + str(a.freertos.resolve() / 'include'),
                    '-I' + str(a.port.resolve()), '-DSUNSTONE_SECURE_MONITOR=1',
                    '-DSUNSTONE_A733_SMP_DIAGNOSTIC=1']
            if source.suffix == '.c':
                cmd += ['-mgeneral-regs-only']
        if extended:
            cmd += ['-DSUNSTONE_A733_EXTENDED_DEVICE_MEMORY=1']
        cmd += list(extra) + ['-MMD', '-MF', str(obj.with_suffix('.d')), '-c', str(source), '-o', str(obj)]
        run('compile-' + name, cmd)
        dependencies.add(source)
        data = obj.with_suffix('.d').read_text().replace('\\\n', ' ')
        dependencies.update(Path(x).resolve() for x in shlex.split(data.split(':', 1)[1]))
        return obj

    # Build the actual large image once. Reuse its native objects in the
    # architectural fixture; only the fixture entry/control units differ.
    production = a.reuse_production.resolve() if a.reuse_production else out / 'production'
    if a.reuse_production:
        saved = json.loads((production / 'a733-smp-cached-devices/manifest.json').read_text())
        if saved['compiler_sha256'] != digest(a.clang.resolve()) or saved['linker_sha256'] != digest(a.ld.resolve()):
            raise RuntimeError('reused production toolchain differs')
        if digest(production / 'sunstone-a733-smp-cached-devices.elf') != saved['final_elf']:
            raise RuntimeError('reused production ELF identity differs')
        for name, expected in saved['inputs'].items():
            if digest(Path(name)) != expected:
                raise RuntimeError('reused production input differs: ' + name)
    else:
        run('configure-production', ['cmake', '-S', str(ROOT / 'platform/a733-smp-cached-devices'),
            '-B', str(production), '-DCMAKE_TOOLCHAIN_FILE=' + str(ROOT / 'cmake/aarch64-clang.cmake'),
            '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DCMAKE_TRY_COMPILE_PLATFORM_VARIABLES=SUNSTONE_LLD', '-DFREERTOS_DIR=' + str(a.freertos.resolve()),
            '-DSUNSTONE_LLD=' + str(a.ld.resolve()), '-DCMAKE_C_COMPILER=' + str(a.clang.resolve()),
            '-DCMAKE_ASM_COMPILER=' + str(a.clang.resolve())])
    configured_compilers = {}
    for language in ('C', 'ASM'):
        records = list((production / 'CMakeFiles').glob('*/CMake' + language + 'Compiler.cmake'))
        if len(records) != 1:
            raise RuntimeError('missing or ambiguous configured compiler record')
        text = records[0].read_text()
        match = re.search(r'set\(CMAKE_' + language + r'_COMPILER "([^"\n]+)"\)', text)
        if not match or Path(match[1]).resolve() != a.clang.resolve():
            raise RuntimeError('production compiler differs from requested fixture compiler')
        actual = Path(match[1]).resolve()
        if digest(actual) != digest(a.clang.resolve()):
            raise RuntimeError('production compiler identity changed')
        configured_compilers[language] = {'path': str(actual), 'sha256': digest(actual)}
        dependencies.add(records[0])
    (out / 'configured-compilers.json').write_text(json.dumps(configured_compilers, indent=2) + '\n')
    if not a.reuse_production:
        run('build-production', ['cmake', '--build', str(production), '--target',
            'sunstone-a733-smp-cached-devices', '-j4'])
    a.port = production / 'freertos-a733-smp-cached'
    produced = {}
    for entry in json.loads((production / 'compile_commands.json').read_text()):
        command = shlex.split(entry['command'])
        obj = Path(entry['directory']) / command[command.index('-o') + 1]
        if 'sunstone-a733-smp-cached-devices.dir' not in str(obj):
            continue
        produced[Path(entry['file']).resolve()] = obj
        dep = Path(str(obj) + '.d')
        if dep.exists():
            data = dep.read_text().replace('\\\n', ' ')
            dependencies.update(Path(x).resolve() for x in shlex.split(data.split(':', 1)[1]))
        elif Path(entry['file']).suffix != '.S':
            raise RuntimeError('missing production C dependency file: ' + str(dep))
    production_manifest = production / 'a733-smp-cached-devices/manifest.json'
    dependencies.update([production_manifest, production / 'compile_commands.json',
        ROOT / 'platform/a733-smp-cached-devices/CMakeLists.txt',
        ROOT / 'cmake/a733-smp-cached-devices.cmake'])
    dependencies.update(Path(x) for x in json.loads(production_manifest.read_text())['inputs'])
    # The host links only pure layout predicates; admission still takes its real v2 branch.
    host = [compile_source('checks/device-memory-capacity.c', 'host-check', True, False, ['-DCAPACITY_HOST=1']),
            compile_source('runtime/device_memory_pool.c', 'host-pool', True, False),
            compile_source('platform/a733/device_memory_layout.c', 'host-layout', True, False),
            compile_source('platform/a733-smp-cached/admission.c', 'host-admission', True)]
    executable = out / 'capacity-host'
    run('link-host', [str(a.clang.resolve()), '-fsanitize=address,undefined', *map(str, host), '-o', str(executable)])
    run('host', [str(executable)])
    native_sources = ['checks/device-memory-capacity.c', 'checks/device-memory-capacity.S',
                      'runtime/device_memory_pool.c', 'runtime/memory.c',
                      'platform/a733/device_memory_layout.c', 'platform/a733/memory_reservations.c',
                      'platform/a733/memory_uncached.c', 'platform/a733-smp-cached/memory.c',
                      'platform/a733-smp-cached/admission.c', 'platform/a733-smp-cached/platform.c',
                      'platform/a733/cpu_start.c', 'platform/a733/cpu_coherence.c',
                      'platform/a733/cpu_coherence.S', 'arch/aarch64/device_memory.c']
    native = [produced[(ROOT / source).resolve()] if (ROOT / source).resolve() in produced else
              compile_source(source, 'native-' + str(i)) for i, source in enumerate(native_sources)]
    elf_path = out / 'device-memory-capacity.elf'
    run('link-native', [str(a.ld.resolve()), '-static', '--gc-sections', '-T',
        str(ROOT / 'platform/a733-smp-cached/link-devices.ld'), '-Map=' + str(out / 'native.map'),
        *map(str, native), '-o', str(elf_path)])
    # Compile real startup callers and direct/transitive reservation consumers once.
    closure = ['platform/a733/gpu_reserved.c',
               'platform/a733/gpu_compute_reserved.c', 'platform/a733/npu_reserved.c',
               'drivers/crypto/a733_ce_native.c', 'drivers/crypto/a733_ce_session.c',
               'drivers/crypto/a733_ce_entropy.c', 'drivers/crypto/a733_ce_hash.c',
               'drivers/crypto/a733_ce_cipher.c']
    for i, source in enumerate(closure):
        compile_source(source, 'consumer-' + str(i))
    # Changed conditional native files also compile with the ordinary profile.
    for i, source in enumerate(['platform/a733/memory_reservations.c', 'platform/a733/memory_uncached.c',
            'platform/a733-smp-cached/memory.c', 'platform/a733-smp-cached/admission.c',
            'platform/a733-smp-cached/boot.c', 'platform/a733-smp-cached/continue.c']):
        compile_source(source, 'compact-' + str(i), extended=False)
    with elf_path.open('rb') as f:
        elf = ELFFile(f)
        symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
        expected = {'__device_memory_profile': 1, '__device_pool_start': 0x48000000,
                    '__device_pool_end': 0x78000000, '__uncached_pool_start': 0x78000000,
                    '__uncached_pool_end': 0x79000000}
        assert all(symbols[k] == v for k, v in expected.items())
        assert symbols['__bss_end'] < 0x42000000 and symbols['__tables_end'] <= 0x42200000
        assert symbols['__tables_end'] - symbols['__tables_start'] == 5 * 4096
        assert elf.get_section_by_name('.device_pool')['sh_type'] == 'SHT_NOBITS'
        assert elf.get_section_by_name('.uncached_pool')['sh_type'] == 'SHT_NOBITS'
        segments = []
        for i, seg in enumerate(elf.iter_segments()):
            if seg['p_type'] != 'PT_LOAD' or not seg['p_filesz']:
                continue
            assert 0x41000000 <= seg['p_paddr'] < seg['p_paddr'] + seg['p_filesz'] <= 0x42000000
            path = out / ('segment-' + str(i) + '.bin')
            path.write_bytes(seg.data())
            segments.append((path, int(seg['p_paddr'])))
        (out / 'layout.json').write_text(json.dumps({k: symbols[k] for k in expected}, indent=2) + '\n')
    snapshots = {}
    for i, path in enumerate(sorted(dependencies)):
        target = out / 'inputs' / str(i) / path.name
        target.parent.mkdir(parents=True)
        shutil.copy2(path, target)
        snapshots[str(path)] = {'sha256': digest(target), 'snapshot': str(target.relative_to(out))}
    (out / 'inputs.json').write_text(json.dumps(snapshots, indent=2) + '\n')
    rom = out / 'reset-rom.bin'
    rom.write_bytes(struct.pack('<IIQ', 0x58000040, 0xd61f0000, 0x41000000))
    # Three distinct transitions: full publication, an interior mapping fault,
    # and immutable-deadline refusal after one batch. No peripheral workloads.
    for mode, label in enumerate(('publication', 'interior-fault', 'deadline-binding')):
        loaders = []
        controls = 0
        for original, address in segments:
            data = bytearray(original.read_bytes())
            path = original
            if address <= symbols['fixture_mode'] < address + len(data):
                offset = symbols['fixture_mode'] - address
                assert offset + 4 <= len(data)
                struct.pack_into('<I', data, offset, mode)
                path = out / (label + '-' + original.name)
                path.write_bytes(data)
                controls += 1
            loaders += ['-device', f"loader,file={path},addr={address},force-raw=on"]
        assert controls == 1
        run('native-' + label, [str(a.qemu.resolve()), '-machine', 'virt,secure=on,virtualization=on',
            '-cpu', 'cortex-a53', '-smp', '8', '-m', '1024M', '-nographic', '-monitor', 'none',
            '-serial', 'none', '-semihosting-config', 'enable=on,target=native', '-bios', str(rom),
            *loaders])
    (out / 'result.json').write_text(json.dumps({'passed': True,
        'elf_sha256': digest(elf_path), 'host_sha256': digest(executable),
        'qemu_sha256': digest(a.qemu.resolve()), 'clang_sha256': digest(a.clang.resolve()),
        'ld_sha256': digest(a.ld.resolve()), 'native_cases': 3, 'cores': 8,
        'loaded_segments': {p.name: digest(p) for p in out.glob('*segment-*.bin')},
        'rom_sha256': digest(rom),
        'production_elf': str(production / 'sunstone-a733-smp-cached-devices.elf'),
        'production_manifest': str(production_manifest), 'physical_memory_qualified': False}, indent=2) + '\n')


if __name__ == '__main__':
    main()
