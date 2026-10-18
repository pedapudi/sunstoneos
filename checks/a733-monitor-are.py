#!/usr/bin/env python3
"""Execute monitor admission/cleanup and reject every enabled ARE-to-zero write."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_LR

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
for name in ('build', 'output', 'clang', 'lld', 'objdump'):
    p.add_argument('--' + name, type=Path, required=True)
a = p.parse_args()
a.build.mkdir(parents=True, exist_ok=False)
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
def command(args):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True)
    with (a.build / 'commands.log').open('a') as stream:
        stream.write(json.dumps(list(map(str, args))) + '\n' + result.stdout + result.stderr)
    assert result.returncode == 0, result.stderr
    return result.stdout

(a.build / 'reset.S').write_text('.text\n.global monitor_reset\nmonitor_reset:\n brk #0\n')
command([a.clang, '--target=aarch64-none-elf', '-c', a.build / 'reset.S', '-o', a.build / 'reset.o'])
results = []
for model in (False, True):
    name = 'qemu' if model else 'physical'
    flags = ['-DMONITOR_QEMU=1'] if model else []
    command([a.clang, '--target=aarch64-none-elf', '-march=armv8-a', '-mgeneral-regs-only', '-ffreestanding',
             '-fno-stack-protector', '-fno-unwind-tables', '-O2', '-g', '-Wall', '-Wextra', *flags,
             '-I' + str(ROOT / 'platform/a733'), '-c', ROOT / 'platform/a733/monitor.c', '-o', a.build / (name + '.o')])
    elfpath = a.build / (name + '.elf')
    command([a.lld, '-Ttext=0x41000000', '-e', 'monitor_init', a.build / (name + '.o'), a.build / 'reset.o', '-o', elfpath])
    with elfpath.open('rb') as stream:
        elf = ELFFile(stream)
        symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
        segments = [(s['p_vaddr'], s.data()) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
    ops = {}
    for line in command([a.objdump, '-d', '--no-show-raw-insn', elfpath]).splitlines():
        m = re.match(r'^\s*([0-9a-f]+):\s+(.+)$', line)
        if m:
            ops[int(m[1], 16)] = m[2].strip()
    cases = ((0, False), (0x10, False), (0x20, False), (0x30, False), (0x37, False)) if not model else ((0, False), (0, True))
    for original, early_timeout in cases:
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        gicd, gicr = (0x08000000, 0x080a0000) if model else (0x03400000, 0x03460000)
        for start, size in ((0x41000000, 0x100000), (0x42000000, 0x10000), (gicd, 0x1000), (gicr, 0x20000)):
            cpu.mem_map(start, size)
        for address, data in segments:
            # ELF headers can occupy the preceding page in the standalone fixture.
            if address < 0x41000000:
                cpu.mem_map(address & ~4095, 4096)
            cpu.mem_write(address, data)
        cpu.reg_write(UC_ARM64_REG_SP, 0x42010000)
        cpu.reg_write(UC_ARM64_REG_LR, 0x410f0000)
        ctlr = [original]
        ticks = [100]
        phase = ['init']
        writes = []
        stopped = []
        sysregs = {'esr_el3': 0x17 << 26, 'elr_el3': 0, 'far_el3': 0}
        def instruction(uc, address, size, unused):
            if address == 0x410f0000:
                stopped.append('initialized')
                uc.emu_stop()
                return
            if address == symbols['monitor_reset']:
                stopped.append('reset')
                uc.emu_stop()
                return
            text = ops.get(address, '')
            read = re.match(r'mrs\s+x(\d+),\s*(\S+)', text, re.I)
            if read:
                key = read[2].lower()
                if key == 'cntpct_el0':
                    ticks[0] += 1000000 if early_timeout else 1
                    value = ticks[0]
                else:
                    assert key in sysregs, key
                    value = sysregs[key]
                uc.reg_write(UC_ARM64_REG_X0 + int(read[1]), value)
                uc.reg_write(UC_ARM64_REG_PC, address + 4)
            elif text.lower().startswith('msr'):
                uc.reg_write(UC_ARM64_REG_PC, address + 4)
            elif text.startswith('hlt'):
                if uc.reg_read(UC_ARM64_REG_X0) == 0x20:
                    stopped.append('semihost-exit')
                    uc.emu_stop()
                else:
                    uc.reg_write(UC_ARM64_REG_PC, address + 4)
        def read_mmio(uc, access, address, size, value, unused):
            if address == gicd:
                data = ctlr[0] | (1 << 31 if early_timeout and writes else 0)
            elif address == gicd + 4:
                data = 1  # Two distributor banks, including the private bank.
            elif address == gicr + 8:
                data = 16  # Boot CPU affinity zero.
            elif address == gicr or address == gicr + 20:
                data = 0
            else:
                return
            uc.mem_write(address, data.to_bytes(size, 'little'))
        def write_mmio(uc, access, address, size, value, unused):
            if not (gicd <= address < gicd + 4096 or gicr <= address < gicr + 0x20000):
                return
            writes.append([phase[0], address, size, value])
            if address == gicd:
                assert not ((ctlr[0] & 0x30) & ~value), 'ARE must never transition from one to zero.'
                ctlr[0] = value & ~(1 << 31)
        cpu.hook_add(UC_HOOK_CODE, instruction)
        cpu.hook_add(UC_HOOK_MEM_READ, read_mmio)
        cpu.hook_add(UC_HOOK_MEM_WRITE, write_mmio)
        cpu.emu_start(symbols['monitor_init'], 0, count=100000)
        refused = not model and original & 0x30 != 0x30
        if not refused and not early_timeout:
            assert stopped == ['initialized']
            phase[0] = 'finish'
            cpu.reg_write(UC_ARM64_REG_X0, 0x733)
            cpu.reg_write(UC_ARM64_REG_X0 + 1, 0)
            cpu.reg_write(UC_ARM64_REG_X0 + 2, 1)
            cpu.emu_start(symbols['monitor_exception'], 0, count=100000)
        assert stopped[-1] == ('semihost-exit' if model else 'reset')
        status = int.from_bytes(cpu.mem_read(0x42000010, 8), 'little')
        assert status == (4 if refused or early_timeout else 0)
        if refused:
            assert not writes and ctlr[0] == original
        elif early_timeout:
            assert not ctlr[0] & 0x30
        else:
            assert ctlr[0] == (0x30 if model else original)
        results.append({'mode': name, 'initial_ctlr': original, 'early_timeout': early_timeout,
                        'status': status, 'final_ctlr': ctlr[0], 'writes': writes})
a.output.write_text(json.dumps({'physical_access': False, 'passed': True, 'cases': results,
    'sources': {name: digest(ROOT / name) for name in ('platform/a733/monitor.c', 'platform/a733/boot.h', 'checks/a733-monitor-are.py')},
    'tools': {str(path): digest(path) for path in (a.clang, a.lld, a.objdump)},
    'limits': 'Actual monitor instructions with modeled system registers and MMIO. Tests ARE admission and monotonic enable bits; does not qualify GIC timeout, redistributor or ROM-reset cleanup.'}, indent=2) + '\n')
print('PASS seven monitor ARE admission/cleanup instruction cases.')
