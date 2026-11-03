#!/usr/bin/env python3
"""Run adopted DMA return through real pool, startup and FreeRTOS tasks."""
import argparse
import hashlib
import json
import os
import shlex
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
CASES = ('normal', 'changed-route', 'registry-busy', 'registry-transient')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=HERE.parent)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--resume', action='store_true')
    for name in ('freertos', 'tinyusb', 'qemu', 'lld'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    root, out = args.source.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=args.resume)
    project, build = out / 'project', out / 'build'
    project.mkdir(exist_ok=args.resume)
    linker = project / 'link.ld'
    linker.write_text((root / 'platform/qemu-smp/link.ld').read_text().replace(
        '    /DISCARD/', '''    __heap_end = .;
    .device_pool (NOLOAD) : ALIGN(4096) {
        __device_pool_start = .; . += 2097152; __device_pool_end = .;
    } > RAM
    ASSERT(__device_pool_end <= 0x41f00000, "pool exceeds image reservation")
    /DISCARD/'''))
    (project / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.24)
project(dma_pool_return LANGUAGES C ASM)
add_subdirectory("{root}" sunstone)
apply_build_patch("{build}/pool-port.c" "{build}/sunstone/freertos-smp/port.c"
  "{root}/patches/freertos-smp-privileged.patch")
set_source_files_properties("{build}/pool-port.c" PROPERTIES
  COMPILE_DEFINITIONS "npu_service_task_privileged=a733_dma_pool_task_privileged")
apply_build_patch("{build}/start.S" "{root}/platform/qemu-smp/start.S"
  "{root}/checks/npu-service-start.patch")
get_target_property(sources dma-startup-model SOURCES)
list(FILTER sources EXCLUDE REGEX "^checks/|^platform/qemu-smp/start.S$|/dma-startup-port.c$")
list(APPEND sources arch/aarch64/device_memory.c runtime/device_memory_pool.c
  platform/a733/memory_reservations.c platform/a733/dma_native.c drivers/dma/dma_memory.c
  drivers/audio/a733_i2s_stream.c drivers/dma/dma_service_i2s_stream.c
  platform/a733/dma_pool_session.c "{HERE}/dma-pool-return.c"
  "{build}/pool-port.c" "{build}/start.S")
list(REMOVE_DUPLICATES sources)
set(absolute_sources)
foreach(source IN LISTS sources)
  if(NOT IS_ABSOLUTE "${{source}}")
    set(source "{root}/${{source}}")
  endif()
  list(APPEND absolute_sources "${{source}}")
endforeach()
add_executable(dma-pool-return ${{absolute_sources}})
target_include_directories(dma-pool-return PRIVATE "{root}/platform/qemu-smp"
  "{root}/include" "{root}/runtime/include" "{args.freertos.resolve()}/include"
  "{build}/sunstone/freertos-smp")
target_compile_definitions(dma-pool-return PRIVATE INCLUDE_xTaskGetSchedulerState=1
  INCLUDE_xTaskGetCurrentTaskHandle=1 INCLUDE_xSemaphoreGetMutexHolder=1)
set_source_files_properties("{HERE}/dma-pool-return.c" PROPERTIES
  COMPILE_DEFINITIONS "DMA_RETURN_CASE=${{DMA_RETURN_CASE}}")
target_compile_options(dma-pool-return PRIVATE -march=armv8-a -mstrict-align
  -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables
  -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections
  -O2 -g -Wall -Wextra -Werror -fstack-usage)
target_link_options(dma-pool-return PRIVATE -nostdlib -static -Wl,--gc-sections
  -Wl,--wrap=a733_dma_platform_native_read -Wl,--wrap=a733_dma_platform_native_write
  -Wl,--wrap=a733_dma_native_operations -Wl,--wrap=a733_dma_startup_retire_adopted
  "-Wl,-T,{linker}" "-Wl,-Map,{build}/dma-pool-return.map")
set_target_properties(dma-pool-return PROPERTIES SUFFIX ".elf" LINK_DEPENDS "{linker}")
''')
    environment = os.environ.copy()
    environment['PATH'] = str(args.lld.absolute().parent) + os.pathsep + environment.get('PATH', '')
    commands = json.loads((out / 'commands.json').read_text()) if args.resume else []
    results = json.loads((out / 'result.json').read_text())['cases'] if args.resume and (out / 'result.json').exists() else {}

    def run(name, command, timeout=120):
        commands.append(command)
        try:
            result = subprocess.run(command, cwd=root, text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=timeout, env=environment)
        except subprocess.TimeoutExpired as error:
            data = error.stdout or b''
            (out / (name + '.log')).write_text(data.decode(errors='replace') if isinstance(data, bytes) else data)
            raise
        (out / (name + '.log')).write_text(result.stdout)
        if result.returncode:
            raise RuntimeError(f'{name} failed ({result.returncode}): {result.stdout[-5000:]}')
        return result.stdout

    try:
        for number, case in enumerate(CASES):
            if case in results:
                continue
            run(case + '-configure', ['cmake', '-S', str(project), '-B', str(build),
                '-DCMAKE_TOOLCHAIN_FILE=' + str(root / 'cmake/aarch64-clang.cmake'),
                '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DFREERTOS_DIR=' + str(args.freertos.resolve()),
                '-DTINYUSB_DIR=' + str(args.tinyusb.resolve()), '-DSUNSTONE_LLD=' + str(args.lld.absolute()),
                '-DCMAKE_EXE_LINKER_FLAGS=--ld-path=' + str(args.lld.absolute()),
                '-DDMA_RETURN_CASE=' + str(number)])
            run(case + '-build', ['cmake', '--build', str(build), '--target', 'dma-pool-return', '-j4'])
            binary = out / (case + '.elf')
            shutil.copy2(build / 'dma-pool-return.elf', binary)
            output = run(case + '-execute', [str(args.qemu.resolve()), '-machine', 'virt,gic-version=3',
                '-icount', 'shift=0,sleep=off', '-cpu', 'cortex-a53', '-smp', '2', '-m', '128M', '-nographic', '-monitor', 'none',
                '-semihosting-config', 'enable=on,target=native', '-kernel', str(binary)], timeout=30)
            if 'PASS DMA pool return:' not in output or 'FAIL ' in output:
                raise RuntimeError(f'{case} missed terminal result: {output}')
            results[case] = {'passed': True, 'elf_sha256': sha(binary), 'output': output.strip()}
            (out / 'result.json').write_text(json.dumps({'cases': results,
                'limits': 'Actual native pool/startup/service and two-core FreeRTOS. Modeled A733 registers, DMA movement and IRQ mask/rearm callbacks; no physical hardware qualification.'}, indent=2) + '\n')
            print(output.strip(), flush=True)
        inputs = {}
        dependencies = set()
        for path in build.rglob('*.obj.d'):
            dependencies.update(Path(p).resolve() for p in shlex.split(path.read_text().replace('\\\n', ' ').split(':', 1)[1]))
        dependencies.update((Path(__file__).resolve(), root / 'platform/qemu-smp/link.ld',
            root / 'cmake/dma-startup.cmake', root / 'patches/freertos-smp-privileged.patch',
            root / 'checks/npu-service-start.patch', project / 'CMakeLists.txt', build / 'compile_commands.json'))
        for index, path in enumerate(sorted(dependencies)):
            target = out / 'inputs' / str(index) / path.name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
            inputs[str(path)] = {'snapshot': str(target.relative_to(out)), 'sha256': sha(target)}
        (out / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
    finally:
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')


if __name__ == '__main__':
    main()
