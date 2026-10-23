#!/usr/bin/env python3
"""Check the CPU PMU module under instruction emulation with modeled system registers.

platform/a733/cpu_pmu.c is compiled with the board image's SMP flags and the
eight-core A733 configuration, linked with a stub vTaskCoreAffinityGet, and run
under Unicorn. The model checks the event encodings and filters, the
PMCNTENSET, PMINTENCLR, PMUSERENR and PMCR values, that P and C are never
written, PMCR_EL0.N below 6 and 0, unimplemented events, overflow marking,
readback refusals, reconfiguration after PMCR_EL0.E reads 0, every begin and
end refusal, the system-register accesses and barriers of the configured path,
and the memory each call touches. It decodes the record layout from the
image's debug information and the header, verifies the pinned sources in
checks/cpu-pmu-sources.json, and checks the MDCR_EL2 write in both
continuation sources, which the module needs before any image uses it.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'checks'))
from harness import CACHE, CLANG, LLD, Image, environment, system_register_access  # noqa: E402

FREERTOS = CACHE / 'sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
FLAGS = ['--target=aarch64-none-elf', '-march=armv8-a', '-mstrict-align', '-mgeneral-regs-only', '-ffreestanding',
         '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
         '-ffunction-sections', '-fdata-sections', '-O2', '-g', '-Wall', '-Wextra', '-Werror']
TEXT, DATA, STACK, BUFFERS, END = 0x10000, 0x20000, 0x30000, 0x40000, 0x1fffc
EVENTS = [0x08, 0x03, 0x17, 0x19, 0x34, 0x35]
RECORD = '<6BH2I2Q6IQ'
RECORD_FIELDS = ['format', 'core', 'counters', 'supported', 'overflow', 'status', 'reserved0', 'midr', 'cntfrq',
                 'cycles', 'ticks', 'events', 'reserved1']
STATUS = {'OK': 0, 'BAD_ARGUMENT': 1, 'NOT_PINNED': 2, 'NO_COUNTERS': 3, 'CONFIG_MISMATCH': 4, 'NOT_BEGUN': 5,
          'CORE_CHANGED': 6, 'GENERATION_CHANGED': 7, 'DISABLED': 8}
DAIFSET_I = 0xd50342df  # msr daifset, #2
BARRIERS = {'isb': (0xfffff0ff, 0xd50330df), 'dsb': (0xfffff0ff, 0xd503309f), 'dmb': (0xfffff0ff, 0xd50330bf),
            'svc': (0xffe0001f, 0xd4000001)}
CURRENT_EL = (3, 0, 4, 2, 2)
CONFIG = {'pmceid0', 'pmceid1', 'pmccfiltr', 'pmintenset', 'pmuserenr', 'pmcntenset', 'midr', 'cntfrq'} | \
    {f'pmevtyper{n}' for n in range(6)}


def register_names():
    names = {(3, 3, 9, 12, 0): 'pmcr', (3, 3, 9, 12, 1): 'pmcntenset', (3, 3, 9, 12, 3): 'pmovsclr',
             (3, 3, 9, 12, 6): 'pmceid0', (3, 3, 9, 12, 7): 'pmceid1', (3, 3, 9, 13, 0): 'pmccntr',
             (3, 3, 9, 14, 0): 'pmuserenr', (3, 0, 9, 14, 1): 'pmintenset', (3, 0, 9, 14, 2): 'pmintenclr',
             (3, 3, 9, 14, 3): 'pmovsset', (3, 3, 14, 15, 7): 'pmccfiltr', (3, 3, 14, 0, 1): 'cntpct',
             (3, 3, 14, 0, 0): 'cntfrq', (3, 0, 0, 0, 0): 'midr', (3, 0, 13, 0, 4): 'tpidr',
             (3, 3, 4, 2, 1): 'daif'}
    for n in range(6):
        names[(3, 3, 14, 8 + (n >> 3), n & 7)] = f'pmevcntr{n}'
        names[(3, 3, 14, 12 + (n >> 3), n & 7)] = f'pmevtyper{n}'
    return names


SYSREGS = register_names()


def decode(data):
    """A record's fields from its 64 little-endian bytes."""
    values = struct.unpack(RECORD, data)
    record = dict(zip(RECORD_FIELDS[:11], values[:11]))
    record['events'] = list(values[11:17])
    record['reserved1'] = values[17]
    return record


