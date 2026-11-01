#!/usr/bin/env python3
"""Execute native DMA retirement IRQ masking with delayed GIC propagation.

The linked callback, GIC disable and completion observer execute in Unicorn.
Posted writes, distributor propagation and architectural counter values are
modeled. These checks establish software behavior, not board qualification.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_PSTATE, UC_ARM64_REG_SP, UC_ARM64_REG_X0

from harness import Image, barriers, hook_addresses, instruction_sites, refused

UNIT = 'apps/os/dma.c'
GIC = 0x03400000
INTID = 143
BIT = 1 << (INTID % 32)
ENABLE = GIC + 0x100 + 4 * (INTID // 32)
DISABLE = GIC + 0x180 + 4 * (INTID // 32)
SCRATCH = 0x81000000  # Above the complete extended device pool (ends 0x79000000).
STOP = SCRATCH + 0x1f000
COUNTER_HZ = 24000000
BUDGET = COUNTER_HZ // 10000
MAX64 = (1 << 64) - 1


def private_layout(path):
    """Resolve the anonymous DMA state type through its compilation-unit variable."""
    with path.open('rb') as stream:
        elf = ELFFile(stream)
        segments = [(int(p['p_vaddr']), int(p['p_memsz'])) for p in elf.iter_segments()
                    if p['p_type'] == 'PT_LOAD' and p['p_memsz']]
        candidates = []
        for cu in elf.get_dwarf_info().iter_CUs():
            if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith(UNIT):
                continue
            for die in cu.iter_DIEs():
                name = die.attributes.get('DW_AT_name')
                if die.tag == 'DW_TAG_variable' and name and name.value == b'dma':
                    kind = die.get_DIE_from_attribute('DW_AT_type')
                    while kind.tag in ('DW_TAG_typedef', 'DW_TAG_const_type', 'DW_TAG_volatile_type'):
                        kind = kind.get_DIE_from_attribute('DW_AT_type')
                    fields = {child.attributes['DW_AT_name'].value.decode():
                              int(child.attributes['DW_AT_data_member_location'].value)
                              for child in kind.iter_children() if child.tag == 'DW_TAG_member'}
                    candidates.append((fields, int(kind.attributes['DW_AT_byte_size'].value)))
    assert len(candidates) == 1, candidates
    return *candidates[0], segments


class Machine:
    def __init__(self, image, fields, segments, case):
        self.image, self.fields, self.case = image, fields, case
        self.cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.cpu.reg_write(UC_ARM64_REG_PSTATE, 4)
        ranges = sorted((address & ~4095, (address + size + 4095) & ~4095)
                        for address, size in segments)
        merged = []
        for start, end in ranges:
            assert end <= SCRATCH or start >= SCRATCH + 0x20000
            if merged and start <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], end)
            else:
                merged.append([start, end])
        for start, end in merged:
            self.cpu.mem_map(start, end - start)
        self.cpu.mem_map(SCRATCH, 0x20000)
        self.cpu.mem_map(GIC, 0x10000)
        for address, data in image.parts:
            if data:
                self.cpu.mem_write(address, data)
        self.dma = image.address('dma', UNIT)
        self.irq = self.dma + fields['irq']
        self.spi = self.irq + image.layouts['dma_service_i2s_irq']['spi']
        self.masked = self.irq + image.layouts['dma_service_i2s_irq']['masked']
        self.put(self.spi + image.layouts['gicv3_spi']['distributor'], GIC, 8)
        self.put(self.spi + image.layouts['gicv3_spi']['intid'], INTID)
        self.put(self.masked, 0)
        self.events, self.writes, self.counts = [], [], {'mask': 0, 'gic_mask': 0, 'gic_masked': 0}
        self.pending, self.issued, self.completed = False, False, False
        self.rwp_reads = 0
        self.enable_reads = 0
        self.now = MAX64 - BUDGET + 1 if case == 'counter-overflow' else 1000
        self.start = self.now
        self.clock_reads = []
        self.at = {image.address('mask', UNIT): 'mask', image.address('gicv3_spi_mask'): 'gic_mask',
                   image.address('gicv3_spi_masked'): 'gic_masked'}
        self.counters = {address: int.from_bytes(self.cpu.mem_read(address, 4), 'little') & 31
                         for address in instruction_sites(image.parts,
                             lambda word: word & 0xffffffe0 in (0xd53be020, 0xd53be040))}
        assert self.counters, 'no CNTPCT_EL0/CNTVCT_EL0 instruction in linked image'
        barrier_sites = instruction_sites(image.parts, lambda word: 'dsb' in barriers(word))
        hook_addresses(self.cpu, [*self.at, *self.counters, *barrier_sites, STOP], self.code)
        self.cpu.hook_add(UC_HOOK_MEM_READ, self.read, begin=GIC, end=GIC + 0xffff)
        self.cpu.hook_add(UC_HOOK_MEM_WRITE, self.write, begin=GIC, end=GIC + 0xffff)

    def put(self, address, value, size=4):
        self.cpu.mem_write(address, value.to_bytes(size, 'little'))

    def get(self, address, size=4):
        return int.from_bytes(self.cpu.mem_read(address, size), 'little')

    def code(self, cpu, address, size, unused):
        if address == STOP:
            cpu.emu_stop()
        elif address in self.at:
            self.counts[self.at[address]] += 1
        elif address in self.counters:
            self.clock_reads.append(self.now)
            cpu.reg_write(UC_ARM64_REG_X0 + self.counters[address], self.now)
            cpu.reg_write(UC_ARM64_REG_PC, address + 4)
        elif self.pending:
            # A posted ICENABLER store reaches the distributor only at DSB.
            self.pending, self.issued = False, True
            self.events.append({'event': 'disable-dsb', 'counter': self.now})

    def write(self, cpu, access, address, size, value, unused):
        self.writes.append([address, size, value])
        assert address == DISABLE and size == 4 and value == BIT, self.writes
        assert not self.pending and not self.issued, 'disable retried'
        self.pending = True
        self.events.append({'event': 'posted-disable', 'counter': self.now})

    def read(self, cpu, access, address, size, value, unused):
        assert size == 4
        if address == GIC:
            assert self.issued and not self.pending, 'RWP observed before disable DSB'
            self.rwp_reads += 1
            step = BUDGET + 1 if self.case == 'completion-after-deadline' else (
                BUDGET if self.case == 'completion-at-deadline' else BUDGET // 10)
            self.now += step
            clear = self.case != 'stuck-rwp' and (
                self.rwp_reads >= 3 or self.case in ('completion-after-deadline', 'completion-at-deadline'))
            self.completed = clear and self.case != 'still-enabled'
            self.put(address, 0 if clear else 1 << 31)
            self.events.append({'event': 'rwp-read', 'counter': self.now, 'pending': not clear})
        elif address == ENABLE:
            self.enable_reads += 1
            assert self.issued and self.rwp_reads >= 1
            # Old enable state remains visible until propagation completes.
            self.put(address, 0 if self.completed else BIT)
            self.events.append({'event': 'enable-read', 'counter': self.now,
                                'enabled': not self.completed})
        else:
            raise AssertionError(f'unexpected GIC read {address:#x}')

    def run(self):
        self.cpu.reg_write(UC_ARM64_REG_SP, SCRATCH + 0x18000)
        self.cpu.reg_write(UC_ARM64_REG_LR, STOP)
        self.cpu.reg_write(UC_ARM64_REG_X0, self.irq)
        self.cpu.emu_start(self.image.address('mask', UNIT), STOP, count=100000)
        assert self.cpu.reg_read(UC_ARM64_REG_PC) == STOP, 'unbounded mask loop'
        result = self.cpu.reg_read(UC_ARM64_REG_X0) & 0xffffffff
        result = result - (1 << 32) if result & (1 << 31) else result
        fault = self.get(self.dma + self.fields['mask_fault'])
        deadline = self.get(self.dma + self.fields['mask_deadline'], 8)
        now = self.get(self.dma + self.fields['mask_now'], 8)
        expected_fault = {'delayed-normal': 0, 'stuck-rwp': 3, 'still-enabled': 2,
                          'completion-after-deadline': 3, 'completion-at-deadline': 3,
                          'counter-overflow': 4}[self.case]
        normal = self.case == 'delayed-normal'
        assert result == (1 if normal else -1), (self.case, result, fault)
        assert fault == expected_fault, (self.case, fault, expected_fault)
        assert self.get(self.masked) == int(normal), (self.case, 'software mask')
        assert self.counts['mask'] == 1
        if self.case == 'counter-overflow':
            assert not self.writes and not self.rwp_reads and self.clock_reads == [self.start]
            assert not self.counts['gic_mask'] and not self.counts['gic_masked']
            assert now == self.start and deadline == 0
        else:
            assert len(self.writes) == 1 and self.counts['gic_mask'] == 1
            assert self.counts['gic_masked'] == self.rwp_reads
            assert deadline == self.start + BUDGET and now == self.now
            if self.case in ('delayed-normal', 'still-enabled'):
                assert self.rwp_reads == 3 and self.enable_reads == 1, 'returned before delayed RWP completion'
                assert now < deadline
            elif self.case == 'stuck-rwp':
                assert self.rwp_reads == 10 and self.enable_reads == 0 and now == deadline
            else:
                assert self.rwp_reads == 1 and self.enable_reads == 1 and now >= deadline
        return {'passed': True, 'return': result, 'mask_fault': fault, 'deadline': deadline,
                'now': now, 'irq_masked': self.get(self.masked), 'entries': self.counts,
                'counter_reads': self.clock_reads, 'rwp_reads': self.rwp_reads,
                'enable_reads': self.enable_reads, 'writes': self.writes, 'events': self.events}


def wait_removal_control(image, fields, segments):
    """Remove the linked loop's retry branch and require the normal case to fail."""
    machine = Machine(image, fields, segments, 'delayed-normal')
    entry = image.address('mask', UNIT)
    observer = image.address('gicv3_spi_masked')
    words = [(entry + offset, int.from_bytes(machine.cpu.mem_read(entry + offset, 4), 'little'))
             for offset in range(0, 256, 4)]

    def relative(word, bits):
        value = word & ((1 << bits) - 1)
        return value - (1 << bits) if value & (1 << (bits - 1)) else value

    calls = [address for address, word in words if word >> 26 == 0x25
             and address + relative(word, 26) * 4 == observer]
    assert len(calls) == 1, ('completion observer call', calls)
    retries = [(address, word) for address, word in words if word & 0xff00001f == 0x54000001
               and address + relative(word >> 5, 19) * 4 == calls[0] - 4]
    assert len(retries) == 1, ('conditional completion retry', retries)
    address, word = retries[0]
    machine.put(address, 0xd503201f)  # NOP: fall through without waiting for propagation.
    failure = refused(machine.run, 'returned before delayed RWP completion', AssertionError)
    return {'passed': True, 'mutation': 'remove completion-loop retry in emulated instruction memory',
            'address': address, 'original_word': word, 'mutated_word': 0xd503201f,
            'refusal': failure, 'rwp_reads': machine.rwp_reads, 'events': machine.events}


