#!/usr/bin/env python3
"""Run deadline C sanitizers and count executed AArch64 helper instructions.

Run with the SunstoneOS cache virtualenv Python (Unicorn and pyelftools).
Artifacts stay in --build-dir. Counts cover this isolated helper, excluding
owner IRQ exclusion, object arbitration, compare programming and hardware cost.
"""
import argparse
import os
from pathlib import Path
import struct
import subprocess
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X30


def run(command, env):
    subprocess.run([str(x) for x in command], check=True, env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=Path.home()/'.cache/sunstoneos/build/scheduler-rt-1101/deadline-build')
    parser.add_argument('--skip-host', action='store_true', help='Reuse unchanged successful host evidence')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(build))
    source = root/'kernel/scheduler/deadline.c'
    common = ['clang-21', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I'+str(source.parent)]
    host = build/'deadline-sanitized'
    if not args.skip_host:
        run(common + ['-pthread', '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', source, root/'checks/scheduler-deadline.c', '-o', host], env)
        run([host], env)
    obj, elf = build/'deadline.o', build/'deadline.elf'
    run(common + ['--target=aarch64-none-elf', '-march=armv8-a', '-O2', '-ffreestanding', '-c', source, '-o', obj], env)
    run([Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld', '--image-base=0x10000', '-Ttext=0x20000', '-e', 'ss_deadline_init', obj, '-o', elf], env)
    undefined = subprocess.check_output(['llvm-nm-21', '-u', obj], text=True)
    assert not undefined.strip(), 'helper must have no allocator or atomic library dependencies'
    disassembly = subprocess.check_output(['llvm-objdump-21', '-d', elf], text=True)
    (build/'deadline-disassembly.txt').write_text(disassembly)
    assert 'ldaxr' not in disassembly and 'stlxr' not in disassembly
    # Armv8-a suffices: publication uses bounded acquire loads/release stores.
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(0x10000, 0x40000)
    with elf.open('rb') as file:
        image = ELFFile(file)
        for segment in image.iter_segments():
            if segment['p_type'] == 'PT_LOAD':
                uc.mem_write(segment['p_vaddr'], segment.data())
        symbols = {s.name: s['st_value'] for s in image.get_section_by_name('.symtab').iter_symbols()}
    uc.mem_map(0x100000, 0x20000)
    uc.mem_map(0x300000, 0x10000)
    uc.mem_map(0x400000, 0x1000)
    count = [0]
    def hook(machine, address, size, data):
        if address == 0x400000:
            machine.emu_stop()
        else:
            count[0] += 1
    uc.hook_add(UC_HOOK_CODE, hook)
    regs = [UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4]
    def call(name, *values):
        count[0] = 0
        for reg, value in zip(regs, values):
            uc.reg_write(reg, value)
        uc.reg_write(UC_ARM64_REG_SP, 0x30fff0)
        uc.reg_write(UC_ARM64_REG_X30, 0x400000)
        uc.emu_start(symbols[name], 0x400004, count=10000)
        assert uc.reg_read(UC_ARM64_REG_PC) == 0x400000, 'instruction bound exceeded'
        return uc.reg_read(UC_ARM64_REG_X0), count[0]
    heap, entries, result = 0x100000, 0x102000, 0x108000
    call('ss_deadline_init', heap)
    maxima = {}
    def sample(name, *values):
        value, instructions = call(name, *values)
        maxima[name] = max(maxima.get(name, 0), instructions)
        return value
    # Both insertion extremes, dense ties and every cancellation position run
    # actual compiled code; each operation must stay within 512 instructions.
    for mode in range(3):
        call('ss_deadline_init', heap)
        for i in range(128):
            entry = entries + 48*i
            call('ss_deadline_entry_init', entry)
            deadline = 128-i if mode == 0 else i if mode == 1 else 5
            assert sample('ss_deadline_insert', heap, entry, entry, i, deadline) == 1
        for i in range(128):
            assert sample('ss_deadline_snapshot', heap+1088, result) in (1, 2)
            assert sample('ss_deadline_pop_expired', heap, 2**64-1, result) == 1
        assert sample('ss_deadline_snapshot', heap+1088, result) == 1
    # Cancellation walks each possible root/middle/leaf position in dense heaps.
    for position in range(128):
        call('ss_deadline_init', heap)
        for i in range(128):
            entry = entries+48*i
            call('ss_deadline_entry_init', entry)
            assert sample('ss_deadline_insert', heap, entry, entry, i, i) == 1
        entry = struct.unpack('<Q', uc.mem_read(heap+8*position, 8))[0]
        generation = struct.unpack('<Q', uc.mem_read(entry+8, 8))[0]
        assert sample('ss_deadline_cancel', heap, entry, generation) == 1
    assert max(maxima.values()) <= 512, maxima
    print('AArch64 executed instruction maxima for selected dense paths:', maxima)
    print('Heap bound: seven parent/child levels per operation; expiry <=16 callbacks; snapshot <=4 loads, no retry.')


if __name__ == '__main__':
    main()
