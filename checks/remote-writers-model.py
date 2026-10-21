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
from pathlib import Path
import subprocess
import sys

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn import arm64_const as a64

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402  (assertion policy and host tools)

ROOT = Path(__file__).resolve().parents[1]
KERNEL_FLAGS = ['-march=armv8-a', '-mstrict-align', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-ffunction-sections',
                '-fdata-sections', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-mgeneral-regs-only']
WRITERS = ['E902', 'OR100', 'DMA0', 'DMA1', 'CE', 'SMHC0', 'SMHC1', 'SMHC2', 'SMHC3', 'NAND0', 'UFS',
           'USB0', 'USB1', 'USB2', 'PCIE0', 'GMAC0', 'DE0', 'DI', 'G2D', 'EINK', 'VE', 'NPU', 'GPU0',
           'CSI', 'ISP']
BIT = {name: 1 << n for n, name in enumerate(WRITERS)}
ALL = (1 << len(WRITERS)) - 1
CCU, PRCM = 0x02002000, 0x07010000
RISCV_24M, RISCV_BGR, CPUIDLE = PRCM + 0x210, PRCM + 0x21c, 0x07050008
ISP_CLK, MBUS_GATE_EN = CCU + 0x1860, CCU + 0x5e4
# Manual 4.1.6 BGR offsets and reset bits, transcribed independently of the C table.
RESETS = {'DMA0': (0x704, [16]), 'DMA1': (0x70c, [16]), 'CE': (0xac4, [16, 17]),
          'SMHC0': (0xd0c, [16]), 'SMHC1': (0xd1c, [16]), 'SMHC2': (0xd2c, [16]), 'SMHC3': (0xd3c, [16]),
          'NAND0': (0xc8c, [16]), 'UFS': (0xd8c, [16, 17, 18, 19]), 'USB0': (0x1304, [16, 20, 24]),
          'USB1': (0x130c, [16, 20]), 'USB2': (0x135c, [16]), 'PCIE0': (0x138c, [16, 17]),
          'GMAC0': (0x141c, [16, 17]), 'DE0': (0xa04, [16]), 'DI': (0xa24, [16]), 'G2D': (0xa44, [16]),
          'EINK': (0xa6c, [16]), 'VE': (0xa8c, [16, 18]), 'NPU': (0xb04, [16, 17, 18]),
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    image = load(build(args.output))
    results = []

    def case(label, expected, changes=None, fail=0, entry='remote_writers_mmio', absent=()):
        regs = defaults()
        regs.update(changes or {})
        mask, reads = run(image, regs, entry, fail)
        assert mask == expected, f'{label}: {names(mask)} != {names(expected)}'
        for address in absent:
            assert address not in reads, f'{label}: read {address:#x}'
        assert all(not 0x07032000 <= a < 0x07034000 for a in reads)
        results.append({'case': label, 'unproven': names(mask), 'reads': len(reads)})
        return reads

    base = BIT['OR100']
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