def measured_rate(record):
    """cycles x counter rate / ticks, the rate rule of docs/cpu-clock.md, in Hz."""
    assert record['status'] == STATUS['OK'] and record['ticks'] > 0
    return record['cycles'] * record['cntfrq'] // record['ticks']


def reset_core(core=0, n=6, ceid0=None, ceid1=None):
    """System registers of one core after a warm reset, at EL1 with IRQs unmasked.

    The A55 TRM lists PMCEID0 bits for 0x03, 0x08, 0x17 and 0x19 and PMCEID1
    bits 20 and 21 for 0x34 and 0x35; the defaults implement all six."""
    if ceid0 is None:
        ceid0 = (1 << 0x03) | (1 << 0x08) | (1 << 0x17) | (1 << 0x19)
    if ceid1 is None:
        ceid1 = (1 << 0x14) | (1 << 0x15)
    state = {'pmcr': 0x41003000 & ~(0x1f << 11) | (n << 11) | 0x8, 'pmcntenset': 0, 'pmovs': 0x80000000,
             'pmceid0': ceid0, 'pmceid1': ceid1, 'pmccntr': 0x1234567890, 'pmuserenr': 0xf,
             'pmintenset': 0xffffffff, 'pmccfiltr': 0xf8000000, 'cntpct': 0x5000, 'cntfrq': 24000000,
             'midr': 0x412fd050 if core < 6 else 0x414fd0b1, 'tpidr': core, 'daif': 0}
    for i in range(6):
        state[f'pmevcntr{i}'] = 0xdead0000 + i
        state[f'pmevtyper{i}'] = 0xffff
    return state


