#!/usr/bin/env python3
"""Execute the linked OS success terminal with modeled service completion and GIC.

The source owners' completion acknowledgments are fixture inputs. The terminal
validates the mapping of every 2 MiB block of image RAM, of the WORK and OUTPUT
regions of the image's application manifest and of the published device ranges,
and both ends of each range; an image without such regions skips the region alias
case. The caches are cleaned by set/way as each core goes down: the model supplies
the measured geometry of each core (CLIDR and CCSIDR from docs/hardware/a733-facts.json:
Cortex-A55 on cores 0 to 5, Cortex-A76 on 6 and 7),
records every DC CISW operand per core and checks that each secondary covers its
L1 and L2, that core 0 covers L1, L2 and L3, that core 0's L3 sweep follows every
secondary's last operation, and that no core stores to Normal memory after its own
first operation. Images linked before the application manifest use the fixed
recognizer arena.
"""
import argparse
import importlib.util
import json
import hashlib
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['{elf:ret-npu-load}', '--out', '{output}']]
spec = importlib.util.spec_from_file_location('os_return_prior_check', ROOT / 'checks/cpu-runtime-qualification.py')
prior = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prior)
spec = importlib.util.spec_from_file_location('os_return_records', ROOT / 'apps/os/host.py')
records = importlib.util.module_from_spec(spec)
spec.loader.exec_module(records)
from harness import Image, save


def cleaned_regions(path):
    """The application ranges that the success terminal cleans after image RAM."""
    if records.image_manifest(path)['manifest'] is None:
        return [(0x46000000, 0x47000000)]
    return [(r['address'], r['address'] + r['bytes']) for r in records.backup_regions(path, scratch=True)]


# The caches as measured on the board (docs/hardware/a733-facts.json, facts cpu.clidr_el1 and cache.ccsidr_el1.selN, status
# measured): cores 0 to 5 are Cortex-A55 (L1D 32 KiB 4 ways, L2 128 KiB 4 ways), cores 6 and 7 Cortex-A76 (L1D 64 KiB 4 ways,
# L2 256 KiB 8 ways), and the DSU L3 is 1 MiB with 16 ways. CCSIDR is indexed by the CSSELR value the sweep writes.
FACTS = json.loads((ROOT / 'docs/hardware/a733-facts.json').read_text())['facts']
CLIDR = {f['core']: f['value'] for f in FACTS if f['name'] == 'cpu.clidr_el1'}
CCSIDR = {core: {sel: next(f['value'] for f in FACTS if f['name'] == 'cache.ccsidr_el1.sel%d' % sel and f['core'] == core)
                 for sel in range(8)} for core in range(8)}
assert len(set(CLIDR.values())) == 1 and (CLIDR[0] >> 24) & 7 == 3


def sweep_operands(core, level, table=None):
    """Every DC CISW operand that cleans cache `level` of `core` with the given CCSIDR table (the measured one by default)."""
    value = (table or CCSIDR)[core][level * 2]
    ways, sets, line = ((value >> 3) & 0x3ff) + 1, ((value >> 13) & 0x7fff) + 1, (value & 7) + 4
    shift = 32 - (ways - 1).bit_length()
    return {(way << shift) | (set_ << line) | (level << 1) for way in range(ways) for set_ in range(sets)}


def assert_sweeps(model):
    """Each secondary cleans exactly its own L1 and L2, core 0 also the L3, each at the geometry of its own core type."""
    for core in range(8):
        levels = (0, 1, 2) if core == 0 else (0, 1)
        expected = sorted(op for level in levels for op in sweep_operands(core, level))
        assert sorted(model.sweeps[core]) == expected, (core, len(model.sweeps[core]), len(expected))
        assert len(set(model.sweeps[core])) == len(model.sweeps[core]), core


