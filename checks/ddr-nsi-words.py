#!/usr/bin/env python3
"""Check that the RAM-only DDR wrapper runs boot0's post-DRAM interconnect step, in boot0's order.

  ddr-nsi-words.py OUTPUT

Executes boot0 routine 0x49050 (pinned boot0, 829ea8e3...) under Unicorn from the pinned binary and records every
data store to the NSI block and every DMB: the nine stores must come in boot0's order with a DMB before each, with the
values 0x40a05, 0x40a05, 0x3f, 0x7f, 0xf, 0xf1, 0xf1, 1, 1 at the addresses that the DRAM interconnect study writes
(apps/dram-nsi/result.h). It then assembles boot/a733/ddr-entry.S in both variants and requires that the wrapper
calls 0x49bdd (init_DRAM), then, in the restore variant, the restore helper at 0x92008, and only then 0x49051 (the
Thumb entry of routine 0x49050), before it records the final stage. No device access.
Case that fails on the wrapper before this change: the wrapper never calls 0x49051.
"""
from pathlib import Path
import re
import shutil
import subprocess
import sys

from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UcError
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_SP, UC_ARM_REG_PC, UC_ARM_REG_R0

ROOT = Path(__file__).resolve().parents[1]
BOOT0 = Path.home() / '.cache/sunstoneos/sources/board-boot/boot0_sdcard_orangepizero3w.fex'
LLVM = Path('/usr/lib/llvm-21/bin')
BASE, ROUTINE, RETURN = 0x47000, 0x49051, 0x1000
ADDRESSES = (0x0202446c, 0x0202466c, 0x02023a14, 0x02023c14, 0x02023e14, 0x02024014, 0x02024214, 0x0202200c, 0x0202220c)
VALUES = (0x40a05, 0x40a05, 0x3f, 0x7f, 0xf, 0xf1, 0xf1, 1, 1)


def require(condition, message):
    if not condition:
        sys.exit('FAIL: ' + message)


def boot0_events():
    blob = BOOT0.read_bytes()
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    uc.mem_map(BASE, 0x3c000)
    uc.mem_write(BASE, blob)
    uc.mem_map(0x02020000, 0x10000)          # the NSI block, plain memory
    uc.mem_map(0x90000, 0x1000)              # stack
    uc.mem_map(RETURN & ~0xfff, 0x1000)
    events = []
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    from capstone import CS_MODE_THUMB
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)

    def code(uc_, address, size, _):
        raw = bytes(uc_.mem_read(address, size))
        for ins in md.disasm(raw, address):
            if ins.mnemonic == 'dmb':
                events.append(('dmb',))
        if address == 0x5d9ee:                # the barrier helper: not part of the order under test
            uc_.reg_write(UC_ARM_REG_PC, uc_.reg_read(UC_ARM_REG_LR) | 1)

    def write(uc_, access, address, size, value, _):
        if 0x02020000 <= address < 0x02030000:
            require(size == 4, 'NSI stores are word stores')
            events.append(('store', address, value & 0xffffffff))

    uc.hook_add(UC_HOOK_CODE, code)
    uc.hook_add(UC_HOOK_MEM_WRITE, write)
    uc.reg_write(UC_ARM_REG_SP, 0x90ff0)
    uc.reg_write(UC_ARM_REG_LR, RETURN | 1)
    try:
        uc.emu_start(ROUTINE, RETURN, count=200)
    except UcError as e:
        # The helper at 0x5d9ee is redirected; any other failure is a real one.
        require(False, f'boot0 routine did not run: {e}')
    return events, uc.reg_read(UC_ARM_REG_R0)


def check_order(events, result):
    require(result == 0, 'routine 0x49050 returns 0')
    stores = [e for e in events if e[0] == 'store']
    require([(a, v) for _, a, v in stores] == list(zip(ADDRESSES, VALUES)), f'store list: {stores}')
    # A DMB before every store, in program order.
    last = None
    for e in events:
        if e[0] == 'dmb':
            last = 'dmb'
        else:
            require(last == 'dmb', f'store to {e[1]:#x} without a DMB before it')
            last = 'store'
    header = (ROOT / 'apps/dram-nsi/result.h').read_text()
    addresses = [int(x, 16) for x in re.search(r'DN_WORD_ADDRESSES \{([^}]*)\}', header).group(1).replace(' ', '').split(',')]
    values = [int(x, 16) for x in re.search(r'DN_WORD_VALUES\s+\{([^}]*)\}', header).group(1).replace(' ', '').split(',')]
    require(tuple(addresses) == ADDRESSES and tuple(values) == VALUES, 'the study writes the words of boot0 in boot0 order')


def blx_targets(out, name, defines):
    obj, elf = out / f'{name}.o', out / f'{name}.elf'
    built = subprocess.run(['clang', '--target=arm-none-eabi', '-march=armv8-a', '-c', *defines,
                            str(ROOT / 'boot/a733/ddr-entry.S'), '-o', str(obj)], capture_output=True, text=True)
    require(built.returncode == 0, 'assembly: ' + built.stderr)
    linked = subprocess.run([shutil.which('ld.lld') or str(Path.home() / '.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'), '-Ttext=0x85000', '-e', '_start', str(obj), '-o', str(elf)], capture_output=True, text=True)
    require(linked.returncode == 0, 'link: ' + linked.stderr)
    binary = out / f'{name}.bin'
    require(subprocess.run([str(LLVM / 'llvm-objcopy'), '-O', 'binary', str(elf), str(binary)]).returncode == 0, 'objcopy')
    data = binary.read_bytes()
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.detail = False
    constants, targets = {}, []
    # The wrapper is ARM code; literals follow the code. Follow `ldr rN, [pc, #x]` and record each `blx rN`.
    for ins in md.disasm(data, 0x85000):
        if ins.mnemonic == 'ldr' and '[pc' in ins.op_str:
            reg = ins.op_str.split(',')[0]
            m = re.search(r'#(-?0x[0-9a-f]+|-?\d+)', ins.op_str)
            offset = int(m.group(1), 0) if m else 0
            at = ins.address + 8 + offset - 0x85000
            if 0 <= at <= len(data) - 4:
                constants[reg] = int.from_bytes(data[at:at + 4], 'little')
        elif ins.mnemonic == 'blx' and ins.op_str in constants:
            targets.append(constants[ins.op_str])
    return targets


def check_wrapper(out):
    plain = blx_targets(out, 'plain', [])
    require(0x49bdd in plain and 0x49051 in plain and plain.index(0x49bdd) < plain.index(0x49051), f'plain wrapper calls: {[hex(t) for t in plain]}')
    restore = blx_targets(out, 'restore', ['-Wa,--defsym,DDR_RESTORE=1'])
    require(all(t in restore for t in (0x49bdd, 0x92008, 0x49051)) and
            restore.index(0x49bdd) < restore.index(0x92008) < restore.index(0x49051), f'restore wrapper calls: {[hex(t) for t in restore]}')
    return plain, restore


if __name__ == '__main__':
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=False)
    events, result = boot0_events()
    check_order(events, result)
    plain, restore = check_wrapper(out)
    print('boot0 routine 0x49050: nine stores in order with a DMB before each; wrapper calls it after init_DRAM'
          ' (and after the restore helper); calls: ' + ' '.join(hex(t) for t in restore))