class Machine:
    """The linked module in Unicorn; registers is the modeled state of the core that runs a call."""

    def __init__(self, elf):
        self.image = Image(elf, {'a733_cpu_pmu_region', 'a733_cpu_pmu_record', 'core_state'})
        self.text = [(a, d) for a, d in self.image.parts if a < DATA][0]
        self.stub = self.image.address('vTaskCoreAffinityGet')
        self.affinity = self.image.address('stub_affinity')
        self.state = self.image.address('a733_cpu_pmu_state')
        self.region = BUFFERS
        self.record = BUFFERS + 0x100

    def call(self, name, registers, args, affinity=1, faults=()):
        """Run one function. Returns its 32-bit result and a log of register,
        barrier and memory events. faults names registers whose writes are ignored."""
        from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, \
            UC_MEM_WRITE
        from unicorn.arm64_const import (UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0,
                                         UC_ARM64_REG_X1, UC_ARM64_REG_X29, UC_ARM64_REG_X30)
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        for base, size in [(TEXT, 0x10000), (DATA, 0x10000), (STACK, 0x10000), (BUFFERS, 0x1000)]:
            uc.mem_map(base, size)
        for address, data in self.image.parts:
            uc.mem_write(address, data)
        uc.mem_write(self.state, self.memory_state)
        uc.mem_write(BUFFERS, self.buffers)
        uc.mem_write(self.affinity, struct.pack('<Q', affinity))
        log = []
        r = registers
        n = (r['pmcr'] >> 11) & 0x1f
        gpr = [UC_ARM64_REG_X0 + i for i in range(29)] + [UC_ARM64_REG_X29, UC_ARM64_REG_X30]

        def key(fields):
            name = SYSREGS[fields]
            if name[:8] in ('pmevcntr', 'pmevtype'):
                assert int(name[-1]) < n, f'{name} accessed with PMCR_EL0.N = {n}'
            return name

        # Unicorn's CPU model implements four event counters and no hook for
        # unknown registers, so the code hook emulates every MRS and MSR
        # (register) and skips the instruction.
        def mrs(machine, rt, fields):
            name = key(fields)
            if name in ('cntpct', 'pmccntr') or name.startswith('pmevcntr'):
                assert r['daif'] & 0x80, f'{name} read with IRQs unmasked'
            value = {'pmovsset': r['pmovs']}.get(name, r.get(name))
            assert value is not None, name
            log.append(('read', name))
            if rt != 31:
                machine.reg_write(gpr[rt], value)

        def msr(machine, rt, fields):
            name, value = key(fields), 0 if rt == 31 else machine.reg_read(gpr[rt])
            log.append(('write', name, value))
            if name in faults:
                return
            if name == 'pmcr':
                assert not value & 6, 'PMCR_EL0 P or C written'
                r['pmcr'] = value & ~6
            elif name == 'pmcntenset':
                r[name] |= value
            elif name == 'pmintenclr':
                r['pmintenset'] &= ~value
            elif name == 'pmovsclr':
                r['pmovs'] &= ~value
            elif name.startswith('pmevtyper'):
                r[name] = value & 0xf800ffff
            elif name.startswith('pmevcntr'):
                assert r['daif'] & 0x80, f'{name} written with IRQs unmasked'
                r[name] = value & 0xffffffff
            else:
                assert name in ('pmccfiltr', 'pmuserenr', 'daif'), name
                r[name] = value

        def code(machine, address, size, user):
            if self.stub <= address < self.stub + 8:
                if address == self.stub:
                    log.append(('affinity',))
                return
            word = struct.unpack('<I', machine.mem_read(address, 4))[0]
            log.append(('instruction',))
            if word == DAIFSET_I:
                r['daif'] |= 0x80
                log.append(('mask',))
            for barrier, (mask, value) in BARRIERS.items():
                if word & mask == value:
                    log.append((barrier,))
            if system_register_access(word):
                fields = (2 + ((word >> 19) & 1), (word >> 16) & 7, (word >> 12) & 15, (word >> 8) & 15,
                          (word >> 5) & 7)
                assert fields != CURRENT_EL, 'CurrentEL read'
                assert word & 0xffd00000 == 0xd5100000, f'SYS instruction {word:#x}'
                (mrs if word & (1 << 21) else msr)(machine, word & 31, fields)
                machine.reg_write(UC_ARM64_REG_PC, address + 4)

        def memory(machine, access, address, size, value, user):
            log.append(('store' if access == UC_MEM_WRITE else 'load', address, size))

        uc.hook_add(UC_HOOK_CODE, code, begin=TEXT, end=TEXT + len(self.text[1]))
        uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, memory)
        uc.reg_write(UC_ARM64_REG_X0, args[0])
        uc.reg_write(UC_ARM64_REG_X1, args[1] if len(args) > 1 else 0)
        uc.reg_write(UC_ARM64_REG_SP, STACK + 0x8000)
        uc.reg_write(UC_ARM64_REG_LR, END)
        uc.emu_start(self.image.address(name), END, count=20000)
        assert uc.reg_read(UC_ARM64_REG_PC) == END, f'{name} did not return'
        self.memory_state = bytes(uc.mem_read(self.state, 512))
        self.buffers = bytes(uc.mem_read(BUFFERS, 0x1000))
        return uc.reg_read(UC_ARM64_REG_X0) & 0xffffffff, log

    def reset(self):
        self.memory_state = bytes(512)
        self.buffers = bytes([0xa5]) * 0x1000

    def region_bytes(self):
        return self.buffers[:24]

    def record_bytes(self):
        return self.buffers[0x100:0x140]

    def begin(self, registers, **kw):
        return self.call('a733_cpu_pmu_begin', registers, [self.region], **kw)

    def end(self, registers, **kw):
        return self.call('a733_cpu_pmu_end', registers, [self.region, self.record], **kw)


def accesses(log):
    """System-register accesses (MSR DAIFSet included), barriers and instructions of one call."""
    count = {'accesses': sum(e[0] in ('read', 'write', 'mask') for e in log),
             'instructions': sum(e[0] == 'instruction' for e in log)}
    for barrier in BARRIERS:
        count[barrier] = sum(e[0] == barrier for e in log)
    return count


def touched(machine, log, core, stack_floor):
    """Memory each call touches: its stack, the region, the record, its own state line, the stub's word."""
    line = machine.state + 64 * core
    for kind, address, size in [e for e in log if e[0] in ('load', 'store')]:
        if stack_floor <= address < STACK + 0x8000:
            continue
        if machine.region <= address < machine.region + 24 or machine.record <= address < machine.record + 64:
            continue
        if line <= address and address + size <= line + 64:
            continue
        if kind == 'load' and address == machine.affinity:
            continue
        raise AssertionError(f'{kind} at {address:#x} outside the call\'s own memory (core {core})')


