#!/usr/bin/env python3
"""Freeze and build one finite H.264 image, then run only its new checks.

Setup performs all independent fixture compilations and the affected native
link before execution. Run-only refuses changed preserved executables/inputs.
The successful independent FFmpeg oracle and vendor trace are reused.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value)
    return value


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def save(path, value): path.write_text(json.dumps(value, indent=2)+'\n')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--fixture-evidence', required=True, type=Path)
    p.add_argument('--reference-trace', required=True, type=Path)
    for name in ('freertos', 'tinyusb', 'tcp', 'rsn', 'headers'): p.add_argument('--'+name, required=True, type=Path)
    p.add_argument('--compiler', default=Path('/usr/bin/clang-21'), type=Path)
    p.add_argument('--linker', default=Path.home()/'.cache/sunstoneos/host/usr/bin/ld.lld-21', type=Path)
    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument('--setup-only', action='store_true'); mode.add_argument('--run-only', action='store_true')
    p.add_argument('--resume-setup', action='store_true', help='Preserve failed setup and resume changed inputs only')
    a = p.parse_args(); out = a.output.resolve()
    if a.setup_only:
        if a.resume_setup:
            assert out.is_dir() and not (out/'runtime-inputs.json').exists(), 'Only incomplete setup may resume'
            attempts=out/'setup-attempts';attempts.mkdir(exist_ok=True)
            previous=attempts/str(len(list(attempts.iterdir()))+1);previous.mkdir()
            shutil.copytree(out/'source', previous/'source')
            for path in out.iterdir():
                if path.is_file() and path.suffix in ('.json','.log'):shutil.copy2(path,previous/path.name)
        else:out.mkdir(parents=True, exist_ok=False)
        names = subprocess.check_output(['git', 'ls-files', '--cached', '--others', '--exclude-standard'], cwd=ROOT, text=True).splitlines()
        for name in names:
            src = ROOT/name
            if src.is_file():
                dst = out/'source'/name; dst.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(src, dst)
        for name in ('freertos', 'tinyusb', 'tcp', 'rsn', 'headers'):
            destination=out/'dependencies'/(a.rsn.name if name=='rsn' else name)
            if not destination.exists():shutil.copytree(getattr(a, name),destination,ignore=shutil.ignore_patterns('.git','__pycache__'))
        # Existing source-authentication code also verifies the pinned archives.
        for src in (a.tcp.parent/'FreeRTOS-Plus-TCP-b86ad086.tar.gz', a.rsn.parent/(a.rsn.name+'.tar.gz')):
            destination=out/'dependencies'/src.name
            if src.resolve()!=destination.resolve():shutil.copy2(src,destination)
        for source, target in [('input.h264', 'slice.bin'), ('expected.yuv', 'reference.yuv'), ('result.json', 'oracle-result.json')]:
            shutil.copy2(a.fixture_evidence/source, out/target)
        shutil.copy2(a.reference_trace, out/'reference-trace.json')
        assert sha(out/'slice.bin') == json.loads((out/'oracle-result.json').read_text())['files']['input.h264']['sha256']
        assert sha(out/'reference.yuv') == '4398156e0c9291091780d378ff220793691a3dd38eabf483f9e2e9400d6af72e'
        save(out/'input-hashes.json', {str(x.relative_to(out)): sha(x) for directory in (out/'source', out/'dependencies') for x in directory.rglob('*') if x.is_file()})
    else:
        for name, value in json.loads((out/'runtime-inputs.json').read_text()).items():
            assert sha(Path(name)) == value, 'Preserved runtime input changed: '+name
    source = out/'source'; steps_path = out/'steps.json'
    steps = json.loads(steps_path.read_text()) if steps_path.exists() else {}
    commands_path = out/('setup-commands.json' if a.setup_only else 'runtime-commands.json')
    commands = json.loads(commands_path.read_text()) if commands_path.exists() else []
    def run(label, command):
        command = list(map(str, command))
        if steps.get(label, {}).get('exit') == 0:
            assert steps[label]['command'] == command
            return
        commands.append({'label': label, 'command': command}); save(commands_path, commands)
        result = subprocess.run(command, capture_output=True, env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1'})
        (out/(label+'.log')).write_bytes(result.stdout+result.stderr)
        steps[label] = {'exit': result.returncode, 'command': command}; save(steps_path, steps)
        if result.returncode: raise RuntimeError(label+' failed; inspect its retained log')
    compiler, linker = str(a.compiler), str(a.linker)
    image = out/'build/sunstone-a733-h264.elf'
    if a.setup_only:
        host = [compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-DSUNSTONE_BOARD_SUPERVISED=1',
                '-I'+str(source/'include'), '-I'+str(source/'checks'), '-I'+str(source/'platform/a733-smp-cached')]
        jobs = {'admission': ['apps/h264/admission.c', 'apps/h264/snapshot.c', 'drivers/storage/storage_digest.c', 'checks/h264-application-admission.c'],
                'executor': ['drivers/video/a733_video_h264.c', 'drivers/video/a733_video_platform.c', 'drivers/video/a733_video_unit.c',
                    'drivers/clock/a733_clock_leases.c', 'drivers/gpu/a733_gpu_resources.c',
                    'drivers/storage/storage_digest.c', 'checks/a733-video-h264.c']}
        failures = []
        for name, files in jobs.items():
            try: run(name+'-build', host+[source/x for x in files]+['-o', out/name])
            except RuntimeError as error: failures.append(str(error))
        try:
            if steps.get('h264-launcher-build',{}).get('exit')!=0:
                prepare = module('h264_prepare_setup', source/'apps/h264/prepare.py')
                launch = out/'launcher'; launch.mkdir(exist_ok=True)
                prepare.build_launcher(launch, compiler, linker)
                steps['h264-launcher-build'] = {'exit': 0}
                save(steps_path, steps)
        except Exception as error:
            failures.append('launcher-build failed: '+str(error))
        if failures: raise RuntimeError('\n'.join(failures))
        toolchain = out/'toolchain.cmake'
        toolchain.write_text(f'set(SUNSTONE_LLD "{linker}" CACHE FILEPATH "")\ninclude("{source}/cmake/aarch64-clang.cmake")\nset(CMAKE_C_COMPILER "{compiler}")\nset(CMAKE_ASM_COMPILER "{compiler}")\n')
        run('configure', ['cmake', '-S', source/'apps/h264', '-B', out/'build',
            '-DPython3_EXECUTABLE='+sys.executable,
            '-DCMAKE_TOOLCHAIN_FILE='+str(toolchain), '-DFREERTOS_DIR='+str(out/'dependencies/freertos'),
            '-DTINYUSB_DIR='+str(out/'dependencies/tinyusb'), '-DSUNSTONE_TCP_DIR='+str(out/'dependencies/tcp'),
            '-DSUNSTONE_RSN_DIR='+str(out/'dependencies'/a.rsn.name), '-DSUNSTONE_NETWORK_C_HEADERS='+str(out/'dependencies/headers'),
            '-DSUNSTONE_BOARD_SUPERVISED=ON', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
            '-DSUNSTONE_H264_FIXTURE='+str(out/'slice.bin'), '-DSUNSTONE_H264_REFERENCE='+str(out/'reference.yuv')])
        run('native-build', ['cmake', '--build', out/'build', '--target', 'sunstone-a733-h264', '-j4'])
        snapshot = module('h264_snapshot', source/'tools/evidence-snapshot.py')
        dependencies, actual, records = snapshot.collect_dependencies(out/'build', 'sunstone-a733-h264')
        save(out/'compiled-dependencies.json', dependencies); save(out/'compiled-commands.json', actual); save(out/'dependency-records.json', records)
        for name, value in dependencies.items():
            path = Path(name)
            if not path.is_relative_to(out):
                dst = out/'compiled-inputs'/str(path).lstrip('/'); dst.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(path, dst)
                assert sha(dst) == value
        # Preflight only: ELF/DWARF and input geometry, no native instructions.
        # Session bytes are emitted later by the independently compiled admission case.
        cases = module('h264_cases', source/'checks/h264-application-cases.py')
        cases.setup(image, out, session_optional=True)
        import unicorn, elftools
        save(out/'image-manifest.json', {'target': 'sunstone-a733-h264', 'execution_profile': 'h264-supervised-v1',
            'final_elf': sha(image), 'build_id': (out/'build/h264-application/build-id.txt').read_text().strip(), 'compiled_dependencies': dependencies})
        paths = [p for p in out.rglob('*') if p.is_file() and '__pycache__' not in p.parts]
        paths += [a.compiler, a.linker, Path(sys.executable), Path(unicorn.__file__), Path(elftools.__file__)]
        paths += [Path(shutil.which(name)) for name in ('cmake', 'make', 'patch', 'python3')]
        paths += [Path('/usr/lib/llvm-21/bin/llvm-objcopy')]
        paths += [p for name in ('unicorn', 'elftools') for p in (Path(unicorn.__file__).parent.parent/name).rglob('*') if p.is_file() and '__pycache__' not in p.parts]
        save(out/'runtime-inputs.json', {str(x): sha(x) for x in dict.fromkeys(paths) if x.name not in ('runtime-inputs.json', 'steps.json')})
        print('Setup complete; no runtime case executed.'); return
    # Every executable, source, model boundary and selected instruction trace was
    # frozen by setup. The following cases have no device access.
    run('admission', [out/'admission', out/'session.bin'])
    run('executor', [out/'executor', out/'slice.bin', out/'executor-registers.bin'])
    data = (out/'executor-registers.bin').read_bytes()
    import struct
    registers = struct.unpack('<1024I', data[:4096]); sram = struct.unpack('<1024I', data[4096:])
    trace = json.loads((out/'reference-trace.json').read_text())['slice_program']
    expected, reference_sram, offset = {}, {}, 0
    for group, address, value in trace:
        if group == 'top': expected[address] = value
        elif address == 0xe0: offset = value//4
        elif address == 0xe4: reference_sram[offset] = value; offset += 1
        else: expected[0x200+address] = value
    for address in (0x200, 0x204, 0x208, 0x20c, 0x21c, 0x230, 0x234, 0x238, 0x23c, 0x24c, 0x250, 0x254, 0xc4, 0xc8):
        assert registers[address//4] == expected[address], hex(address)
    for address, value in reference_sram.items(): assert sram[address] == value, ('SRAM', address)
    save(out/'program-comparison.json', {'passed': True, 'scope': 'Selected final register and frame SRAM fields against preserved original library instructions; no hardware emulation.'})
    cases = module('h264_cases', source/'checks/h264-application-cases.py')
    cases.run(image, out, source, compiler, linker, steps, steps_path)
    run('host-boundary', [sys.executable, source/'checks/h264-host.py', out])
    save(out/'result.json', {'passed': True, 'physical_access': False, 'image_sha256': sha(image),
        'scope': 'Actual executor/provider and native copy/serializer instructions over explicit MMIO, DMA and USB boundaries.'})
    print('PASS: focused H.264 composition and actual native copy/serializer boundaries.')


if __name__ == '__main__': main()
