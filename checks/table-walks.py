#!/usr/bin/env python3
"""Execute application-table publication and the forced-walk interval in linked OS images."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import sys
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_READ
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'checks'))
import harness
sys.path.insert(0, str(ROOT / 'tools'))
import a733_tcr
GATE_RUNS = [['--image', '{elf:ret-walks-uncached}', '--image', '{elf:ret-walks-cached}', '--output', '{output}']]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def machine(image):
    u = harness.machine()
    u.mem_map(0x40000000, 0x08000000)
    for address, data in image.parts:
        u.mem_write(address, data)
    u.reg_write(UC_ARM64_REG_SP, 0x47e00000)
    u.reg_write(UC_ARM64_REG_LR, 0x47fff000)
    return u


LEDGER = load('table_walk_ledger', ROOT / 'checks/ledger-nc.py')


class PublicationModel(LEDGER.Model):
    """Observe application publication after an independently prepared ledger."""
    def __init__(self, image, variant):
        super().__init__(image)
        self.variant = variant
        self.tracking = False
        self.arena = image.address('arena_pages', unit='apps/os/large.c')
        self.active_address = image.address('os_space_active')
        self.dirty, self.cleaned = set(), set()
        self.trace = dict(stores=0, maintenance=0, published=False, barrier=None)
        # Peripheral memory admission is outside this case. The ledger,
        # allocation, export and application table builder execute normally.
        names = ['os_npu_pool_prepare', 'os_capture_memory', 'os_usb_service_memory', 'board_fail_note']
        if image.symbols.get('os_headset_memory', set()) - {0}:
            names.append('os_headset_memory')
        self.peripherals = {image.address(name): name for name in names}
        harness.hook_addresses(self.u, self.peripherals, self.code)

    def store(self, u, access, address, size, value, unused):
        if self.tracking and self.arena <= address < self.arena + 13 * 4096:
            assert not self.trace['published'], 'table store after publication'
            for line in range(address & ~63, (address + size + 63) & ~63, 64):
                self.dirty.add(line)
                self.cleaned.discard(line)
            self.trace['stores'] += size
        if self.tracking and address == self.active_address and value:
            assert self.trace['stores'] >= 8192 and self.trace['published'], 'table stores lack a completion barrier'
            u.emu_stop()
        super().store(u, access, address, size, value, unused)

    def code(self, u, address, size, unused):
        name = self.peripherals.get(address)
        if name:
            if name == 'board_fail_note':
                raise AssertionError(('space preparation refused', hex(u.reg_read(UC_ARM64_REG_X0 + 1))))
            u.reg_write(UC_ARM64_REG_X0, 1)
            u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))
            return
        if address in (LEDGER.RETURN, self.entry, self.critical_enter, self.critical_exit) or address in self.return_hooks:
            return super().code(u, address, size, unused)
        if address not in self.instructions:
            self.instructions[address] = next(self.decoder.disasm(bytes(u.mem_read(address, 4)), address))
        op = self.instructions[address]
        args = op.op_str.lower().split(', ') if op.op_str else []
        if op.mnemonic == 'mrs' and args[1] == 'ttbr0_el1':
            u.reg_write(UC_ARM64_REG_X0 + int(args[0][1:]), self.root)
            return self.skip(address)
        if self.tracking and op.mnemonic == 'dc':
            assert args[0] == 'civac', args
            target = u.reg_read(UC_ARM64_REG_X0 + int(args[1][1:])) & ~63
            assert self.arena <= target < self.arena + 13 * 4096, 'publication cleans outside its table arena'
            self.trace['maintenance'] += 1
            if self.variant != 'missing-clean':
                self.cleaned.add(target)
        elif self.tracking and op.mnemonic == 'dsb' and self.dirty and self.variant != 'missing-barrier':
            assert args[0] in ('ish', 'sy'), args
            if self.uncached_walk:
                assert self.dirty <= self.cleaned, 'uncached walker sees unclean descriptors'
            self.trace['published'] = True
            self.trace['barrier'] = args[0]
        return super().code(u, address, size, unused)

    def reserve_region(self):
        owner = self.read(self.image.address('owner_asid', unit='apps/os/large.c'), 2)
        assert owner, 'prepared ledger has no application ASID'
        request, result = LEDGER.SCRATCH, LEDGER.SCRATCH + 256
        self.u.mem_write(request, bytes(128))
        fields = {'bytes': (0x200000, 8), 'align': (0x200000, 8), 'base': (0x80000000, 8),
                  'owner': (owner, 2), 'flags': (9, 2), 'access': (3, 1)}
        for name, (value, width) in fields.items():
            self.u.mem_write(request + self.field('umem_request', name), value.to_bytes(width, 'little'))
        assert self.call('umem_alloc', self.ledger, request, result) == 0
        assert self.describe(0x80000000) == dict(base=0x80000000, bytes=0x200000, owner=owner, policy=0)


def publication(image, tcr, variant='normal'):
    model = PublicationModel(image, variant)
    # The ledger's own boot-table preparation is covered by ledger-nc.py.
    # Finish it before observing the separate application-table publication.
    model.bootstrap()
    model.reserve_region()
    model.tracking = True
    model.u.emu_start(image.address('os_large_prepare'), 0, count=1000000)
    trace = model.trace
    assert trace['published']
    root = model.read(image.address('space', unit='apps/os/large.c') + model.field('app_space', 'root'))
    assert root == model.arena, 'the builder did not use the application table arena'
    child = model.read(root + 2 * 8)
    assert child & 3 == 3, 'a partial upper region does not get a private L2 table'
    child &= 0x0000fffffffff000
    assert model.arena <= child < model.arena + 13 * 4096
    descriptor = model.read(child)
    assert descriptor & 0x0000ffffffe00000 == 0x80000000 and descriptor & (3 << 6) == 1 << 6
    assert model.read(child + 8) & (3 << 6) == 0, 'a neighboring EL1-only block becomes visible at EL0'
    if tcr & 0xf00:
        assert trace['maintenance'] == 0 and trace['barrier'] == 'ish', trace
    else:
        assert trace['maintenance'] == 13 * 4096 // 64 and trace['barrier'] == 'sy', trace
    return trace


def loop(image):
    u = machine(image)
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    counts = dict(cycles=0, ticks=0, data=0, instruction=0, bus=0)
    loads = []
    result = 0x47f00000

    def code(cpu, address, size, cookie):
        if address == 0x47fff000:
            u.emu_stop()
            return
        counts['cycles'] += 100
        counts['ticks'] += 1
        op = next(decoder.disasm(bytes(u.mem_read(address, 4)), address))
        if op.mnemonic == 'tlbi':
            assert op.op_str == 'vmalle1'
            counts['instruction'] += 1
            u.reg_write(UC_ARM64_REG_PC, address + 4)
        elif op.mnemonic == 'mrs':
            dest, reg = op.op_str.split(', ')
            key = {'pmccntr_el0': 'cycles', 'cntpct_el0': 'ticks', 'pmevcntr4_el0': 'data',
                   'pmevcntr5_el0': 'instruction', 'pmevcntr3_el0': 'bus'}[reg]
            u.reg_write(UC_ARM64_REG_X0 + int(dest[1:]), counts[key])
            u.reg_write(UC_ARM64_REG_PC, address + 4)

    def read(cpu, access, address, size, value, cookie):
        assert 0x40000000 <= address < 0x48000000 and not 0x42000000 <= address < 0x42200000
        assert size == 8
        counts['data'] += 1
        counts['bus'] += 1
        loads.append(address)

    u.hook_add(UC_HOOK_CODE, code)
    u.hook_add(UC_HOOK_MEM_READ, read)
    u.reg_write(UC_ARM64_REG_X0, result)
    u.reg_write(UC_ARM64_REG_X0 + 1, 1024)
    u.emu_start(image.address('table_walk_loop'), 0, count=100000)
    values = struct.unpack('<6Q', bytes(u.mem_read(result, 48)))
    assert len(loads) == 1024 and values[2:5] == (1024, 1024, 1024), values
    assert len({(a - 0x40000000) >> 21 for a in loads}) == 63
    return dict(iterations=len(loads), mapped_blocks=63, measured_data_walks=values[2])


def decoder_cases(tcr, out):
    host = load('table_walk_host', ROOT / 'apps/table-walks/host.py')
    frozen = out / f'decoder-{tcr:x}.py'
    host.freeze(frozen)
    host = load('frozen_table_walk_host', frozen)
    clock = b'CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=900 transitions=3 dsu_mhz=1274 core=6 pattern=0'
    def payload(value, cycles=1024 * 700):
        return (host.HEAD.pack(b'SSTWALK1', value, 0x414fd0b1, 6, 1024, 24000000) +
                clock.ljust(256, b'\0') * 2 + host.SAMPLE.pack(cycles, round(cycles * 24 / 2002), 1024, 1024, 0, 0) * 6)
    a = host.decode(payload(a733_tcr.UNCACHED_WALK))
    b = host.decode(payload(a733_tcr.CACHED_WALK, 1024 * 400))
    assert host.compare(a, b)['gain']
    assert not host.compare(a, host.decode(payload(a733_tcr.CACHED_WALK)))['gain']
    assert not host.compare(a, host.decode(payload(a733_tcr.CACHED_WALK, 1024 * 800)))['gain']
    for offset, size, reason in ((8, 8, 'translation-control profile'), (24, 8, 'CPU'), (host.HEAD.size, 1, 'operating points'),
                                (host.HEAD.size + 512 + 16, 8, 'walk counters did not advance')):
        bad = bytearray(payload(tcr))
        bad[offset:offset + size] = bytes(size)
        harness.refused(lambda: host.decode(bytes(bad)), reason, ValueError)
    harness.refused(lambda: host.decode(payload(tcr)[:-1]), 'length', ValueError)
    bad = bytearray(payload(tcr))
    struct.pack_into('<Q', bad, 8, tcr ^ (1 << 12))
    harness.refused(lambda: host.decode(bytes(bad)), 'translation-control profile', ValueError)
    # Native core-6 record 1794465341494995923-e29c2535: the warm-up counts
    # 1066 data walks, followed by 1024, 1024, 1023, 1023, 1024. TRM C2.1
    # explicitly permits approximate absolute event counts from pipeline effects.
    observed = bytearray(payload(tcr))
    for i, count in enumerate((1066, 1024, 1024, 1023, 1023, 1024)):
        struct.pack_into('<Q', observed, host.HEAD.size + 512 + i * host.SAMPLE.size + 16, count)
    warm_cycles = 1024 * 900
    struct.pack_into('<2Q', observed, host.HEAD.size + 512, warm_cycles, round(warm_cycles * 24 / 2002))
    observed_result = host.decode(bytes(observed))
    assert [s['dtlb_walk'] for s in observed_result['samples']] == [1066, 1024, 1024, 1023, 1023, 1024]
    assert observed_result['median_cycles_per_iteration'] == 700
    incompatible = host.decode(payload(a733_tcr.CACHED_WALK, 1024 * 400))
    for sample in incompatible['samples'][1:]:
        sample['dtlb_walk'] = 512
    assert host.compare(a, incompatible)['gain']
    incompatible['samples'][-1]['dtlb_walk'] = 1024
    assert host.compare(a, incompatible)['gain']
    # Native cached record 1794466032800309049-ed5f20e8 returns successfully
    # but reports 1816 measured data walks per block. The accepted fixed-work
    # rule compares digests and instruction-side walks, reporting data walks.
    cached_data = bytearray(payload(a733_tcr.CACHED_WALK, 389726))
    for i in range(host.SAMPLES):
        struct.pack_into('<Q', cached_data, host.HEAD.size + 512 + i * host.SAMPLE.size + 16, 1818 if i == 0 else 1816)
    captured_cached = host.decode(bytes(cached_data))
    assert captured_cached['median_cycles_per_iteration'] == 380.591796875
    accepted = host.compare(a, captured_cached)
    assert accepted['gain'] and accepted['data_walks_cached'] == [1816] * 5
    captured_cached['samples'][-1]['itlb_walk'] = 1023
    harness.refused(lambda: host.compare(a, captured_cached), 'instruction-walk counts differ', ValueError)
    captured_cached['samples'][-1]['itlb_walk'] = 1024
    captured_cached['samples'][-1]['digest'] = 1
    harness.refused(lambda: host.compare(a, captured_cached), 'load digests differ', ValueError)
    return 17


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, action='append', required=True)
    p.add_argument('--decoder-only', action='store_true', help='run only decoder cases after a host fixture correction')
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.decoder_only:
        counts = [decoder_cases(tcr, args.output) for tcr in (a733_tcr.UNCACHED_WALK, a733_tcr.CACHED_WALK)]
        harness.save(args.output / 'result.json', dict(passed=True, physical_access=False, decoder_cases=counts))
        print('PASS frozen decoder: A76 data-walk observations, fixed-work acceptance and mismatched work refusal')
        return
    reports = []
    for path in args.image:
        image = harness.Image(path, layout_names=('umem', 'umem_request', 'umem_buf', 'umem_extent',
            'umem_el1_map', 'umem_policy_table', 'app_space'))
        tcr = image.tcr = a733_tcr.for_image(path)
        image.sizes = LEDGER.structure_sizes(path, ('umem_policy_table',))
        report = dict(image=str(path), tcr=hex(tcr), publication=publication(image, tcr), loop=loop(image))
        harness.refused(lambda: publication(image, tcr, 'missing-barrier'), 'completion barrier', AssertionError)
        if not tcr & 0xf00:
            harness.refused(lambda: publication(image, tcr, 'missing-clean'), 'unclean descriptors', AssertionError)
        report['decoder_cases'] = decoder_cases(tcr, args.output)
        reports.append(report)
    harness.save(args.output / 'result.json', dict(passed=True, physical_access=False, images=reports,
        scope='Linked instruction execution; cache visibility and PMU events are modeled. No physical latency or coherence claim.'))
    print('PASS linked application publication, barrier omissions, forced-walk addresses and decoder acceptance')


if __name__ == '__main__':
    main()
