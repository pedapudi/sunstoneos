#!/usr/bin/env python3
"""Check local GIC sequencing and execute native diagnostic CPU continuation."""
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
SOURCES = ['include/a733_cpu_continue.h', 'platform/a733/cpu_continue.c', 'platform/a733/cpu_continue.S',
           'include/a733_cpu_entry.h', 'platform/a733/cpu_entry.S', 'include/a733_cpu_start.h',
           'platform/a733/cpu_start.c', 'checks/a733-cpu-continue.c', 'checks/a733-cpu-continue.py']
MASK64 = (1 << 64) - 1


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def emulate(path, system_only=False, vectors_only=False):
    with path.open('rb') as f:
        elf = ELFFile(f)
        segments = [(s['p_vaddr'], s.data()) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    regs_gpr = [UC_ARM64_REG_X0 + i for i in range(29)] + [UC_ARM64_REG_X29, UC_ARM64_REG_LR]
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    decoded = {}
    results = []

    def case(core, mode='success'):
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(0x41000000, 0x1200000)
        uc.mem_map(0x03400000, 0x160000)
        uc.mem_map(0x07050000, 0x1000)
        for address, data in segments:
            uc.mem_write(address, data)
        end = 0x411ff000
        phase = ['prepare']
        clock = [24]
        epoch = 0x123456789abcdef
        model = {'currentel': 12, 'sctlr_el3': 0, 'mpidr_el1': 0x81000000,
                 'cntfrq_el0': 24000000, 'sctlr_el1': 0, 'mair_el1': 0, 'tcr_el1': 0, 'ttbr0_el1': 0,
                 'esr_el3': 0x96000010, 'far_el3': 0xdead0000, 'elr_el3': 0,
                 'esr_el1': 0x96000011, 'far_el1': 0xdead1000, 'elr_el1': 0x41000004}
        r = 0x03460000 + ((core + 3) % 8) * 0x20000
        g = r + 0x10000
        control = 0x07050200 + core * 4
        mmio = {0x03400000: 0x33, control: 0x42, control + 0x20: 0x80,
                r: 0, r + 0x14: 6, g + 0x80: 0, g + 0xd00: 0xffffffff,
                g + 0x100: 0xffffffff, g + 0x300: 0xffffffff,
                g + 0xc04: 0xffffffff, g + 0x400: 0x76543210}
        report = symbols['a733_cpu_continue_records'] + core * 128
        slot = symbols['a733_cpu_mailboxes'] + core * 256
        exclusive = symbols['a733_cpu_continue_exclusives'] + core * 128
        el3_stack = symbols['a733_cpu_entry_stacks'] + core * 4096
        el1_stack = symbols['a733_cpu_el1_stacks'] + core * 32768
        writes, observed, system_writes = [], [], []
        attempts = [0]

        def read_gpr(name):
            if name in ('xzr', 'wzr'): return 0
            return uc.reg_read(regs_gpr[int(name[1:])])

        def instruction(cpu, address, size, unused):
            if address == symbols['a733_smp_exit_privileged']:
                assert mode == 'exit_svc'
                assert cpu.reg_read(UC_ARM64_REG_X0) == 7 and cpu.reg_read(UC_ARM64_REG_X0 + 1) == 9
                observed.append('diagnostic_exit')
                cpu.emu_stop()
                return
            if address == symbols['a733_smp_primary_el1']:
                assert core == 0
                observed.append('primary_kernel')
                cpu.emu_stop()
                return
            if address == symbols['a733_smp_pre_scheduler'] and mode == 'shared_fail':
                cpu.reg_write(UC_ARM64_REG_X0, 0)
                cpu.reg_write(UC_ARM64_REG_PC, cpu.reg_read(UC_ARM64_REG_LR))
                return
            if address not in decoded:
                decoded[address] = next(decoder.disasm(bytes(cpu.mem_read(address, 4)), address))
            insn = decoded[address]
            args = insn.op_str.split(', ')
            if insn.mnemonic == 'mrs':
                if args[1] == 'cntpct_el0':
                    value = clock[0]
                    clock[0] += 24
                else:
                    assert args[1] in model, (insn.mnemonic, args)
                    value = model[args[1]]
                    if mode == 'sre_fail' and args[1] == 'icc_sre_el3': value = 0
                cpu.reg_write(regs_gpr[int(args[0][1:])], value)
                cpu.reg_write(UC_ARM64_REG_PC, address + 4)
            elif insn.mnemonic == 'msr':
                model[args[0]] = int(args[1][1:], 0) if args[1].startswith('#') else read_gpr(args[1])
                system_writes.append((args[0], model[args[0]]))
                cpu.reg_write(UC_ARM64_REG_PC, address + 4)
            elif insn.mnemonic == 'eret':
                assert model['spsr_el3'] == 0x3c5
                model['currentel'] = 4
                cpu.reg_write(UC_ARM64_REG_SP, model['sp_el1'])
                cpu.reg_write(UC_ARM64_REG_PC, model['elr_el3'])
            elif insn.mnemonic in ('dsb', 'dmb', 'isb', 'sev', 'sevl', 'tlbi', 'yield'):
                cpu.reg_write(UC_ARM64_REG_PC, address + 4)
            elif insn.mnemonic == 'ldaxr' and mode == 'exclusive_abort':
                model['esr_el1'] = 0x96000035
                model['far_el1'] = exclusive
                model['elr_el1'] = address
                cpu.reg_write(UC_ARM64_REG_PC, symbols['a733_cpu_continue_el1_vectors'] + 0x200)
            elif insn.mnemonic == 'stlxr':
                attempts[0] += 1
                if mode == 'exclusive_fail':
                    cpu.reg_write(regs_gpr[int(args[0][1:])], 1)
                    cpu.reg_write(UC_ARM64_REG_PC, address + 4)
            elif insn.mnemonic == 'svc':
                assert args == ['#0x6a'], args
                if mode == 'exit_svc':
                    model['esr_el1'] = (0x15 << 26) | 119
                    cpu.reg_write(UC_ARM64_REG_X0, 7)
                    cpu.reg_write(UC_ARM64_REG_X0 + 1, 9)
                    cpu.reg_write(UC_ARM64_REG_PC, symbols['a733_cpu_continue_el1_vectors'] + 0x400)
                else:
                    observed.append('scheduler_svc')
                    cpu.emu_stop()
            elif insn.mnemonic == 'wfe':
                observed.append('park')
                cpu.emu_stop()

        def memory(cpu, access, address, size, value, unused):
            if 0x41000000 <= address < 0x42200000:
                if access == UC_MEM_WRITE and phase[0] != 'prepare':
                    allowed = [(report, 128), (exclusive, 8), (el3_stack, 4096), (el1_stack, 32768),
                               (symbols['ucSecondaryCoresReadyFlags'] + max(core - 1, 0), 1)]
                    assert any(a <= address and address + size <= a + n for a, n in allowed), (core, mode, hex(address), size)
                return
            if access != UC_MEM_WRITE and 0x03460008 <= address <= 0x03540008 and (address - 0x03460008) % 0x20000 == 0:
                assert size == 8
                index = (address - 0x03460008) // 0x20000
                typer = (((index + 5) % 8) << 40) | (16 if index == 7 else 0)
                cpu.mem_write(address, struct.pack('<Q', typer))
                return
            assert size == 4
            if access != UC_MEM_WRITE:
                assert address in mmio, (core, mode, 'read', hex(address))
                cpu.mem_write(address, struct.pack('<I', mmio[address]))
            else:
                assert address in {control, r + 0x14, g + 0x180, g + 0x280, g + 0x380,
                                   g + 0x80, g + 0xd00, g + 0xc04, g + 0x400, g + 0x100}
                if address == control: assert core != 0
                writes.append((address, value))
                if address == r + 0x14: mmio[address] = value & ~4
                elif address == g + 0x180: mmio[g + 0x100] &= ~value
                elif address == g + 0x380: mmio[g + 0x300] &= ~value
                elif address == g + 0x100: mmio[address] |= value
                elif address != g + 0x280: mmio[address] = value
                if mode == 'late_gic' and len(writes) == 2: clock[0] = 1000 * 24

        uc.hook_add(UC_HOOK_CODE, instruction)
        uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, memory)
        uc.reg_write(UC_ARM64_REG_SP, el3_stack + 4096)
        uc.reg_write(UC_ARM64_REG_LR, end)
        uc.reg_write(UC_ARM64_REG_X0, epoch)
        uc.reg_write(UC_ARM64_REG_X0 + 1, 1000)
        uc.emu_start(symbols['a733_cpu_continue_prepare'], end, count=200000)
        assert uc.reg_read(UC_ARM64_REG_PC) == end and uc.reg_read(UC_ARM64_REG_X0) == 1, (core, mode, 'prepare')
        phase[0] = 'continue'
        model['mpidr_el1'] = 0x81000000 + (core << 8)
        uc.mem_write(symbols['ucPrimaryCoreInitDoneFlag'], bytes([0 if mode == 'missing_primary' else 1]))
        uc.mem_write(slot, struct.pack('<16Q', 0x53435045, epoch, core, 2, epoch ^ MASK64, *([0] * 11)))
        uc.mem_write(slot + 128, struct.pack('<16Q', epoch, core, 0x410fd050 if core < 6 else 0x410fd0b0,
                                            model['mpidr_el1'], 0, 2, *([0] * 10)))
        uc.reg_write(UC_ARM64_REG_X0, core if core else epoch)
        uc.reg_write(UC_ARM64_REG_X0 + 1, epoch)
        uc.reg_write(UC_ARM64_REG_X0 + 2, slot + 128)
        uc.reg_write(UC_ARM64_REG_X0 + 3, slot)
        uc.emu_start(symbols['a733_cpu_secondary_continue' if core else 'a733_cpu_primary_continue'], end, count=1000000)
        state = struct.unpack('<16Q', uc.mem_read(report, 128))
        if mode == 'exit_svc':
            assert observed == ['diagnostic_exit'] and state[2] == 4
        elif mode == 'success':
            assert observed == ['scheduler_svc' if core else 'primary_kernel'], (core, mode, observed, state)
            assert state[:5] == (epoch, core, 4 if core else 3, 0, r)
            assert model['tcr_el1'] == 0x200803019 and model['mair_el1'] == 0x4400
            assert model['sctlr_el1'] & 0x1005 == 1 and model['tpidr_el1'] == core
            assert model['sctlr_el3'] == 0 and model['sp_el1'] == el1_stack + 32768
            expected = {'scr_el3': 0x535, 'mdcr_el3': 0x10000000, 'cptr_el3': 0,
                        'hcr_el2': 0x80000000, 'hstr_el2': 0, 'mdcr_el2': 0, 'vttbr_el2': 0,
                        'cptr_el2': 0x33ff, 'cnthctl_el2': 3, 'cntvoff_el2': 0,
                        'sctlr_el2': 0x30c50830, 'sctlr_el1': 0x30d00801,
                        'cpacr_el1': 0x300000, 'cntkctl_el1': 3}
            assert {k: model[k] for k in expected} == expected
            assert model['vbar_el1'] == symbols['a733_cpu_continue_el1_vectors']
            assert attempts[0] == 1 and struct.unpack('<Q', uc.mem_read(exclusive, 8))[0] == core + 1
            if core: assert uc.mem_read(symbols['ucSecondaryCoresReadyFlags'] + core - 1, 1) == b'\1'
        else:
            assert observed == ['park'] and state[2] == 5, (core, mode, observed, state)
            if mode == 'exclusive_fail': assert attempts[0] == 64 and state[3] == 5
            if mode == 'sre_fail': assert state[3] == 19
            if mode == 'missing_primary': assert state[3] == 2
            if mode == 'exclusive_abort': assert state[3] == 17 and state[5:7] == (0x96000035, exclusive)
        results.append({'core': core, 'mode': mode, 'result': observed, 'record': state,
                        'mmio_writes': writes, 'system_writes': system_writes, 'exclusive_attempts': attempts[0]})

    if vectors_only:
        case(5, 'exclusive_abort')
        case(6, 'exit_svc')
        return results
    if system_only:
        case(1)
        return results
    for core in range(8): case(core)
    for mode in ['sre_fail', 'exclusive_fail', 'shared_fail', 'missing_primary', 'late_gic']:
        case(4, mode)
    return results


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--evidence', type=Path, required=True)
    p.add_argument('--vectors-only', action='store_true', help='Check EL1 exclusive-abort and diagnostic SVC routes only')
    p.add_argument('--reuse-build', type=Path, help='Reuse unchanged C objects/host evidence; check only changed system-register assembly')
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
    if not args.reuse_build:
        run(['clang-21', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I', str(ROOT / 'include'),
         *[str(ROOT / p) for p in ['checks/a733-cpu-continue.c', 'platform/a733/cpu_continue.c', 'platform/a733/cpu_start.c']],
         '-o', str(out / 'host-check')], 'host-build.log')
        run([str(out / 'host-check')], 'host-run.log')
    (out / 'FreeRTOSConfig.h').write_text('#define configNUMBER_OF_CORES 8\n')
    (out / 'stubs.c').write_text('''#include <stddef.h>
#include <stdint.h>
volatile uint8_t ucPrimaryCoreInitDoneFlag,ucSecondaryCoresReadyFlags[7];
void *memcpy(void *d,const void *s,size_t n){unsigned char *a=d;const unsigned char*b=s;while(n--)*a++=*b++;return d;}
void *memset(void *d,int x,size_t n){unsigned char*a=d;while(n--)*a++=(unsigned char)x;return d;}
void *memmove(void *d,const void *s,size_t n){unsigned char*a=d;const unsigned char*b=s;if(a<b)while(n--)*a++=*b++;else{a+=n;b+=n;while(n--)*--a=*--b;}return d;}
int a733_smp_pre_scheduler(unsigned long c,uint64_t e,uint64_t d){return c<8 && e && d;}
void a733_smp_primary_el1(void){for(;;){}}
void a733_smp_exit_privileged(unsigned s,uint64_t d){(void)s;(void)d;for(;;){}}
void FreeRTOS_IRQ_Handler(void){for(;;){}}
void FreeRTOS_SWI_Handler(void){for(;;){}}
''')
    flags = ['clang-21', '--target=aarch64-none-elf', '-march=armv8-a', '-mstrict-align', '-mgeneral-regs-only',
             '-O2', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-Wall', '-Wextra', '-Werror', '-fstack-usage',
             '-DSUNSTONE_A733_SMP_DIAGNOSTIC', '-I', str(out), '-I', str(ROOT / 'include')]
    if args.reuse_build:
        previous = json.loads((args.reuse_build / 'results.json').read_text())
        for source, identity in previous['inputs'].items():
            if source not in ('platform/a733/cpu_continue.S', 'checks/a733-cpu-continue.py'):
                assert digest(ROOT / source) == identity, source
        for name in ('stubs.c', 'FreeRTOSConfig.h'):
            assert digest(out / name) == previous['artifacts'][name]
    objects = []
    for source, name in [(ROOT / 'platform/a733/cpu_continue.c', 'continue'), (ROOT / 'platform/a733/cpu_continue.S', 'continue-asm'),
                         (ROOT / 'platform/a733/cpu_entry.S', 'entry'), (ROOT / 'platform/a733/cpu_start.c', 'topology'), (out / 'stubs.c', 'stubs')]:
        obj = out / (name + '.o')
        if args.reuse_build and (name != 'continue-asm' or digest(source) == previous['inputs']['platform/a733/cpu_continue.S']):
            old = args.reuse_build / (name + '.o')
            assert digest(old) == previous['artifacts'][old.name]
            shutil.copyfile(old, obj)
        else:
            run(flags + ['-c', str(source), '-o', str(obj)], name + '-build.log')
        objects.append(str(obj))
    (out / 'continue.ld').write_text('''ENTRY(a733_cpu_secondary_continue)
SECTIONS {
 . = 0x41000000; .text : { *(.text.a733_cpu_entry) *(.text*) } .rodata : { *(.rodata*) }
 . = 0x41200000; .data : { *(.data*) } .bss (NOLOAD) : { *(.bss) *(COMMON) }
 . = 0x42010000; .monitor (NOLOAD) : { *(.bss.a733_cpu_mailboxes) *(.bss.a733_cpu_entry_stacks) *(.bss.a733_cpu_continue_monitor) }
 /DISCARD/ : { *(.comment) *(.note*) }
}
''')
    lld = Path.home() / '.cache/sunstoneos/host/usr/bin/ld.lld-21'
    if args.reuse_build and digest(ROOT / 'platform/a733/cpu_continue.S') == previous['inputs']['platform/a733/cpu_continue.S']:
        assert digest(out / 'continue.ld') == previous['artifacts']['continue.ld']
        old_elf = args.reuse_build / 'continue.elf'
        assert digest(old_elf) == previous['artifacts']['continue.elf']
        shutil.copyfile(old_elf, out / 'continue.elf')
    else:
        run([str(lld), '-T', str(out / 'continue.ld'), *objects, '-o', str(out / 'continue.elf')], 'link.log')
    cases = emulate(out / 'continue.elf', bool(args.reuse_build), args.vectors_only)
    target = run(['/usr/lib/llvm-21/bin/llvm-size', str(out / 'continue.o'), str(out / 'continue-asm.o')], 'size.log')
    result = {'passed': True, 'scope': 'Host GIC/descriptor checks and native continuation under instruction emulation with modeled system registers, MMIO and privilege/translation transitions. Kernel entry hooks are test fixtures; no full FreeRTOS or physical coherence/atomicity qualification.',
              'commands': commands, 'inputs': {s: digest(ROOT / s) for s in SOURCES},
              'artifacts': {p.name: digest(p) for p in out.iterdir() if p.suffix in ('.o', '.elf', '.ld') or p.name in ('host-check', 'stubs.c', 'FreeRTOSConfig.h')},
              'cases': cases, 'size': target,
              'reused_evidence': str(args.reuse_build.resolve()) if args.reuse_build else None,
              'reused_results_sha256': digest(args.reuse_build / 'results.json') if args.reuse_build else None}
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    prefix = 'REUSE host/native build evidence; ' if args.reuse_build else 'PASS host sanitizer/native build; '
    print(prefix + f'PASS {len(cases)} selected native continuation cases')


if __name__ == '__main__':
    main()
