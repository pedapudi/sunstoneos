#!/usr/bin/env python3
"""Exercise CPU0 production cache adoption and restoration; no physical claims."""
import argparse
import copy
import ctypes
import importlib.util
import json
import struct
from pathlib import Path
from unicorn import UC_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_PC
from unicorn import arm64_const as registers

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


parent = module('primary_cache_parent_check', ROOT/'checks/primary-publication-probe.py')
probe = module('primary_cache_prepare', ROOT/'tools/prepare-primary-cache-probe.py')


class Probe(parent.Probe):
    def __init__(self, image, primary, bundle):
        super().__init__(image, primary)
        self.cache_manifest = json.loads((bundle/'manifest.json').read_text())
        assert self.cache_manifest['primary_manifest']['sha256'] == probe.digest(primary/'manifest.json')
        self.el1 = self.cache_manifest.get('el1', False)
        self.interrupts = self.cache_manifest.get('interrupts', False)
        self.context = self.cache_manifest.get('context', False)
        self.context_pages = {self.symbols['a733_cpu_el1_stacks'], self.symbols['a733_cpu_el1_stacks']+0x7000, self.symbols['ullPortInterruptNestings'] & ~4095}
        self.context_dirty = set(); self.context_cleaned = set(); self.context_stores = []
        self.context_entries = 0; self.context_returns = 0; self.irq_registers = None
        self.late_delivery = False
        self.secure_pmr = 0
        self.delivered = []
        assert self.cache_manifest['sources'] == {name: probe.digest(ROOT/name) for name in probe.sources(self.el1, self.interrupts, self.context)}
        for item in self.cache_manifest['payloads']:
            data = (bundle/item['name']).read_bytes()
            assert probe.digest(bundle/item['name']) == item['sha256']
            ctypes.memmove(self.ram_pointer+item['address']-0x41000000, data, len(data))
        data = (bundle/'overlay.bin').read_bytes()
        assert probe.digest(bundle/'overlay.bin') == self.cache_manifest['overlay']['sha256']
        ctypes.memmove(self.ram_pointer+parent.probe.OVERLAY[0]-0x41000000, data, len(data))
        self.suffix = False
        self.inject_cache_restore_fault = False
        self.cache_writes = []
        self.fault = None
        self.el1_seen = self.smc_seen = False
        if self.el1:
            self.mmio[0x03400000] = 0x37
            self.mmio[0x03400004] = 0x7b0408
            self.mmio[0x03460014] = 0  # Observed primary is awake, unlike secondaries.
            self.mmio[0x03470200] = 0
            self.mmio[0x0347041c] = 0
            for i in range(1, 9): self.mmio[0x03400100+4*i] = 0x10101*i
            self.regs[0].update(icc_sre_el3=9, icc_igrpen0_el1=0, icc_igrpen1_el3=0,
                                ttbr0_el1=0xace000, ttbr1_el1=0x135000, mair_el1=0x1234)
        self.original_mmio = copy.deepcopy(self.mmio)

    def saved_irq_registers(self):
        u = self.cpus[0]
        ids = [getattr(registers, 'UC_ARM64_REG_X'+str(n)) for n in range(31)]
        ids += [getattr(registers, 'UC_ARM64_REG_Q'+str(n)) for n in range(32)]
        ids += [registers.UC_ARM64_REG_SP, registers.UC_ARM64_REG_FPCR, registers.UC_ARM64_REG_FPSR]
        return [u.reg_read(n) for n in ids]

    def exception(self, core, level, address, *args, **kwargs):
        if self.context and kwargs.get('irq', False):
            assert self.irq_registers is None, 'Unexpected nested IRQ'
            self.irq_registers = self.saved_irq_registers()
            self.context_entries += 1
        return super().exception(core, level, address, *args, **kwargs)

    def memory(self, u, access, address, size, value, core):
        write = access == UC_MEM_WRITE
        if self.suffix and self.el1:
            if self.regs[0]['currentel'] == 4:
                if self.context and 0x41200000 <= address < 0x42000000:
                    assert address & ~4095 in self.context_pages and (address+size-1) & ~4095 in self.context_pages, ('Unowned context access', hex(address), size)
                    if write:
                        assert not self.context_cleaned, 'Context store after cache cleaning'
                        self.context_stores.append((address, size))
                        self.context_dirty.update(range(address & ~63, (address+size+63) & ~63, 64))
                    return
                if self.interrupts and address < 0x41000000:
                    assert address in (0x03460000, 0x03470100, 0x03470180, 0x03470200,
                                       0x03470280, 0x03470300, 0x0347041c), hex(address)
                else:
                    assert not write and (0x41000000 <= address < 0x41200000 or self.sealed_el1_table_read(address, size)), (
                        'EL1 must not dirty Normal data', hex(address))
            if self.interrupts:
                if address == 0x03470200:
                    self.mmio[address] = sum(1 << n for n in self.pending[0])
                if address == 0x03470300:
                    self.mmio[address] = 0 if self.irq_active[0] is None else 1 << self.irq_active[0]
                if write and address == 0x03470280:
                    self.pending[0] = {n for n in self.pending[0] if not value & (1 << n)}

            if address < 0x41000000:
                if write: self.mmio_writes.append((address, size, value))
                if 0x03400000 <= address < 0x03600000:
                    if address < 0x03460000:
                        assert address in {0x03400000, 0x03400004, *range(0x03400104, 0x03400124, 4),
                                           *range(0x03400184, 0x034001a4, 4)}
                        if write and 0x03400184 <= address < 0x034001a4:
                            self.mmio[address-0x80] &= ~value; return
                        if write and 0x03400104 <= address < 0x03400124:
                            self.mmio[address] |= value; return
                    else:
                        frame = (address-0x03460000)//0x20000
                        offset = address-(0x03460000+frame*0x20000)
                        assert frame == 0 or (not write and offset == 8), 'Unexpected secondary register access'
                        if write and offset == 0x14:
                            assert value == 0, 'Boot CPU must remain awake'
                            self.mmio[address] = 0; return
                        if write and offset == 0x24:
                            assert value == 0
                            self.mmio[address] = 0; return
                        if write and offset in (0x10280, 0x10380):
                            self.mmio[address-0x80] &= ~value; return
                        if write and offset == 0x10080:
                            self.mmio[address] = value & (0xfff0fffe if self.fault == 'group-refusal' else 0xfff0ffff)
                            return
                        if write and offset == 0x10400 and self.fault == 'cleanup-readback' and value == self.original_mmio[address]:
                            self.mmio[address] = value ^ 0x10; return
                elif write:
                    assert (address, size, value) in ((0x08001004, 4, 0x20), (0x08001008, 4, 0))
                return parent.publication.model.Machine.memory(self, u, access, address, size, value, core)
        if self.suffix and self.regs[0]['sctlr_el3'] & 0x1005 == 0x1005 and write:
            assert 0x42000000 <= address and address+size <= 0x421f0000
            self.cache_writes.append((address, size))
        super().memory(u, access, address, size, value, core)

    def instruction(self, u, address, size, core):
        if not self.suffix:
            super().instruction(u, address, size, core)
            if address == self.hooks['primary_publication_after_prepare']:
                self.suffix = True
            return
        if address in (self.cache_manifest['hooks']['primary_cache_park'],
                       self.hooks['primary_publication_park']):
            self.retained = True
            u.emu_stop()
            return
        ins = next(self.decoder.disasm(bytes(u.mem_read(address, 4)), address))
        op, args = ins.mnemonic, ins.op_str.split(', ')
        if self.context:
            if address == self.cache_manifest['hooks']['primary_context_begin'] and self.fault == 'unexpected-yield':
                u.mem_write(self.symbols['ullPortYieldRequired'], struct.pack('<Q', 1))
            if address == self.cache_manifest['hooks']['primary_context_clean'] and self.fault == 'cleanup-deadline':
                self.clock = self.reg(core, 'x22') + 1
            if op == 'dc' and args[0] == 'civac' and not self.production_table_clean(address):
                line = self.reg(core, args[1])
                assert self.regs[0]['currentel'] == 4 and line % 64 == 0 and line & ~4095 in self.context_pages
                self.context_cleaned.add(line); self.context_dirty.discard(line)
            if op == 'eret' and self.regs[0]['currentel'] == 4 and self.irq_registers is not None:
                if self.fault == 'context-corruption' and not self.context_returns:
                    u.reg_write(registers.UC_ARM64_REG_Q7, u.reg_read(registers.UC_ARM64_REG_Q7) ^ 1)
                else:
                    assert self.saved_irq_registers() == self.irq_registers, 'Production IRQ changed interrupted registers'
                self.context_returns += 1; self.irq_registers = None
            if op == 'smc':
                assert not self.context_dirty, 'Dirty Normal-memory lines on SMC return'
                assert len(self.context_cleaned) == 192, 'Owned context pages not completely cleaned'
        if self.interrupts:
            m = self.regs[0]
            if address == self.cache_manifest['hooks']['primary_irq_delivery_stop']:
                self.late_delivery = True
            if op == 'mrs' and args[1] == 'cntpct_el0' and m['currentel'] == 4:
                self.clock += 1000
            if m['currentel'] == 4 and m['cntp_ctl_el0'] & 3 == 1 and self.clock >= m['cntp_cval_el0'] and self.irq_active[0] != 30:
                self.pending[0].add(30)
            if address == self.cache_manifest['hooks']['primary_irq_delivery_stopped']:
                assert m['daif'] & 0x80 and not m['cntp_ctl_el0'] and not m['icc_igrpen1_el1']
                if self.fault == 'cleanup-pending': self.pending[0].add(9)
            if op == 'mrs' and args[1] == 'cntp_ctl_el0' and not m['cntp_ctl_el0'] & 1:
                self.put(core, args[0], m['cntp_ctl_el0'] | 4); u.reg_write(UC_ARM64_REG_PC, address+4); return
            if op == 'mrs' and args[1] == 'icc_pmr_el1':
                value = self.secure_pmr if m['currentel'] == 12 else (self.secure_pmr << 1) & 255 if self.secure_pmr & 128 else 0
                self.put(core, args[0], value); u.reg_write(UC_ARM64_REG_PC, address+4); return
            if op == 'msr' and args[0] == 'icc_pmr_el1':
                value = self.reg(core, args[1])
                if m['currentel'] == 12:
                    self.secure_pmr = 0 if self.fault == 'secure-mask-refusal' else value & 0xf8
                elif self.secure_pmr & 128:
                    self.secure_pmr = ((value >> 1) | 128) & 0xf8
                m['icc_pmr_el1'] = (self.secure_pmr << 1) & 255 if self.secure_pmr & 128 else 0
                u.reg_write(UC_ARM64_REG_PC, address+4); return
            if op == 'msr' and args[0] == 'icc_sgi1r_el1' and self.fault == 'unexpected-irq':
                self.pending[0].add(17)
            if op == 'mrs' and args[1] == 'icc_iar1_el1' and self.pending[0]:
                self.delivered.append(min(self.pending[0]))
        if self.el1:
            m = self.regs[0]
            if op == 'wfi':
                self.retained = True; u.emu_stop(); return
            if op == 'smc':
                assert m['currentel'] == 4 and args == ['#0x733']
                self.smc_seen = True
                self.exception(core, 3, address+4, (0x17 << 26) | (1 << 25) | 0x733); return
            if op == 'mrs' and args[1] == 'tpidr_el1' and self.fault == 'wrong-el1-core':
                self.put(core, args[0], 1); u.reg_write(UC_ARM64_REG_PC, address+4); return
            if op == 'msr' and args[0] == 'sctlr_el1':
                value = self.reg(core, args[1])
                if value & 0x1005 == 0x1005:
                    assert self.el3_enabled == {0} and m['ttbr0_el1'] == self.symbols['a733_cached_el1_l1']
                    self.el1_seen = True; self.el1_enabled.add(0)
                else: self.el1_enabled.discard(0)
                m['sctlr_el1'] = value; u.reg_write(UC_ARM64_REG_PC, address+4); return
        if op == 'msr' and args[0] == 'rmr_el3':
            assert self.reg(core, args[1]) == 2
            self.returned = True
            u.emu_stop()
            return
        hidden = set(self.pending[0]) if self.interrupts and (self.fault == 'missing-irqs' or
            self.fault == 'late-irqs' and not self.late_delivery) else set()
        if hidden: self.pending[0].clear()
        parent.publication.model.Machine.instruction(self, u, address, size, core)
        self.pending[0].update(hidden)
        if self.inject_cache_restore_fault and op == 'msr' and args[0] == 'sctlr_el3' and not self.reg(core, args[1]) & 1:
            self.regs[0]['sctlr_el3'] ^= 4


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'primary', 'bundle', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    machine = Probe(args.image, args.primary, args.bundle)
    if machine.el1:
        return check_el1(args, machine)
    machine.cpus[0].emu_start(machine.symbols['a733_smp_entry'], 0, count=3000000)
    assert machine.returned and not machine.retained and machine.el3_enabled == {0}
    assert not any(machine.active[1:]) and machine.cache_writes
    assert machine.mmio_writes == [(0x08001004, 4, 0x20), (0x08001008, 4, 0)]
    raw = machine.read(probe.REPORT[0], probe.REPORT[1])
    result = probe.decode(raw, machine.nonce)
    assert result['epoch'] == 5 and result['startup_deadline'] == 24000000
    cases = [{'case': 'primary-production-cache-adoption-restored-return', 'pass': True,
              'report': result, 'cached_device_stores': len(machine.cache_writes)}]
    # Reuse the executed preparation prefix; inject only the new restore fault.
    ram, regs, context = machine.post_prepare
    ctypes.memmove(machine.ram_pointer, ram, len(ram))
    machine.regs[0] = copy.deepcopy(regs)
    machine.cpus[0].context_restore(context)
    machine.el3_enabled.clear()
    machine.suffix = True
    machine.inject_cache_restore_fault = True
    machine.returned = machine.retained = False
    machine.mmio_writes = []
    machine.cpus[0].emu_start(machine.hooks['primary_publication_after_prepare'], 0, count=20000)
    assert machine.retained and not machine.returned and not machine.mmio_writes
    try:
        probe.decode(machine.read(probe.REPORT[0], probe.REPORT[1]), machine.nonce)
    except ValueError:
        pass
    else:
        raise AssertionError('Host accepted failed cache restoration')
    cases.append({'case': 'cache-restoration-failure-retains-without-reset', 'pass': True,
                  'prefix_reused': True})
    args.output.write_text(json.dumps({'physical_access': False,
        'native_sha256': probe.digest(args.image), 'manifest_sha256': probe.digest(args.bundle/'manifest.json'),
        'sources': {n: probe.digest(ROOT/n) for n in ('checks/primary-cache-probe.py',
                    'checks/primary-publication-probe.py', 'checks/a733-supervised-publication.py',
                    'checks/a733-smp-cached.py')}, 'cases': cases,
        'limits': 'Actual instructions with modeled translation controls; physical adoption, cache visibility and ROM return remain untested.'}, indent=2)+'\n')
    print('PASS CPU0 EL3 cache adoption/restoration and retained restoration failure; prefix reused')


