#!/usr/bin/env python3
"""Check the EL3 MBUS restore: its admission, its SRAM window and the pinned ROM and boot0 sequences.

  el3-mbus.py OUTPUT

Builds checks/el3-mbus.c with AddressSanitizer and UndefinedBehaviorSanitizer
and runs its cases: the restore changes only the ROM's 400 MHz state, refuses
with no window while the GPU or NPU is clocked, DMA0 is busy or CE has a task
loaded, and refuses a wrong read-back. It assembles boot/a733/mbus-window.S,
requires its bytes to equal a733_mbus_window_code[] in
include/a733_el3_mbus.h, and executes those bytes under Unicorn at the SRAM
address with only SRAM and the two registers mapped and no stack: the ROM's
write order with boot0's source, the 10 us settles, and each update wait
bounded at 10 ms with the bridge released and the gate turned on. It then
checks the instruction and literal words that the header cites in the
physical boot ROM (a8e31821...) and boot0 (829ea8e3...), and compiles the
restore for AArch64 to report its size. No device access."""
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'
ROM = CACHE / 'evidence/rom-copy-20261016-084055/physical-rom.bin'
ROM_SHA = 'a8e31821e8a1611d'
BOOT0 = CACHE / 'sources/board-boot/boot0_sdcard_orangepizero3w.fex'
BOOT0_SHA = '829ea8e3b65d71a02f2d0e094bc073de3a10869f948f62db49dbf023d07fabe3'
LLVM = Path('/usr/lib/llvm-21/bin')
MBUS, MSI, SRAM, RETURN = 0x02002588, 0x02002594, 0x0005f000, 0x00010000
# The ROM's sequence with boot0's source: gate off, bridge reset, divider 1,
# source 1, bridge released, gate on (boot/a733/mbus-window.S).
EXPECTED = [(MBUS, 0x0c000000), (MSI, 0x00010001), (MBUS, 0x0c000000), (MBUS, 0x09000000),
            (MSI, 0x00030001), (MBUS, 0x89000000)]


def require(condition, message):
    if not condition:
        sys.exit('FAIL: ' + message)


def run(command):
    return subprocess.run(list(map(str, command)), capture_output=True, text=True)


