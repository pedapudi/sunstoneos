#!/usr/bin/env python3
"""Check cached shared mappings and primary device preparation on eight QEMU CPUs."""
import argparse
import hashlib
import json
import os
import shlex
import shutil
import struct
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
    parser.add_argument('--resume-build', action='store_true')
    for name in ('freertos', 'tinyusb', 'fixtures', 'firmware', 'qemu', 'lld'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    if args.resume_build and (not out.is_dir() or (out / 'execute.log').exists()):
        raise RuntimeError('build resumption requires an existing directory without execution')
    out.mkdir(parents=True, exist_ok=args.resume_build)
    project, build, fixtures = out / 'project', out / 'build', out / 'fixtures'
    project.mkdir(exist_ok=args.resume_build)
    fixtures.mkdir(exist_ok=args.resume_build)
    if not args.resume_build:
        shutil.copy2(args.fixtures / 'npu-convolution-data.h', fixtures)
        shutil.copy2(args.firmware, fixtures / 'gpu.fw')
    assert sha(fixtures / 'gpu.fw') == '1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2'
    (project / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.24)
project(cached_device_check LANGUAGES C ASM)
add_subdirectory("{ROOT}" sunstone)
get_target_property(gpu gpu-service-model SOURCES)
list(FILTER gpu INCLUDE REGEX "^(drivers/|platform/a733/|arch/aarch64/)|gpu-service-firmware.S$")
get_target_property(npu npu-convolution-model SOURCES)
list(FILTER npu INCLUDE REGEX "^(drivers/|platform/a733/|arch/aarch64/|runtime/counter_clock.c$)")
set(sources ${{gpu}} ${{npu}}
  platform/a733-smp-cached/memory.c platform/a733-smp-cached/devices.c
  platform/a733-smp-cached/platform.c platform/a733/cpu_coherence.c platform/a733/cpu_coherence.S
  platform/a733/cpu_start.c runtime/memory.c runtime/memmove.c runtime/device_memory_pool.c
  platform/a733/memory_reservations.c platform/a733/memory_uncached.c
  platform/a733/gpu_memory.c platform/a733/gpu_reserved.c platform/a733/gpu_compute_reserved.c
  platform/a733/npu_reserved.c drivers/npu/npu_convolution.c drivers/gpu/a733_gpu_resources.c
  checks/cached-device-reservations.c checks/cached-device-reservations.S
  "{args.freertos.resolve()}/tasks.c" "{args.freertos.resolve()}/queue.c" "{args.freertos.resolve()}/list.c"
  "{build}/sunstone/freertos-a733-smp-cached/port.c"
  "{build}/sunstone/freertos-a733-smp-cached/portASM.S")
list(REMOVE_DUPLICATES sources)
set(absolute_sources)
foreach(source IN LISTS sources)
  if(NOT IS_ABSOLUTE "${{source}}")
    set(source "{ROOT}/${{source}}")
  endif()
  list(APPEND absolute_sources "${{source}}")
endforeach()
add_executable(cached-device-check ${{absolute_sources}})
target_include_directories(cached-device-check PRIVATE "{ROOT}/platform/a733-smp-cached"
  "{ROOT}/include" "{ROOT}/runtime/include" "{args.freertos.resolve()}/include"
  "{build}/sunstone/freertos-a733-smp-cached" "{fixtures}")
target_compile_definitions(cached-device-check PRIVATE SUNSTONE_A733_CACHED_DEVICES=1
  SUNSTONE_SECURE_MONITOR=1 SUNSTONE_A733_SMP_DIAGNOSTIC=1 A733_LOCAL_POOLS_BLOCK_MAPPED=1
  INCLUDE_xTaskGetSchedulerState=1 INCLUDE_xTaskGetCurrentTaskHandle=1
  INCLUDE_xSemaphoreGetMutexHolder=1 GPU_FIXTURE_BYTES={(fixtures / 'gpu.fw').stat().st_size})
target_compile_options(cached-device-check PRIVATE -march=armv8-a -mstrict-align
  $<$<COMPILE_LANGUAGE:C>:-mgeneral-regs-only> -ffreestanding -fno-builtin -fno-stack-protector
  -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections
  -O2 -g -Wall -Wextra -Werror)
target_link_options(cached-device-check PRIVATE -nostdlib -static -Wl,--gc-sections
  -Wl,--wrap=platform_panic -Wl,--defsym=__device_pool_size=2097152
  -Wl,--defsym=__uncached_pool_size=2097152
  "-Wl,-T,{ROOT}/platform/a733-smp-cached/link.ld" "-Wl,-Map,{build}/cached-device-check.map")
set_target_properties(cached-device-check PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "{ROOT}/platform/a733-smp-cached/link.ld")
# Compile the actual primary handoff caller as well; the harness does not run GIC setup.
add_library(cached-device-continuation OBJECT "{ROOT}/platform/a733-smp-cached/continue.c")
foreach(property IN ITEMS INCLUDE_DIRECTORIES COMPILE_OPTIONS COMPILE_DEFINITIONS)
  get_target_property(value cached-device-check ${{property}})
  set_property(TARGET cached-device-continuation PROPERTY ${{property}} "${{value}}")
endforeach()
''')
    # Minimal immutable reset ROM: branch to the loaded image from secure EL3.
    rom = out / 'reset-rom.bin'
    rom.write_bytes(struct.pack('<IIQ', 0x58000040, 0xd61f0000, 0x41000000))
    environment = os.environ.copy()
    environment['PATH'] = str(args.lld.resolve().parent) + os.pathsep + environment.get('PATH', '')
    commands = []
    def run(name, command, timeout=None):
        commands.append(command)
        log, suffix = out / (name + '.log'), 0
        while log.exists():
            suffix += 1
            log = out / (name + '-' + str(suffix) + '.log')
        log.write_text('')
        try:
            r = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=timeout, env=environment)
        except subprocess.TimeoutExpired as error:
            output = error.stdout or b''
            log.write_text(output.decode(errors='replace') if isinstance(output, bytes) else output)
            raise
        log.write_text(r.stdout)
        if r.returncode:
            raise RuntimeError(f'{name} failed ({r.returncode}): {r.stdout[-6000:]}')
        return r.stdout
    try:
        run('configure', ['cmake', '-S', str(project), '-B', str(build),
            '-DCMAKE_TOOLCHAIN_FILE=' + str(ROOT / 'cmake/aarch64-clang.cmake'),
            '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DFREERTOS_DIR=' + str(args.freertos.resolve()),
            '-DTINYUSB_DIR=' + str(args.tinyusb.resolve()), '-DNPU_CONVOLUTION_FIXTURES=' + str(fixtures),
            '-DSUNSTONE_GPU_FIRMWARE=' + str(fixtures / 'gpu.fw'),
            '-DSUNSTONE_LLD=' + str(args.lld.absolute()),
            '-DCMAKE_EXE_LINKER_FLAGS=--ld-path=' + str(args.lld.absolute())])
        run('build', ['cmake', '--build', str(build), '--target',
                      'cached-device-check', 'cached-device-continuation', '-j4'])
        binary = out / 'cached-device-reservations.elf'
        shutil.copy2(build / 'cached-device-check.elf', binary)
        with binary.open('rb') as stream:
            elf = ELFFile(stream)
            symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
            layout = {name: symbols[name] for name in ('__bss_start', '__bss_end', '__heap_end',
                '__device_pool_start', '__device_pool_end', '__uncached_pool_start', '__uncached_pool_end',
                '__tables_start', '__tables_end', '__monitor_bss_start', '__monitor_bss_end')}
        assert layout['__device_pool_end'] - layout['__device_pool_start'] == 2097152
        assert layout['__bss_end'] <= layout['__device_pool_start'] < layout['__device_pool_end'] <= 0x41c00000
        assert layout['__uncached_pool_start'] == 0x41c00000 and layout['__uncached_pool_end'] == 0x41e00000
        assert layout['__tables_end'] - layout['__tables_start'] == 5 * 4096 + 64
        (out / 'layout.json').write_text(json.dumps(layout, indent=2) + '\n')
        dependencies = set()
        for path in build.rglob('*.obj.d'):
            data = path.read_text().replace('\\\n', ' ')
            dependencies.update(Path(p) for p in shlex.split(data.split(':', 1)[1]))
        dependencies.update([ROOT / 'checks/cached-device-reservations.py',
            ROOT / 'platform/a733-smp-cached/link.ld', ROOT / 'cmake/a733-smp-cached.cmake',
            ROOT / 'cmake/npu-convolution.cmake', ROOT / 'cmake/gpu.cmake', ROOT / 'CMakeLists.txt',
            project / 'CMakeLists.txt', fixtures / 'gpu.fw', build / 'compile_commands.json'])
        inputs = {}
        for index, source in enumerate(sorted(p.resolve() for p in dependencies)):
            target = out / 'inputs' / str(index) / source.name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            inputs[str(source)] = {'snapshot': str(target.relative_to(out)), 'sha256': sha(target)}
        (out / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
        (out / 'execution-input.json').write_text(json.dumps({'elf_sha256': sha(binary),
            'rom_sha256': sha(rom), 'qemu_sha256': sha(args.qemu.resolve())}, indent=2) + '\n')
        if not args.build_only:
            run('execute', [str(args.qemu.resolve()), '-machine', 'virt,secure=on,virtualization=on',
                '-cpu', 'cortex-a53', '-smp', '8', '-m', '128M', '-nographic', '-monitor', 'none',
                '-serial', 'none', '-semihosting-config', 'enable=on,target=native',
                '-bios', str(rom), '-device', 'loader,file=' + str(binary)], timeout=60)
            (out / 'result.json').write_text(json.dumps({'passed': True, 'cores': 8,
                'wb_pages': 399, 'nc_pages': 51,
                'limits': 'Actual AT/table/cache instructions and preparation with QEMU CPUs; no A733 cache/interconnect, CPU release, watchdog, device or scheduler qualification.'}, indent=2) + '\n')
    finally:
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')


if __name__ == '__main__':
    main()
