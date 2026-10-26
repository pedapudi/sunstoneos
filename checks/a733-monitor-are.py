#!/usr/bin/env python3
"""Execute monitor ARE admission, the EL3 security setup and bounded GIC restoration with injected faults.

The physical build must make the vendor BL31's cold-boot security writes
(include/a733_el3_security.h) in their order before EL1 entry, the DMA
security words only when the controller's bus gate and reset are open; the
QEMU build makes none. Before them, a start that finds MBUS at the boot ROM's
400 MHz restores boot0's 600 MHz (include/a733_el3_mbus.h) from SRAM; with a
bus master active it skips the restore, logs the code and continues; with any
other MBUS value it makes no MBUS write."""
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
p.add_argument('--monitor-source', type=Path, default=ROOT/'platform/a733/monitor.c')
p.add_argument('--case', action='append', help='Run only named cases; used to reproduce the old cleanup failure.')
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
# BL31 sunxi_security_setup, cold-boot writes 9 to 17 (docs/designs/gpu-vendor-default.md).
SECURITY_PAGES = (0x02054000, 0x07002000, 0x07003000, 0x07010000, 0x02003000, 0x02002000, 0x04601000, 0x04024000)
def security_writes(dma_open):
    rows = [(0x02054004 + 0x10 * n, 0xffffffff) for n in list(range(14)) + list(range(16, 24))]
    rows += [(0x07003000 + 4 * n, 0) for n in range(3)] + [(0x07002004 + 0x10 * n, 0xffffffff) for n in range(3)]
    rows += [(0x07010290, 0x17), (0x02003f00, 7)]
    rows += [(0x04601020, 0xffff), (0x04024020, 0xffff)] if dma_open else []
    return rows + [(0x020540e0, 0xfffffbff)]
# The MBUS restore from the ROM's 0x84000000 (checks/el3-mbus.py has the order).
MBUS_WRITES = [(0x02002588, 0x0c000000), (0x02002594, 0x10001), (0x02002588, 0x0c000000),
               (0x02002588, 0x09000000), (0x02002594, 0x30001), (0x02002588, 0x89000000)]
