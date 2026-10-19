#!/usr/bin/env python3
"""Build one isolated FreeRTOS/native-memory composition and run selected cases."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def run(command, out, name, timeout=120):
    p = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, timeout=timeout)
    (out / (name + '.log')).write_text(p.stdout)
    if p.returncode:
        raise RuntimeError(name + ': ' + p.stdout[-6000:])
    return p.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--freertos', type=Path, required=True)
    parser.add_argument('--tinyusb', type=Path, required=True)
    parser.add_argument('--firmware', type=Path, required=True)
    parser.add_argument('--lld', type=Path, required=True)
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--cases', nargs='+', type=int, choices=tuple(range(1, 15)), default=[1, 2, 3])
    parser.add_argument('--package',type=Path)
    parser.add_argument('--expected',type=Path)
    parser.add_argument('--expected-second',type=Path)
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--run-only', action='store_true')
    a = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    source = out / 'project'; source.mkdir(exist_ok=True)
    build = out / 'build'
    extra = ['drivers/gpu/pvr_compute_service.c', 'drivers/gpu/pvr_compute_package.c', 'drivers/gpu/pvr_compute_job.c',
             'drivers/gpu/pvr_compute_programs.c', 'drivers/gpu/pvr_compute_context.c',
             'drivers/gpu/pvr_cccb.c', 'drivers/gpu/pvr_gpu_vm.c', 'platform/a733/gpu_vm_memory.c']
    cmake = '''cmake_minimum_required(VERSION 3.24)
project(compute_composition LANGUAGES C ASM)
add_subdirectory("ROOT" sunstone)
get_target_property(sources gpu-memory-model SOURCES)
set(resolved "")
foreach(path IN LISTS sources)
 if(path STREQUAL "checks/pvr-gpu-service.c")
  list(APPEND resolved "ROOT/checks/pvr-compute-service.c")
 elseif(IS_ABSOLUTE "${path}")
  list(APPEND resolved "${path}")
 else()
  list(APPEND resolved "ROOT/${path}")
 endif()
endforeach()
list(APPEND resolved EXTRA)
list(REMOVE_DUPLICATES resolved)
add_executable(compute-model ${resolved})
foreach(property IN ITEMS INCLUDE_DIRECTORIES COMPILE_OPTIONS COMPILE_DEFINITIONS LINK_OPTIONS LINK_DEPENDS)
 get_target_property(value gpu-memory-model ${property})
 set_property(TARGET compute-model PROPERTY ${property} "${value}")
endforeach()
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/sunstone/gpu-service-port.c" PROPERTIES
 COMPILE_DEFINITIONS "npu_service_task_privileged=pvr_gpu_service_task_privileged")
set_target_properties(compute-model PROPERTIES SUFFIX ".elf")
'''.replace('ROOT', str(root)).replace('EXTRA', ' '.join('"' + str(root / p) + '"' for p in extra))
    if a.package:
        assert a.expected and a.package.stat().st_size==8288
        assembly=source/'package.S'
        assembly.write_text('.section .rodata\n.balign 8\n.global fixture_package\nfixture_package:\n.incbin "'+str(a.package.resolve())+'"\n.global fixture_expected\nfixture_expected:\n.incbin "'+str(a.expected.resolve())+'"\n.global fixture_expected_second\nfixture_expected_second:\n.incbin "'+str((a.expected_second or a.expected).resolve())+'"\n')
        cmake+='\ntarget_sources(compute-model PRIVATE "'+str(assembly)+'")\ntarget_compile_definitions(compute-model PRIVATE PVR_COMPUTE_SERVICE_PACKAGE_FIXTURE=1)\n'
    (source / 'CMakeLists.txt').write_text(cmake)
    commands = [['cmake', '-S', str(source), '-B', str(build),
                 '-DCMAKE_TOOLCHAIN_FILE=' + str(root / 'cmake/aarch64-clang.cmake'),
                 '-DFREERTOS_DIR=' + str(a.freertos.resolve()),
                 '-DTINYUSB_DIR=' + str(a.tinyusb.resolve()),
                 '-DSUNSTONE_GPU_FIRMWARE=' + str(a.firmware.resolve()),
                 '-DSUNSTONE_LLD=' + str(a.lld.absolute()),
                 '-DCMAKE_EXE_LINKER_FLAGS=--ld-path=' + str(a.lld.absolute()), '-DCMAKE_TRY_COMPILE_PLATFORM_VARIABLES=SUNSTONE_LLD',
                 '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'],
                ['cmake', '--build', str(build), '--target', 'compute-model', '-j4']]
    if not a.run_only:
        for i, command in enumerate(commands): run(command, out, 'build-' + str(i))
        (out / 'build-commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        original = (build / 'compute-model.elf').read_bytes()
        snapshot = out / 'inputs'; snapshot.mkdir(exist_ok=True)
        records = json.loads((build / 'compile_commands.json').read_text())
        # Snapshot only this target's compiler inputs plus included project headers.
        files = {Path(r['file']).resolve() for r in records if 'compute-model.dir' in r['command']}
        files.update(root.glob('include/*.h'))
        for dep in (build / 'CMakeFiles/compute-model.dir').rglob('*.obj.d'):
            names = dep.read_text().replace('\\\n', ' ').split(':', 1)[1].split()
            files.update(Path(name).resolve() for name in names)
        files.update([root / 'checks/pvr-gpu-service.c', root / 'drivers/gpu/pvr_compute_probe.inc',
                      root / 'drivers/gpu/pvr_compute_usc.inc', root / 'checks/pvr-compute-service.py',
                      a.firmware.resolve(), root / 'platform/qemu-smp/link.ld'])
        if a.package:files.update([a.package.resolve(),a.expected.resolve()])
        hashes = {}
        for p in sorted(files):
            data = p.read_bytes(); hashes[str(p)] = hashlib.sha256(data).hexdigest()
            name = str(p).lstrip('/'); target = snapshot / name; target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        (out / 'compute-model.elf').write_bytes(original)
        manifest = {'scope': 'Two-core FreeRTOS with native translation/cache instructions and modeled firmware/GPU writes',
                    'commands': commands, 'inputs': hashes, 'elf_sha256': hashlib.sha256(original).hexdigest(),
                    'compile_commands': records, 'results': []}
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    else:
        original = (out / 'compute-model.elf').read_bytes()
        manifest = json.loads((out / 'manifest.json').read_text())
    if a.build_only: return
    symbols = subprocess.check_output(['llvm-nm-21', '-S', str(out / 'compute-model.elf')], text=True)
    symbol = [line.split() for line in symbols.splitlines() if line.split()[-1] == 'compute_service_case']
    assert len(symbol) == 1 and int(symbol[0][1], 16) == 4
    address = int(symbol[0][0], 16)
    phoff = struct.unpack_from('<Q', original, 32)[0]
    entry_size, count = struct.unpack_from('<HH', original, 54)
    offsets = []
    for i in range(count):
        kind, flags, offset, va, pa, filesz, memsz, align = struct.unpack_from('<II6Q', original, phoff + i * entry_size)
        if kind == 1 and va <= address and address + 4 <= va + filesz: offsets.append(offset + address - va)
    assert len(offsets) == 1 and struct.unpack_from('<I', original, offsets[0])[0] == 1
    for case in a.cases:
        data = bytearray(original); struct.pack_into('<I', data, offsets[0], case)
        path = out / f'case-{case}.elf'; path.write_bytes(data)
        command = [str(a.qemu.resolve()), '-machine', 'virt,gic-version=3,secure=off,virtualization=off',
                   '-cpu', 'cortex-a55', '-smp', '2', '-m', '128M', '-nographic', '-monitor', 'none',
                   '-semihosting-config', 'enable=on,target=native', '-kernel', str(path)]
        output = run(command, out, f'case-{case}', 15)
        assert f'PASS compute service case 0x{case:016x};' in output, output
        manifest['results'].append({'case': case, 'command': command,
                                    'sha256': hashlib.sha256(data).hexdigest(), 'output': output})
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        print(output, end='')


if __name__ == '__main__':
    main()