def module_cases(machine):
    cases = []

    def case(name):
        cases.append(name)

    reg = reset_core()
    machine.reset()
    # First begin: configure, zero counters 5..0, clear overflow, read cycles and ticks.
    result, log = machine.begin(reg)
    assert result == STATUS['OK'], result
    writes = [e for e in log if e[0] == 'write']
    assert [e[1:] for e in writes if e[1].startswith('pmevtyper')] == \
        [(f'pmevtyper{i}', EVENTS[i]) for i in reversed(range(6))]
    assert ('pmccfiltr', 0) in [e[1:] for e in writes] and reg['pmccfiltr'] == 0
    assert ('pmintenclr', 0x8000003f) in [e[1:] for e in writes] and reg['pmintenset'] == 0x7fffffc0
    assert ('pmuserenr', 0) in [e[1:] for e in writes] and reg['pmuserenr'] == 0
    assert ('pmcntenset', 0x8000003f) in [e[1:] for e in writes]
    assert reg['pmcr'] == (0x41003000 | 0x41), hex(reg['pmcr'])  # E and LC set, D clear, N kept
    assert [e[1] for e in writes if e[1].startswith('pmevcntr')] == [f'pmevcntr{i}' for i in reversed(range(6))]
    assert all(reg[f'pmevcntr{i}'] == 0 for i in range(6))
    assert ('pmovsclr', 0x3f) in [e[1:] for e in writes] and reg['pmovs'] == 0x80000000
    assert reg['daif'] == 0 and writes[-1][1:] == ('daif', 0)
    generation, core, cycles, ticks = struct.unpack('<IB3xQQ', machine.region_bytes())
    assert (generation, core, cycles, ticks) == (1, 0, 0x1234567890, 0x5000)
    order = [i for i, e in enumerate(log) if e[0] in ('read', 'write')]
    names = [log[i][1] for i in order]
    assert names.index('pmevcntr0') < names.index('pmovsclr') < names.index('pmccntr') < names.index('cntpct')
    first = accesses(log)
    assert first['isb'] == 2 and first['dsb'] == first['dmb'] == first['svc'] == 0, first
    case('first begin configures and reads back once')
    # End: counts, cycles and ticks from modeled advances.
    for i in range(6):
        reg[f'pmevcntr{i}'] = 100 * (i + 1)
    reg['pmccntr'] += 1000
    reg['cntpct'] += 12
    result, log = machine.end(reg)
    assert result == STATUS['OK']
    record = decode(machine.record_bytes())
    assert record == {'format': 1, 'core': 0, 'counters': 6, 'supported': 0x3f, 'overflow': 0, 'status': 0,
                      'reserved0': 0, 'midr': 0x412fd050, 'cntfrq': 24000000, 'cycles': 1000, 'ticks': 12,
                      'events': [100, 200, 300, 400, 500, 600], 'reserved1': 0}, record
    assert measured_rate(record) == 2000000000
    assert reg['daif'] == 0
    end_count = accesses(log)
    touched(machine, log, 0, STACK)
    case('end writes the record')
    # Configured path: no configuration register, CurrentEL, DSB, DMB or SVC.
    result, log = machine.begin(reg)
    assert result == STATUS['OK']
    begin_count = accesses(log)
    assert not [e for e in log if e[0] in ('read', 'write') and e[1] in CONFIG | {'pmintenclr'}], log
    assert sum(e[0] == 'affinity' for e in log) == 1
    touched(machine, log, 0, STACK)
    result, log = machine.end(reg)
    assert result == STATUS['OK']
    end_count = accesses(log)
    assert not [e for e in log if e[0] in ('read', 'write') and e[1] in CONFIG], log
    for count in (begin_count, end_count):
        assert count['isb'] == 1 and count['dsb'] == count['dmb'] == count['svc'] == 0, count
    assert begin_count['accesses'] + end_count['accesses'] <= 28, (begin_count, end_count)
    case('configured path: no readback, %d + %d accesses, 1 + 1 ISB' % (begin_count['accesses'],
                                                                         end_count['accesses']))
    configured_counts = {'begin': begin_count, 'end': end_count}
    # Overflow: the event is marked and not counted.
    machine.begin(reg)
    reg['pmovs'] |= 0x4
    reg['pmevcntr2'] = 7
    assert machine.end(reg)[0] == STATUS['OK']
    record = decode(machine.record_bytes())
    assert record['overflow'] == 0x4 and record['events'][2] == 0 and record['supported'] == 0x3f
    case('overflowed event marked without a count')
    # Unimplemented L2D_CACHE_REFILL and ITLB_WALK: left out of the supported mask.
    reg = reset_core(ceid0=(1 << 0x03) | (1 << 0x08) | (1 << 0x19), ceid1=1 << 0x14)
    machine.reset()
    assert machine.begin(reg)[0] == STATUS['OK']
    reg['pmevcntr2'] = reg['pmevcntr5'] = 9
    assert machine.end(reg)[0] == STATUS['OK']
    record = decode(machine.record_bytes())
    assert record['supported'] == 0x1b and record['events'][2] == record['events'][5] == 0, record
    case('unimplemented events left out')
    # N = 4: counters 4 and 5 are never accessed (the model asserts on access).
    reg = reset_core(n=4)
    machine.reset()
    assert machine.begin(reg)[0] == STATUS['OK']
    reg['pmevcntr3'] = 5
    assert machine.end(reg)[0] == STATUS['OK']
    record = decode(machine.record_bytes())
    assert record['counters'] == 4 and record['supported'] == 0xf and record['events'][3] == 5, record
    assert reg['pmcntenset'] == 0x8000000f
    case('PMCR_EL0.N = 4 uses four counters')
    # N = 0: refused before any counter access; the region is marked.
    reg = reset_core(n=0)
    machine.reset()
    result, log = machine.begin(reg)
    assert result == STATUS['NO_COUNTERS'] and reg['daif'] == 0
    assert not [e for e in log if e[0] == 'write' and e[1] != 'daif']
    assert machine.region_bytes()[4] == 0xff
    result, log = machine.end(reg)
    assert result == STATUS['NOT_BEGUN'] and decode(machine.record_bytes())['core'] == 0xff
    case('PMCR_EL0.N = 0 refused; end refuses the region')
    # Readback mismatches: the configured flag stays clear.
    for fault in ('pmevtyper3', 'pmcr', 'pmccfiltr', 'pmuserenr', 'pmintenclr', 'pmcntenset'):
        reg = reset_core()
        machine.reset()
        result, _ = machine.begin(reg, faults={fault})
        assert result == STATUS['CONFIG_MISMATCH'] and reg['daif'] == 0, (fault, result)
        assert machine.memory_state[4] == 0 and machine.region_bytes()[4] == 0xff, fault
        result, log = machine.begin(reg)
        assert result == STATUS['OK'] and ('write', 'pmcr', reg['pmcr']) in log, fault
    case('readback mismatch refused, configuration retried')
    # Warm reset (E reads 0) with the configured flag set: begin configures again.
    reg = reset_core()
    machine.reset()
    machine.begin(reg)
    machine.end(reg)
    reg.update(reset_core())
    result, log = machine.begin(reg)
    assert result == STATUS['OK'] and [e for e in log if e[0] == 'write' and e[1] == 'pmevtyper0']
    case('PMCR_EL0.E = 0 reconfigures')
    # E cleared inside a region: end refuses.
    reg['pmcr'] &= ~1
    result, _ = machine.end(reg)
    assert result == STATUS['DISABLED'] and decode(machine.record_bytes())['status'] == STATUS['DISABLED']
    case('PMCR_EL0.E = 0 at end refused')
    # Affinity and core refusals.
    for name, change, affinity in [('two-core mask', {}, 3), ('mask of another core', {'tpidr': 1}, 1),
                                   ('core index 8', {'tpidr': 8}, 1 << 8), ('empty mask', {}, 0)]:
        reg = {**reset_core(), **change}
        machine.reset()
        result, log = machine.begin(reg, affinity=affinity)
        assert result == STATUS['NOT_PINNED'] and reg['daif'] == 0, name
        assert not [e for e in log if e[0] == 'write' and e[1] != 'daif'], name
        assert machine.region_bytes()[4] == 0xff
        case('begin refuses a ' + name)
    # IRQs already masked on entry stay masked.
    reg = {**reset_core(), 'daif': 0x3c0}
    machine.reset()
    assert machine.begin(reg)[0] == STATUS['OK'] and reg['daif'] == 0x3c0
    assert machine.end(reg)[0] == STATUS['OK'] and reg['daif'] == 0x3c0
    case('DAIF restored when IRQs were masked on entry')
    # Core 7 uses its own state line only.
    reg = reset_core(core=7)
    machine.reset()
    result, log = machine.begin(reg, affinity=1 << 7)
    assert result == STATUS['OK']
    touched(machine, log, 7, STACK)
    assert machine.memory_state[:448] == bytes(448) and machine.memory_state[448] == 1
    result, log = machine.end(reg)
    assert result == STATUS['OK'] and decode(machine.record_bytes())['midr'] == 0x414fd0b1
    touched(machine, log, 7, STACK)
    case('core 7 touches only its own line')
    # Final-core mismatch: counters other than 0 are not read on the other core.
    reg = reset_core()
    machine.reset()
    machine.begin(reg)
    reg['tpidr'] = 1
    result, log = machine.end(reg)
    assert result == STATUS['CORE_CHANGED'] and decode(machine.record_bytes())['core'] == 1
    assert not [e for e in log if e[0] == 'read' and e[1].startswith('pmevcntr') and e[1] != 'pmevcntr0']
    assert reg['daif'] == 0
    case('end refuses a final-core mismatch')
    # Nested region: the outer end sees a new generation.
    reg = reset_core()
    machine.reset()
    machine.begin(reg)
    outer = machine.region_bytes()
    machine.begin(reg)
    assert machine.end(reg)[0] == STATUS['OK']
    machine.buffers = outer + machine.buffers[24:]
    result, _ = machine.end(reg)
    assert result == STATUS['GENERATION_CHANGED']
    record = decode(machine.record_bytes())
    assert record['status'] == STATUS['GENERATION_CHANGED'] and record['events'] == [0] * 6 and record['midr'] == 0
    case('nested region refused through the generation')
    # Null arguments.
    machine.reset()
    reg = reset_core()
    before = machine.buffers
    result, log = machine.call('a733_cpu_pmu_begin', reg, [0])
    assert result == STATUS['BAD_ARGUMENT'] and not [e for e in log if e[0] in ('read', 'write', 'affinity')]
    assert machine.buffers == before
    result, log = machine.call('a733_cpu_pmu_end', reg, [machine.region, 0])
    assert result == STATUS['BAD_ARGUMENT'] and not [e for e in log if e[0] in ('read', 'write')]
    assert machine.buffers == before
    result, log = machine.call('a733_cpu_pmu_end', reg, [0, machine.record])
    assert result == STATUS['BAD_ARGUMENT'] and decode(machine.record_bytes())['core'] == 0xff
    case('null region and record')
    return cases, configured_counts


