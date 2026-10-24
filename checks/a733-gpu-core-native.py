#!/usr/bin/env python3
"""Check actual CORE native access widths, allowlists and shared-lock refusal."""
import argparse
import hashlib
import json
from pathlib import Path
from harness import Image, LLD, Steps
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_LR

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--libc-include', type=Path, required=True)
a = p.parse_args()
out = a.output.resolve(); out.mkdir(parents=True, exist_ok=False)
steps = Steps(out, 'commands.json')
objects = []
for i, source in enumerate(('checks/a733-gpu-core-native.c', 'runtime/memory.c')):
    obj = out / f'{i}.o'; objects.append(obj)
    steps.command(f'compile-{i}', ['clang-21', '--target=aarch64-none-elf', '-mcpu=cortex-a55',
        '-mgeneral-regs-only', '-ffreestanding', '-fno-builtin', '-ffunction-sections', '-fdata-sections',
        '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'include'),
        '-I' + str(ROOT / 'platform/a733'), '-I' + str(a.libc_include.resolve()),
        '-c', ROOT / source, '-o', obj])
elf = out / 'native.elf'
steps.command('link', [LLD, '-Ttext=0x41000000', '--entry=core_native_check', '--gc-sections', *objects, '-o', elf])
image = Image(elf); results = []
for mode, name in enumerate(('exact-access', 'allowlist-refusal', 'lock-refusal', 'expired-write')):
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM); pages = set()
    with elf.open('rb') as stream:
        for segment in ELFFile(stream).iter_segments():
            if segment['p_type'] != 'PT_LOAD': continue
            base, size = int(segment['p_vaddr']), int(segment['p_memsz'])
            for page in range(base & ~4095, (base + size + 4095) & ~4095, 4096):
                if page not in pages: uc.mem_map(page, 4096); pages.add(page)
            uc.mem_write(base, segment.data())
    uc.mem_map(0x80000000, 0x10000); uc.mem_map(0x07066000, 4096)
    for offset, value in ((0, 8), (8, 8), (0x20, 0x010101ff)):
        uc.mem_write(0x07066000 + offset, value.to_bytes(4, 'little'))
    uc.cpr_write(3, 3, 14, 0, 0, 24000000)
    ticks = [100]; accesses = []
    def instruction(machine, address, size, unused):
        word = int.from_bytes(machine.mem_read(address, 4), 'little')
        if word & 0xffffffe0 == 0xd53be020:
            machine.reg_write(UC_ARM64_REG_X0 + (word & 31), ticks[0] * 24)
            ticks[0] += 1; machine.reg_write(UC_ARM64_REG_PC, address + 4)
    def access(machine, kind, address, size, value, write):
        assert size == 4, (address, size)
        if write:
            assert int.from_bytes(machine.mem_read(image.address('core_lock'), 4), 'little') == 1
        accesses.append(dict(write=write, address=address, size=size, value=value if write else None))
    uc.hook_add(UC_HOOK_CODE, instruction)
    uc.hook_add(UC_HOOK_MEM_READ, access, False, 0x07066000, 0x07066fff)
    uc.hook_add(UC_HOOK_MEM_WRITE, access, True, 0x07066000, 0x07066fff)
    uc.reg_write(UC_ARM64_REG_SP, 0x8000f000); uc.reg_write(UC_ARM64_REG_X0, mode)
    uc.reg_write(UC_ARM64_REG_LR, 0x80000000)
    uc.emu_start(image.address('core_native_check'), 0x80000000, count=100000)
    assert uc.reg_read(UC_ARM64_REG_PC) == 0x80000000, 'instruction budget exhausted'
    assert uc.reg_read(UC_ARM64_REG_X0) == 0, (name, uc.reg_read(UC_ARM64_REG_X0))
    expected = [(False, 0), (False, 8), (False, 0x20), (True, 0x20),
                (True, 0), (True, 0), (False, 0), (False, 8)] if mode == 0 else []
    assert [(v['write'], v['address'] - 0x07066000) for v in accesses] == expected
    results.append(dict(case=name, accesses=accesses, passed=True))
(out / 'results.json').write_text(json.dumps(dict(cases=results,
    elf_sha256=hashlib.sha256(elf.read_bytes()).hexdigest(), physical_access=False), indent=2) + '\n')
print('PASS four actual AArch64 CORE native cases: exact 32-bit accesses, write locks, allowlists and refusals')