def word(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def model_cases(out):
    model = out / 'el3-mbus-model'
    built = run([LLVM / 'clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                 '-fno-sanitize-recover=all', '-I', ROOT / 'include', ROOT / 'checks/el3-mbus.c', '-o', model])
    require(built.returncode == 0, 'model build: ' + built.stderr)
    ran = run([model])
    (out / 'model.jsonl').write_text(ran.stdout)
    require(ran.returncode == 0, 'model cases failed: ' + ran.stdout + ran.stderr)
    cases = {c['case']: c for c in map(json.loads, ran.stdout.splitlines())}
    require(len(cases) == 22 and all(c['pass'] for c in cases.values()), 'model case list')
    return sorted(cases)


def window_code(out):
    obj = out / 'mbus-window.o'
    built = run([LLVM / 'clang', '--target=aarch64-none-elf', '-march=armv8-a', '-c', ROOT / 'boot/a733/mbus-window.S',
                 '-o', obj])
    require(built.returncode == 0, 'window assembly: ' + built.stderr)
    relocations = run([LLVM / 'llvm-objdump', '-r', obj]).stdout
    require('R_AARCH64' not in relocations, 'the window is not position-independent')
    binary = out / 'mbus-window.bin'
    require(run([LLVM / 'llvm-objcopy', '-O', 'binary', '-j', '.text', obj, binary]).returncode == 0, 'objcopy')
    code = binary.read_bytes()
    header = (ROOT / 'include/a733_el3_mbus.h').read_text()
    body = header[header.index('a733_mbus_window_code[] = {'):].split('};')[0]
    words = [int(x, 16) for x in re.findall(r'0x([0-9a-f]{8})u', body)]
    require(struct.pack(f'<{len(words)}I', *words) == code, 'header words differ from boot/a733/mbus-window.S')
    return code


def execute(code, clear_after=1, stuck=0, tick_step=1):
    """Run the window's bytes at SRAM; return (result, writes, ticks, final MBUS)."""
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
    from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_PC
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(SRAM, 0x1000)
    uc.mem_write(SRAM, code)
    uc.mem_map(0x02002000, 0x1000)
    uc.mem_map(RETURN, 0x1000)
    state = {'mbus': 0x84000000, 'msi': 0x00030001, 'ticks': 1000, 'updates': 0, 'since': 0, 'writes': []}

    def store():
        uc.mem_write(MBUS, struct.pack('<I', state['mbus']))
        uc.mem_write(MSI, struct.pack('<I', state['msi']))

    def on_read(uc, access, address, size, value, unused):
        require(address in (MBUS, MSI) and size == 4, f'read outside the two registers: {address:#x}')
        if address == MBUS and state['mbus'] & (1 << 27):
            state['since'] += 1
            if state['updates'] != stuck and state['since'] >= clear_after:
                state['mbus'] &= ~(1 << 27)
        store()

    def on_write(uc, access, address, size, value, unused):
        require(address in (MBUS, MSI) and size == 4, f'write outside the two registers: {address:#x}')
        state['writes'].append((address, value, state['ticks']))
        if address == MBUS:
            state['mbus'] = value
            state['since'] = 0
            if value & (1 << 27):
                state['updates'] += 1
        else:
            state['msi'] = value

    def on_counter(uc, address, size, unused):
        rt = word(bytes(uc.mem_read(address, 4)), 0) & 31
        state['ticks'] += tick_step
        uc.reg_write(UC_ARM64_REG_X0 + rt, state['ticks'])
        uc.reg_write(UC_ARM64_REG_PC, address + 4)

    for offset in range(0, len(code), 4):
        if word(code, offset) & 0xffffffe0 == 0xd53be020:  # mrs xN, cntpct_el0
            uc.hook_add(UC_HOOK_CODE, on_counter, None, SRAM + offset, SRAM + offset)
    uc.hook_add(UC_HOOK_MEM_READ, on_read)
    uc.hook_add(UC_HOOK_MEM_WRITE, on_write)
    store()
    uc.reg_write(UC_ARM64_REG_X0, state['msi'])
    uc.reg_write(UC_ARM64_REG_LR, RETURN)
    uc.reg_write(UC_ARM64_REG_SP, 0x8)  # unmapped: any stack access faults
    uc.emu_start(SRAM, RETURN, count=20_000_000)
    require(uc.reg_read(UC_ARM64_REG_PC) == RETURN, 'the window did not return')
    return (uc.reg_read(UC_ARM64_REG_X0) & 0xffffffff, state['writes'], state['ticks'] - 1000,
            state['mbus'] & ~(1 << 27), state['msi'])


def window_cases(code):
    rows = {}
    for name, after in (('rom-state', 1), ('rom-state-late-update', 10), ('rom-state-slow-update', 1000)):
        result, writes, ticks, mbus, msi = execute(code, clear_after=after)
        require(result == 0 and mbus == 0x81000000 and msi == 0x00030001, f'{name}: {result} {mbus:#x}')
        require([w[:2] for w in writes] == EXPECTED, f'{name} write order {[(hex(a), hex(v)) for a, v, _ in writes]}')
        at = [w[2] for w in writes]
        require(at[2] - at[1] >= 240 and at[3] - at[2] >= 240 and at[4] - at[3] >= 240, f'{name} settles {at}')
        rows[name] = {'result': result, 'ticks': ticks}
    for name, stuck, step in (('stuck-gate-off', 1, 1), ('stuck-divider', 2, 3), ('stuck-source', 3, 4),
                              ('stuck-gate-on', 4, 6)):
        result, writes, ticks, mbus, msi = execute(code, stuck=stuck, tick_step=16)
        require(result == step and 240000 <= ticks < 260000, f'{name}: result {result}, {ticks} ticks')
        require(writes[-2][:2] == (MSI, 0x00030001) and writes[-1][0] == MBUS and
                writes[-1][1] & 0x88000000 == 0x88000000 and msi == 0x00030001, f'{name} recovery writes')
        rows[name] = {'result': result, 'ticks': ticks}
    return rows


def references():
    rom = ROM.read_bytes()
    require(hashlib.sha256(rom).hexdigest().startswith(ROM_SHA), 'physical ROM changed')
    boot0 = BOOT0.read_bytes()
    require(hashlib.sha256(boot0).hexdigest() == BOOT0_SHA, 'boot0 changed')
    facts = {
        'rom 0x224 mov r0, #127': word(rom, 0x224) == 0xe3a0007f,
        'rom 0x22c beq 0x2514': word(rom, 0x22c) == 0x0a0008b8,
        'rom 0x252c cmp r4, #127': word(rom, 0x252c) == 0xe354007f,
        'rom 0x2534 bl 0x2444': word(rom, 0x2534) == 0xebffffc2,
        'rom 0x2170 bl 0x2084': word(rom, 0x2170) == 0xebffffc3,
        'rom literal MBUS_CLK': word(rom, 0x2154) == 0x02002588,
        'rom literal update bound 240000': word(rom, 0x2158) == 240000,
        'rom literal MSI_LITE0_BGR': word(rom, 0x215c) == 0x02002594,
        'rom gate off with update': word(rom, 0x2094) == 0xe3c44102 and word(rom, 0x2098) == 0xe3844302,
        'rom bridge reset bit 17 cleared': word(rom, 0x20b4) == 0xe3c44802,
        'rom divider cleared': word(rom, 0x20cc) == 0xe3c4401f,
        'rom source 4': word(rom, 0x20f8) == 0xe3c44407 and word(rom, 0x20fc) == 0xe3844301,
        'rom bridge released': word(rom, 0x2128) == 0xe3844802,
        'rom gate on with update': word(rom, 0x2138) == 0xe3844102 and word(rom, 0x213c) == 0xe3844302,
        'boot0 literal MBUS_CLK at 0x57b5c': word(boot0, 0x57b5c - 0x47000) == 0x02002588,
        'boot0 orr 0x89000000 at 0x57b24': boot0[0x57b24 - 0x47000:0x57b28 - 0x47000] == bytes.fromhex('43f00943'),
    }
    for name, ok in facts.items():
        require(ok, 'reference word: ' + name)
    return sorted(facts)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=False)
    cases = model_cases(out)
    code = window_code(out)
    windows = window_cases(code)
    facts = references()
    tu = out / 'restore.c'
    tu.write_text('#include "a733_el3_mbus.h"\nunsigned restore(void) { return a733_el3_mbus_restore(); }\n')
    obj = out / 'restore.o'
    built = run([LLVM / 'clang', '--target=aarch64-none-elf', '-march=armv8-a', '-mgeneral-regs-only',
                 '-ffreestanding', '-O2', '-Wall', '-Wextra', '-Werror', '-I', ROOT / 'include', '-c', tu, '-o', obj])
    require(built.returncode == 0, 'AArch64 build: ' + built.stderr)
    sizes = {line.split()[0]: int(line.split()[1]) for line in run([LLVM / 'llvm-size', '-A', obj]).stdout.splitlines()
             if line.startswith('.')}
    text, rodata = sizes.get('.text', 0), sizes.get('.rodata', 0)
    require(text <= 512 and rodata <= 512 and len(code) <= 512, f'restore {text} + {rodata} bytes, window {len(code)}')
    result = {'physical_access': False, 'model_cases': cases, 'window_cases': windows, 'reference_words': facts,
              'window_bytes': len(code), 'aarch64_text_bytes': text, 'aarch64_rodata_bytes': rodata,
              'sources': {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest()
                          for p in ('include/a733_el3_mbus.h', 'boot/a733/mbus-window.S', 'checks/el3-mbus.c',
                                    'checks/el3-mbus.py')}}
    (out / 'result.json').write_text(json.dumps(result, indent=1) + '\n')
    print(f'PASS el3-mbus: {len(cases)} model cases, {len(windows)} SRAM window cases, {len(facts)} reference words, '
          f'window {len(code)} bytes, restore {text} + {rodata} bytes of AArch64')


if __name__ == '__main__':
    main()
