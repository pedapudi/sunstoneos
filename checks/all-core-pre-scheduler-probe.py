#!/usr/bin/env python3
"""Execute the production eight-core barrier and lock before returning to ROM."""
import argparse
import copy
import ctypes
import importlib.util
import json
import pickle
import struct
from pathlib import Path
from unicorn import UC_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_PC

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('cached_check', ROOT/'checks/secondary-cache-probe.py')
cached = importlib.util.module_from_spec(spec); spec.loader.exec_module(cached)
local, model, digest = cached.local, cached.model, cached.digest
SHARED, STATE = 0x41243000, 0x41201000
SOURCES = ('checks/all-core-pre-scheduler-probe.py', 'checks/secondary-cache-probe.py',
           'checks/secondary-local-probe.py', 'checks/secondary-identity-probe.py',
           'checks/primary-publication-probe.py', 'checks/a733-supervised-publication.py',
           'checks/a733-smp-cached.py')


class AllCore(cached.Cached):
    cache_lines = 192
    def restore(self, snapshot, fault):
        super().restore(snapshot, fault)
        ctypes.memset(self.ram_pointer+0x42018100-0x41000000, 0, 0x5100)
        self.mmio[0x03460014] = 0  # CPU0 stays awake throughout.
        # The secondary fixture assigns reset controls to every CPU. CPU0 has
        # already executed the preserved preparation prefix; retain that state.
        for name in ('sctlr_el1', 'tcr_el1'):
            self.regs[0][name] = snapshot[2]['regs'][0][name]
        self.regs[0].update(icc_pmr_el1=0, ttbr0_el1=0xace000,
                            ttbr1_el1=0x135000, mair_el1=0x1234)
        self.original_mmio = copy.deepcopy(self.mmio)
        self.original_regs = copy.deepcopy(self.regs)
        assert self.read(SHARED, 4096) == bytes(4096)
        assert self.read(STATE, 76) == bytes(76)
        self.initial_state = self.read(STATE, 4096)
        self.cleaned = {c: [] for c in range(8)}
        self.dirty = {c: set() for c in range(8)}
        self.clean_complete, self.called = set(), set()
        self.highest_active = 0
        self.fault_injected = False
        self.security = []

    def pages(self, core):
        return (SHARED, SHARED+core*32768+0x7000, STATE)

    def memory(self, u, access, address, size, value, core):
        write = access == UC_MEM_WRITE
        if address in model.EL3_SECURITY_ADDRESSES:
            # The native order: every secondary released, CPU0 still at EL3.
            assert self.started == list(range(1, 8)) and 0 not in self.el1_seen, (
                'EL3 security setup out of order', self.started, sorted(self.el1_seen))
        if 0x07050204 <= address <= 0x0705021c and write and value & 1:
            target = (address-0x07050200)//4
            assert core == 0 and target == len(self.started)+1
            self.started.append(target)
            self.highest_active = max(self.highest_active, sum(self.active)+1)
            self.mmio_writes.append((address, size, value))
            return model.Machine.memory(self, u, access, address, size, value, core)
        if self.regs[core]['currentel'] == 4 and 0x41200000 <= address < 0x42000000:
            assert address & ~4095 in self.pages(core) and (address+size-1) & ~4095 in self.pages(core), ('Unowned data', core, hex(address), size)
            assert core in self.el1_enabled
            if write:
                assert not self.cleaned[core], 'Normal store after cleaning began'
                self.dirty[core].update(range(address & ~63, (address+size+63) & ~63, 64))
                self.changed(address, size)
            return
        if core == 0 and (SHARED <= address < SHARED+4096 or STATE <= address < STATE+4096):
            assert not write and self.stopped == list(range(1, 8)) and len(self.clean_complete) == 8
            return
        if core == 0 and 0x03460000 <= address < 0x03600000:
            frame = (address-0x03460000)//0x20000
            offset = address-(0x03460000+frame*0x20000)
            assert frame == 0 or (not write and offset == 8)
            if write:
                self.mmio_writes.append((address, size, value))
                if offset == 0x14:
                    assert value == 0
                    self.mmio[address] = 0; return
                if offset == 0x24:
                    assert value == 0
                    self.mmio[address] = 0x100; return
                if offset in (0x10280, 0x10380):
                    self.mmio[address-0x80] &= ~value; return
                if offset == 0x10080:
                    self.mmio[address] = value & 0xfff0ffff; return
            return model.Machine.memory(self, u, access, address, size, value, core)
        if self.regs[core]['currentel'] == 4:
            assert not write and (0x41000000 <= address < 0x41200000 or self.sealed_el1_table_read(address, size)), (
                'EL1 access outside code/owned data', core, hex(address))
        return local.Local.memory(self, u, access, address, size, value, core)

    def instruction(self, u, address, size, core):
        if address not in self.decoded:
            self.decoded[address] = next(self.decoder.disasm(bytes(u.mem_read(address, 4)), address))
        ins = self.decoded[address]; op, args = ins.mnemonic, ins.op_str.split(', ')
        m = self.regs[core]
        if op == 'msr' and args[0] == 'sctlr_el3' and self.reg(core, args[1]) & 0x1005 == 0x1005:
            assert m['sctlr_el1'] & 1 and m['tcr_el1'] & 0x800080 == 0x800080, 'EL3 cache adoption requires blocked EL1 walks'
        if self.fault == 'counter-corruption' and self.fault_injected and self.q(SHARED) == self.epoch and not self.q(SHARED+8):
            # The production primary rejected 257 increments. Its peers wait for
            # release until their absolute deadline; advance modeled time to that
            # deadline rather than interpreting millions of identical polls.
            self.clock = max(self.clock, self.deadline+1)
        if address == self.symbols['a733_smp_pre_scheduler']:
            assert core not in self.called and m['currentel'] == 4
            assert self.reg(core, 'x0') == core and self.reg(core, 'x1') == self.epoch
            assert self.reg(core, 'x2') == self.deadline//24
            self.called.add(core)
            if core == 0 and self.fault == 'counter-corruption':
                u.mem_write(STATE+32, struct.pack('<I', 1)); self.changed(STATE+32, 4)
                self.fault_injected = True
        if op == 'dc' and args[0] == 'civac' and not self.production_table_clean(address):
            assert m['currentel'] == 4
            assert self.called == set(range(8)) and all(self.q(SHARED+c*64) == self.epoch for c in range(8)), 'Cleaning before all production calls return'
            line = self.reg(core, args[1]); index = len(self.cleaned[core])
            assert index < self.cache_lines and line == self.pages(core)[index//64]+64*(index%64)
            self.cleaned[core].append(line); self.dirty[core].discard(line)
        if address == self.survey_hooks['identity_pre_scheduler_clean_done']:
            assert len(self.cleaned[core]) == self.cache_lines and not self.dirty[core]
            previous = next(self.decoder.disasm(bytes(u.mem_read(address-4, 4)), address-4))
            assert previous.mnemonic == 'dsb' and previous.op_str == 'sy'
            self.clean_complete.add(core)
        if op == 'smc':
            assert core in self.clean_complete and not self.dirty[core]
            if not core:
                assert args == ['#0x733'] and m['currentel'] == 4
                self.smc_seen.add(0)
                self.exception(core, 3, address+4, (0x17 << 26) | (1 << 25) | 0x733)
                return
        if not core and op == 'wfi' and m['currentel'] > 0:
            self.retained = True; u.emu_stop(); return
        if not core and op == 'msr':
            name, src = args; value = self.reg(core, src)
            if name == 'sctlr_el3':
                if value & 0x1005 == 0x1005:
                    assert m['currentel'] == 12 and not m['scr_el3'] & 0x200
                    assert m['ttbr0_el3'] == self.symbols['a733_cached_el3_l1']
                    assert value == self.original_regs[0]['sctlr_el3'] | 0x1005
                    self.mappings(); self.el3_seen.add(0); self.el3_enabled.add(0)
                else:
                    assert value == self.original_regs[0]['sctlr_el3']
                    self.el3_enabled.discard(0)
                m[name] = value; u.reg_write(UC_ARM64_REG_PC, address+4); return
            if name == 'sctlr_el1':
                if value & 0x1005 == 0x1005:
                    assert 0 in self.el3_enabled and m['ttbr0_el1'] == self.symbols['a733_cached_el1_l1']
                    # The overlay zeroed every active-priority copy before EL1 entry.
                    assert self.active_priorities[0] == {'ap0r0', 'ap1r0_s', 'ap1r0_ns'}, self.active_priorities[0]
                    self.el1_seen.add(0); self.el1_enabled.add(0)
                else: self.el1_enabled.discard(0)
                m[name] = value; u.reg_write(UC_ARM64_REG_PC, address+4); return
            if name == 'tcr_el3' or name.startswith('icc_'):
                if name in ('icc_ap0r0_el1', 'icc_ap1r0_el1'):
                    self.active_priority(0, name, value); u.reg_write(UC_ARM64_REG_PC, address+4); return
                if name.startswith('icc_'):
                    assert name in ('icc_sre_el3', 'icc_ctlr_el3', 'icc_igrpen0_el1', 'icc_igrpen1_el3', 'icc_sre_el2', 'icc_sre_el1', 'icc_pmr_el1')
                    entry = self.symbols['a733_cached_enter_el1']
                    if name in ('icc_igrpen0_el1', 'icc_igrpen1_el3'): assert value == 0
                    # PMR: zero from the probe's own restoration; 255 from the native
                    # entry, which sets the Secure mask at EL3 before Nonsecure entry.
                    if name == 'icc_pmr_el1':
                        assert value == (255 if entry <= address < entry+self.sizes['a733_cached_enter_el1'] else 0)
                m[name] = value; u.reg_write(UC_ARM64_REG_PC, address+4); return
        return super().instruction(u, address, size, core)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'bundle', 'prefix', 'output'): parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--case', choices=('success', 'counter-corruption', 'restore-el3'), action='append')
    a = parser.parse_args(); a.output.parent.mkdir(parents=True, exist_ok=True)
    machine = AllCore(a.image, a.bundle)
    machine.local_manifest = json.loads((a.bundle/'manifest.json').read_text())
    machine.overlay = (a.bundle/'overlay.bin').read_bytes()
    meta = json.loads(a.prefix.with_suffix('.json').read_text())
    assert digest(a.prefix) == meta['snapshot_sha256']
    for path, sha in meta['inputs'].items(): assert digest(Path(path)) == sha
    prior = json.loads(next(Path(n).read_text() for n in meta['inputs'] if n.endswith('/manifest.json')))
    assert prior['primary_manifest'] == machine.local_manifest['primary_manifest']
    base = Path(prior['primary_manifest']['path']); bm = json.loads(base.read_text())
    checkpoint = bm['hooks']['primary_publication_return']-0x41020000
    assert machine.overlay[:checkpoint] == (base.parent/'overlay.bin').read_bytes()[:checkpoint]
    snapshot = pickle.loads(a.prefix.read_bytes())  # Trusted, hash-checked local snapshot.
    for name in a.case or ('success', 'counter-corruption', 'restore-el3'):
        machine.restore(snapshot, None if name == 'success' else name)
        machine.execute()
        raw = machine.read(0x42018000, 0x5200)
        report = a.output.parent/('report-'+name+'.bin'); report.write_bytes(raw)
        if name == 'success':
            assert machine.returned and not machine.retained
            assert machine.security == model.el3_security_writes(), machine.security
            assert machine.started == machine.stopped == list(range(1, 8)) and machine.highest_active == 8
            assert machine.el3_seen == machine.el1_seen == machine.smc_seen == machine.clean_complete == set(range(8))
            assert raw[0x3200:0x4200] == machine.read(SHARED, 4096)
            assert raw[0x4200:] == machine.read(STATE, 4096)
            assert struct.unpack('<19I', machine.read(STATE, 76)) == (*([1]*8), 256, 0, *([1]*8), 1)
            assert machine.read(STATE+76, 4096-76) == machine.initial_state[76:]
            for c in range(8):
                assert struct.unpack_from('<8Q', raw, 0x3000+c*64) == (3, c, machine.epoch, 1, 256, 192, 255, 0)
            assert not any(machine.el3_enabled | machine.el1_enabled)
            assert machine.q(local.LOCAL) == 3
            assert struct.unpack_from('<4Q', raw, 0x140) == tuple(machine.original_regs[0][n] for n in ('sctlr_el1','tcr_el1'))*2
            assert all(machine.regs[0][n] == machine.original_regs[0][n] for n in ('sctlr_el1','tcr_el1'))
            for address, value in machine.original_mmio.items():
                if 0x03400000 <= address < 0x03600000 and address & 0xffff not in (0x180, 0x280, 0x380):
                    assert machine.mmio[address] == value, ('Unrestored GIC', hex(address))
        else:
            assert machine.retained and not machine.returned and machine.called == set(range(8))
            if name == 'counter-corruption':
                assert machine.fault_injected and machine.stopped == [] and not machine.clean_complete
                assert struct.unpack('<I', machine.read(STATE+32, 4))[0] == 257
            else:
                assert 6 in machine.target_parked and 6 not in machine.stopped
                assert 6 in machine.clean_complete
        record = {'case': name, 'pass': True, 'manifest_sha256': digest(a.bundle/'manifest.json'),
                  'native_sha256': digest(a.image), 'sources': {n: digest(ROOT/n) for n in SOURCES},
                  'production_calls': sorted(machine.called), 'off': machine.stopped,
                  'clean_complete': sorted(machine.clean_complete), 'returned': machine.returned,
                  'retained': machine.retained, 'report_sha256': digest(report)}
        (a.output.parent/('case-'+name+'.json')).write_text(json.dumps(record, indent=2)+'\n')
        print('PASS', name, flush=True)
    results = []
    for name in ('success', 'counter-corruption', 'restore-el3'):
        path = a.output.parent/('case-'+name+'.json')
        if not path.exists(): return  # Resume only remaining cases after any fixture repair.
        record = json.loads(path.read_text())
        assert record['manifest_sha256'] == digest(a.bundle/'manifest.json') and record['native_sha256'] == digest(a.image)
        assert record['report_sha256'] == digest(a.output.parent/('report-'+name+'.bin'))
        results.append(record)
    a.output.write_text(json.dumps({'physical_access': False, 'cases': results,
        'manifest_sha256': digest(a.bundle/'manifest.json'), 'native_sha256': digest(a.image),
        'sources': {n: digest(ROOT/n) for n in SOURCES},
        'limits': 'Actual production barrier/lock instructions; modeled register, MMIO, exclusive-monitor and coherent-memory effects. Cache cleaning requests and write ownership checked. No physical result or task scheduling claim.'}, indent=2)+'\n')


if __name__ == '__main__': main()
