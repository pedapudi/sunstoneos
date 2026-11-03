#!/usr/bin/env python3
"""Build the DMA service retirement model and run its cases in fresh QEMU processes."""
import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['include/dma_service.h', 'drivers/dma/dma_service.c',
                'checks/dma-service.c', 'checks/dma-retirement.c']
GATE_RUNS = [
    ['--output', '{output}', '--freertos', '{cache}/sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2', '--tinyusb', '{cache}/sources/tinyusb-d668b0abb7219b9ce324fa88d46604bb9e869973', '--qemu', '{cache}/host/usr/bin/qemu-system-aarch64', '--lld', '{cache}/host/usr/lib/llvm-21/bin/ld.lld'],
]
LABELS = {0: 'normal-and-delayed-drain', 1: 'failed-mask-retained',
          2: 'uncollected-peer-reply', 3: 'queued-peer-request', 4: 'stuck-busy-retained'}

if not __debug__:
    raise SystemExit('checks require assertions')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cases', default='0,1,2,3,4')
    for name in ('freertos', 'tinyusb', 'qemu', 'lld'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    project, build = out / 'project', out / 'build'
    project.mkdir()
    (project / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.24)
project(dma_retirement_check LANGUAGES C ASM)
add_subdirectory("{ROOT}" sunstone)
get_target_property(sources dma-service-model SOURCES)
list(FILTER sources EXCLUDE REGEX "checks/")
list(APPEND sources checks/dma-retirement.c)
set(absolute_sources)
foreach(source IN LISTS sources)
  if(NOT IS_ABSOLUTE "${{source}}")
    set(source "{ROOT}/${{source}}")
  endif()
  list(APPEND absolute_sources "${{source}}")
endforeach()
add_executable(dma-retirement-model ${{absolute_sources}})
foreach(property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_OPTIONS)
  get_target_property(value dma-service-model ${{property}})
  set_property(TARGET dma-retirement-model PROPERTY ${{property}} "${{value}}")
endforeach()
target_compile_definitions(dma-retirement-model PRIVATE npu_service_task_privileged=dma_service_task_privileged)
set_target_properties(dma-retirement-model PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "{ROOT}/platform/qemu-smp/link.ld")
''')
    environment = os.environ.copy()
    environment['PATH'] = str(args.lld.absolute().parent) + os.pathsep + environment.get('PATH', '')
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
                ('build', ['cmake', '--build', str(build), '--target', 'dma-retirement-model', '-j6'])):
            result = run(name, command)
            if result.returncode:
                raise RuntimeError(f'{name} failed ({result.returncode}): {result.stdout[-6000:]}')
        binary = build / 'dma-retirement-model.elf'
        with binary.open('rb') as stream:
            elf = ELFFile(stream)
            assert not any(s['p_vaddr'] <= 0x47fff004 < s['p_vaddr'] + s['p_memsz']
                           for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD')
        results['image_sha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
        for case in map(int, args.cases.split(',')):
            label = LABELS[case]
            result = run(f'{case}-{label}', [str(args.qemu.resolve()),
                '-machine', 'virt,gic-version=3,secure=off,virtualization=off', '-icount', 'shift=0,sleep=off', '-cpu', 'cortex-a55',
                '-smp', '2', '-m', '128M', '-nographic', '-monitor', 'none',
                '-semihosting-config', 'enable=on,target=native', '-kernel', str(binary),
                '-device', f'loader,addr=0x47fff004,data={case},data-len=4'], timeout=30)
            passed = result.returncode == 0 and 'PASS DMA retirement:' in result.stdout
            results['cases'][label] = {'passed': passed, 'exit': result.returncode}
            if not passed:
                raise RuntimeError(f'case {label} failed: {result.stdout[-3000:]}')
    finally:
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        (out / 'result.json').write_text(json.dumps(results, indent=2) + '\n')
    print('PASS DMA retirement cases ' + ','.join(results['cases']) +
          '; DMA registers and IRQ delivery modeled')


if __name__ == '__main__':
    main()
