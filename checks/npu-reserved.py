#!/usr/bin/env python3
"""Run the native reserved-convolution preparation modes, without NPU commands."""
import argparse
import hashlib
import json
import os
import shlex
import shutil
import subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--resume', action='store_true', help='Resume a failed build before any mode ran')
    parser.add_argument('--freertos', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--lld', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    if args.resume:
        if not out.is_dir() or list(out.glob('mode-*.log')):
            raise RuntimeError('resume requires a failed build without executed modes')
    out.mkdir(parents=True, exist_ok=args.resume)
    project, build = out / 'project', out / 'build'
    project.mkdir(exist_ok=args.resume)
    fixtures = out / 'fixtures'
    fixtures.mkdir(exist_ok=args.resume)
    fixture = args.fixtures.resolve() / 'npu-convolution-data.h'
    if not args.resume:
        shutil.copy2(fixture, fixtures / fixture.name)
    # The production model authenticator also checks the compiled bytes.
    fixture_hash = sha(fixtures / fixture.name)
    linker = project / 'link.ld'
    text = (ROOT / 'platform/qemu-smp/link.ld').read_text()
    text = text.replace('    /DISCARD/', '''    .device_pool (NOLOAD) : ALIGN(4096) {
        __heap_end = .;
        __device_pool_start = .;
        . += 2097152;
        __device_pool_end = .;
    } > RAM
    ASSERT(__device_pool_end <= 0x41f00000, "native pool exceeds image boundary")
    /DISCARD/''')
    if not linker.exists() or linker.read_text() != text:
        linker.write_text(text)
    # Reuse the actual native preparation target's source dependencies and flags.
    # Only its test entry, pool sources, linker and MMIO-admission substitution differ.
    project_text = f'''cmake_minimum_required(VERSION 3.24)
project(npu_reservation_check LANGUAGES C ASM)
add_subdirectory("{ROOT}" sunstone)
get_target_property(sources npu-convolution-model SOURCES)
list(FILTER sources EXCLUDE REGEX "checks/npu-convolution.c$")
list(APPEND sources "{ROOT}/checks/npu-reserved.c"
  "{ROOT}/drivers/npu/npu_convolution.c" "{ROOT}/platform/a733/npu_reserved.c"
  "{ROOT}/runtime/device_memory_pool.c" "{ROOT}/platform/a733/memory_reservations.c"
  "{ROOT}/drivers/gpu/a733_gpu_resources.c")
set_property(TARGET npu-convolution-model PROPERTY SOURCES "${{sources}}")
get_target_property(options npu-convolution-model LINK_OPTIONS)
list(FILTER options EXCLUDE REGEX "--wrap=a733_npu_native_operations|--wrap=a733_npu_startup_prepared|-Wl,-T,")
list(APPEND options "-Wl,-T,{linker}")
set_property(TARGET npu-convolution-model PROPERTY LINK_OPTIONS "${{options}}")
set_property(TARGET npu-convolution-model PROPERTY LINK_DEPENDS "{linker}")
'''
    project_file = project / 'CMakeLists.txt'
    if not project_file.exists() or project_file.read_text() != project_text:
        project_file.write_text(project_text)
    environment = os.environ.copy()
    environment["PATH"] = str(args.lld.resolve().parent) + os.pathsep + environment.get("PATH", "")
    commands, results = [], {}
    def run(name, command, timeout=None):
        commands.append(command)
        r = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=timeout, env=environment)
        log = out / (name + '.log')
        suffix = 0
        while log.exists():
            suffix += 1
            log = out / (name + '-resume-' + str(suffix) + '.log')
        log.write_text(r.stdout)
        if r.returncode:
            raise RuntimeError(f'{name} failed ({r.returncode}): {r.stdout[-6000:]}')
        return r.stdout
    try:
        run('configure', ['cmake', '-S', str(project), '-B', str(build),
            '-DCMAKE_TOOLCHAIN_FILE=' + str(ROOT / 'cmake/aarch64-clang.cmake'),
            '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DFREERTOS_DIR=' + str(args.freertos.resolve()),
            '-DNPU_CONVOLUTION_FIXTURES=' + str(fixtures), '-DSUNSTONE_LLD=' + str(args.lld.absolute()),
            '-DCMAKE_EXE_LINKER_FLAGS=--ld-path=' + str(args.lld.absolute())])
        run('build', ['cmake', '--build', str(build), '--target', 'npu-convolution-model', '-j4'])
        binary = build / 'sunstone/npu-convolution-model.elf'
        shutil.copy2(binary, out / 'npu-reserved.elf')
        binary = out / 'npu-reserved.elf'
        with binary.open('rb') as stream:
            elf = ELFFile(stream)
            symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
            selector = symbols['npu_reserved_case']
            section = next(s for s in elf.iter_sections()
                           if s['sh_addr'] <= selector < s['sh_addr'] + s['sh_size'])
            selector_offset = section['sh_offset'] + selector - section['sh_addr']
        assert symbols['__device_pool_end'] - symbols['__device_pool_start'] == 2097152
        assert 0x41000000 <= symbols['__device_pool_start'] < symbols['__device_pool_end'] <= 0x41f00000
        dependencies = set()
        for path in build.rglob('*.obj.d'):
            data = path.read_text().replace('\\\n', ' ')
            dependencies.update(Path(p) for p in shlex.split(data.split(':', 1)[1]))
        dependencies.update([ROOT / 'checks/npu-reserved.py', ROOT / 'platform/a733/link.ld',
                             ROOT / 'cmake/npu-convolution.cmake', ROOT / 'CMakeLists.txt',
                             project / 'CMakeLists.txt', linker, fixtures / fixture.name,
                             build / 'compile_commands.json'])
        inputs = {}
        for index, source in enumerate(sorted(p.resolve() for p in dependencies)):
            target = out / 'inputs' / str(index) / source.name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            inputs[str(source)] = {'snapshot': str(target.relative_to(out)), 'sha256': sha(target)}
            assert sha(source) == inputs[str(source)]['sha256']
        (out / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
        (out / 'execution-input.json').write_text(json.dumps({'elf_sha256': sha(binary),
            'fixture_sha256': fixture_hash, 'case_symbol': symbols['npu_reserved_case'],
            'modes': [0, 1, 2], 'qemu_sha256': sha(args.qemu.resolve())}, indent=2) + '\n')
        for mode in (0, 1, 2):
            payload = bytearray(binary.read_bytes())
            payload[selector_offset:selector_offset + 4] = mode.to_bytes(4, 'little')
            selected = out / ('mode-' + str(mode) + '.elf')
            selected.write_bytes(payload)
            (out / ('mode-' + str(mode) + '-input.json')).write_text(json.dumps({
                'sha256': sha(selected), 'selector_file_offset': selector_offset,
                'selector': mode}, indent=2) + '\n')
            log = run('mode-' + str(mode), [str(args.qemu.resolve()),
                '-machine', 'virt,gic-version=3,secure=off,virtualization=off',
                '-cpu', 'cortex-a55', '-smp', '2', '-m', '128M', '-nographic', '-monitor', 'none',
                '-semihosting-config', 'enable=on,target=native', '-kernel', str(selected)], timeout=15)
            assert 'PASS reserved NPU:' in log, log
            results[str(mode)] = log.strip()
        assert all(sha(Path(p)) == entry['sha256'] for p, entry in inputs.items()), 'inputs changed'
        results.update(status='pass', elf_sha256=sha(binary), fixture_sha256=fixture_hash,
                       pool_start=symbols['__device_pool_start'], pool_end=symbols['__device_pool_end'])
    finally:
        (out / ('commands-resume.json' if args.resume else 'commands.json')).write_text(json.dumps(commands, indent=2) + '\n')
        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
