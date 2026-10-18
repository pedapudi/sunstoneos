#!/usr/bin/env python3
"""Execute boot publication from the real supervised ELF; no physical cache model.

The existing instruction machine supplies CPU registers. This check adds explicit
security-selected translations and maintenance coverage, including initial EL1
data. It does not establish interconnect visibility or physical cache behavior.
"""
import argparse
import ctypes
import importlib.util
import json
from pathlib import Path
import struct
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('cached_model', ROOT/'checks/a733-smp-cached.py')
model = importlib.util.module_from_spec(spec)
spec.loader.exec_module(model)


class Publication(model.Machine):
    def __init__(self, image, case):
        super().__init__(image)
        self.case = case
        self.original = {'sctlr_el3': 0xc50838, 'tcr_el3': 0x81234560,
                         'ttbr0_el3': 0xabc000, 'mair_el3': 0x12345678}
        self.regs[0].update(self.original)
        self.maintained = set()
        self.table_cleaned = set()
        self.entered = False
        self.in_publication = False
        self.invalidated_instructions = False
        self.retained = False
        self.end = self.symbols['__bss_end']
        self.root = self.symbols['a733_supervised_publish_l1']
        self.expected = set(range(0x41000000, self.end, 64))
        for name in ('a733_cached_el1_l1', 'a733_cached_el1_l2'):
            self.expected.update(range(self.symbols[name], self.symbols[name]+4096, 64))
        self.sentinel = 0x41000000
        self.cpus[0].reg_write(UC_ARM64_REG_SP, self.symbols['__a733_smp_boot_stack_top'])

    def invoke(self, name, argument=0):
        self.put(0, 'x0', argument)
        self.cpus[0].reg_write(UC_ARM64_REG_LR, self.sentinel)
        self.cpus[0].emu_start(self.symbols[name], self.sentinel, count=2000000)
        assert self.retained or self.cpus[0].reg_read(UC_ARM64_REG_PC) == self.sentinel
        return self.reg(0, 'x0')

    def instruction(self, u, address, size, core):
        ins = next(self.decoder.disasm(bytes(u.mem_read(address, 4)), address))
        op, args = ins.mnemonic, ins.op_str.split(', ')
        m = self.regs[core]
        if address == self.symbols['a733_supervised_ns_publish']:
            self.in_publication = True
        if op == 'msr' and args[0] == 'sctlr_el3' and self.reg(core, args[1]) & 1:
            assert core == 0 and not any(self.active[1:])
            assert self.reg(core, args[1]) == self.original['sctlr_el3'] | 1
            assert m['ttbr0_el3'] == self.root and m['tcr_el3'] == 0x80823019
            assert m['mair_el3'] == 0x44ff00
            assert self.table_cleaned == set(range(self.root, self.root+8192, 64))
            assert self.q(self.root+8) == self.root+4096+3
            assert self.q(self.root+16) == 0x0040000040000769
            code = self.q(self.root+4096+64)
            assert not code & (1 << 5) and (code >> 2) & 7 == 2 and not code & (1 << 54)
            # The copied hierarchy preserves the Secure Device stack mapping.
            stack = u.reg_read(UC_ARM64_REG_SP)
            l3 = self.q(self.root+4096+16*8) & ~4095
            desc = self.q(l3+((stack >> 12) & 511)*8)
            assert desc & 3 == 3 and not desc & (1 << 5) and (desc >> 2) & 7 == 0
            self.entered = True
        if op == 'dc' and self.in_publication:
            va = self.reg(core, args[1])
            if args[0] == 'cvac':
                assert not m['sctlr_el3'] & 1 and self.root <= va < self.root+8192
                self.table_cleaned.add(va)
            elif args[0] == 'ivac':
                assert self.entered and m['sctlr_el3'] == self.original['sctlr_el3'] | 1
                assert 0x80000000 <= va < 0xc0000000
                pa = va - 0x40000000
                assert pa in self.expected
                self.maintained.add(pa)
                if self.case == 'deadline-during-maintenance': self.clock = 1001
            else:
                raise AssertionError('Unexpected maintenance '+ins.op_str)
        if op == 'ic':
            assert args == ['iallu'] and self.maintained == self.expected
            self.invalidated_instructions = True
        if op == 'msr' and args[0] == 'sctlr_el3' and not self.reg(core, args[1]) & 1:
            assert self.invalidated_instructions
        if op == 'msr' and args[0] == 'ttbr0_el3' and self.entered and self.case == 'restore-readback-failure':
            super().instruction(u, address, size, core)
            m['ttbr0_el3'] ^= 4096
            return
        if op == 'wfi':
            self.retained = True
            u.emu_stop()
            return
        super().instruction(u, address, size, core)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--boot-only', action='store_true')
    args = parser.parse_args()
    results = []
    if args.boot_only:
        class BootPublication(Publication):
            def instruction(self, u, address, size, core):
                if address == self.symbols['a733_cpu_start_init']:
                    assert self.maintained == self.expected and self.invalidated_instructions
                    assert all(self.regs[0][k] == v for k, v in self.original.items())
                    assert self.regs[0]['tcr_el1'] & 0x800080 == 0x800080
                    assert self.regs[0]['sctlr_el1'] & 1 and not any(self.active[1:])
                    self.reached_release = True
                    u.emu_stop()
                    return
                if address == self.symbols['a733_smp_stop']:
                    raise AssertionError(('boot refused', self.reg(core, 'x0'), self.reg(core, 'x1')))
                if address == self.symbols['a733_supervised_ns_publish']:
                    assert self.regs[0]['tcr_el1'] & 0x800080 == 0x800080
                    assert self.regs[0]['sctlr_el1'] & 1
                super().instruction(u, address, size, core)

        machine = BootPublication(args.image, 'boot')
        machine.mmio.update({0x02050014: 0, 0x02050010: 0, 0x070501c0: 0, 0x070501d4: 0})
        build_id = machine.read(machine.symbols['a733_smp_build_id'], 32)
        # Actual v3 ABI; nonzero fixture authority is never a physical admission.
        data = struct.pack('<6Q', 0x4153373353554333, 3, 224, 5, 7, model.MASK-7)+build_id
        data += struct.pack('<18Q', 0, 24000000, 480000000, 480000000, 9,
                            *([0]*7), 1, 0, 1, 2, 3, 4)
        ctypes.memmove(machine.ram_pointer+0x1004000, data, len(data))
        machine.reached_release = False
        machine.cpus[0].emu_start(machine.symbols['a733_smp_boot'], 0, count=2500000)
        assert machine.reached_release
        results.append({'case': 'real-boot-publishes-before-first-secondary-release', 'pass': True,
                        'maintained_lines': len(machine.maintained)})
        args.output.write_text(json.dumps({'image_sha256': model.sha(args.image), 'cases': results,
            'limits': 'Modeled boot through first power-owner construction; no secondary execution or physical claim.'}, indent=2)+'\n')
        print('PASS real boot publishes before first secondary release')
        return
    for case in ('published-code-data-tables', 'deadline-before-entry',
                 'deadline-during-maintenance', 'restore-readback-failure'):
        machine = Publication(args.image, case)
        assert machine.invoke('a733_cached_memory_prepare') == 1
        if case == 'deadline-before-entry': machine.clock = 1001
        result = machine.invoke('a733_supervised_ns_publish', 1000)
        if case == 'restore-readback-failure':
            assert machine.retained
        else:
            assert not machine.retained
            assert result == (case == 'published-code-data-tables')
            assert all(machine.regs[0][k] == v for k, v in machine.original.items())
        if case == 'deadline-before-entry':
            assert not machine.entered and not machine.maintained and not machine.table_cleaned
        else:
            assert machine.maintained == machine.expected and machine.invalidated_instructions
        results.append({'case': case, 'pass': True, 'maintained_lines': len(machine.maintained),
                        'retained': machine.retained})
    args.output.write_text(json.dumps({'image': str(args.image), 'image_sha256': model.sha(args.image),
        'cases': results, 'limits': __doc__}, indent=2)+'\n')
    print('PASS four publication cases; physical publication remains unqualified for this image')


if __name__ == '__main__':
    main()
