#!/usr/bin/env python3
"""Run actual AArch64 OS thermal publication, dispatch and recognition checkpoints.

Use a configured OS build for its accepted compiler/dependency paths. Sensor
reads, task identity, delays, board admission and GPU OPEN are modeled. The
thermal policy, CPU/GPU recognizer patches and dispatch logic are actual code.
No board, queue, clock or voltage operation occurs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

from harness import Image, LLD, Steps
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_X0

ROOT = Path(__file__).resolve().parents[1]
CASES = {0: 'repeated-throttle-normal-and-critical', 1: 'owner-and-worker-checkpoints',
         2: 'before-thermal-start', 3: 'closed-before-checkpoint', 4: 'closed-during-sleep',
         5: 'placement-pause-resume-and-incomplete', 6: 'faulted-placement-stays-disabled',
         7: 'stale-sensors-throttle-then-critical',
         8: 'ready-without-samples-stays-throttled', 9: 'last-channel-releases-startup-throttle',
         10: 'partial-hot-remains-throttled-until-cool', 11: 'partial-critical-retains-without-more-reads',
         12: 'last-channel-does-not-override-earlier-stale'}
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--lld', type=Path, default=LLD)
parser.add_argument('--resume', action='store_true', help='reuse unchanged successful build steps and cases')
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=args.resume)
steps = Steps(out, 'commands.json')


def stamp(paths):
    return hashlib.sha256(b''.join(Path(p).read_bytes() for p in paths)).hexdigest()[:12]


def run(name, command):
    steps.command(name, command)
    return Path(steps.steps[name]['log']).read_text()


database = json.loads((args.build / 'compile_commands.json').read_text())
c_unit = next(v for v in database if v['file'].endswith('/apps/os/thermal.c'))
old_root = str(Path(c_unit['file']).parents[2])
cxx_unit = next(v for v in database if '/os-image/recognition' in v['file'] and v['file'].endswith('.cc'))


def flags(unit):
    original = shlex.split(unit['command'])
    selected = [v.replace(old_root, str(ROOT)) for v in original[:original.index('-o')]
                if not v.startswith(('-DSUNSTONE_A733_OS_AUDIO', '-DSUNSTONE_A733_OS_HEADSET',
                                     '-DSUNSTONE_A733_OS_RADIO', '-DSUNSTONE_A733_OS_CPU_CLOCK',
                                     '-DA733_A76_ADMITTED', '-DSUNSTONE_A733_OS_GPU_LINEAR',
                                     '-DOS_RECOGNITION_CORE'))]
    for value in selected:
        if value.startswith('-I'): assert Path(value[2:]).is_dir(), value
    return selected


c_flags, cxx_flags = flags(c_unit), flags(cxx_unit)
(out / 'compile-inputs.json').write_text(json.dumps(dict(c=c_unit, cxx=cxx_unit,
    c_flags=c_flags, cxx_flags=cxx_flags), indent=2) + '\n')
(out / 'placement.h').write_text('''#define OS_PLACEMENT_TENSORS 1u
#define OS_PLACEMENT_GPU_TENSORS 1u
const struct os_placement os_placement_table[]={{16,32}};
''')
common = []
for index, source in enumerate(('drivers/thermal/a733_thermal.c', 'drivers/clock/a733_clock_leases.c',
                                'runtime/memory.c', 'runtime/memmove.c')):
    obj = out / f'common-{index}.o'; common.append(obj)
    run('compile-common-' + str(index), [*c_flags, '-c', ROOT / source, '-o', obj])

results = {}
for variant, patch in (('cpu', 'recognition-placement.patch'), ('gpu', 'recognition-dispatch.patch')):
    generated = out / f'recognition-{variant}.cc'
    revision = stamp([ROOT / 'apps/os' / patch])
    run('patch-' + variant + '-' + revision, ['patch', '-s', '--fuzz=0', '--reject-file=-', '--output', generated,
                            ROOT / 'apps/mimus-board/recognition.cc', ROOT / 'apps/os' / patch])
    # Capture builds compose the same placement patch after their input patch.
    captured = out / f'capture-{variant}.cc'
    run('capture-patch-' + variant, ['patch', '-s', '--fuzz=0', '--reject-file=-', '--output', captured,
                                    ROOT / 'apps/mimus-board/recognition.cc', ROOT / 'apps/os/recognition.patch'])
    run('capture-placement-' + variant + '-' + revision, ['patch', '-s', '--fuzz=0', '--reject-file=-', '--output',
                                         out / f'capture-placed-{variant}.cc', captured, ROOT / 'apps/os' / patch])
    extra = [f'-I{out}'] + (['-DSUNSTONE_A733_OS_GPU_LINEAR=1', '-DOS_RECOGNITION_CORE=2'] if variant == 'gpu' else [])
    fixture, recognition = out / (variant + '-fixture.o'), out / (variant + '-recognition.o')
    fixture_revision = stamp([ROOT / p for p in ('checks/os-thermal-throttle.c', 'apps/os/thermal.c',
                                               'apps/os/dispatch.c', 'apps/os/os.h', 'apps/os/dispatch.h')])
    run('compile-' + variant + '-' + fixture_revision, [*c_flags, *extra, '-c', ROOT / 'checks/os-thermal-throttle.c', '-o', fixture])
    run('compile-recognition-' + variant + '-' + revision, [*cxx_flags, *extra, '-c', generated, '-o', recognition])
    image_path = out / (variant + '.elf')
    run('link-' + variant + '-' + stamp([fixture, recognition]), [args.lld, '-Ttext=0x41000000', '--entry=os_throttle_check', '--gc-sections',
                            fixture, recognition, *common, '-o', image_path])
    image = Image(image_path)
    assert image.address('os_throttle_check') and image.address('os_thermal_throttled')
    assert image.address('mimus_native_checkpoint')
    for mode, case in CASES.items():
        if variant == 'cpu' and mode in (5, 6): continue
        record_path = out / f'{variant}-{case}.json'
        identity = hashlib.sha256(image_path.read_bytes()).hexdigest()
        if args.resume and record_path.exists():
            previous = json.loads(record_path.read_text())
            if previous.get('status') == 1 and previous.get('image_sha256') == identity:
                results[variant + '/' + case] = True
                continue
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM); pages = set()
        for address, data in image.parts:
            for page in range(address & ~4095, (address + len(data) + 4095) & ~4095, 4096):
                if page not in pages: uc.mem_map(page, 4096); pages.add(page)
            uc.mem_write(address, data)
        # BSS is NOBITS; map its extent from the actual ELF, not a guessed pool.
        from elftools.elf.elffile import ELFFile
        with image_path.open('rb') as stream:
            elf = ELFFile(stream)
            for segment in elf.iter_segments():
                if segment['p_type'] != 'PT_LOAD': continue
                start, size = int(segment['p_vaddr']), int(segment['p_memsz'])
                for page in range(start & ~4095, (start + size + 4095) & ~4095, 4096):
                    if page not in pages: uc.mem_map(page, 4096); pages.add(page)
        uc.mem_map(0x50000000, 0x10000); uc.reg_write(UC_ARM64_REG_SP, 0x5000f000)
        uc.reg_write(UC_ARM64_REG_X0, mode)
        uc.hook_add(UC_HOOK_INTR, lambda machine, interrupt, unused: machine.emu_stop())
        uc.emu_start(image.address('os_throttle_check'), 0, count=1000000)
        status = int.from_bytes(uc.mem_read(image.address('os_check_result'), 4), 'little')
        record_path.write_text(json.dumps(dict(status=status, image_sha256=identity, physical_access=False)) + '\n')
        assert status == 1, f'{variant}/{case}: fixture code {status}'
        results[variant + '/' + case] = True
    run('disassembly-' + variant + '-' + stamp([image_path]), ['llvm-objdump-21', '-d', '--no-show-raw-insn', image_path])
sources = ['apps/os/thermal.c', 'apps/os/os.h', 'apps/os/dispatch.c', 'apps/os/dispatch.h',
           'apps/os/recognition-placement.patch', 'apps/os/recognition-dispatch.patch',
           'checks/os-thermal-throttle.c', 'checks/os-thermal-throttle.py']
(out / 'result.json').write_text(json.dumps(dict(passed=True, physical_access=False, cases=results,
    sources={name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in sources},
    scope='Actual AArch64 thermal policy/publication, patched recognition checkpoint and GPU placement; sensor, RTOS and GPU device boundaries modeled.'), indent=2) + '\n')
print('PASS: OS thermal publication, bounded checkpoint backoff, stale/critical stop and GPU placement retention.')