class Model(prior.Model):
    def __init__(self, image, ccsidr=None):
        super().__init__(image)
        self.ccsidr = ccsidr or CCSIDR
        self.sweeps = [[] for _ in range(8)]
        self.sweep_order = []
        self.swept = set()
        self.regions = cleaned_regions(image.path)
        with image.path.open('rb') as stream:
            elf = ELFFile(stream)
            symbols = list(elf.get_section_by_name('.symtab').iter_symbols())
            self.local = {}
            for name in ('ready', 'arrived'):
                matches = [s for s in symbols if s.name == name]
                assert len(matches) == 1, ('ambiguous local native hook', name)
                self.local[name] = matches[0]['st_value']
        self.pool_deadline = False
        # The ranges that the OS publishes for the terminal to clean (apps/os/device-ranges.h): two pages, the first with a range in
        # level-1 slot 1 and one in upper memory, the second with one range from another gigabyte, so that the walk crosses a page.
        self.published = [(0x60000000, 0x400000), (0x100000000, 0x200000), (0x140000000, 0x200000)]
        self.walk_symbol = image.symbols['os_cpu_return_walk']
        self.pages = (0x70000000, 0x70001000)
        for page in self.pages:
            self.uc.mem_map(page, 0x1000)
        self.spi_corrupt = False
        self.gpu_spi_corrupt = False

    def get(self, name):
        if name == 'clidr_el1':
            return CLIDR[self.core]
        if name == 'ccsidr_el1':
            return self.ccsidr[self.core][self.reg[self.core].get('csselr_el1', 0)]
        if name == 'id_aa64mmfr2_el1':
            return 0
        return super().get(name)

    def code(self, uc, address, size, unused):
        ins = self.image.insn(uc, address)
        if ins.mnemonic == 'dc':
            args = ins.op_str.split(', ')
            assert args[0] == 'cisw', ins.op_str
            for c in range(8):
                assert self.u64(self.report + 688 + c * 1024 + 8) == prior.EPOCH, ('sweep before all captures', c)
                assert self.u64(self.report + 688 + c * 1024 + 104) == prior.EPOCH, ('sweep before all drains', c)
            self.sweeps[self.core].append(self.gpr(args[1]))
            self.sweep_order.append((self.core, (self.gpr(args[1]) >> 1) & 7))
            self.swept.add(self.core)
            self.count += 1
            self.advance(address)
            return
        super().code(uc, address, size, unused)

    def write(self, uc, access, address, size, value, unused):
        super().write(uc, access, address, size, value, unused)
        if self.pool_deadline and self.walk_symbol <= address < self.walk_symbol + 128 and address == self.walk_symbol + 16 * self.core:
            # The walk of the published ranges has begun: the deadline expires now.
            self.put64(self.report + 32, self.time)
        regions = self.regions + [(a, a + n) for a, n in self.published]
        if self.core in self.swept and (0x41200000 <= address < 0x42000000 or any(low <= address < high for low, high in regions)):
            self.postclean.append((self.core, address, size, value))
        if 0x03400180 <= address < 0x03400200 or 0x03400280 <= address < 0x03400300:
            dst = address - 0x80
            self.put32(dst, self.u32(dst) & ~value)

    def read(self, uc, access, address, size, value, unused):
        super().read(uc, access, address, size, value, unused)
        if self.gpu_spi_corrupt and self.clean_started and address==0x0340045c:
            self.put32(address,self.u32(address)^1)
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
        # The platform maps every gigabyte of DRAM above 0x80000000 at EL3 (platform/a733-smp-cached/memory.c), whatever the
        # application holds there, because the ranges a device uses are chosen at run time.
        for g in range(2, 13):
            self.put64(self.s['a733_cached_el3_l1'] + g * 8, (g << 30) | (1 << 54) | 0x765)
        first, second = self.pages
        self.put64(first, second)
        self.put64(first + 8, 2)
        for i, (base, size) in enumerate(self.published[:2]):
            self.put64(first + 16 + 16 * i, base); self.put64(first + 24 + 16 * i, size)
        self.put64(second, 0)
        self.put64(second + 8, 1)
        self.put64(second + 16, self.published[2][0]); self.put64(second + 24, self.published[2][1])
        self.put64(self.s['os_device_ranges_head'], first)
        if 'os_npu_published_report' in self.s:
            # Terminal-only fixture: preceding service close/local stop and
            # worker park are supplied inputs; their real functions have a
            # separate linked integration check.
            layout = Image(self.image.path, {'os_npu_application', 'npu_runtime', 'npu_service'})
            npu = layout.address('npu', '/apps/os/npu.c')
            nf = layout.layouts['os_npu_application']
            service = npu + nf['runtime'] + layout.layouts['npu_runtime']['service']
            self.put32(npu + nf['stopped'], 1)
            self.put32(npu + nf['active'], 1)
            self.put32(service + layout.layouts['npu_service']['irq_stopped'], 1)
            self.put32(service + layout.layouts['npu_service']['runtime_parked'], 1)
            self.put64(service + layout.layouts['npu_service']['runtime'],
                       npu + nf['runtime']) # installed runtime-mode authority, supplied fixture
            report = self.s['os_npu_published_report']
            self.put32(report + 12, 1)
            self.put32(report + 56, 1)
        self.put32(self.local['ready'], 1)
        # Model the one configured THS route, after its owner stopped the
        # converter and local interrupt enables. Its pending bit may remain.
        self.put32(0x03400088, 1 << 7)
        self.put32(0x03400444, 0xd8 << 24)
        self.put32(0x03400208, 1 << 7)
        if 'os_gpu_return_report' in self.s:
            self.put32(0x03400088,self.u32(0x03400088)|(1<<31))
            self.put32(0x0340045c,0xd8<<24)
            self.put32(0x03400208,self.u32(0x03400208)|(1<<31))
            for i in (54,55):
                self.put64(self.s['a733_cached_el1_l2']+i*8,(0x40000000+i*0x200000)|(3<<6)|(1<<53)|(1<<54)|0x705)
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
    regions = cleaned_regions(args.elf)
    cases = ('success', 'ordinary-exit-zero', 'wrong-core', 'missing-peer', 'active-interrupt',
             'unrelated-pending', 'wrong-image-alias', 'expired-deadline',
             'restore-readback', 'ack-without-off', 'ths-active', 'spi-restore-readback', 'service-not-ready')
    if any(low < 0x80000000 for low, _ in regions):
        cases += ('wrong-region-alias',)
    if any(low >= 0x80000000 for low, _ in regions):
        # Regions from 0x80000000 are cleaned through the EL3 level-1 table: a gigabyte with no block, with another
        # gigabyte's block, or with the wrong attributes parks the return before any of those regions is cleaned.
        cases += ('upper-region-no-block', 'upper-region-wrong-block', 'upper-region-wrong-alias')
    # The published device ranges: a range that an EL3 block does not map as normal write-back memory, a page or range outside
    # the DRAM above the image, and one that is not on a 64-byte line park the return before the ranges are cleaned.
    cases += ('wrong-published-alias', 'published-upper-no-block', 'published-page-outside-dram', 'published-range-unaligned',
              'published-range-wraps', 'published-deadline')
    if '__os_npu_pool_start' in image.symbols:
        raise SystemExit('the image still links a fixed NPU pool')
    if 'os_npu_published_report' in image.symbols:
        cases += ('npu-late-irq',)
    if 'os_gpu_return_report' in image.symbols:
        cases += ('gpu-active','gpu-spi-restore-readback','wrong-pool-alias')
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
        elif case in ('wrong-image-alias', 'wrong-region-alias'):
            block = 9 if case == 'wrong-image-alias' else (next(low for low, _ in regions if low < 0x80000000) - 0x40000000) >> 21
            entry = image.symbols['a733_cached_el3_l2'] + block * 8
            model.put64(entry, model.u64(entry) & ~32)
        elif case in ('upper-region-no-block', 'upper-region-wrong-block', 'upper-region-wrong-alias'):
            g = next(low for low, _ in regions if low >= 0x80000000) >> 30
            entry = image.symbols['a733_cached_el3_l1'] + g * 8
            if case == 'upper-region-no-block':
                model.put64(entry, 0)
            elif case == 'upper-region-wrong-block':
                model.put64(entry, model.u64(entry) + (1 << 30))
            else:
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
        elif case == 'gpu-active':
            model.put32(0x03400308,1<<31)
        elif case == 'gpu-spi-restore-readback':
            model.gpu_spi_corrupt=True
        elif case == 'wrong-pool-alias':
            block=(image.symbols['__device_pool_start']-0x40000000)>>21
            entry=image.symbols['a733_cached_el3_l2']+block*8
            model.put64(entry,model.u64(entry)&~32)
        elif case == 'wrong-published-alias':
            block = (model.published[0][0] - 0x40000000) >> 21
            entry = image.symbols['a733_cached_el3_l2'] + block * 8
            model.put64(entry, model.u64(entry) & ~32)
        elif case == 'published-upper-no-block':
            model.put64(image.symbols['a733_cached_el3_l1'] + (model.published[1][0] >> 30) * 8, 0)
        elif case == 'published-page-outside-dram':
            model.put64(image.symbols['os_device_ranges_head'], 0x41f00000)
        elif case == 'published-range-unaligned':
            model.put64(model.pages[0] + 16, model.published[0][0] + 1)
        elif case == 'published-range-wraps':
            model.put64(model.pages[0] + 24, 0xfffffffffffff000)
        elif case == 'published-deadline':
            model.pool_deadline = True
        elif case == 'npu-late-irq':
            layout = Image(image.path, {'os_npu_application', 'npu_runtime', 'npu_service'})
            npu = layout.address('npu', '/apps/os/npu.c')
            service = npu + layout.layouts['os_npu_application']['runtime'] + layout.layouts['npu_runtime']['service']
            model.put32(service + layout.layouts['npu_service']['irq_pending'], 1)
        elif case == 'service-not-ready':
            model.put32(model.local['ready'], 0)
        if case != 'success' and case != 'expired-deadline':
            model.put64(model.report + 32, model.time + 300000)
        outcome = model.execute(contexts, missing=missing, budget=4000000)
        assert outcome == ('rom' if case == 'success' else 'retained'), (case, outcome)
        assert not model.postclean, (case, model.postclean[:4])
        if case == 'success':
            # Each secondary cleans its own L1 and L2 by set/way, exactly once per set and way; core 0 also cleans L3, and only
            # after every secondary's last operation.
            assert_sweeps(model)
            l3 = [i for i, (c, level) in enumerate(model.sweep_order) if c == 0 and level == 2]
            last_secondary = max(i for i, (c, _) in enumerate(model.sweep_order) if c)
            assert l3 and min(l3) > last_secondary, 'core 0 cleaned L3 before a secondary finished'
            # Every core recorded the counter at the start and end of its walk of the published ranges, and the duration of its
            # sweep, in the image RAM and report words that the runner copies out.
            for core in range(8):
                assert model.u64(model.report + 688 + core * 1024 + 56) > 0, core
            walk = image.symbols['os_cpu_return_walk']
            for core in range(8):
                start, end = model.u64(walk + 16 * core), model.u64(walk + 16 * core + 8)
                assert start and end >= start, (core, start, end)
            assert model.u32(0x03400088) == model.u32(0x03400444) == model.u32(0x03400208) == 0
            if 'os_gpu_return_report' in image.symbols:
                assert model.u32(0x0340045c)==0
                for i in (54,55):assert model.u64(image.symbols['a733_cached_el1_l2']+i*8)&(3<<6)==3<<6
            raw = bytes(model.uc.mem_read(model.report, 8880))
            (args.out / 'return-report.bin').write_bytes(raw)
        rows.append({'name': case, 'outcome': outcome, 'instructions': model.count,
                     'sweep_operations': [len(x) for x in model.sweeps]})
    # A sweep fixed to the Cortex-A55 shape would leave dirty lines in a Cortex-A76 core: present cores 6 and 7 with the A55
    # geometry, and the check of the operands must fail.
    if not args.case:
        a55 = {**CCSIDR, 6: CCSIDR[0], 7: CCSIDR[0]}
        model = Model(image, ccsidr=a55)
        assert model.execute(model.initialize(), budget=4000000) == 'rom'
        try:
            assert_sweeps(model)
        except AssertionError:
            rows.append({'name': 'a76-with-a55-geometry', 'outcome': 'refused', 'sweep_operations': [len(x) for x in model.sweeps]})
        else:
            raise AssertionError('cores 6 and 7 swept with the A55 geometry and the check accepted it')
    save(args.out / 'results.json', {'physical_access': False, 'cases': rows,
         'image_sha256': hashlib.sha256(args.elf.read_bytes()).hexdigest(),
         'checker_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
         'model_boundary': 'Service completion and the cache geometry are supplied; the terminal executes the block and range checks and the set/way sweep.'})
    print('PASS linked OS CPU terminal:', len(rows), 'cases')


if __name__ == '__main__':
    main()
