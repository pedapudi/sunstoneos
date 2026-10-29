#!/usr/bin/env python3
"""Execute the linked OS success terminal with modeled service completion and GIC.

The source owners' completion acknowledgments are fixture inputs. Cache-line
maintenance is modeled in contiguous batches ending at each 2 MiB boundary;
the actual loop validates every visited block's mapping and both range ends.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('os_return_prior_check', ROOT / 'checks/cpu-runtime-qualification.py')
prior = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prior)
from harness import save


class Model(prior.Model):
    def __init__(self, image):
        super().__init__(image)
        self.ranges = [[] for _ in range(8)]
        with image.path.open('rb') as stream:
            elf = ELFFile(stream)
            symbols = list(elf.get_section_by_name('.symtab').iter_symbols())
            self.local = {}
            for name in ('ready', 'arrived'):
                matches = [s for s in symbols if s.name == name]
                assert len(matches) == 1, ('ambiguous local native hook', name)
                self.local[name] = matches[0]['st_value']
        self.spi_corrupt = False

    def code(self, uc, address, size, unused):
        ins = self.image.insn(uc, address)
        if ins.mnemonic == 'dc':
            args = ins.op_str.split(', ')
            assert args == ['civac', 'x24'], ins.op_str
            start = self.gpr('x24')
            end = min((start + 0x200000) & ~0x1fffff, self.gpr('x25'))
            assert end > start and (end - start) % 64 == 0
            self.ranges[self.core].append((start, end))
            # Execute the real next-loop arithmetic/branch at the end of a
            # modeled contiguous cache-maintenance batch.
            uc.reg_write(prior.fault.REGS[24], end - 64)
        super().code(uc, address, size, unused)

    def write(self, uc, access, address, size, value, unused):
        super().write(uc, access, address, size, value, unused)
        if self.clean_started and 0x46000000 <= address < 0x47000000:
            self.postclean.append((self.core, address, size, value))
        if 0x03400180 <= address < 0x03400200 or 0x03400280 <= address < 0x03400300:
            dst = address - 0x80
            self.put32(dst, self.u32(dst) & ~value)

    def read(self, uc, access, address, size, value, unused):
        super().read(uc, access, address, size, value, unused)
        if self.spi_corrupt and self.clean_started and address == 0x03400444:
            self.put32(address, self.u32(address) ^ 1)

    def initialize(self):
        assert self.call(0, 'a733_qualification_boot', (prior.EPOCH, prior.DEADLINE)) == 1
        self.call(0, 'os_cpu_return_spi_save')
        for c in range(8):
            assert self.call(c, 'a733_qualification_local', (c, prior.EPOCH)) == 1
        for i in range(512):
            self.put64(self.s['a733_cached_el3_l2'] + i * 8, (0x40000000 + i * 0x200000) | 0x725)
            self.put64(self.s['a733_cached_el3_l3'] + i * 8, (0x42000000 + i * 4096) | (1 << 54) | 0x443)
        self.put64(self.s['a733_cached_el3_l2'] + 128, self.s['a733_cached_el3_l3'] | 3)
        self.put32(self.local['ready'], 1)
        # Model the one configured THS route, after its owner stopped the
        # converter and local interrupt enables. Its pending bit may remain.
        self.put32(0x03400088, 1 << 7)
        self.put32(0x03400444, 0xd8 << 24)
        self.put32(0x03400208, 1 << 7)
        contexts = []
        for c in range(8):
            self.core = c
            self.reg[c].update(scr_el3=0x531, sctlr_el3=0x1005,
                               ttbr0_el3=self.s['a733_cached_el3_l1'], icc_sre_el3=15, icc_igrpen1_el3=0)
            self.set('tpidr_el1', c)
            self.set('sctlr_el1', 0x30d01805)
            self.set('tcr_el1', 0x80803519)
            self.set('icc_sre_el1', 1)
            self.set('icc_sre_el2', 9)
            self.put64(self.local['arrived'] + c * 8, prior.EPOCH)
            self.uc.mem_write(self.s['a733_cached_records'] + c * 128, struct.pack('<3Q', prior.EPOCH, c, 3))
            rdist = 0x03460000 + c * 0x20000
            self.put32(rdist + 0x14, 0)
            self.put32(rdist + 0x10100, 0x40000001)
            self.put32(rdist + 0x10200, 0x40000001)
            if c:
                self.put32(0x07052008 + c * 0x1000, 8)
            self.uc.reg_write(prior.fault.REGS[0], c)
            self.uc.reg_write(UC_ARM64_REG_SP, self.s['a733_cpu_el1_stacks'] + (c + 1) * 32768)
            self.uc.reg_write(UC_ARM64_REG_PC, self.s['a733_qualification_terminal_el0'])
            contexts.append(self.uc.context_save())
        return contexts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--epoch', type=lambda value: int(value, 0), default=prior.EPOCH)
    parser.add_argument('--case', action='append')
    args = parser.parse_args()
    prior.EPOCH = args.epoch
    args.out.mkdir(parents=True, exist_ok=False)
    image = prior.fault.Image(args.elf)
    rows = []
    cases = ('success', 'ordinary-exit-zero', 'wrong-core', 'missing-peer', 'active-interrupt',
                 'unrelated-pending', 'wrong-image-alias', 'wrong-arena-alias', 'expired-deadline',
                 'restore-readback', 'ack-without-off', 'ths-active', 'spi-restore-readback', 'service-not-ready')
    if args.case:
        assert set(args.case) <= set(cases), args.case
        cases = args.case
    for case in cases:
        model = Model(image)
        contexts = model.initialize()
        missing = None
        if case == 'ordinary-exit-zero':
            model.uc.context_restore(contexts[3])
            model.uc.reg_write(UC_ARM64_REG_PC, image.symbols['a733_cached_smc'])
            for reg in prior.fault.REGS[:3]:
                model.uc.reg_write(reg, 0)
            contexts[3] = model.uc.context_save()
        elif case == 'wrong-core':
            model.uc.context_restore(contexts[3])
            model.uc.reg_write(prior.fault.REGS[0], 2)
            contexts[3] = model.uc.context_save()
        elif case == 'missing-peer':
            missing = 3
        elif case == 'active-interrupt':
            model.put32(0x03460000 + 3 * 0x20000 + 0x10300, 1)
        elif case == 'unrelated-pending':
            model.put32(0x03460000 + 3 * 0x20000 + 0x10200, 0x40000003)
        elif case in ('wrong-image-alias', 'wrong-arena-alias'):
            block = 9 if case == 'wrong-image-alias' else (0x46000000 - 0x40000000) >> 21
            entry = image.symbols['a733_cached_el3_l2'] + block * 8
            model.put64(entry, model.u64(entry) & ~32)
        elif case == 'expired-deadline':
            model.put64(model.report + 32, model.time)
        elif case == 'restore-readback':
            model.corrupt_readback = True
        elif case == 'ack-without-off':
            model.skip_off = True
        elif case == 'ths-active':
            model.put32(0x03400308, 1 << 7)
        elif case == 'spi-restore-readback':
            model.spi_corrupt = True
        elif case == 'service-not-ready':
            model.put32(model.local['ready'], 0)
        if case != 'success' and case != 'expired-deadline':
            model.put64(model.report + 32, model.time + 300000)
        outcome = model.execute(contexts, missing=missing, budget=4000000)
        assert outcome == ('rom' if case == 'success' else 'retained'), (case, outcome)
        assert not model.postclean, (case, model.postclean[:4])
        if case == 'success':
            for ranges in model.ranges:
                expected = [(0x41200000, image.symbols['__heap_end']), (0x46000000, 0x47000000)]
                merged = []
                for lo, hi in ranges:
                    if merged and merged[-1][1] == lo:
                        merged[-1] = (merged[-1][0], hi)
                    else:
                        merged.append((lo, hi))
                assert merged == expected, (merged, expected)
            assert model.u32(0x03400088) == model.u32(0x03400444) == model.u32(0x03400208) == 0
            raw = bytes(model.uc.mem_read(model.report, 8880))
            (args.out / 'return-report.bin').write_bytes(raw)
        rows.append({'name': case, 'outcome': outcome, 'instructions': model.count,
                     'cache_batches': [len(x) for x in model.ranges]})
    save(args.out / 'results.json', {'physical_access': False, 'cases': rows,
         'model_boundary': 'Service completion is supplied; contiguous cache batches execute block/range checks.'})
    print('PASS linked OS CPU terminal:', len(rows), 'cases')


if __name__ == '__main__':
    main()
