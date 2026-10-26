#!/usr/bin/env python3
"""Execute the AArch64 remote-writer exclusion check against modeled register states.

platform/a733/remote_writers.c is compiled with the standalone-boot flags and run
in Unicorn. Every register it may read holds a modeled value; the E902
configuration block and all other addresses are unmapped. Cases cover reset
defaults, the shipped monitor's E902 start writes, boot ROM and boot0 leftovers,
each reset bit, DMA and SD-controller activity, read failures and missing
callbacks. Register values are synthetic, so this establishes the decision
logic and its read set, not the state a physical board leaves.
"""
import argparse
import json
import struct
from pathlib import Path
import subprocess
import sys

from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn import arm64_const as a64

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402  (assertion policy and host tools)

ROOT = Path(__file__).resolve().parents[1]
KERNEL_FLAGS = ['-march=armv8-a', '-mstrict-align', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-ffunction-sections',
                '-fdata-sections', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-mgeneral-regs-only']
WRITERS = ['E902', 'OR100', 'DMA0', 'DMA1', 'CE', 'SMHC0', 'SMHC1', 'SMHC2', 'SMHC3', 'NAND0', 'UFS',
           'SPIF', 'USB0', 'USB1', 'USB2', 'PCIE0', 'GMAC0', 'DE0', 'DI', 'G2D', 'EINK', 'VE', 'NPU',
           'GPU0', 'CSI', 'ISP']
BIT = {name: 1 << n for n, name in enumerate(WRITERS)}
ALL = ((1 << len(WRITERS)) - 1) & ~BIT['OR100']
CCU, PRCM = 0x02002000, 0x07010000
RISCV_24M, RISCV_BGR, CPUIDLE = PRCM + 0x210, PRCM + 0x21c, 0x07050008
ISP_CLK, MBUS_GATE_EN = CCU + 0x1860, CCU + 0x5e4
# Manual 4.1.6 BGR offsets and reset bits, transcribed independently of the C table.
RESETS = {'DMA0': (0x704, [16]), 'DMA1': (0x70c, [16]), 'CE': (0xac4, [16, 17]),
          'SMHC0': (0xd0c, [16]), 'SMHC1': (0xd1c, [16]), 'SMHC2': (0xd2c, [16]), 'SMHC3': (0xd3c, [16]),
          'NAND0': (0xc8c, [16]), 'UFS': (0xd8c, [16, 17, 18, 19]), 'SPIF': (0xf1c, [16]),
          'USB0': (0x1304, [16, 20, 24]), 'USB1': (0x130c, [16, 20]), 'USB2': (0x135c, [16]),
          'PCIE0': (0x138c, [16, 17]), 'GMAC0': (0x141c, [16, 17]), 'DE0': (0xa04, [16]), 'DI': (0xa24, [16]),
          'G2D': (0xa44, [16]), 'EINK': (0xa6c, [16]), 'VE': (0xa8c, [16, 18]), 'NPU': (0xb04, [16, 17, 18]),
          'GPU0': (0xb24, [16]), 'CSI': (0x1844, [16])}
DMA = {'DMA0': 0x04601000, 'DMA1': 0x04024000}
SMHC = {'SMHC0': 0x04020000, 'SMHC1': 0x04021000, 'SMHC2': 0x04022000, 'SMHC3': 0x04023000}
PAGES = [(0x02002000, 0x2000), (0x04020000, 0x5000), (0x04601000, 0x1000),
         (0x07010000, 0x1000), (0x07050000, 0x1000)]
CODE, STACK, SENTINEL = 0x40000000, 0x40200000, 0x40300000


def build(output):
    flags = ['--target=aarch64-none-elf', *KERNEL_FLAGS, f'-I{ROOT / "include"}']
    objects = []
    for source in (ROOT / 'platform/a733/remote_writers.c', ROOT / 'checks/remote-writers-driver.c'):
        obj = output / (source.stem + '.o')
        subprocess.run([str(harness.CLANG), *flags, '-c', str(source), '-o', str(obj)], check=True)
        objects.append(obj)
    elf, script = output / 'remote-writers.elf', output / 'remote-writers.ld'
    script.write_text(f'SECTIONS {{ . = {CODE:#x}; .text : {{ *(.text*) }} .rodata : {{ *(.rodata*) }}\n'
                      '  .data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } }\n')
    subprocess.run([str(harness.CLANG), '--target=aarch64-none-elf', f'--ld-path={harness.LLD}',
                    '-nostdlib', '-static', f'-Wl,-T,{script}', '-Wl,-e,remote_writers_mmio',
                    *map(str, objects), '-o', str(elf)], check=True)
    return elf


def load(elf):
    with elf.open('rb') as f:
        e = ELFFile(f)
        segments = [(int(p['p_vaddr']), p.data(), int(p['p_memsz'])) for p in e.iter_segments()
                    if p['p_type'] == 'PT_LOAD']
        symbols = {s.name: int(s['st_value']) for s in e.get_section_by_name('.symtab').iter_symbols()}
    for address, data, size in segments:
        assert CODE <= address and address + size <= STACK - 0x10000, 'image outside the modeled RAM'
    return segments, symbols


def defaults():
    """Manual reset values for every register the check may read."""
    regs = {CCU + offset: 0 for offset, _ in RESETS.values()}
    regs[CCU + 0xb04] = 0x00080000          # NPU_SRAM_RST defaults de-asserted (4.1.6.120).
    regs.update({RISCV_24M: 0, RISCV_BGR: 0, CPUIDLE: 0, ISP_CLK: 0x3, MBUS_GATE_EN: 0})
    for base in DMA.values():
        regs[base + 0x30] = 0
        regs.update({base + 0x100 + n * 0x40: 0 for n in range(16)})
    regs.update({base + 0x80: 0 for base in SMHC.values()})
    return regs


def run(image, regs, entry='remote_writers_mmio', fail=0):
    segments, symbols = image
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(CODE, STACK - CODE)
    uc.mem_map(SENTINEL, 0x1000)
    for address, data, _ in segments:
        uc.mem_write(address, data)
    uc.mem_write(symbols['remote_writers_fail_address'], fail.to_bytes(8, 'little'))
    for base, size in PAGES:
        uc.mem_map(base, size)
    for address, value in regs.items():
        uc.mem_write(address, value.to_bytes(4, 'little'))
    reads, writes = [], []
    mmio = lambda a: any(b <= a < b + s for b, s in PAGES)

    def on_read(u, access, address, size, value, unused):
        if mmio(address):
            assert size == 4 and address % 4 == 0, f'unaligned or narrow MMIO read {address:#x}/{size}'
            assert address in regs, f'read of unmodeled register {address:#x}'
            reads.append(address)

    def on_write(u, access, address, size, value, unused):
        if mmio(address):
            writes.append(address)

    uc.hook_add(UC_HOOK_MEM_READ, on_read)
    uc.hook_add(UC_HOOK_MEM_WRITE, on_write)
    uc.reg_write(a64.UC_ARM64_REG_SP, STACK - 0x100)
    uc.reg_write(a64.UC_ARM64_REG_X30, SENTINEL)
    uc.emu_start(symbols[entry], SENTINEL, count=200000)
    assert uc.reg_read(a64.UC_ARM64_REG_PC) == SENTINEL, 'did not return'
    assert not writes, f'MMIO writes: {[hex(a) for a in writes]}'
    return uc.reg_read(a64.UC_ARM64_REG_X0) & 0xffffffff, reads


def names(mask):
    return sorted(n for n in WRITERS if mask & BIT[n])


def stage2_checks(build, output, ce_only=False):
    """Actual linked admission predicates and early refusal; no card model runs."""
    elf = build / 'a733-stage2.elf'
    with elf.open('rb') as stream:
        image = ELFFile(stream)
        segments = [(p['p_vaddr'], p.data()) for p in image.iter_segments() if p['p_type'] == 'PT_LOAD']
        symbols = {s.name: s['st_value'] for s in image.get_section_by_name('.symtab').iter_symbols()}
    code = (build / 'a733-stage2.bin').read_bytes()
    disassembler = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    system = {}
    for offset in range(0, len(code) - 3, 4):
        i = next(disassembler.disasm(code[offset:offset + 4], 0x47000000 + offset), None)
        if i and i.mnemonic in ('mrs', 'msr', 'wfe'): system[i.address] = (i.mnemonic, i.op_str)
    cases = []

    def case(name, entry, expected, controls=None, changes=None, grant_remote=False, mask=None):
        if ce_only and not name.startswith("cold-CE-"): return
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(0x47000000, 0x400000); uc.mem_map(SENTINEL, 0x1000)
        for address, data in segments: uc.mem_write(address, data)
        regs = defaults()
        regs.update({a: 0 for a in (0x02050010, 0x02050014, 0x070501c0, 0x070501c4,
                                   0x070501d4, 0x07090108, 0x08001004, 0x08001008, 0x03000024)})
        regs.update({0x030060a0: 0, 0x47004: 0, 0x47008: 0, 0x47028: 0})
        regs[0x0701022c] = 0  # Unsupported CPU configuration bus: clean refusal.
        regs.update(changes or {})
        pages = PAGES + [(0x02050000, 0x1000), (0x07090000, 0x1000),
                         (0x08001000, 0x1000), (0x03000000, 0x1000),
                         (0x03006000, 0x1000), (0x47000, 0x1000)]
        for base, size in pages: uc.mem_map(base, size)
        for address, value in regs.items(): uc.mem_write(address, value.to_bytes(4, 'little'))
        values = dict(currentel=12, mpidr_el1=0x81000000, midr_el1=0x412fd050,
                      sctlr_el3=0x30c50832, daif=0x3c0, cntfrq_el0=0, cntpct_el0=24000123)
        values.update(controls or {})
        reads, writes, reset = [], [], []

        def instruction(u, address, size, unused):
            mnemonic, operands = system[address]
            if mnemonic == 'wfe': raise AssertionError('unexpected park')
            first, second = operands.lower().split(', ')
            if mnemonic == 'mrs':
                u.reg_write(getattr(a64, 'UC_ARM64_REG_' + first.upper()), values[second])
                if second == 'cntpct_el0': values[second] += 24
            elif first == 'rmr_el3':
                reset.append(True); u.emu_stop(); return
            else: assert first == 'daifset', first
            u.reg_write(a64.UC_ARM64_REG_PC, address + 4)

        def read(u, access, address, size, value, unused):
            if any(b <= address < b + n for b, n in pages):
                assert (size == 4 or (address == 0x47028 and size == 1)) and address in regs, hex(address)
                reads.append(address)

        def write(u, access, address, size, value, unused):
            if not 0x47008000 <= address < 0x47400000:
                writes.append(address)
                assert address in (0x08001004, 0x08001008), hex(address)

        if grant_remote or entry == 'a733_stage2_cpu_cache_owned':
            def time_grant(u, address, size, unused):
                u.reg_write(a64.UC_ARM64_REG_X0, 1)
                u.reg_write(a64.UC_ARM64_REG_PC, u.reg_read(a64.UC_ARM64_REG_X30))
            timer = symbols['a733_stage2_time_valid']
            uc.hook_add(UC_HOOK_CODE, time_grant, begin=timer, end=timer)
        if grant_remote:
            def grant(u, address, size, unused):
                u.reg_write(a64.UC_ARM64_REG_X0, 1)
                u.reg_write(a64.UC_ARM64_REG_PC, u.reg_read(a64.UC_ARM64_REG_X30))
            remote = symbols['a733_stage2_remote_writers_excluded']
            uc.hook_add(UC_HOOK_CODE, grant, begin=remote, end=remote)
        for address in system: uc.hook_add(UC_HOOK_CODE, instruction, begin=address, end=address)
        uc.hook_add(UC_HOOK_MEM_READ, read); uc.hook_add(UC_HOOK_MEM_WRITE, write)
        uc.reg_write(a64.UC_ARM64_REG_SP, 0x473fff00); uc.reg_write(a64.UC_ARM64_REG_X30, SENTINEL)
        uc.emu_start(symbols[entry], SENTINEL, count=100000)
        if entry == 'a733_stage2_main':
            report = list(struct.unpack('<5Q', uc.mem_read(symbols['a733_stage2_report'], 40)))
            assert reset and report[3:] == [10 if grant_remote else 9, expected], report
            assert not any(0x04020000 <= a < 0x04021000 for a in reads), reads
        else:
            assert uc.reg_read(a64.UC_ARM64_REG_PC) == SENTINEL and uc.reg_read(a64.UC_ARM64_REG_X0) == expected, name
            assert not writes
        if entry == "a733_stage2_reset_policy":
            assert 0x03000024 not in reads  # Reset policy itself does not sample BOOT.
        if mask is not None:
            actual = int.from_bytes(uc.mem_read(symbols["remote_mask"], 4), "little")
            assert actual == mask, (name, actual, mask)
            assert not any(0x03040000 <= a < 0x03041000 for a in reads), "CE registers touched"
        cases.append(dict(case=name, expected=expected, reads=[hex(a) for a in reads], writes=[hex(a) for a in writes]))

    for label, change, expected in [('zero-frequency-metadata', {}, 1), ('24MHz-metadata', {'cntfrq_el0': 24000000}, 1),
        ('wrong-frequency', {'cntfrq_el0': 25000000}, 0), ('wrong-EL', {'currentel': 4}, 0),
        ('secondary-core', {'mpidr_el1': 0x81000100}, 0), ('wrong-CPU', {'midr_el1': 0x410fd0b0}, 0),
        ('unmasked', {'daif': 0}, 0), ('data-cache', {'sctlr_el3': 0x30c50836}, 0)]:
        case(label, 'a733_stage2_entry_state', expected, controls=change)
    case('normal-card-reset-policy', 'a733_stage2_reset_policy', 1, changes={0x03000024: 0x100})
    for address, value in ((0x02050014, 1), (0x02050010, 0x100), (0x070501c0, 1),
                           (0x070501c4, 0x20), (0x070501d4, 1), (0x07090108, 1)):
        case('prior-owner-' + hex(address), 'a733_stage2_reset_policy', 0, changes={address: value})
    case('CPU-cache-refuses-unavailable-bus', 'a733_stage2_cpu_cache_owned', 0)
    case('CPU-cache-refused-before-SD', 'a733_stage2_main', 0, grant_remote=True)
    case('E902-refused-before-SD', 'a733_stage2_main', BIT['E902'], changes={RISCV_BGR: 1})
    case('CE-out-of-reset-refused-before-SD', 'a733_stage2_main', BIT['CE'], changes={CCU + 0xac4: 0x30003})
    cold = {CCU + 0xac4: 0x30003, 0x030060a0: 0, 0x03000024: 0x100,
            0x47004: 0x4e4f4765, 0x47008: 0x3054422e, 0x47028: 0}
    remote = 'a733_stage2_remote_writers_excluded'
    case('cold-CE-fixed-predecessor', remote, 1, changes=cold, mask=0)
    for label, address, value in [('secure', 0x030060a0, 1), ('BOOT-low', 0x03000024, 0),
        ('magic-first', 0x47004, 0), ('magic-second', 0x47008, 0), ('other-media', 0x47028, 1)]:
        case('cold-CE-' + label, remote, 0, changes={**cold, address: value}, mask=BIT['CE'])
    case('cold-CE-media-high-bits', remote, 1, changes={**cold, 0x47028: 0xf0}, mask=0)
    case('cold-CE-reset-provider', remote, 1, mask=0)
    for label, change in [('E902', {RISCV_BGR: 1}), ('DMA0', {CCU + 0x704: 0x10000}),
                          ('SMHC0', {CCU + 0xd0c: 0x10001, SMHC['SMHC0']+0x80: 0x80})]:
        case('cold-CE-preserves-' + label, remote, 0, changes={**cold, **change}, mask=BIT[label])
    case('cold-CE-main-other-writer', 'a733_stage2_main', BIT['E902'],
         changes={**cold, RISCV_BGR: 1})
    (output / 'stage2-admission.json').write_text(json.dumps(dict(passed=True, physical_access=False, cases=cases,
        scope='Local system registers and hardware reads modeled; no ownership, cache publication or reset qualification.'), indent=1) + '\n')
    print('PASS stage-2 admission:', len(cases), 'focused instruction cases')



def stage2_cpu_checks(build, output, selected=()):
    """Actual static-OFF owner and joined loader failure; other owners are granted."""
    with (build / 'a733-stage2.elf').open('rb') as stream:
        image = ELFFile(stream)
        segments = [(p['p_vaddr'], p.data()) for p in image.iter_segments() if p['p_type'] == 'PT_LOAD']
        symbols = {s.name: s['st_value'] for s in image.get_section_by_name('.symtab').iter_symbols()}
    code = (build / 'a733-stage2.bin').read_bytes()
    dis = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    system = {}
    for offset in range(0, len(code)-3, 4):
        op = next(dis.disasm(code[offset:offset+4], 0x47000000+offset), None)
        if op and op.mnemonic in ('mrs', 'msr', 'dc', 'dsb', 'isb', 'wfe'):
            system[op.address] = (op.mnemonic, op.op_str)
    results = []
    ppu = lambda core: 0x07052000 + core*0x1000
    control = lambda core: 0x07050200 + core*4

    def case(name, change=None, fault='', expected=0, retained=False, main=False):
        if selected and name not in selected: return
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(0x47000000, 0x400000); uc.mem_map(SENTINEL, 0x1000)
        for address, data in segments: uc.mem_write(address, data)
        for address, size in ((0x07010000, 0x1000), (0x07050000, 0x10000), (0x08001000, 0x1000)):
            uc.mem_map(address, size)
        regs = {0x0701022c: 0x10001, 0x08001004: 0, 0x08001008: 0}
        for core in range(1,8):
            regs.update({ppu(core): 0x100, ppu(core)+4: 0, ppu(core)+8: 0x100,
                         ppu(core)+0xfb0: 0x10110100, ppu(core)+0xfc8: 0x0b61043b,
                         ppu(core)+0xfcc: 0x11, control(core): 0x40, control(core)+0x20: 0x80})
        regs.update(change or {})
        for address, value in regs.items(): uc.mem_write(address, value.to_bytes(4,'little'))
        values = dict(currentel=12, mpidr_el1=0x81000000, midr_el1=0x412fd050,
                      sctlr_el3=0x30c50832, daif=0x3c0, cntfrq_el0=0, cntpct_el0=24000123,
                      clidr_el1=0xc3000123, csselr_el1=0)
        geometries = [0x700fe01a, 0x703fe01a, 0x707fe07a]
        if fault=='geometry': geometries[2] ^= 8
        writes, maintenance, reads = [], [], []
        cache_operands=set()
        held, polls = set(), {}
        parks, resets, barriers = [], [], []

        def gpr(name): return 0 if name=='xzr' else uc.reg_read(getattr(a64,'UC_ARM64_REG_'+name.upper()))
        def hook_return(value):
            def hook(u, address, size, unused):
                u.reg_write(a64.UC_ARM64_REG_X0,value)
                u.reg_write(a64.UC_ARM64_REG_PC,u.reg_read(a64.UC_ARM64_REG_X30))
            return hook
        # Unchanged clock/remote contracts are explicit fixture inputs. This
        # check cannot establish those separate physical prerequisites.
        for symbol in ('a733_stage2_time_valid','a733_stage2_remote_writers_excluded'):
            address=symbols[symbol];uc.hook_add(UC_HOOK_CODE,hook_return(1),begin=address,end=address)
        if main and fault=='card':
            address=symbols['sunxi_mmc_init'];uc.hook_add(UC_HOOK_CODE,hook_return(0),begin=address,end=address)

        def instruction(u,address,size,unused):
            mnemonic,operands=system[address]
            if mnemonic=='wfe': parks.append(True);u.emu_stop();return
            if mnemonic=='mrs':
                target,source=operands.lower().split(', ')
                value=geometries[values['csselr_el1']//2] if source=='ccsidr_el1' else values[source]
                u.reg_write(getattr(a64,'UC_ARM64_REG_'+target.upper()),value)
                if source=='cntpct_el0':
                    if fault=='freeze' and writes: pass
                    else: values[source] += 2400000 if (fault=='deadline' and writes) or (fault=='cache-deadline' and maintenance) else 24
            elif mnemonic=='msr':
                target,source=operands.lower().split(', ')
                if target=='rmr_el3': resets.append(True);u.emu_stop();return
                assert target in ('csselr_el1','daifset'),target
                if target=='csselr_el1': values[target]=gpr(source)
            elif mnemonic=='dc':
                kind,source=operands.lower().split(', ');assert kind=='cisw'
                assert len(held)==7 and all(regs[ppu(c)]==regs[ppu(c)+8]==0 for c in range(1,8))
                operand=gpr(source);assert operand not in cache_operands
                cache_operands.add(operand);maintenance.append(operand)
            else: barriers.append((mnemonic,len(maintenance)))
            u.reg_write(a64.UC_ARM64_REG_PC,address+4)

        def read(u,access,address,size,value,unused):
            if not 0x07000000<=address<0x09000000:return
            assert size==4 and address in regs,hex(address)
            reads.append(address)
            if fault=='control-readback' and address==control(1) and (address,0x42) in writes:
                regs[address]=0x40;u.mem_write(address,(0x40).to_bytes(4,'little'))
            for core in range(1,8):
                if address==ppu(core)+8 and core in polls:
                    polls[core]+=1
                    if fault not in ('freeze','deadline') or core!=1:
                        if polls[core]>=3:
                            regs[address]=0;held.add(core)
                        else: regs[address]=0x108 if fault=='wake' else 0x100
                    if fault=='request' and core==1: regs[control(1)]|=1
                    uc.mem_write(address,regs[address].to_bytes(4,'little'))
                    uc.mem_write(control(core),regs[control(core)].to_bytes(4,'little'))
                    break

        def write(u,access,address,size,value,unused):
            if not 0x07000000<=address<0x09000000:return
            assert size==4 and address in regs,hex(address)
            writes.append((address,value));regs[address]=value
            for core in range(1,8):
                if address==ppu(core):
                    assert value==0 and not maintenance
                    polls[core]=0
                    if fault=='denial' and core==1:
                        regs[address]=0x100
                        # Unicorn performs the original store after this hook;
                        # inject denial at the following policy read instead.
                    break
                if address==control(core):
                    assert core in held and value==0x42
                    break
            else: assert address in (0x08001004,0x08001008),hex(address)
        def denial(u,access,address,size,value,unused):
            if fault=='denial' and address==ppu(1) and 1 in polls:
                u.mem_write(address,(0x100).to_bytes(4,'little'))
        for address in system: uc.hook_add(UC_HOOK_CODE,instruction,begin=address,end=address)
        uc.hook_add(UC_HOOK_MEM_READ,read);uc.hook_add(UC_HOOK_MEM_READ,denial);uc.hook_add(UC_HOOK_MEM_WRITE,write)
        uc.reg_write(a64.UC_ARM64_REG_SP,0x473fff00);uc.reg_write(a64.UC_ARM64_REG_X30,SENTINEL)
        uc.emu_start(symbols['a733_stage2_main' if main else 'a733_stage2_cpu_cache_owned'],SENTINEL,count=12000000)
        if main:
            report=list(struct.unpack('<5Q',uc.mem_read(symbols['a733_stage2_report'],40)))
            assert parks and not resets and report[3]==12,report
        else:
            assert uc.reg_read(a64.UC_ARM64_REG_PC)==SENTINEL,name
            assert uc.reg_read(a64.UC_ARM64_REG_X0)==expected,name
        policy_writes=[a for a,v in writes if a in [ppu(c) for c in range(1,8)]]
        assert bool(policy_writes)==retained,(name,writes)
        if expected or fault=='card':
            assert len(maintenance)==18944 and len(held)==7
            assert ('dsb',18944) in barriers and ('isb',18944) in barriers
        elif fault=='cache-deadline': assert 0<len(maintenance)<18944
        else: assert not maintenance
        results.append(dict(case=name,passed=True,retained=retained,maintenance=len(maintenance),
                            policy_writes=len(policy_writes),status_polls=sum(polls.values()),parked=bool(parks)))

    case('normal',expected=1,retained=True)
    case('already-static-OFF',{a:0 for c in range(1,8) for a in (ppu(c),ppu(c)+8)},expected=1,retained=True)
    case('mismatched-static-policy',{ppu(7):0})
    case('mismatched-dynamic-policy',{ppu(7)+8:0})
    case('wake-before-static-ack',fault='wake',expected=1,retained=True)
    for label,change in [('bus',{0x0701022c:0}),('identity',{ppu(7)+0xfc8:0}),
                         ('architecture',{ppu(7)+0xfcc:0x10}),('capability',{ppu(7)+0xfb0:0}),
                         ('operating-mode',{ppu(7)+0xfb0:0x10110110}),('emulation',{ppu(7)+4:1}),
                         ('policy',{ppu(7):8}),('active',{ppu(7)+8:0x108}),
                         ('request',{control(7):1})]: case('preflight-'+label,change)
    case('preflight-geometry',fault='geometry')
    for fault in ('denial','request','deadline','freeze','cache-deadline','control-readback'):case(fault,fault=fault,retained=True)
    case('loader-retains-policy-timeout',fault='deadline',retained=True,main=True)
    case('loader-retains-after-card-refusal',fault='card',retained=True,main=True)
    (output/'stage2-cpu.json').write_text(json.dumps(dict(passed=True,physical_access=False,cases=results,
        scope='Actual linked PPU owner and loader retention; system-register/MMIO/cache effects modeled; remote and clock prerequisites granted.'),indent=2)+'\n')
    print('PASS static-OFF/cache owner:',len(results),'focused native cases')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--stage2-build', type=Path, help='only changed stage-2 admission using its already-built ELF')
    parser.add_argument('--cpu-case', action='append', default=[], help='select added CPU owner cases without replaying accepted cases')
    parser.add_argument('--ce-only', action='store_true', help='only changed cold CE predecessor cases')
    parser.add_argument('--cpu-only', action='store_true', help='changed static-OFF owner in --stage2-build')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    if args.stage2_build:
        if args.cpu_only: stage2_cpu_checks(args.stage2_build, args.output, args.cpu_case)
        else: stage2_checks(args.stage2_build, args.output, args.ce_only)
        return
    image = load(build(args.output))
    results = []

    def case(label, expected, changes=None, fail=0, entry='remote_writers_mmio', absent=()):
        regs = defaults()
        regs.update(changes or {})
        mask, reads = run(image, regs, entry, fail)
        assert mask == expected, f'{label}: {names(mask)} != {names(expected)}'
        assert not mask & BIT['OR100'], 'reserved processor bit emitted'
        for address in absent:
            assert address not in reads, f'{label}: read {address:#x}'
        assert all(not 0x07032000 <= a < 0x07034000 for a in reads)
        results.append({'case': label, 'unproven': names(mask), 'reads': len(reads)})
        return reads

    base = 0  # OR100 bit 1 is reserved; A733 implements the E902 only.
    reads = case('reset-defaults', base)
    assert sorted(set(reads)) == sorted(reads), 'a register was read twice'
    gated_reads = {a for a in reads if a >= 0x04000000 and a < 0x05000000}
    assert not gated_reads, 'device registers read while held in reset'
    # Inherited from boot0's SD reads: SMHC0 out of reset with its IDMAC disabled.
    reads = case('boot0-sd-idle', base, {CCU + 0xd0c: 0x10001})
    assert SMHC['SMHC0'] + 0x80 in reads
    for name, address in SMHC.items():
        case(f'{name.lower()}-idmac-enabled', base | BIT[name], {CCU + RESETS[name][0]: 0x10001, address + 0x80: 0x80})
        case(f'{name.lower()}-other-dmac-bits', base, {CCU + RESETS[name][0]: 0x10001, address + 0x80: 0x2})
        case(f'{name.lower()}-bus-gated', base | BIT[name], {CCU + RESETS[name][0]: 0x10000},
             absent=[address + 0x80])
    for name, address in DMA.items():
        bgr = {CCU + RESETS[name][0]: 0x10001}
        reads = case(f'{name.lower()}-idle', base, bgr)
        assert {address + 0x30, *(address + 0x100 + n * 0x40 for n in range(16))} <= set(reads)
        for n in (0, 7, 15):
            case(f'{name.lower()}-channel{n}-enabled', base | BIT[name], {**bgr, address + 0x100 + n * 0x40: 1})
        for bit in (0, 15, 31):
            case(f'{name.lower()}-status-bit{bit}', base | BIT[name], {**bgr, address + 0x30: 1 << bit})
        case(f'{name.lower()}-status-reserved', base, {**bgr, address + 0x30: 0x7fff0000})
        case(f'{name.lower()}-bus-gated', base | BIT[name], {CCU + RESETS[name][0]: 0x10000},
             absent=[address + 0x30, address + 0x100])
    # The shipped monitor's start writes, together and one at a time.
    case('e902-monitor-start', base | BIT['E902'],
         {RISCV_BGR: 0x10003, RISCV_24M: 0x80000000, CPUIDLE: 1})
    for label, change in (('bus-gate', {RISCV_BGR: 1}), ('cfg-gate', {RISCV_BGR: 2}),
                          ('cfg-reset', {RISCV_BGR: 0x10000}), ('clock', {RISCV_24M: 0x80000000}),
                          ('cpuidle', {CPUIDLE: 1})):
        case(f'e902-{label}', base | BIT['E902'], change)
    case('e902-source-select-only', base, {RISCV_24M: 0x03000000, CPUIDLE: 0xfffffffe})
    case('isp-clock-on', base | BIT['ISP'], {ISP_CLK: 0x80000003})
    case('isp-mbus-pass', base | BIT['ISP'], {MBUS_GATE_EN: 1 << 9})
    case('mbus-other-gates', base, {MBUS_GATE_EN: 0x0004082f})
    # Each reset bit of each reset-only or engine master, and gates alone.
    for name, (offset, bits) in RESETS.items():
        for bit in bits:  # DMA and SD controllers here also have their bus clock masked.
            case(f'{name.lower()}-reset{bit}-deasserted', base | BIT[name], {CCU + offset: 1 << bit})
        case(f'{name.lower()}-gates-only', base, {CCU + offset: 0xffff if name != 'NPU' else 0x8ffff})
    # BROM and boot0 leftovers that the sources show: SD controller, CE and DMA0.
    case('rom-ce-enabled', base | BIT['CE'], {CCU + 0xac4: 0x30003, MBUS_GATE_EN: 1 << 2})
    case('rom-dma0-reasserted', base, {CCU + 0x704: 0x0})
    # Read failures affect only the master being read.
    for label, address, writer in (('riscv-bgr', RISCV_BGR, 'E902'), ('cpuidle', CPUIDLE, 'E902'),
                                   ('isp-clock', ISP_CLK, 'ISP'), ('mbus-gate', MBUS_GATE_EN, 'ISP'),
                                   ('dma0-bgr', CCU + 0x704, 'DMA0'), ('gpu-bgr', CCU + 0xb24, 'GPU0')):
        case(f'read-failure-{label}', base | BIT[writer], fail=address)
    case('read-failure-dma1-channel', base | BIT['DMA1'], {CCU + 0x70c: 0x10001}, fail=DMA['DMA1'] + 0x100 + 9 * 0x40)
    case('read-failure-smhc2-dmac', base | BIT['SMHC2'], {CCU + 0xd2c: 0x10001}, fail=SMHC['SMHC2'] + 0x80)
    case('null-ops', ALL, entry='remote_writers_null_ops')
    case('null-read', ALL, entry='remote_writers_null_read')
    everything = {RISCV_BGR: 0x10003, RISCV_24M: 0x80000000, CPUIDLE: 1, ISP_CLK: 0x80000003,
                  MBUS_GATE_EN: 0xffffffff}
    everything.update({CCU + offset: 0xffffffff for offset, _ in RESETS.values()})
    everything.update({a + 0x30: 1 for a in DMA.values()})
    everything.update({a + 0x80: 0x80 for a in SMHC.values()})
    case('all-running', ALL, everything)
    (args.output / 'remote-writers-model.json').write_text(json.dumps(
        {'schema': 'remote-writers-model-v1', 'physical_access': False, 'modeled': 'all register values',
         'writers': WRITERS, 'cases': results}, indent=1) + '\n')
    print(f'{len(results)} cases passed')


if __name__ == '__main__':
    main()
