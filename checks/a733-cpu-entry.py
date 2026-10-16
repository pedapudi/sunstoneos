#!/usr/bin/env python3
"""Check owner failure retention and execute the native secondary entry instructions."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X29, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_PC
ROOT = Path(__file__).resolve().parents[1]
SOURCES = ['include/a733_cpu_entry.h', 'platform/a733/cpu_entry.c', 'platform/a733/cpu_entry_native.c',
           'platform/a733/cpu_entry.S', 'include/a733_cpu_start.h', 'platform/a733/cpu_start.c',
           'checks/a733-cpu-entry.c', 'checks/a733-cpu-entry.py']
MASK64 = (1 << 64) - 1


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def emulate(elf_path):
    with elf_path.open('rb') as f:
        elf = ELFFile(f)
        segments = [(s['p_vaddr'], s.data()) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    base = symbols['a733_cpu_mailboxes']
    stack = symbols['a733_cpu_entry_stacks']
    entry = symbols['a733_cpu_secondary_entry']
    next_entry = symbols['a733_cpu_secondary_continue']
    vector = symbols['a733_cpu_entry_vectors']
    wait = symbols['a733_cpu_entry_wait']
    dis = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    registers = [UC_ARM64_REG_X0 + n for n in range(29)] + [UC_ARM64_REG_X29, UC_ARM64_REG_LR]
    results = []

    def case(name, core=1, mpidr=None, sctlr=0, el=12, bad=None, action='release'):
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(0x48000000, 0x20000)
        for address, data in segments:
            uc.mem_write(address, data)
        epoch = 0x123456789abcdef
        cmd = [0x53435045, epoch, core, 1, epoch ^ MASK64] + [0] * 11
        if bad == 'magic': cmd[0] ^= 1
        if bad == 'epoch': cmd[1] = 0
        if bad == 'core': cmd[2] = (core + 1) % 8
        if bad == 'inverse': cmd[4] ^= 1
        if bad == 'released': cmd[3] = 2
        if bad == 'reserved': cmd[15] = 1
        slot = base + core * 256
        uc.mem_write(slot, struct.pack('<16Q', *cmd))
        if bad == 'stale_report': uc.mem_write(slot + 248, struct.pack('<Q', 1))
        before = bytes(uc.mem_read(base, 2048))
        regs = {'currentel': el, 'sctlr_el3': sctlr,
                'mpidr_el1': 0x81000000 + (core << 8) if mpidr is None else mpidr,
                'midr_el1': 0x412fd052 if core < 6 else 0x413fd0b1,
                'tpidr_el3': 0xdeadbeef, 'vbar_el3': 0,
                'esr_el3': 0x96000010, 'far_el3': 0xdead0000,
                'elr_el3': 0x48000010, 'spsr_el3': 0x3cd}
        observed, writes, barriers = [], [], []
        phase = ['arrival']
        nested = [False]

        def read_gpr(text):
            return 0 if text == 'xzr' else uc.reg_read(registers[int(text[1:])])

        def instruction(cpu, address, size, unused):
            if address == next_entry:
                observed.append('continuation')
                cpu.emu_stop()
                return
            op = next(dis.disasm(bytes(cpu.mem_read(address, 4)), address))
            args = op.op_str.split(', ')
            if op.mnemonic == 'mrs':
                assert args[1] in regs, (name, op.mnemonic, args)
                cpu.reg_write(registers[int(args[0][1:])], regs[args[1]])
                cpu.reg_write(UC_ARM64_REG_PC, address + 4)
            elif op.mnemonic == 'msr':
                if args[0] in ('daifset', 'spsel'):
                    regs[args[0]] = int(args[1][1:], 0)
                else:
                    assert args[0] in ('vbar_el3', 'tpidr_el3')
                    regs[args[0]] = read_gpr(args[1])
                cpu.reg_write(UC_ARM64_REG_PC, address + 4)
            elif op.mnemonic in ('dsb', 'isb', 'sev', 'sevl'):
                barriers.append(op.mnemonic)
                cpu.reg_write(UC_ARM64_REG_PC, address + 4)
            elif op.mnemonic == 'wfe':
                if address == wait and phase[0] == 'arrival':
                    observed.append('arrived_wait')
                    cpu.emu_stop()
                elif address == wait:
                    cpu.reg_write(UC_ARM64_REG_PC, address + 4)
                else:
                    observed.append('park')
                    cpu.emu_stop()
            elif op.mnemonic == 'b' and int(args[0][1:], 16) == address:
                observed.append('park')
                cpu.emu_stop()
            elif action == 'nested_fault' and phase[0] == 'fault' and op.mnemonic == 'str' and not nested[0]:
                nested[0] = True
                cpu.reg_write(UC_ARM64_REG_PC, vector)

        def memory(cpu, access, address, size, value, unused):
            assert slot <= address and address + size <= slot + 256, (name, hex(address), size)
            assert size == 8
            if access == UC_MEM_WRITE:
                assert slot + 128 <= address, (name, 'secondary wrote primary command')
                assert regs['currentel'] == 12 and not (regs['sctlr_el3'] & 0x02001005)
                writes.append((address, value))

        uc.hook_add(UC_HOOK_CODE, instruction)
        uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, memory)
        uc.reg_write(UC_ARM64_REG_SP, 0xdeadf000)
        uc.emu_start(entry, 0x48020000, count=10000)
        refused = bad or el != 12 or sctlr & 0x02001005 or mpidr is not None
        if refused:
            assert observed == ['park'], (name, observed)
            assert not writes and bytes(uc.mem_read(base, 2048)) == before
        else:
            assert observed == ['arrived_wait'], (name, observed)
            report = struct.unpack('<16Q', uc.mem_read(slot + 128, 128))
            assert report[:6] == (epoch, core, regs['midr_el1'], regs['mpidr_el1'], sctlr, 1)
            assert report[6:] == (0,) * 10
            assert regs['vbar_el3'] == vector and regs['daifset'] == 15
            assert uc.reg_read(UC_ARM64_REG_SP) == 0xdeadf000
            if action == 'release':
                uc.mem_write(slot + 24, struct.pack('<Q', 2))
                phase[0] = 'release'
                uc.emu_start(wait, 0x48020000, count=10000)
                assert observed[-1] == 'continuation'
                assert [uc.reg_read(r) for r in registers[:4]] == [core, epoch, slot + 128, slot]
                assert uc.reg_read(UC_ARM64_REG_SP) == stack + (core + 1) * 4096
                assert regs['spsel'] == 1
                assert struct.unpack('<Q', uc.mem_read(slot + 168, 8))[0] == 2
                if core == 1:
                    phase[0] = 'return'
                    uc.emu_start(uc.reg_read(UC_ARM64_REG_LR), 0x48020000, count=10000)
                    assert observed[-1] == 'park'
                    assert struct.unpack('<Q', uc.mem_read(slot + 208, 8))[0] == 3
            elif action == 'mutate':
                uc.mem_write(slot + 8, struct.pack('<Q', epoch + 1))
                phase[0] = 'release'
                uc.emu_start(wait, 0x48020000, count=10000)
                assert observed[-1] == 'park'
                assert struct.unpack('<Q', uc.mem_read(slot + 208, 8))[0] == 1
            else:
                for reg in registers:
                    uc.reg_write(reg, 0xffffffffffffffff)
                phase[0] = 'fault'
                initial_writes = len(writes)
                uc.emu_start(vector + (core % 16) * 128, 0x48020000, count=10000)
                assert observed[-1] == 'park'
                if action == 'nested_fault':
                    assert nested[0] and len(writes) == initial_writes
                else:
                    report = struct.unpack('<16Q', uc.mem_read(slot + 128, 128))
                    assert report[5:11] == (3, regs['esr_el3'], regs['far_el3'], regs['elr_el3'], regs['spsr_el3'], 2)
            after = bytes(uc.mem_read(base, 2048))
            assert after[:core * 256] == before[:core * 256]
            assert after[(core + 1) * 256:] == before[(core + 1) * 256:]
        results.append({'name': name, 'observed': observed, 'writes': len(writes), 'barriers': len(barriers)})

    for core in range(1, 8):
        case(f'core-{core}-release', core=core)
        case(f'core-{core}-exception', core=core, action='exception')
    for bad in ['magic', 'epoch', 'core', 'inverse', 'released', 'reserved', 'stale_report']:
        case('refuse-' + bad, bad=bad)
    for mpidr in [0x81000000, 0x81000800, 0x81000101, 0x81010100, 0x181000100, 0x83000100]:
        case(f'refuse-mpidr-{mpidr:x}', mpidr=mpidr)
    for sctlr in [1, 4, 0x1000, 0x2000000]:
        case(f'refuse-sctlr-{sctlr:x}', sctlr=sctlr)
    for el in [0, 4, 8]:
        case(f'refuse-el-{el}', el=el)
    case('mutated-epoch', action='mutate')
    case('nested-exception', action='nested_fault')
    return results


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--evidence', type=Path, required=True)
    args = p.parse_args()
    out = args.evidence.resolve()
    assert ROOT not in [out, *out.parents]
    out.mkdir(parents=True, exist_ok=False)
    commands = []

    def run(command, log):
        commands.append(command)
        result = subprocess.run(command, capture_output=True, text=True)
        (out / log).write_text(result.stdout + result.stderr)
        result.check_returncode()
        return result.stdout

    for source in SOURCES:
        dest = out / 'source' / source
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / source, dest)
    run(['clang-21', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I', str(ROOT / 'include'),
         *[str(ROOT / p) for p in ['checks/a733-cpu-entry.c', 'platform/a733/cpu_entry.c', 'platform/a733/cpu_start.c']],
         '-o', str(out / 'owner-check')], 'owner-build.log')
    run([str(out / 'owner-check')], 'owner-run.log')
    flags = ['clang-21', '--target=aarch64-none-elf', '-march=armv8-a', '-mstrict-align', '-mgeneral-regs-only',
             '-O2', '-ffreestanding', '-fno-stack-protector', '-Wall', '-Wextra', '-Werror', '-fstack-usage', '-I', str(ROOT / 'include')]
    for source, name in [('platform/a733/cpu_entry.c', 'owner'), ('platform/a733/cpu_entry_native.c', 'native'), ('platform/a733/cpu_entry.S', 'entry')]:
        run(flags + ['-c', str(ROOT / source), '-o', str(out / (name + '.o'))], name + '-target.log')
    (out / 'continuation.S').write_text('.text\n.balign 4\n.global a733_cpu_secondary_continue\na733_cpu_secondary_continue:\n b .\n')
    run(flags + ['-c', str(out / 'continuation.S'), '-o', str(out / 'continuation.o')], 'continuation-build.log')
    (out / 'entry.ld').write_text('ENTRY(a733_cpu_secondary_entry)\nSECTIONS { . = 0x48000000; .text : { *(.text.a733_cpu_entry) *(.text*) } . = ALIGN(4096); .bss (NOLOAD) : { *(.bss*) } /DISCARD/ : { *(.comment) *(.note*) } }\n')
    lld = Path.home() / '.cache/sunstoneos/host/usr/bin/ld.lld-21'
    run([str(lld), '-T', str(out / 'entry.ld'), str(out / 'entry.o'), str(out / 'continuation.o'), '-o', str(out / 'entry.elf')], 'link.log')
    cases = emulate(out / 'entry.elf')
    target = run(['/usr/lib/llvm-21/bin/llvm-size', str(out / 'owner.o'), str(out / 'native.o'), str(out / 'entry.o')], 'size.log')
    run(['/usr/lib/llvm-21/bin/llvm-nm', '-u', str(out / 'owner.o'), str(out / 'native.o'), str(out / 'entry.o')], 'undefined.log')
    result = {'passed': True, 'scope': 'Owner/power composition model and actual entry instructions with modeled system registers/barriers; no physical CPU, coherence or GIC qualification',
              'commands': commands, 'inputs': {s: digest(ROOT / s) for s in SOURCES},
              'artifacts': {p.name: digest(p) for p in out.iterdir() if p.suffix in ('.o', '.elf') or p.name == 'owner-check'},
              'instruction_cases': cases, 'size': target}
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'PASS owner sanitizer checks and {len(cases)} native entry instruction cases; freestanding objects compile')


if __name__ == '__main__':
    main()
