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
        assert self.cache_manifest['sources'] == {name: probe.digest(ROOT/name) for name in probe.sources(self.el1)}
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
            for i in range(1, 9): self.mmio[0x03400100+4*i] = 0x10101*i
            self.regs[0].update(icc_sre_el3=9, icc_igrpen0_el1=0, icc_igrpen1_el3=0,
                                ttbr0_el1=0xace000, ttbr1_el1=0x135000, mair_el1=0x1234)
        self.original_mmio = copy.deepcopy(self.mmio)

    def memory(self, u, access, address, size, value, core):
        write = access == UC_MEM_WRITE
        if self.suffix and self.el1:
            if self.regs[0]['currentel'] == 4:
                assert not write and 0x41000000 <= address < 0x41200000, 'EL1 must not dirty data'
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
        parent.publication.model.Machine.instruction(self, u, address, size, core)
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
            machine.fault = name
            machine.inject_cache_restore_fault = name == 'cache-restoration-failure'
            if name == 'baseline-refusal': machine.mmio[0x03460014] = 6
            start = machine.hooks['primary_publication_after_prepare']
        else:
            start = machine.symbols['a733_smp_entry']
        machine.cpus[0].emu_start(start, 0, count=3000000 if name == 'success' else 100000)
        raw = machine.read(*probe.EL1_REPORT)
        path = args.output.parent/('report-'+name+'.bin'); path.write_bytes(raw)
        record = {'case': name, 'pass': True, 'returned': machine.returned, 'retained': machine.retained,
                  'EL1': machine.el1_seen, 'SMC': machine.smc_seen, 'report_sha256': probe.digest(path)}
        if name in ('success', 'baseline-refusal', 'group-refusal'):
            assert machine.returned and not machine.retained, (name, machine.trace[-10:])
            result = probe.decode(raw, machine.nonce, machine.cache_manifest)
            assert result['primary_cached_el1_completed'] == (name == 'success')
            assert machine.mmio_writes[-2:] == [(0x08001004, 4, 0x20), (0x08001008, 4, 0)]
            if name == 'baseline-refusal': assert len(machine.mmio_writes) == 2
            else:
                for address in (0x03400000, *range(0x03400104, 0x03400124, 4), 0x03460014,
                                0x03470080, 0x03470d00, 0x03470c04, 0x03470400, 0x03470100):
                    assert machine.mmio[address] == machine.original_mmio[address], hex(address)
            if name == 'success': assert machine.el1_seen and machine.smc_seen and machine.cache_writes
            else: assert not machine.el1_seen and not machine.smc_seen
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
    args.output.write_text(json.dumps({'physical_access': False, 'el1': True,
        'native_sha256': probe.digest(args.image), 'manifest_sha256': probe.digest(args.bundle/'manifest.json'),
        'sources': {n: probe.digest(ROOT/n) for n in ('checks/primary-cache-probe.py',
                   'checks/primary-publication-probe.py', 'checks/a733-supervised-publication.py',
                   'checks/a733-smp-cached.py')}, 'cases': cases,
        'limits': 'Production instructions with modeled GIC and CPU controls. No physical cache visibility, interrupt delivery or scheduler result.'}, indent=2)+'\n')


if __name__ == '__main__':
    main()