for model in (False, True):
    name = 'qemu' if model else 'physical'
    flags = ['-DMONITOR_QEMU=1'] if model else []
    command([a.clang, '--target=aarch64-none-elf', '-march=armv8-a', '-mgeneral-regs-only', '-ffreestanding',
             '-fno-stack-protector', '-fno-unwind-tables', '-O2', '-g', '-Wall', '-Wextra', *flags,
             '-I' + str(ROOT / 'platform/a733'), '-I' + str(ROOT / 'include'), '-c', a.monitor_source,
             '-o', a.build / (name + '.o')])
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
    faults = ('dist-disable-rwp', 'dist-disable-dropped', 'local-disable-rwp',
              'local-disable-dropped', 'pending-clear-dropped', 'active-clear-dropped',
              'group-restore-dropped', 'modifier-restore-dropped', 'config-restore-dropped',
              'priority-restore-dropped', 'local-enable-restore-dropped', 'spi-disable-rwp',
              'spi-disable-dropped', 'spi-enable-restore-dropped', 'banks-changed',
              'final-ctlr-dropped', 'final-rwp', 'dist-read-at-deadline',
              'local-read-at-deadline', 'spi-read-after-deadline', 'final-read-at-deadline')
    cases = [(f'are-{value:x}', value, False, '', 0) for value in (0, 0x10, 0x20, 0x30, 0x37)] + [
        ('are-37-mbus-rom', 0x37, False, '', 0), ('are-37-mbus-gpu-clocked', 0x37, False, '', 0),
        ('are-37-mbus-dma-busy', 0x37, False, '', 0)] if not model else [
        ('qemu-cold', 0, False, '', 0), ('qemu-init-timeout', 0, True, '', 0)]
    if not model:
        cases += [('cleanup-' + fault, 0x37, False, fault, 0) for fault in faults]
        cases += [('cleanup-delayed-rwp', 0x37, False, 'delayed-rwp', 0),
                  ('controlled-timeout', 0x37, False, '', 2),
                  ('cleanup-overrides-timeout', 0x37, False, 'priority-restore-dropped', 2),
                  ('cleanup-overrides-client-error', 0x37, False, 'priority-restore-dropped', 4)]
    for case, original, early_timeout, fault, requested_status in cases:
        if a.case and case not in a.case:
            continue
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        gicd, gicr = (0x08000000, 0x080a0000) if model else (0x03400000, 0x03460000)
        sgi = gicr + 0x10000
        for start, size in ((0x41000000, 0x100000), (0x42000000, 0x10000), (gicd, 0x1000), (gicr, 0x20000)):
            cpu.mem_map(start, size)
        for address, data in segments:
            if address < 0x41000000:
                cpu.mem_map(address & ~4095, 4096)
            cpu.mem_write(address, data)
        for page in SECURITY_PAGES:
            cpu.mem_map(page, 4096)
        # PRCM_SEC_SWITCH_REG holds 0x10 to show the OR keeps other bits; the
        # DMA controllers are gated and in reset, as FEL leaves them, except
        # in are-37, where an earlier image left both running.
        dma_open = case.startswith('are-37')
        cpu.mem_write(0x07010290, (0x10).to_bytes(4, 'little'))
        for bgr in (0x02002704, 0x0200270c):
            cpu.mem_write(bgr, (0x10001 if dma_open else 0).to_bytes(4, 'little'))
        # MBUS at the ROM's value in the -mbus-rom case; zero, which the
        # restore leaves alone, in every other case. Its update bit clears
        # before the next read.
        # The restore copies its window to SRAM and runs it there.
        mbus_case = '-mbus-' in case
        mbus_rom = case.endswith('-mbus-rom')
        mbus_refused = 8 if case.endswith('-gpu-clocked') else 9 if case.endswith('-dma-busy') else 0
        cpu.mem_map(0x0005f000, 0x1000)
        if mbus_case:
            cpu.mem_write(0x02002588, (0x84000000).to_bytes(4, 'little'))
            cpu.mem_write(0x02002594, (0x30001).to_bytes(4, 'little'))
        if mbus_refused == 8:
            cpu.mem_write(0x02002b20, (0x80000000).to_bytes(4, 'little'))
        if mbus_refused == 9:
            cpu.mem_write(0x04601030, (1).to_bytes(4, 'little'))
        def read_mbus(uc, access, address, size, value, unused):
            word = int.from_bytes(uc.mem_read(0x02002588, 4), 'little')
            uc.mem_write(0x02002588, (word & ~(1 << 27)).to_bytes(4, 'little'))
        cpu.hook_add(UC_HOOK_MEM_READ, read_mbus, None, 0x02002588, 0x0200258b)
        security = []
        def write_security(uc, access, address, size, value, unused):
            assert size == 4 and phase[0] == 'init', (hex(address), size, phase[0])
            security.append((address, value))
        for page in SECURITY_PAGES:
            cpu.hook_add(UC_HOOK_MEM_WRITE, write_security, None, page, page + 4095)
        cpu.reg_write(UC_ARM64_REG_SP, 0x42010000)
        cpu.reg_write(UC_ARM64_REG_LR, 0x410f0000)
        implemented = 0xfff0ffff  # A733 has unimplemented PPIs 16..19.
        registers = {gicd: original, gicd+4: 8, gicr: 0, gicr+8: 16, gicr+20: 0,
                     sgi+0x80: 0x80000001, sgi+0xd00: 0x40000000,
                     sgi+0x100: 0x80000004, sgi+0x200: 0, sgi+0x300: 0,
                     sgi+0xc04: 0x88000000, sgi+0x41c: 0x6080a0c0}
        for bank in range(1, 9):
            registers[gicd+0x100+4*bank] = 0x10000000 if bank == 5 else 0
        saved_registers = registers.copy()
        ticks = [100]
        phase = ['init']
        cleanup_phase = ['']
        phase_deadline = [0]
        late = [None]
        stalled = [False]
        fault_applied = []
        delayed = {}
        writes, reads, stopped = [], [], []
        instructions = [0, 0]
        sysregs = {'esr_el3': 0x17 << 26, 'elr_el3': 0, 'far_el3': 0}
        def instruction(uc, address, size, unused):
            instructions[phase[0] == 'finish'] += 1
            if address == 0x410f0000:
                stopped.append('initialized'); uc.emu_stop(); return
            if address == symbols['monitor_reset']:
                stopped.append('reset'); uc.emu_stop(); return
            text = ops.get(address, '')
            if 0x0005f000 <= address < 0x00060000:
                # The SRAM window's own counter reads (mrs xN, cntpct_el0).
                code = int.from_bytes(uc.mem_read(address, 4), 'little')
                text = f'mrs x{code & 31}, cntpct_el0' if code & 0xffffffe0 == 0xd53be020 else ''
            read = re.match(r'mrs\s+x(\d+),\s*(\S+)', text, re.I)
            if read:
                key = read[2].lower()
                if key == 'cntpct_el0':
                    if late[0] is not None:
                        ticks[0] = late[0]; late[0] = None
                    else:
                        ticks[0] += 1000000 if early_timeout or stalled[0] else 1
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
                    stopped.append('semihost-exit'); uc.emu_stop()
                else:
                    uc.reg_write(UC_ARM64_REG_PC, address + 4)
        def read_mmio(uc, access, address, size, value, unused):
            if not (gicd <= address < gicd+4096 or gicr <= address < gicr+0x20000):
                return
            reads.append([phase[0], address, size])
            word = address & ~3
            data = registers.get(word, 0)
            if address == gicr+8:
                data = 16
            if phase[0] == 'finish':
                if address == gicd+4:
                    cleanup_phase[0] = 'spi'; phase_deadline[0] = ticks[0]+2400000
                    if fault == 'banks-changed':
                        data = 7; fault_applied.append(fault)
                stage = cleanup_phase[0]
                if address == gicd and ((fault == 'dist-disable-rwp' and stage == 'dist') or
                       (fault == 'spi-disable-rwp' and stage == 'spi') or
                       (fault == 'final-rwp' and stage == 'final')):
                    data |= 1 << 31; stalled[0] = True; fault_applied.append(fault)
                if address == gicr and fault == 'local-disable-rwp' and stage == 'local':
                    data |= 8; stalled[0] = True; fault_applied.append(fault)
                if fault == 'delayed-rwp' and address in (gicd, gicr) and stage:
                    key = (address, stage)
                    delayed[key] = delayed.get(key, 0)+1
                    if delayed[key] <= 3:
                        data |= (1 << 31) if address == gicd else 8
                        fault_applied.append(fault)
                timed_read = ((fault == 'dist-read-at-deadline' and address == gicd and stage == 'dist') or
                    (fault == 'local-read-at-deadline' and address == sgi+0x41c and stage == 'local') or
                    (fault == 'spi-read-after-deadline' and address == gicd+0x114 and stage == 'spi') or
                    (fault == 'final-read-at-deadline' and address == gicd and stage == 'final'))
                if timed_read:
                    late[0] = phase_deadline[0]+(fault == 'spi-read-after-deadline')
                    fault_applied.append(fault)
            if address == gicd and early_timeout and writes: data |= 1 << 31
            uc.mem_write(address, ((data >> (8*(address-word))) & ((1 << (size*8))-1)).to_bytes(size, 'little'))
        def write_mmio(uc, access, address, size, value, unused):
            if not (gicd <= address < gicd+4096 or gicr <= address < gicr+0x20000): return
            writes.append([phase[0], address, size, value])
            if phase[0] == 'finish':
                if address == gicd:
                    if not cleanup_phase[0]:
                        cleanup_phase[0] = 'dist'; phase_deadline[0] = ticks[0]+2400000
                    else: cleanup_phase[0] = 'final'
                if address == sgi+0x180:
                    cleanup_phase[0] = 'local'; phase_deadline[0] = ticks[0]+2400000
                if address in (sgi+0x80, sgi+0xd00, sgi+0xc04):
                    # Dropped disables must be detected before changing group/configuration.
                    assert not registers[gicd] & 7, 'Configuration with distributor groups enabled'
                    assert not registers[sgi+0x100], 'Configuration with local sources enabled'
                dropped = {'dist-disable-dropped': (gicd, 'dist'),
                    'local-disable-dropped': (sgi+0x180, 'local'),
                    'pending-clear-dropped': (sgi+0x280, 'local'),
                    'active-clear-dropped': (sgi+0x380, 'local'),
                    'group-restore-dropped': (sgi+0x80, 'local'),
                    'modifier-restore-dropped': (sgi+0xd00, 'local'),
                    'config-restore-dropped': (sgi+0xc04, 'local'),
                    'priority-restore-dropped': (sgi+0x41c, 'local'),
                    'local-enable-restore-dropped': (sgi+0x100, 'local'),
                    'spi-disable-dropped': (gicd+0x18c, 'spi'),
                    'spi-enable-restore-dropped': (gicd+0x114, 'spi'),
                    'final-ctlr-dropped': (gicd, 'final')}
                if dropped.get(fault) == (address, cleanup_phase[0]):
                    fault_applied.append(fault); return
            if address == gicd:
                assert not ((registers[gicd] & 0x30) & ~value), 'ARE must never transition from one to zero.'
                registers[gicd] = value & ~(1 << 31); return
            word = address & ~3
            shift = 8*(address-word)
            value = (registers.get(word, 0) & ~(((1 << (size*8))-1) << shift)) | (value << shift)
            if word in (sgi+0x80, sgi+0xd00): value &= implemented
            if word == sgi+0xc04: value &= 0xaaaaaa00
            if word == sgi+0x41c: value &= 0xf8f8f8f8
            if word in (sgi+0x100, sgi+0x200, sgi+0x300):
                registers[word] |= value & implemented
            elif word in (sgi+0x180, sgi+0x280, sgi+0x380):
                registers[word-0x80] &= ~(value & implemented)
            elif gicd+0x104 <= word <= gicd+0x120:
                registers[word] |= value
            elif gicd+0x184 <= word <= gicd+0x1a0:
                registers[word-0x80] &= ~value
            else: registers[word] = value
        cpu.hook_add(UC_HOOK_CODE, instruction)
        cpu.hook_add(UC_HOOK_MEM_READ, read_mmio)
        cpu.hook_add(UC_HOOK_MEM_WRITE, write_mmio)
        cpu.emu_start(symbols['monitor_init'], 0, count=100000)
        refused = not model and original & 0x30 != 0x30
        cleanup_started = ticks[0]
        if not refused and not early_timeout:
            assert stopped == ['initialized'], (case, stopped)
            phase[0] = 'finish'
            # Model the kernel's tick and a peripheral source, then the owned
            # timer state at exception entry. The cleanup itself executes.
            registers[sgi+0x100] |= 1 << 30
            registers[sgi+0x200] |= (1 << 29) | (1 << 30)
            registers[sgi+0x300] |= (1 << 29) | (1 << 30)
            registers[sgi+0x41c] = (registers[sgi+0x41c] & ~0xff0000) | 0xf00000
            registers[gicd+0x10c] |= 0x80
            cpu.reg_write(UC_ARM64_REG_X0, 0x733)
            cpu.reg_write(UC_ARM64_REG_X0 + 1, requested_status)
            cpu.reg_write(UC_ARM64_REG_X0 + 2, 1)
            cpu.emu_start(symbols['monitor_exception'], 0, count=100000)
        assert stopped[-1] == ('semihost-exit' if model else 'reset'), (case, stopped)
        # Every admitted physical start makes the security writes, after the GIC
        # setup; a refused start and the QEMU build make none.
        # A skipped restore (an active bus master) makes no MBUS write and the
        # start continues.
        expected_security = [] if model or refused or early_timeout else (
            (MBUS_WRITES if mbus_rom else []) + security_writes(dma_open))
        assert security == expected_security, (case, [(hex(a), hex(v)) for a, v in security])
        status = int.from_bytes(cpu.mem_read(0x42000010, 8), 'little')
        # The restore's log line opens the log; other cases start it empty.
        log_length = int.from_bytes(cpu.mem_read(0x42000018, 8), 'little')
        log = bytes(cpu.mem_read(0x42000100, log_length)).decode()
        assert log == ('MBUS 0x84000000 0x81000000 0\n' if mbus_rom else
                       f'MBUS 0x84000000 0x84000000 {mbus_refused:x}\n' if mbus_refused else ''), (case, log)
        replaced = int.from_bytes(cpu.mem_read(0x42000078, 8), 'little')
        failed_cleanup = bool(fault and fault != 'delayed-rwp') or early_timeout
        expected_status = 4 if refused else 6 if failed_cleanup else requested_status
        assert not fault or fault_applied, (case, 'fault not reached')
        row = {'case': case, 'mode': name, 'initial_ctlr': original, 'status': status, 'replaced_status': replaced,
               'security_writes': len(security), 'dma_open': dma_open,
               'expected_status': expected_status, 'fault_applied': bool(fault_applied), 'final_ctlr': registers[gicd],
               'cleanup_ticks': ticks[0]-cleanup_started,
               'cleanup_instructions': instructions[1],
               'cleanup_reads': sum(row[0] == 'finish' for row in reads),
               'cleanup_writes': sum(row[0] == 'finish' for row in writes), 'writes': writes}
        results.append(row)
        if status != expected_status:
            a.output.write_text(json.dumps({'passed': False, 'cases': results}, indent=2)+'\n')
        assert status == expected_status, (case, status, expected_status)
        # Status 6 keeps the status that cleanup failure replaced: the
        # initialization timeout's failure status 4, or the requested status.
        assert replaced == ((4 if early_timeout else requested_status) if status == 6 else 0), (case, replaced, requested_status)
        if refused:
            assert not writes and registers[gicd] == original
        elif not failed_cleanup:
            assert registers[gicd] == (0x30 if model else original)
            for address, value in saved_registers.items():
                if address not in (gicd, gicd+4, gicr+8):
                    assert registers[address] == value, (case, hex(address), hex(registers[address]), hex(value))
        print('PASS', name, case, flush=True)
assert results, 'No selected case matched'
a.output.write_text(json.dumps({'physical_access': False, 'passed': True, 'cases': results,
    'sources': {name: digest(ROOT / name) for name in ('platform/a733/boot.h', 'include/a733_el3_security.h',
                                                       'include/a733_el3_mbus.h',
                                                       'checks/a733-monitor-are.py')},
    'monitor_source': {'path': str(a.monitor_source.resolve()), 'sha256': digest(a.monitor_source)},
    'tools': {str(path): digest(path) for path in (a.clang, a.lld, a.objdump)},
    'limits': 'Actual monitor instructions; system-register/MMIO completion and time modeled. No physical GIC timing, bus-stall recovery, arbitrary peripheral quiescence or ROM-reset qualification.'}, indent=2) + '\n')
print('PASS', len(results), 'monitor admission/cleanup instruction cases, with the BL31 security writes in order.')
