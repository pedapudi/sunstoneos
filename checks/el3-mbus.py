#!/usr/bin/env python3
"""Check the EL3 MBUS restore against a register model and the pinned ROM and boot0 sequences.

  el3-mbus.py OUTPUT

Builds checks/el3-mbus.c with AddressSanitizer and UndefinedBehaviorSanitizer
and runs its cases: the ROM's 400 MHz state restored to boot0's 600 MHz with
the ROM's write order and 10 us settles, no write for any other state, and
each update wait bounded at 10 ms with the bus left gated on. It then checks
the instruction and literal words that include/a733_el3_mbus.h cites in the
physical boot ROM (a8e31821...) and boot0 (829ea8e3...), and compiles the
header for AArch64 to report its size. No device access."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'
ROM = CACHE / 'evidence/rom-copy-20261016-084055/physical-rom.bin'
ROM_SHA = 'a8e31821e8a1611d'
BOOT0 = CACHE / 'sources/board-boot/boot0_sdcard_orangepizero3w.fex'
BOOT0_SHA = '829ea8e3b65d71a02f2d0e094bc073de3a10869f948f62db49dbf023d07fabe3'
CLANG = '/usr/lib/llvm-21/bin/clang'
MBUS, MSI = '0x02002588', '0x02002594'
# The ROM's sequence with boot0's source: gate off, bridge reset, divider 1,
# source 1, bridge released, gate on (include/a733_el3_mbus.h).
EXPECTED = [[MBUS, '0x0c000000'], [MSI, '0x00010001'], [MBUS, '0x0c000000'], [MBUS, '0x09000000'],
            [MSI, '0x00030001'], [MBUS, '0x89000000']]


def require(condition, message):
    if not condition:
        sys.exit('FAIL: ' + message)


def run(command, **kwargs):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, **kwargs)
    return result


def word(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def references():
    rom = ROM.read_bytes()
    require(hashlib.sha256(rom).hexdigest().startswith(ROM_SHA), 'physical ROM changed')
    boot0 = BOOT0.read_bytes()
    require(hashlib.sha256(boot0).hexdigest() == BOOT0_SHA, 'boot0 changed')
    facts = {
        # Forced-FEL entry: r0 = 127, then the dispatcher when r6 == 92.
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
        # boot0's DRAM clock routine: the MBUS literal and the 0x89000000 write.
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
    model = out / 'el3-mbus-model'
    built = run([CLANG, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                 '-fno-sanitize-recover=all', '-I', ROOT / 'include', ROOT / 'checks/el3-mbus.c', '-o', model])
    require(built.returncode == 0, 'model build: ' + built.stderr)
    ran = run([model])
    (out / 'model.jsonl').write_text(ran.stdout)
    require(ran.returncode == 0, 'model cases failed: ' + ran.stdout + ran.stderr)
    cases = {c['case']: c for c in map(json.loads, ran.stdout.splitlines())}
    require(len(cases) == 15 and all(c['pass'] for c in cases.values()), 'model case list')
    for name in ('rom-state', 'rom-state-late-update', 'rom-state-slow-update'):
        c = cases[name]
        require([w[:2] for w in c['writes']] == EXPECTED, f'{name} write order {c["writes"]}')
        at = [w[2] for w in c['writes']]
        # 10 us (240 ticks) between the bridge reset and the divider write,
        # and after each of the divider and source writes.
        require(at[2] - at[1] >= 240 and at[3] - at[2] >= 240 and at[4] - at[3] >= 240, f'{name} settles {at}')
    for name in ('boot0-state', 'reset-default', 'other-divider', 'dfs-enabled', 'update-pending', 'gate-off',
                 'bridge-reset'):
        require(not cases[name]['writes'], f'{name} wrote')
    for name, step in (('stuck-gate-off', 1), ('stuck-divider', 3), ('stuck-source', 4), ('stuck-gate-on', 6)):
        c = cases[name]
        require(c['result'] == step and c['ticks'] >= 240000 and c['ticks'] < 300000, f'{name} bound {c}')
        require(c['writes'][-2:] == [[MSI, '0x00030001', c['writes'][-2][2]],
                                     [MBUS, c['writes'][-1][1], c['writes'][-1][2]]] and
                int(c['writes'][-1][1], 16) & 0x88000000 == 0x88000000, f'{name} recovery writes')
    require(cases['source-ignored']['result'] == 7, 'read-back refusal')
    facts = references()
    # AArch64 size of the restore as the EL3 callers compile it.
    tu = out / 'restore.c'
    tu.write_text('#include "a733_el3_mbus.h"\nunsigned restore(void) { return a733_el3_mbus_restore(); }\n')
    obj = out / 'restore.o'
    built = run([CLANG, '--target=aarch64-none-elf', '-march=armv8-a', '-mgeneral-regs-only', '-ffreestanding',
                 '-O2', '-Wall', '-Wextra', '-Werror', '-I', ROOT / 'include', '-c', tu, '-o', obj])
    require(built.returncode == 0, 'AArch64 build: ' + built.stderr)
    size = run(['/usr/lib/llvm-21/bin/llvm-size', '-A', obj]).stdout
    text = next(int(line.split()[1]) for line in size.splitlines() if line.startswith('.text'))
    require(text <= 512, f'restore text {text} bytes over the 512-byte budget')
    result = {'physical_access': False, 'cases': sorted(cases), 'reference_words': facts, 'aarch64_text_bytes': text,
              'sources': {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest()
                          for p in ('include/a733_el3_mbus.h', 'checks/el3-mbus.c', 'checks/el3-mbus.py')}}
    (out / 'result.json').write_text(json.dumps(result, indent=1) + '\n')
    print(f'PASS el3-mbus: {len(cases)} model cases, {len(facts)} reference words, {text} bytes of AArch64 text')


if __name__ == '__main__':
    main()