def layout_cases(machine, elf):
    """Structure layouts from DWARF, the state symbol's size and alignment."""
    layouts = machine.image.layouts
    offsets = [0, 1, 2, 3, 4, 5, 6, 8, 12, 16, 24, 32, 56]
    assert layouts['a733_cpu_pmu_record'] == dict(zip(RECORD_FIELDS, offsets)), layouts['a733_cpu_pmu_record']
    assert struct.calcsize(RECORD) == 64
    assert layouts['a733_cpu_pmu_region'] == {'generation': 0, 'core': 4, 'reserved': 5, 'cycles': 8, 'ticks': 16}
    from elftools.elf.elffile import ELFFile
    with Path(elf).open('rb') as f:
        symbol = ELFFile(f).get_section_by_name('.symtab').get_symbol_by_name('a733_cpu_pmu_state')[0]
    assert symbol['st_size'] == 512 and symbol['st_value'] % 64 == 0, (symbol['st_size'], symbol['st_value'])
    return ['record, region and state layouts']


def continuation_cases():
    """MDCR_EL2.HPMN must equal PMCR_EL0.N in both continuation sources: the one
    MDCR_EL2 write takes a register that an MRS of PMCR_EL0 in the eight lines
    before it produced, and the continuation model expects 6."""
    failures = []
    for path in ['platform/a733/cpu_continue.S', 'platform/a733-smp-cached/continue.S']:
        lines = [line.split('//')[0].strip() for line in (ROOT / path).read_text().splitlines()]
        writes = [i for i, line in enumerate(lines) if line.startswith('msr mdcr_el2,')]
        if len(writes) != 1 or lines[writes[0]].endswith('xzr') or not any(
                line.startswith('mrs ') and line.endswith('pmcr_el0') for line in lines[max(0, writes[0] - 8):writes[0]]):
            failures.append(f'{path} does not copy PMCR_EL0.N into MDCR_EL2.HPMN')
    if "'mdcr_el2': 6" not in (ROOT / 'checks/a733-cpu-continue.py').read_text():
        failures.append('checks/a733-cpu-continue.py does not expect MDCR_EL2 = 6')
    return failures


