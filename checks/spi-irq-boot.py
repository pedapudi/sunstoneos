#!/usr/bin/env python3
"""Check SPI3 inherited-interrupt refusal before common distributor writes."""
import argparse
import hashlib
import importlib.util
import json
import re
import struct
import subprocess
from pathlib import Path
import unicorn.arm64_const as arm
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PSTATE, UC_ARM64_REG_TPIDR_EL1

ROOT = Path(__file__).resolve().parents[1]
CASES = ('irq-quiet', 'poll-quiet', 'enabled', 'pending', 'active')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'session', 'contract', 'output'):
        p.add_argument('--'+name, type=Path, required=True)
    p.add_argument('--case', action='append', choices=CASES)
    a = p.parse_args()
    image_hash = hashlib.sha256(a.image.read_bytes()).hexdigest()
    spec = importlib.util.spec_from_file_location('spi_boot_image', ROOT/'checks/storage-read-native.py')
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    e = m.Image(a.image, layout_names=('a733_smp_contract',))
    raw, template = a.session.read_bytes(), a.contract.read_bytes()
    assert len(raw) == 832 and struct.unpack_from('<3Q', raw, 592) == (0x4153373353504131, 2, 832)
    assert len(template) >= 224 and not any(template[224:])
    fields = e.layouts['a733_smp_contract']
    ops = {}
    for line in subprocess.check_output(['/usr/lib/llvm-21/bin/llvm-objdump', '-d', '--no-show-raw-insn', str(a.image)], text=True).splitlines():
        match = re.match(r'^\s*([0-9a-f]+):\s+mrs\s+x(\d+),\s*(CurrentEL|MPIDR_EL1|SCTLR_EL3|CNTPCT_EL0)\s*$', line, re.I)
        if match:
            ops[int(match[1], 16)] = (int(match[2]), match[3].lower())
    assert ops
    a.output.mkdir(parents=True, exist_ok=True)
    for case in a.case or CASES:
        output = a.output/(case+'.json')
        assert not output.exists(), ('Preserve completed evidence', output)
        session, contract = bytearray(raw), bytearray(template[:224])
        if case == 'poll-quiet':
            struct.pack_into('<Q', session, 600, 1)
        contract[fields['reserved']+16:fields['reserved']+48] = hashlib.sha256(session).digest()
        for name, value in dict(magic=0x4153373353504331, armed_at=100,
                                startup_deadline=24000100, diagnostic_deadline=1440000100,
                                expires_at=1440000100).items():
            struct.pack_into('<Q', contract, fields[name], value)
        u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.reg_write(UC_ARM64_REG_PSTATE, 13)
        u.reg_write(UC_ARM64_REG_TPIDR_EL1, 0)
        for base, size in ((0x41000000, 0x1200000), (0x90000000, 0x200000),
                           (0x03400000, 0x10000), (0x02050000, 0x1000), (0x07050000, 0x1000)):
            u.mem_map(base, size)
        for base, data in e.parts:
            if data:
                u.mem_write(base, data)
        u.mem_write(0x42004000, bytes(contract))
        u.mem_write(0x41f00000, bytes(session))
        regs = {0x03400000: 0x30, 0x03400004: 7}
        offsets = dict(enabled=0x104, pending=0x204, active=0x304)
        if case in offsets:
            regs[0x03400000+offsets[case]] = 1 << 27
        # Neighboring ownership must not be mistaken for the SPI3 interrupt.
        for offset in (0x104, 0x204, 0x304):
            regs[0x03400000+offset] = regs.get(0x03400000+offset, 0) | (1 << 26)
        writes, terminal = [], []

        def ret(value=0):
            u.reg_write(UC_ARM64_REG_X0, value)
            u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))

        hooks = {e.address(name): name for name in (
            'a733_smp_counter', 'a733_smp_contract_valid', 'board_session_prepare',
            'spi_session_prepare', 'spi_admitted_session', 'a733_cpu_coherence_publish',
            'a733_cached_prepare', 'a733_cpu_start_init', 'a733_smp_stop')}

        def code(cpu, at, size, unused):
            name = hooks.get(at)
            if name is None and at in ops:
                reg, kind = ops[at]
                u.reg_write(getattr(arm, 'UC_ARM64_REG_X'+str(reg)), 12 if kind == 'currentel' else 200 if kind == 'cntpct_el0' else 0)
                u.reg_write(UC_ARM64_REG_PC, at+4)
            elif name == 'a733_smp_counter':
                ret(200)
            elif name == 'spi_admitted_session':
                ret(0x41f00000)
            elif name in ('a733_smp_contract_valid', 'board_session_prepare',
                          'spi_session_prepare', 'a733_cpu_coherence_publish'):
                ret(1)
            elif name == 'a733_cached_prepare':
                ret()
            else:
                terminal.append(name)
                u.emu_stop()

        for at in set(hooks) | set(ops):
            u.hook_add(UC_HOOK_CODE, code, begin=at, end=at)

        def read(cpu, access, at, size, value, unused):
            cpu.mem_write(at, regs.get(at, 0).to_bytes(size, 'little'))

        def write(cpu, access, at, size, value, unused):
            writes.append((at, value))
            if 0x03400180 <= at < 0x034001a0:
                regs[at-0x80] = regs.get(at-0x80, 0) & ~value
            else:
                regs[at] = value

        u.hook_add(UC_HOOK_MEM_READ, read, begin=0x03400000, end=0x0340ffff)
        u.hook_add(UC_HOOK_MEM_WRITE, write, begin=0x03400000, end=0x0340ffff)
        u.reg_write(UC_ARM64_REG_SP, 0x901fe000)
        u.reg_write(UC_ARM64_REG_LR, 0x901ff000)
        u.emu_start(e.address('a733_smp_boot'), 0x901ff000, count=50000)
        if case in offsets:
            assert terminal == ['a733_smp_stop'] and not writes, (case, terminal, writes)
            assert (u.reg_read(UC_ARM64_REG_X0), u.reg_read(UC_ARM64_REG_X1)) == (3, 0x30d)
        else:
            assert terminal == ['a733_cpu_start_init'], (case, terminal, writes)
            assert any(at == 0x034061d8 for at, value in writes) == (case == 'irq-quiet')
        output.write_text(json.dumps(dict(
            passed=True, case=case, image_sha256=image_hash,
            session_sha256=hashlib.sha256(session).hexdigest(), writes=writes, physical_access=False,
            scope='Actual private boot guard and conditional route before secondary construction. Session validation, publication, system registers and GIC effects modeled. No CPU startup or physical execution.'
        ), indent=2)+'\n')
        print(case, 'PASS', flush=True)


if __name__ == '__main__':
    main()