class TokenBudget(Machine):
    """Execute the retirement wrapper with an owned task and a busy registry."""
    def __init__(self, image, fields, segments):
        super().__init__(image, fields, segments, 'delayed-normal')
        self.binding = SCRATCH + 0x2000
        self.task = SCRATCH + 0x6000
        self.startup = image.layouts['a733_dma_startup']
        self.put(self.binding + self.startup['initialized'], 1)
        self.put(self.binding + self.startup['task'], self.task, 8)
        self.put(self.binding + self.startup['clock'], SCRATCH + 0x7000, 8)
        self.put(self.binding + self.startup['registry'], SCRATCH + 0x8000, 8)
        self.calls = {'scheduler': 0, 'task': 0, 'clock': 0, 'token': 0, 'platform': 0}
        self.services = {
            image.address('xTaskGetSchedulerState'): 'scheduler',
            image.address('xTaskGetCurrentTaskHandle'): 'task',
            image.address('a733_counter_clock_now'): 'clock',
            image.address('a733_clock_token_status'): 'token',
            image.address('a733_dma_platform_verify_adopted'): 'platform',
        }
        hook_addresses(self.cpu, self.services, self.service)

    def service(self, cpu, address, size, unused):
        name = self.services[address]
        self.calls[name] += 1
        if name == 'scheduler':
            result = 2  # taskSCHEDULER_RUNNING
        elif name == 'task':
            result = self.task
        elif name == 'clock':
            assert cpu.reg_read(UC_ARM64_REG_X0) == SCRATCH + 0x7000
            self.put(cpu.reg_read(UC_ARM64_REG_X0 + 1), self.now, 8)
            result = 1
        elif name == 'token':
            assert cpu.reg_read(UC_ARM64_REG_X0) == SCRATCH + 0x8000
            result = 1  # A733_CLOCK_BUSY
        else:
            raise AssertionError('busy token reached platform verification')
        cpu.reg_write(UC_ARM64_REG_X0, result)
        cpu.reg_write(UC_ARM64_REG_PC, cpu.reg_read(UC_ARM64_REG_LR))

    def state(self, field, size=4):
        return self.get(self.binding + self.startup[field], size)

    def call(self, deadline):
        self.cpu.reg_write(UC_ARM64_REG_SP, SCRATCH + 0x18000)
        self.cpu.reg_write(UC_ARM64_REG_LR, STOP)
        self.cpu.reg_write(UC_ARM64_REG_X0, self.binding)
        self.cpu.reg_write(UC_ARM64_REG_X0 + 1, deadline)
        self.cpu.emu_start(self.image.address('a733_dma_startup_retire_adopted'), STOP, count=100000)
        assert self.cpu.reg_read(UC_ARM64_REG_PC) == STOP, 'unbounded token retirement'
        assert not self.writes and not self.rwp_reads
        return self.cpu.reg_read(UC_ARM64_REG_X0) & 0xffffffff

    def run_budget(self):
        observations = []
        for now in (1000, 1040, 1099, 1100):
            self.now = now
            result = self.call(1100)
            assert result == (1 if now < 1100 else 3), (now, result, self.calls)
            assert self.state('retire_deadline', 8) == 1100
            assert self.state('retire_now', 8) == now
            assert self.state('failed') == int(now == 1100)
            assert self.state('retire_fault') == (2 if now == 1100 else 0)
            observations.append({'now': now, 'return': result, 'calls': dict(self.calls)})
            if now == 1000:
                before = dict(self.calls)
                assert self.call(1200) == 3, 'caller extended retirement deadline'
                assert self.state('retire_deadline', 8) == 1100
                assert self.calls['clock'] == before['clock'] and self.calls['token'] == before['token']
                assert not self.state('failed')
        assert self.calls['clock'] == 4 and self.calls['token'] == 3 and not self.calls['platform']
        before = dict(self.calls)
        self.now = 1200
        assert self.call(1100) == 3
        assert self.calls == before and self.state('retire_fault') == 2
        assert self.state('retire_now', 8) == 1100, 'later refusal overwrote first fault'
        return {'passed': True, 'observations': observations, 'fixed_deadline': 1100,
                'changed_deadline_refused': True, 'first_fault_preserved': True,
                'calls': self.calls, 'writes': self.writes}

    def run_retired(self):
        # Model a previously completed retirement; the wrapper's fast path must
        # remain complete when the old deadline has elapsed.
        self.put(self.binding + self.startup['retired'], 1)
        self.put(self.binding + self.startup['retire_deadline'], 1100, 8)
        self.now = 1200
        assert self.call(1100) == 0
        assert not self.calls['clock'] and not self.calls['token'] and not self.calls['platform']
        assert not self.state('failed') and not self.state('retire_fault')
        return {'passed': True, 'previously_retired_state_modeled': True,
                'now': self.now, 'return': 0, 'calls': self.calls, 'writes': self.writes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='new evidence directory')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    image = Image(args.image, {'dma_service_i2s_irq', 'gicv3_spi', 'a733_dma_startup'})
    fields, size, segments = private_layout(args.image)
    report = {'evidence': 'linked AArch64 instructions; modeled GIC and counter; no board qualification',
              'command': [sys.executable, *sys.argv], 'image': str(args.image.resolve()),
              'image_sha256': hashlib.sha256(args.image.read_bytes()).hexdigest(),
              'callback': image.address('mask', UNIT), 'dma': image.address('dma', UNIT),
              'dma_size': size, 'dma_fields': fields, 'counter_hz': COUNTER_HZ,
              'budget_ticks': BUDGET, 'cases': {}}
    for case in ('delayed-normal', 'stuck-rwp', 'still-enabled', 'completion-after-deadline',
                 'completion-at-deadline', 'counter-overflow'):
        machine = Machine(image, fields, segments, case)
        try:
            report['cases'][case] = machine.run()
        except Exception as error:
            report['cases'][case] = {'passed': False, 'error': repr(error),
                                      'pc': machine.cpu.reg_read(UC_ARM64_REG_PC),
                                      'events': machine.events, 'writes': machine.writes}
            (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
            raise
        (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        print(f'PASS native DMA IRQ retirement mask: {case}')
    report['token_budget'] = TokenBudget(image, fields, segments).run_budget()
    report['already_retired'] = TokenBudget(image, fields, segments).run_retired()
    report['wait_removal_control'] = wait_removal_control(image, fields, segments)
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS native DMA token contention deadline, fixed budget, first fault and retired fast path')


if __name__ == '__main__':
    main()
