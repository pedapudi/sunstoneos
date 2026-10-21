#!/usr/bin/env python3
"""Check retained render CLOSE cleanup with actual native ownership and queue receipts."""
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
    parser.add_argument('--cases',nargs='+',type=int,choices=range(8),default=list(range(8)))
    parser.add_argument('--build-only',action='store_true')
    parser.add_argument('--run-only',action='store_true')
    parser.add_argument('--resume', action='store_true', help='Resume a failed build before any mode ran')
    for name in ('freertos', 'tinyusb', 'firmware', 'qemu', 'lld'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    if args.resume and (not out.is_dir() or list(out.glob('mode-*.log'))):
        raise RuntimeError('resume requires a failed build without executed modes')
    out.mkdir(parents=True, exist_ok=args.resume or args.run_only)
    project, build, fixtures = out / 'project', out / 'build', out / 'fixtures'
    project.mkdir(exist_ok=args.resume or args.run_only)
    fixtures.mkdir(exist_ok=args.resume or args.run_only)
    if not args.resume and not args.run_only:
        shutil.copy2(args.firmware, fixtures / 'gpu.fw')
    assert sha(fixtures / 'gpu.fw') == '1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2'
    def write(path, text):
        if not path.exists() or path.read_text() != text:
            path.write_text(text)
    # Production reservation section text remains authoritative in this fixture.
    native_linker = (ROOT / 'platform/a733/link.ld').read_text()
    reservations = native_linker[native_linker.index('    /* An absent pool'):native_linker.index('    /DISCARD/')]
    linker = project / 'link.ld'
    write(linker, (ROOT / 'platform/qemu-smp/link.ld').read_text().replace('    /DISCARD/',
        '    __heap_end = .;\n' + reservations + '    /DISCARD/'))
    platform = project / 'platform.c'
    platform_text = (ROOT / 'platform/qemu-smp/platform.c').read_text()
    platform_text = platform_text.replace('#include "platform.h"', '#include "platform.h"\n#include "a733_memory_boot.h"')
    platform_text = platform_text.replace('uint64_t mair = 0xff00', 'uint64_t mair = 0x44ff00')
    platform_text = platform_text.replace('    // Code is read-only',
        '    configASSERT(a733_memory_boot_uncached(level2));\n    // Code is read-only')
    write(platform, platform_text)
    write(project / 'CMakeLists.txt', f'''cmake_minimum_required(VERSION 3.24)
project(gpu_render_check LANGUAGES C ASM)
add_subdirectory("{ROOT}" sunstone)
get_target_property(sources gpu-service-model SOURCES)
list(FILTER sources EXCLUDE REGEX "checks/pvr-gpu-service.c$|platform/qemu-smp/platform.c$")
list(APPEND sources "{platform}"
  "{ROOT}/checks/gpu-render-cleanup.c" "{ROOT}/platform/a733/gpu_memory.c"
  "{ROOT}/platform/a733/gpu_reserved.c" "{ROOT}/platform/a733/memory_uncached.c"
  "{ROOT}/runtime/device_memory_pool.c" "{ROOT}/platform/a733/memory_reservations.c"
  "{ROOT}/platform/a733/gpu_compute_reserved.c"
  "{ROOT}/platform/a733/gpu_graphics_memory.c" "{ROOT}/drivers/gpu/pvr_render_target.c"
  "{ROOT}/drivers/gpu/pvr_render_programs.c" "{ROOT}/drivers/gpu/pvr_render_context.c"
  "{ROOT}/drivers/gpu/pvr_render_texture.c" "{ROOT}/drivers/gpu/pvr_render_job.c" "{ROOT}/drivers/gpu/pvr_render_service.c")
set(normalized)
foreach(source IN LISTS sources)
  if(NOT IS_ABSOLUTE "${{source}}")
    set(source "{ROOT}/${{source}}")
  endif()
  list(APPEND normalized "${{source}}")
endforeach()
set(sources "${{normalized}}")
list(REMOVE_DUPLICATES sources)
set_property(TARGET gpu-service-model PROPERTY SOURCES "${{sources}}")
target_include_directories(gpu-service-model PRIVATE "{fixtures}")
get_target_property(options gpu-service-model LINK_OPTIONS)
list(FILTER options EXCLUDE REGEX "-Wl,-T,")
list(APPEND options "-Wl,-T,{linker}" -Wl,--defsym=__device_pool_size=2097152
  -Wl,--defsym=__uncached_pool_size=2097152)
set_property(TARGET gpu-service-model PROPERTY LINK_OPTIONS "${{options}}")
set_property(TARGET gpu-service-model PROPERTY LINK_DEPENDS "{linker}")
''')
    environment = os.environ.copy()
    environment['PATH'] = str(args.lld.resolve().parent) + os.pathsep + environment.get('PATH', '')
    commands, results = [], {}
    def run(name, command, timeout=None):
        commands.append(command)
        r = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=timeout, env=environment)
        log, suffix = out / (name + '.log'), 0
        while log.exists():
            suffix += 1
            log = out / (name + '-resume-' + str(suffix) + '.log')
        log.write_text(r.stdout)
        if r.returncode:
            raise RuntimeError(f'{name} failed ({r.returncode}): {r.stdout[-6000:]}')
        return r.stdout
    try:
        if not args.run_only: run('configure', ['cmake', '-S', str(project), '-B', str(build),
            '-DCMAKE_TOOLCHAIN_FILE=' + str(ROOT / 'cmake/aarch64-clang.cmake'),
            '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DFREERTOS_DIR=' + str(args.freertos.resolve()),
            '-DTINYUSB_DIR=' + str(args.tinyusb.resolve()),
            '-DSUNSTONE_GPU_FIRMWARE=' + str(fixtures / 'gpu.fw'),
            '-DSUNSTONE_LLD=' + str(args.lld.absolute()),
            '-DCMAKE_EXE_LINKER_FLAGS=--ld-path=' + str(args.lld.absolute())])
        if not args.run_only: run('build', ['cmake', '--build', str(build), '--target', 'gpu-service-model', '-j4'])
        binary = out / 'gpu-render-cleanup.elf'
        if not args.run_only: shutil.copy2(build / 'sunstone/gpu-service-model.elf', binary)
        with binary.open('rb') as stream:
            elf = ELFFile(stream)
            symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
            selector = symbols['gpu_render_case']
            section = next(s for s in elf.iter_sections() if s['sh_addr'] <= selector < s['sh_addr'] + s['sh_size'])
            selector_offset = section['sh_offset'] + selector - section['sh_addr']
        assert symbols['__device_pool_end'] - symbols['__device_pool_start'] == 2097152
        assert symbols['__device_pool_end'] <= symbols['__uncached_pool_start'] == 0x41c00000
        assert symbols['__uncached_pool_end'] == 0x41e00000
        dependencies = set()
        selected_commands = [entry for entry in json.loads((build / 'compile_commands.json').read_text())
                             if 'CMakeFiles/gpu-service-model.dir/' in entry.get('command', '')]
        assert selected_commands
        for entry in selected_commands:
            dependencies.add(Path(entry['file']))
        compiler = Path(shlex.split(selected_commands[0]['command'])[0]).resolve()
        toolchain = {'compiler': {'path': str(compiler), 'sha256': sha(compiler)},
                     'linker': {'path': str(args.lld.resolve()), 'sha256': sha(args.lld.resolve())},
                     'qemu': {'path': str(args.qemu.resolve()), 'sha256': sha(args.qemu.resolve())}}
        (out / 'toolchain.json').write_text(json.dumps(toolchain, indent=2) + '\n')
        dependencies.update(ROOT.glob('cmake/*.cmake'))
        dependencies.update(ROOT.glob('patches/freertos-smp-*.patch'))
        dependencies.update([ROOT / 'checks/npu-service-start.patch', ROOT / 'platform/qemu-smp/start.S'])
        dependencies.update(args.freertos / 'portable/GCC/ARM_CR82' / name
                            for name in ('port.c', 'portASM.S', 'portmacro.h'))
        (out / 'objects.json').write_text(json.dumps({str(p.relative_to(build)): sha(p)
            for p in sorted(build.rglob('gpu-service-model.dir/**/*.obj'))}, indent=2) + '\n')
        for path in build.rglob('*.obj.d'):
            data = path.read_text().replace('\\\n', ' ')
            dependencies.update(Path(p) for p in shlex.split(data.split(':', 1)[1]))
        dependencies.update([ROOT / 'checks/gpu-render-cleanup.py', ROOT / 'platform/a733/link.ld',
            ROOT / 'platform/qemu-smp/platform.c', ROOT / 'platform/qemu-smp/link.ld',
            ROOT / 'cmake/gpu.cmake', ROOT / 'CMakeLists.txt',
            project / 'CMakeLists.txt', linker, platform, fixtures / 'gpu.fw', build / 'compile_commands.json'])
        inputs = {}
        for index, source in enumerate(sorted(p.resolve() for p in dependencies)):
            target = out / 'inputs' / str(index) / source.name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            inputs[str(source)] = {'snapshot': str(target.relative_to(out)), 'sha256': sha(target)}
        (out / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
        (out / 'execution-input.json').write_text(json.dumps({'elf_sha256': sha(binary),
            'qemu_sha256': sha(args.qemu.resolve()), 'modes': args.cases}, indent=2) + '\n')
        if args.build_only:return
        if args.run_only and (out / 'results.json').exists():results.update(json.loads((out / 'results.json').read_text()))
        for mode in args.cases:
            payload = bytearray(binary.read_bytes())
            payload[selector_offset:selector_offset + 4] = mode.to_bytes(4, 'little')
            selected = out / ('mode-' + str(mode) + '.elf')
            selected.write_bytes(payload)
            log = run('mode-' + str(mode), [str(args.qemu.resolve()),
                '-machine', 'virt,gic-version=3,secure=off,virtualization=off',
                '-cpu', 'cortex-a55', '-smp', '2', '-m', '128M', '-nographic', '-monitor', 'none',
                '-semihosting-config', 'enable=on,target=native', '-kernel', str(selected)], timeout=15)
            assert 'PASS render cleanup case' in log, log
            results[str(mode)] = {'result': log.strip(), 'elf_sha256': sha(selected), 'selector_offset': selector_offset}
        assert all(sha(Path(p)) == v['sha256'] for p, v in inputs.items()), 'inputs changed'
        results.update(status='pass', elf_sha256=sha(binary), symbols={k: v for k, v in symbols.items()
                       if k.startswith('__device_pool') or k.startswith('__uncached_pool')})
    finally:
        (out / ('commands-run.json' if args.run_only else 'commands-resume.json' if args.resume else 'commands.json')).write_text(json.dumps(commands, indent=2) + '\n')
        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
