#!/usr/bin/env python3
"""Build one native DMA pool composition and compile the cached application entry."""
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
    parser.add_argument('--build-only', action='store_true')
    for name in ('freertos', 'tinyusb', 'qemu', 'lld'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    project, build = out / 'project', out / 'build'
    project.mkdir()
    # The QEMU image reserves its pool after all image state and exception stacks.
    # Native cached linker/mapping behavior has separate reusable evidence.
    linker = project / 'link.ld'
    source = (ROOT / 'platform/qemu-smp/link.ld').read_text()
    source = source.replace('    /DISCARD/', '''    __heap_end = .;
    .device_pool (NOLOAD) : ALIGN(4096) {
        __device_pool_start = .; . += 2097152; __device_pool_end = .;
    } > RAM
    ASSERT(__device_pool_end <= 0x41f00000, "pool exceeds image reservation")
    /DISCARD/''')
    linker.write_text(source)
    (project / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.24)
project(dma_pool_check LANGUAGES C ASM)
add_subdirectory("{ROOT}" sunstone)
apply_build_patch("{build}/pool-port.c" "{build}/sunstone/freertos-smp/port.c"
  "{ROOT}/patches/freertos-smp-privileged.patch")
set_source_files_properties("{build}/pool-port.c" PROPERTIES
  COMPILE_DEFINITIONS "npu_service_task_privileged=a733_dma_pool_task_privileged")
apply_build_patch("{build}/start.S" "{ROOT}/platform/qemu-smp/start.S"
  "{ROOT}/checks/npu-service-start.patch")
get_target_property(sources dma-startup-model SOURCES)
# Anchor the repository-relative entries; generated sources such as portASM.S
# carry absolute build paths that may themselves contain "checks/".
list(FILTER sources EXCLUDE REGEX "^checks/|^platform/qemu-smp/start.S$|/dma-startup-port.c$")
list(APPEND sources arch/aarch64/device_memory.c runtime/device_memory_pool.c
  platform/a733/memory_reservations.c platform/a733/dma_native.c drivers/dma/dma_memory.c
  drivers/audio/a733_i2s_stream.c drivers/dma/dma_service_i2s_stream.c
  platform/a733/dma_pool_session.c checks/dma-pool-session.c
  "{build}/pool-port.c" "{build}/start.S")
list(REMOVE_DUPLICATES sources)
set(absolute_sources)
foreach(source IN LISTS sources)
  if(NOT IS_ABSOLUTE "${{source}}")
    set(source "{ROOT}/${{source}}")
  endif()
  list(APPEND absolute_sources "${{source}}")
endforeach()
add_executable(dma-pool-check ${{absolute_sources}})
target_include_directories(dma-pool-check PRIVATE "{ROOT}/platform/qemu-smp"
  "{ROOT}/include" "{ROOT}/runtime/include" "{args.freertos.resolve()}/include"
  "{build}/sunstone/freertos-smp")
target_compile_definitions(dma-pool-check PRIVATE INCLUDE_xTaskGetSchedulerState=1
  INCLUDE_xTaskGetCurrentTaskHandle=1 INCLUDE_xSemaphoreGetMutexHolder=1)
target_compile_options(dma-pool-check PRIVATE -march=armv8-a -mstrict-align
  -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables
  -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections
  -O2 -g -Wall -Wextra -Werror -fstack-usage)
target_link_options(dma-pool-check PRIVATE -nostdlib -static -Wl,--gc-sections
  -Wl,--wrap=a733_dma_platform_native_read -Wl,--wrap=a733_dma_platform_native_write
  -Wl,--wrap=a733_dma_native_operations "-Wl,-T,{linker}"
  "-Wl,-Map,{build}/dma-pool-check.map")
set_target_properties(dma-pool-check PROPERTIES SUFFIX ".elf" LINK_DEPENDS "{linker}")
# Compile actual production entry and constructor with the eight-core ABI.
# This object target deliberately retains the real handoff/boot input symbols.
add_library(dma-copy-cached OBJECT "{ROOT}/apps/dma-copy/main.c"
  "{ROOT}/platform/a733/dma_pool_session.c")
target_include_directories(dma-copy-cached PRIVATE "{ROOT}/platform/a733-smp-cached"
  "{ROOT}/include" "{ROOT}/runtime/include" "{args.freertos.resolve()}/include"
  "{build}/sunstone/freertos-a733-smp-cached")
target_compile_definitions(dma-copy-cached PRIVATE SUNSTONE_SECURE_MONITOR=1
  INCLUDE_xTaskGetSchedulerState=1 INCLUDE_xTaskGetCurrentTaskHandle=1 INCLUDE_xSemaphoreGetMutexHolder=1)
get_target_property(options dma-pool-check COMPILE_OPTIONS)
target_compile_options(dma-copy-cached PRIVATE ${{options}} -mgeneral-regs-only)
''')
    environment = os.environ.copy()
    environment['PATH'] = str(args.lld.resolve().parent) + os.pathsep + environment.get('PATH', '')
    commands = []

    def run(name, command, timeout=None):
        commands.append(command)
        try:
            result = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=timeout, env=environment)
        except subprocess.TimeoutExpired as error:
            output = error.stdout or b''
            (out / (name + '.log')).write_text(output.decode(errors='replace') if isinstance(output, bytes) else output)
            raise
        (out / (name + '.log')).write_text(result.stdout)
        if result.returncode:
            raise RuntimeError(f'{name} failed ({result.returncode}): {result.stdout[-6000:]}')
        return result.stdout

    try:
        run('configure', ['cmake', '-S', str(project), '-B', str(build),
            '-DCMAKE_TOOLCHAIN_FILE=' + str(ROOT / 'cmake/aarch64-clang.cmake'),
            '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DFREERTOS_DIR=' + str(args.freertos.resolve()),
            '-DTINYUSB_DIR=' + str(args.tinyusb.resolve()), '-DSUNSTONE_LLD=' + str(args.lld.absolute()),
            '-DCMAKE_EXE_LINKER_FLAGS=--ld-path=' + str(args.lld.absolute())])
        run('build', ['cmake', '--build', str(build), '--target', 'dma-pool-check', 'dma-copy-cached', '-j4'])
        binary = out / 'dma-pool-session.elf'
        shutil.copy2(build / 'dma-pool-check.elf', binary)
        with binary.open('rb') as stream:
            elf = ELFFile(stream)
            symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
            layout = {name: symbols[name] for name in ('__bss_end', '__heap_end',
                '__device_pool_start', '__device_pool_end')}
        assert layout['__bss_end'] <= layout['__heap_end'] <= layout['__device_pool_start']
        assert layout['__device_pool_end'] - layout['__device_pool_start'] == 2097152
        assert layout['__device_pool_end'] <= 0x41f00000
        (out / 'layout.json').write_text(json.dumps(layout, indent=2) + '\n')
        dependencies = set()
        for path in build.rglob('*.obj.d'):
            data = path.read_text().replace('\\\n', ' ')
            dependencies.update(Path(p) for p in shlex.split(data.split(':', 1)[1]))
        dependencies.update([ROOT / 'checks/dma-pool-session.py', ROOT / 'platform/qemu-smp/link.ld',
            ROOT / 'cmake/dma-startup.cmake', ROOT / 'cmake/smp.cmake', ROOT / 'cmake/a733-smp-cached.cmake',
            ROOT / 'patches/freertos-smp-privileged.patch', ROOT / 'checks/npu-service-start.patch',
            ROOT / 'CMakeLists.txt', project / 'CMakeLists.txt', linker, build / 'compile_commands.json'])
        inputs = {}
        for index, source in enumerate(sorted(p.resolve() for p in dependencies)):
            target = out / 'inputs' / str(index) / source.name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            inputs[str(source)] = {'snapshot': str(target.relative_to(out)), 'sha256': sha(target)}
        (out / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
        artifacts = {str(binary.relative_to(out)): sha(binary)}
        for path in (build / 'CMakeFiles/dma-copy-cached.dir').rglob('*.obj'):
            target = out / 'cached-objects' / path.relative_to(build / 'CMakeFiles/dma-copy-cached.dir')
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
            artifacts[str(target.relative_to(out))] = sha(target)
            if path.name == 'main.c.obj':
                with path.open('rb') as stream:
                    elf = ELFFile(stream)
                    undefined = {s.name for s in elf.get_section_by_name('.symtab').iter_symbols()
                                 if s['st_shndx'] == 'SHN_UNDEF'}
                required = {'a733_dma_copy_handoff', 'a733_smp_device_preparation_admitted',
                            'a733_dma_pool_session_init'}
                assert required <= undefined
                (out / 'cached-entry-inputs.json').write_text(json.dumps(sorted(required), indent=2) + '\n')
        (out / 'artifacts.json').write_text(json.dumps(artifacts, indent=2) + '\n')
        (out / 'execution-input.json').write_text(json.dumps({'elf_sha256': sha(binary),
            'qemu_sha256': sha(args.qemu.resolve()), 'lld_sha256': sha(args.lld.resolve()),
            'clang_sha256': sha(Path(shutil.which('clang', path=environment['PATH'])).resolve())}, indent=2) + '\n')
        if not args.build_only:
            output = run('execute', [str(args.qemu.resolve()), '-machine', 'virt,gic-version=3',
                '-cpu', 'cortex-a53', '-smp', '2', '-m', '128M', '-nographic', '-monitor', 'none',
                '-semihosting-config', 'enable=on,target=native', '-kernel', str(binary)], timeout=30)
            if 'PASS DMA pool session:' not in output:
                raise RuntimeError('missing selected composition result')
            (out / 'result.json').write_text(json.dumps({'passed': True, 'cores': 2,
                'parent_pages': 6, 'neighbor_pages': 1,
                'limits': 'Actual native AT/cache/counter and FreeRTOS logic; modeled A733 registers and DMA movement. No physical cache coherence, peripheral drain, handoff or watchdog qualification. Production eight-core entry compiled only.'}, indent=2) + '\n')
    finally:
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')


if __name__ == '__main__':
    main()