def check_el1(args, machine):
    # Execute the accepted prefix once; later cases restore its captured state.
    cases = []
    names = ('success', 'baseline-refusal', 'group-refusal', 'wrong-el1-core',
             'cleanup-readback', 'cache-restoration-failure')
    if machine.interrupts:
        names = ('success', 'baseline-refusal', 'missing-irqs', 'unexpected-irq', 'cleanup-pending', 'secure-mask-refusal')
    if machine.context:
        names = ('success', 'baseline-refusal', 'missing-irqs', 'late-irqs', 'context-corruption', 'cleanup-deadline', 'unexpected-yield')
    for name in names:
        if name != 'success':
            ram, regs, context = machine.post_prepare
            ctypes.memmove(machine.ram_pointer, ram, len(ram))
            machine.regs[0] = copy.deepcopy(regs)
            machine.cpus[0].context_restore(context)
            machine.mmio = copy.deepcopy(machine.original_mmio)
            machine.el3_enabled.clear(); machine.el1_enabled.clear()
            machine.returned = machine.retained = machine.el1_seen = machine.smc_seen = False
            machine.mmio_writes = []; machine.cache_writes = []
            machine.pending[0].clear(); machine.irq_active[0] = None
            machine.secure_pmr = 0; machine.delivered = []
            machine.context_dirty.clear(); machine.context_cleaned.clear(); machine.context_stores = []
            machine.context_entries = machine.context_returns = 0; machine.irq_registers = None
            machine.late_delivery = False
            if machine.interrupts: machine.clock = 1000000
            machine.fault = name
            machine.inject_cache_restore_fault = name == 'cache-restoration-failure'
            if name == 'baseline-refusal': machine.mmio[0x03460014] = 6
            start = machine.hooks['primary_publication_after_prepare']
        else:
            start = machine.symbols['a733_smp_entry']
        machine.cpus[0].emu_start(start, 0, count=3000000 if name == 'success' else 100000)
        raw = machine.read(*probe.report_spec(machine.cache_manifest))
        path = args.output.parent/('report-'+name+'.bin'); path.write_bytes(raw)
        record = {'case': name, 'pass': True, 'returned': machine.returned, 'retained': machine.retained,
                  'EL1': machine.el1_seen, 'SMC': machine.smc_seen, 'report_sha256': probe.digest(path)}
        if name in ('success', 'baseline-refusal', 'group-refusal', 'missing-irqs', 'late-irqs'):
            assert machine.returned and not machine.retained, (name, machine.trace[-10:])
            result = probe.decode(raw, machine.nonce, machine.cache_manifest)
            assert result['primary_cached_el1_completed'] == (name in ('success', 'missing-irqs', 'late-irqs'))
            assert machine.mmio_writes[-2:] == [(0x08001004, 4, 0x20), (0x08001008, 4, 0)]
            if name == 'baseline-refusal': assert len(machine.mmio_writes) == 2
            else:
                for address in (0x03400000, *range(0x03400104, 0x03400124, 4), 0x03460014,
                                0x03470080, 0x03470d00, 0x03470c04, 0x03470400, 0x03470100):
                    assert machine.mmio[address] == machine.original_mmio[address], hex(address)
            if name in ('success', 'missing-irqs', 'late-irqs'): assert machine.el1_seen and machine.smc_seen and machine.cache_writes
            else: assert not machine.el1_seen and not machine.smc_seen
            if machine.interrupts:
                assert result['primary_interrupts_completed'] == (name in ('success', 'late-irqs'))
                assert not machine.pending[0] and machine.irq_active[0] is None
                assert machine.mmio[0x0347041c] == machine.original_mmio[0x0347041c]
                if name in ('success', 'late-irqs'): assert sorted(machine.delivered) == [0, 30]
                if machine.context and name != 'baseline-refusal':
                    assert result['primary_context_completed'] == (name in ('success', 'late-irqs'))
                    assert machine.context_entries == machine.context_returns == (2 if name in ('success', 'late-irqs') else 0)
                    assert machine.context_stores and len(machine.context_cleaned) == 192 and not machine.context_dirty
                    record['context'] = {'entries': machine.context_entries, 'returns': machine.context_returns, 'stores': len(machine.context_stores), 'cleaned_lines': len(machine.context_cleaned)}
                if name == 'missing-irqs': assert not machine.delivered and result['interrupt_outcome'] == 1
            record['result'] = result
        else:
            assert machine.retained and not machine.returned
            assert not any(a in (0x08001004, 0x08001008) for a, _, _ in machine.mmio_writes)
            try: probe.decode(raw, machine.nonce, machine.cache_manifest)
            except ValueError: pass
            else: raise AssertionError('Incomplete report accepted')
        assert not any(machine.active[1:])
        cases.append(record)
        (args.output.parent/('case-'+name+'.json')).write_text(json.dumps(record, indent=2)+'\n')
        print('PASS primary EL1 '+name, flush=True)
    args.output.write_text(json.dumps({'physical_access': False, 'el1': True, 'interrupts': machine.interrupts, 'context': machine.context,
        'native_sha256': probe.digest(args.image), 'manifest_sha256': probe.digest(args.bundle/'manifest.json'),
        'sources': {n: probe.digest(ROOT/n) for n in ('checks/primary-cache-probe.py',
                   'checks/primary-publication-probe.py', 'checks/a733-supervised-publication.py',
                   'checks/a733-smp-cached.py')}, 'cases': cases,
        'limits': 'Production instructions with modeled GIC and CPU controls. No physical cache visibility, interrupt delivery or scheduler result.'}, indent=2)+'\n')


if __name__ == '__main__':
    main()