def source_cases(read):
    """Hashes of the pinned sources; documents not yet fetched are listed."""
    sources = json.loads(read(ROOT / 'checks/cpu-pmu-sources.json'))
    pinned, missing = [], []
    for entry in sources['sources']:
        if entry.get('sha256') is None:
            missing.append(entry['name'])
            continue
        path = CACHE / 'sources' / entry['archive']
        assert hashlib.sha256(path.read_bytes()).hexdigest() == entry['sha256'], entry['name']
        pinned.append(entry['name'])
    return pinned, missing


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('output', type=Path, help='new directory for logs and results')
    ap.add_argument('--freertos', type=Path, default=FREERTOS)
    a = ap.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    inputs = {}

    def read(path):
        data = Path(path).read_bytes()
        inputs[str(path)] = hashlib.sha256(data).hexdigest()
        return data.decode()

    def run(name, argv):
        r = subprocess.run([str(x) for x in argv], cwd=ROOT, env=environment(), text=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / f'{name}.log').write_text(' '.join(map(str, argv)) + '\n' + r.stdout)
        assert r.returncode == 0, f'{name} failed:\n{r.stdout}'
        return r.stdout

    for path in ['platform/a733/cpu_pmu.c', 'include/a733_cpu_pmu.h', 'platform/a733-smp/FreeRTOSConfig.h',
                 'patches/freertos-smp-macro.patch']:
        read(ROOT / path)
    port = out / 'port'
    port.mkdir()
    run('patch-macro', ['patch', '--fuzz=0', '--reject-file=-', '--output', port / 'portmacro.h',
                        a.freertos / 'portable/GCC/ARM_CR82/portmacro.h', ROOT / 'patches/freertos-smp-macro.patch'])
    (out / 'stub.S').write_text('.section .text.stub, "ax"\n.global vTaskCoreAffinityGet\nvTaskCoreAffinityGet:\n'
                                '    ldr x0, stub_affinity\n    ret\n'
                                '.section .data.stub, "aw"\n.balign 8\n.global stub_affinity\nstub_affinity: .quad 0\n')
    (out / 'module.ld').write_text('ENTRY(a733_cpu_pmu_begin)\nSECTIONS {\n . = 0x10000; .text : { *(.text*) }\n'
                                   ' . = 0x20000; .data : { *(.data*) *(.rodata*) } .bss : { *(.bss*) }\n'
                                   ' /DISCARD/ : { *(.comment) *(.note*) }\n}\n')
    includes = [ROOT / 'platform/a733-smp', ROOT / 'include', ROOT / 'runtime/include', a.freertos / 'include', port]
    obj = out / 'cpu_pmu.o'
    run('compile', [CLANG, *FLAGS, *['-I' + str(i) for i in includes], '-c', ROOT / 'platform/a733/cpu_pmu.c',
                    '-o', obj])
    run('stub', [CLANG, '--target=aarch64-none-elf', '-c', out / 'stub.S', '-o', out / 'stub.o'])
    elf = out / 'cpu-pmu.elf'
    run('link', [LLD, '-T', out / 'module.ld', obj, out / 'stub.o', '-o', elf])
    size = run('size', ['/usr/lib/llvm-21/bin/llvm-size', '-A', obj])
    machine = Machine(elf)
    cases, counts = module_cases(machine)
    cases += layout_cases(machine, elf)
    pinned, missing = source_cases(read)
    read(ROOT / 'platform/a733/cpu_continue.S')
    read(ROOT / 'platform/a733-smp-cached/continue.S')
    read(ROOT / 'checks/a733-cpu-continue.py')
    pending = continuation_cases()
    status = 'FAIL' if pending else 'PASS'
    (out / 'results.json').write_text(json.dumps({
        'status': status, 'inputs': inputs, 'object_sha256': hashlib.sha256(obj.read_bytes()).hexdigest(),
        'cases': cases, 'configured_path': counts, 'size': size, 'pinned_sources': pinned,
        'unfetched_sources': missing, 'continuation_failures': pending,
        'limits': 'Instruction emulation with modeled system registers and a stub affinity call. '
                  'No QEMU or physical PMU, no EL2 or EL3 trap, no scheduler.'}, indent=2) + '\n')
    print('module: %d cases pass; configured path %d + %d accesses; pinned sources %s; not fetched: %s' % (
        len(cases), counts['begin']['accesses'], counts['end']['accesses'], ', '.join(pinned),
        ', '.join(missing) or 'none'))
    if pending:
        print('FAIL: ' + '; '.join(pending))
        sys.exit(1)
    print('PASS')


if __name__ == '__main__':
    main()
