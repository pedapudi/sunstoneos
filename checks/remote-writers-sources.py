#!/usr/bin/env python3
"""Survey which remote bus masters the pinned boot0 and the captured board ROM can start.

Scans the PC-relative literal loads of boot0 (Thumb, code below its strings) and
of the captured board ROM (Arm) for E902, CPUIDLE and CCU bus-reset addresses,
checks the ROM's CE and DMA0 reset sequences instruction by instruction, and
confirms the manual and datasheet statements that docs/remote-writers.md cites.
A literal scan cannot see addresses computed from other bases; the result
narrows the boot chain's writers, it does not prove hardware state.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
from capstone.arm import ARM_INS_LDR, ARM_OP_MEM, ARM_REG_PC

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402  (assertion policy and cache location)

INPUTS = {
    'boot0': ('sources/board-boot/boot0_sdcard_orangepizero3w.fex',
              '829ea8e3b65d71a02f2d0e094bc073de3a10869f948f62db49dbf023d07fabe3'),
    'rom': ('evidence/rom-copy-20261016-084055/physical-rom.bin',
            'a8e31821e8a1611d75867825914c58373c732742cb8f2235e8168c1d7e93b5aa'),
    'manual': ('sources/camera-research/A733_User_Manual_V1.00.txt',
               '219d3c21e5377d18a096765f429555d0693f7fa9b2ae2eb5bb3f225d8b6a09c2'),
    'datasheet': ('sources/A733-datasheet.txt',
                  '0c7c3c1984f49019f421b33089bc0f334cb084a489488ff10c69b7062a5accf0'),
}
BGR = {0x704: 'DMA0', 0x70c: 'DMA1', 0xac4: 'CE', 0xd0c: 'SMHC0', 0xd1c: 'SMHC1', 0xd2c: 'SMHC2',
       0xd3c: 'SMHC3', 0xc8c: 'NAND0', 0xd8c: 'UFS', 0xf1c: 'SPIF', 0x1304: 'USB0', 0x130c: 'USB1',
       0x135c: 'USB2', 0x138c: 'PCIE0', 0x141c: 'GMAC0', 0xa04: 'DE0', 0xa24: 'DI', 0xa44: 'G2D',
       0xa6c: 'EINK', 0xa8c: 'VE', 0xb04: 'NPU', 0xb24: 'GPU0', 0x1844: 'CSI', 0x1860: 'ISP clock',
       0x5e4: 'MBUS gates'}


def literals(data, base, end, mode):
    """Values of PC-relative word loads decoded at every instruction slot below end."""
    cs, step = Cs(CS_ARCH_ARM, mode), (2 if mode == CS_MODE_THUMB else 4)
    cs.detail = True
    found = {}
    for off in range(0, end - base, step):
        for i in cs.disasm(data[off:off + 4], base + off):
            if i.id == ARM_INS_LDR and i.operands[1].type == ARM_OP_MEM and i.operands[1].mem.base == ARM_REG_PC:
                pc = (i.address + 4) & ~3 if mode == CS_MODE_THUMB else i.address + 8
                slot = pc + i.operands[1].mem.disp
                if base <= slot <= base + len(data) - 4:
                    found.setdefault(struct.unpack_from('<I', data, slot - base)[0], []).append(i.address)
            break
    return found


def survey(found):
    remote = sorted(v for v in found if 0x07032000 <= v < 0x07034000 or 0x07050000 <= v < 0x070501c0
                    or v in (0x07010210, 0x0701021c))
    resets = sorted(BGR[v - 0x02002000] for v in found if v - 0x02002000 in BGR)
    engines = sorted(hex(v) for v in found if 0x04601000 <= v < 0x04606000 or 0x04024000 <= v < 0x04026000)
    return {'e902_or_cpuidle_start': [hex(v) for v in remote], 'master_resets': resets, 'dma_ce_registers': engines}


def arm(data, start, count):
    return [(i.mnemonic, i.op_str) for i in Cs(CS_ARCH_ARM, CS_MODE_ARM).disasm(data[start:start + 4 * count], start)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    raw = {}
    for name, (path, digest) in INPUTS.items():
        raw[name] = (harness.CACHE / path).read_bytes()
        assert hashlib.sha256(raw[name]).hexdigest() == digest, f'{name} identity'
    args.output.mkdir(parents=True, exist_ok=False)
    boot0, rom = raw['boot0'], raw['rom']

    # boot0 code ends below its string table at 0x5e700; a few wrappers are Arm code.
    found = literals(boot0, 0x47000, 0x5e700, CS_MODE_THUMB)
    for value, sites in literals(boot0, 0x47000, 0x5e700, CS_MODE_ARM).items():
        found.setdefault(value, []).extend(sites)
    b0 = survey(found)
    assert b0 == {'e902_or_cpuidle_start': [], 'master_resets': ['SMHC0'], 'dma_ce_registers': []}, b0
    # The arisc reset and RISC-V messages have no pointer anywhere in the image.
    words = {struct.unpack_from('<I', boot0, i)[0] for i in range(0, len(boot0) - 3, 2)}
    for text in (b'set arisc reset to assert state', b'set arisc reset to de-assert state', b'not get riscv base addr!'):
        assert 0x47000 + boot0.index(text) not in words, text
    # The parser copies an scp item to DRAM at 0x48100000; it does not start a core.
    assert boot0[0x5df98 - 0x47000:0x5df9c - 0x47000] == (0x6035d).to_bytes(4, 'little')
    assert boot0[0x6035d - 0x47000:0x60361 - 0x47000] == b'scp\0'
    assert boot0[0x5dfa4 - 0x47000:0x5dfa8 - 0x47000] == (0x48100000).to_bytes(4, 'little')

    rs = survey(literals(rom, 0, len(rom) - 4, CS_MODE_ARM))
    assert rs == {'e902_or_cpuidle_start': [], 'master_resets': ['CE', 'DMA0', 'NAND0', 'SMHC0', 'SMHC2', 'UFS'],
                  'dma_ce_registers': ['0x4601000', '0x4601100']}, rs
    # CE: both resets de-asserted, then the CE MBUS gate (CCU+0x5e4 bit 2), then both bus gates.
    assert struct.unpack_from('<I', rom, 0x21c8)[0] == 0x02002ac4
    assert arm(rom, 0x2190, 13)[1:] == [
        ('orr', 'r4, r4, #0x10000'), ('orr', 'r4, r4, #0x20000'), ('str', 'r4, [r0]'),
        ('sub', 'r0, r0, #0x4e0'), ('ldr', 'r4, [r0]'), ('orr', 'r4, r4, #4'), ('str', 'r4, [r0]'),
        ('add', 'r0, r0, #0x4e0'), ('ldr', 'r4, [r0]'), ('orr', 'r4, r4, #1'), ('orr', 'r4, r4, #2'),
        ('str', 'r4, [r0]')]
    # Both outcomes of the clock setup at 0x21cc reach that sequence through 0x2160.
    setup = arm(rom, 0x2444, 12)
    assert setup[3] == ('bl', '#0x2b30') and setup[6] == ('beq', '#0x246c') and setup[10] == ('bl', '#0x21cc')
    assert arm(rom, 0x2b30, 2) == [('mov', 'r0, #0'), ('bx', 'lr')] and arm(rom, 0xba8, 1) == [('bl', '#0x2444')]
    for call in (0x2250, 0x2270, 0x229c, 0x2300):
        assert arm(rom, call, 1) == [('bl', '#0x2160')]
    # DMA0: reset de-asserted and bus gate opened at 0x10d0; reasserted at 0xc91c.
    assert struct.unpack_from('<I', rom, 0x1168)[0] == 0x02002704 == struct.unpack_from('<I', rom, 0xc948)[0]
    assert ('orr', 'r2, r2, #0x10000') in arm(rom, 0x10d8, 16)
    assert ('bic', 'r0, r0, #0x10000') in arm(rom, 0xc91c, 4)

    manual = raw['manual'].decode()
    for text in ('RISCV_BGR_REG                           0x021C', 'CPUIDLE            0x07050000',
                 'CPUS_E902_CFG      0x07032000', 'DMA0_BGR_REG                   0x0704',
                 'MBUS_GATE_EN_REG               0x05E4', 'GPU0_GATING_REG                0x0B24',
                 'SPIF_BGR_REG                   0x0F1C', '11.5.5.7       SPI Flash Private DMA'):
        assert text in manual, text
    assert manual.count('OR100') == 2 and 'E902_CORE_RST' not in manual
    assert ('Some modules shown in this block diagram are not offered on all devices, please refer to\n'
            'A733_Datasheet.') in manual
    datasheet = raw['datasheet'].decode()
    assert 'RISC-V CPU                Single E902' in datasheet and 'OR100' not in datasheet
    ordering = datasheet[datasheet.index('Table 2-1 Ordering Information'):datasheet.index('3.1     Processors', datasheet.index('Table 2-1 Ordering Information'))]
    assert all(device in ordering for device in ('A733MX-HN3', 'A733MX-N3X', 'A733MX-1XX'))
    assert 'all the orderable devices listed' in ordering
    assert 'RISC-V E902 CPU, up to 200 MHz' in datasheet

    result = {'schema': 'remote-writers-sources-v1', 'physical_access': False,
              'inputs': {n: d for n, (_, d) in INPUTS.items()}, 'boot0': b0, 'board_rom': rs,
              'rom_ce_enable': '0x218c', 'rom_dma0_deassert': '0x10d0', 'rom_dma0_reassert': '0xc91c',
              'boot0_scp_destination': '0x48100000', 'manual_or100_mentions': 2,
              'or100_applicability': 'Not in the A733 device inventory; manual defers availability to datasheet.'}
    (args.output / 'remote-writers-sources.json').write_text(json.dumps(result, indent=1) + '\n')
    print(json.dumps(result, indent=1))


if __name__ == '__main__':
    main()
