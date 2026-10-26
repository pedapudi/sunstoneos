#!/usr/bin/env python3
"""Check the actual AArch64 native GPU thermal caller with modeled service replies."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
from harness import Image, LLD, Steps
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_X0

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, required=True, help='Accepted OS build with compile_commands.json')
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--lld', type=Path, default=LLD)
parser.add_argument('--cases', type=int, nargs='+', choices=range(31), default=list(range(14)))
args = parser.parse_args()
out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
steps = Steps(out, 'commands.json')
unit = next(v for v in json.loads((args.build / 'compile_commands.json').read_text())
            if v['file'].endswith('/apps/os/thermal.c'))
original = shlex.split(unit['command']); old_root = str(Path(unit['file']).parents[2])
flags = [v.replace(old_root, str(ROOT)) for v in original[:original.index('-o')]]
flags += ['-DSUNSTONE_A733_OS_GPU_LINEAR=1', '-DSUNSTONE_A733_FAULT_PAGE=1', '-DOS_RECOGNITION_CORE=2']
for flag in flags:
    if flag.startswith('-I'): assert Path(flag[2:]).is_dir(), flag
(out / 'compile-inputs.json').write_text(json.dumps(dict(unit=unit, flags=flags), indent=2) + '\n')
objects = []
for i, source in enumerate(('checks/os-gpu-thermal.c', 'runtime/memory.c', 'runtime/memmove.c')):
    obj = out / f'unit-{i}.o'; objects.append(obj)
    steps.command(f'compile-{i}', [*flags, '-c', ROOT / source, '-o', obj])
elf = out / 'os-gpu-thermal.elf'
steps.command('link', [args.lld, '-Ttext=0x41000000', '--entry=os_gpu_thermal_check', '--gc-sections',
                       *objects, '-o', elf])
image = Image(elf); identity = hashlib.sha256(elf.read_bytes()).hexdigest()
cases = ['repeated-park-reopen', 'throttle-during-reopen', 'critical-during-reopen',
         'close-observation-deadline', 'wrong-client', 'expired-lifetime', 'stale-open-record',
         'terminal-retire', 'retire-close-busy', 'retire-deadline', 'retire-critical',
         'retire-wrong-client', 'retire-pending-ticket', 'retire-stale-stopped-epoch',
         'job-timely', 'job-no-record', 'job-deadline-equality', 'job-late-record',
         'job-revoked', 'job-counter-backwards', 'job-counter-stalled', 'job-already-pending',
         'open-no-reply', 'open-not-queued', 'open-late-reply', 'open-no-ready-record', 'job-both-clocks-stalled',
         'close-shared-status-budget', 'open-shared-reply-ready-budget', 'job-invalid-record', 'caller-ownership']
results = {}
for mode, name in enumerate(cases):
    if mode not in args.cases: continue
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM); pages = set()
    with elf.open('rb') as stream:
        for segment in ELFFile(stream).iter_segments():
            if segment['p_type'] != 'PT_LOAD': continue
            address, size = int(segment['p_vaddr']), int(segment['p_memsz'])
            for page in range(address & ~4095, (address + size + 4095) & ~4095, 4096):
                if page not in pages: uc.mem_map(page, 4096); pages.add(page)
            uc.mem_write(address, segment.data())
    uc.mem_map(0x80000000, 0x10000); uc.reg_write(UC_ARM64_REG_SP, 0x8000f000)
    uc.reg_write(UC_ARM64_REG_X0, mode)
    uc.hook_add(UC_HOOK_INTR, lambda machine, interrupt, unused: machine.emu_stop())
    uc.emu_start(image.address('os_gpu_thermal_check'), 0, count=1000000)
    status = int.from_bytes(uc.mem_read(image.address('os_check_result'), 4), 'little')
    (out / (name + '.json')).write_text(json.dumps(dict(status=status, image_sha256=identity,
        physical_access=False)) + '\n')
    assert status == 1, (name, status)
    results[name] = True
(out / 'results.json').write_text(json.dumps(dict(cases=results, elf_sha256=identity,
    scope='Actual native thermal caller; service, board clock/lifetime and RTOS modeled.'), indent=2) + '\n')
print(f'PASS: {len(results)} native GPU thermal lifecycle cases; no physical access.')
