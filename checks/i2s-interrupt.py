#!/usr/bin/env python3
"""Build the finite I2S receive model with the DMA0 interrupt adapter and run its cases."""
import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
LABELS = {0: 'queue-end-wake-bridges-each-period', 1: 'one-tick-fallback-without-delivery'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cases', default='0,1')
    for name in ('freertos', 'tinyusb', 'qemu', 'lld'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    project, build = out / 'project', out / 'build'
    project.mkdir()
    # Reuse the I2S stream model composition; the check, the interrupt
    # adapter and its private task-privilege hook differ.
    (project / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.24)
project(i2s_interrupt_check LANGUAGES C ASM)
add_subdirectory("{ROOT}" sunstone)
apply_build_patch("{build}/interrupt-port.c" "{build}/sunstone/freertos-smp/port.c"
  "{ROOT}/patches/freertos-smp-privileged.patch")
set_source_files_properties("{build}/interrupt-port.c" PROPERTIES
  COMPILE_DEFINITIONS "npu_service_task_privileged=i2s_interrupt_task_privileged")
get_target_property(sources i2s-stream-model SOURCES)
list(FILTER sources EXCLUDE REGEX "checks/|i2s-stream-port.c$")
list(APPEND sources checks/i2s-interrupt.c drivers/audio/dma_service_i2s_irq.c "{build}/interrupt-port.c")
set(absolute_sources)
foreach(source IN LISTS sources)
  if(NOT IS_ABSOLUTE "${{source}}")
    set(source "{ROOT}/${{source}}")
  endif()
  list(APPEND absolute_sources "${{source}}")
endforeach()
add_executable(i2s-interrupt-model ${{absolute_sources}})
target_include_directories(i2s-interrupt-model PRIVATE "{ROOT}/platform/qemu-smp"
  "{ROOT}/include" "{ROOT}/runtime/include" "{args.freertos.resolve()}/include"
  "{build}/sunstone/freertos-smp")
target_compile_definitions(i2s-interrupt-model PRIVATE INCLUDE_xTaskGetSchedulerState=1
  INCLUDE_xTaskGetCurrentTaskHandle=1 INCLUDE_xSemaphoreGetMutexHolder=1)
target_compile_options(i2s-interrupt-model PRIVATE -march=armv8-a -mstrict-align
  -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables
  -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections
  -O2 -g -Wall -Wextra -Werror -fstack-usage)
target_link_options(i2s-interrupt-model PRIVATE -nostdlib -static -Wl,--gc-sections
  "-Wl,-T,{ROOT}/platform/qemu-smp/link.ld" "-Wl,-Map,{build}/i2s-interrupt-model.map")
set_target_properties(i2s-interrupt-model PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "{ROOT}/platform/qemu-smp/link.ld")
''')
    environment = os.environ.copy()
    environment['PATH'] = str(args.lld.resolve().parent) + os.pathsep + environment.get('PATH', '')
    commands = []

    def run(name, command, timeout=None):
        commands.append(command)
        result = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=timeout, env=environment)
        (out / (name + '.log')).write_text(result.stdout)
        return result

    results = {'physical_access': False, 'cases': {}}
    try:
        for name, command in (
                ('configure', ['cmake', '-S', str(project), '-B', str(build),
                    '-DCMAKE_TOOLCHAIN_FILE=' + str(ROOT / 'cmake/aarch64-clang.cmake'),
                    '-DFREERTOS_DIR=' + str(args.freertos.resolve()),
                    '-DTINYUSB_DIR=' + str(args.tinyusb.resolve()), '-DSUNSTONE_LLD=' + str(args.lld.absolute()),
                    '-DCMAKE_EXE_LINKER_FLAGS=--ld-path=' + str(args.lld.absolute())]),
                ('build', ['cmake', '--build', str(build), '--target', 'i2s-interrupt-model', '-j6'])):
            result = run(name, command)
            if result.returncode:
                raise RuntimeError(f'{name} failed ({result.returncode}): {result.stdout[-6000:]}')
        binary = build / 'i2s-interrupt-model.elf'
        with binary.open('rb') as stream:
            elf = ELFFile(stream)
            assert not any(s['p_vaddr'] <= 0x47fff008 < s['p_vaddr'] + s['p_memsz']
                           for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD')
        results['image_sha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
        for case in map(int, args.cases.split(',')):
            label = LABELS[case]
            result = run(f'{case}-{label}', [str(args.qemu.resolve()),
                '-machine', 'virt,gic-version=3,secure=off,virtualization=off', '-cpu', 'cortex-a55',
                '-smp', '2', '-m', '128M', '-nographic', '-monitor', 'none',
                '-semihosting-config', 'enable=on,target=native', '-kernel', str(binary),
                '-device', f'loader,addr=0x47fff008,data={case},data-len=4'], timeout=30)
            passed = result.returncode == 0 and 'PASS I2S interrupt:' in result.stdout
            results['cases'][label] = {'passed': passed, 'exit': result.returncode}
            if not passed:
                raise RuntimeError(f'case {label} failed: {result.stdout[-3000:]}')
    finally:
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        (out / 'result.json').write_text(json.dumps(results, indent=2) + '\n')
    print('PASS I2S interrupt cases ' + ','.join(results['cases']) +
          '; DMA/I2S registers, GIC SPI and interrupt delivery modeled')


if __name__ == '__main__':
    main()
