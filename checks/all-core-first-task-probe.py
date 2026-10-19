#!/usr/bin/env python3
"""Execute actual first-task restoration and the returning diagnostic overlay.

Register, MMIO and coherent-memory effects are modeled. No physical scheduling
or cache visibility follows from these instruction checks.
"""
from collections import deque
import argparse
import importlib.util
import json
import pickle
import struct
from pathlib import Path
from unicorn import UC_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('barrier', ROOT/'checks/all-core-pre-scheduler-probe.py')
barrier = importlib.util.module_from_spec(spec); spec.loader.exec_module(barrier)
digest = barrier.digest


class FirstTask(barrier.AllCore):
    cache_lines = 9280
    task_switch = False
    task_workload = False

    def restore(self, snapshot, fault):
        super().restore(snapshot, fault)
        # Baseline priority registers must obey the implemented five-bit model
        # too; otherwise saving then restoring them invents a hardware mismatch.
        for r in self.rdist:
            for address in range(r+0x10400, r+0x10420, 4):
                self.mmio[address] = self.mmio.get(address, 0) & 0xf8f8f8f8
                self.original_mmio[address] = self.mmio[address]
        self.secure_pmr = [0]*8
        self.entries = {}
        self.idle_entries, self.idle_yields, self.selected = set(), set(), set()
        self.recent = [deque(maxlen=24) for _ in range(8)]

    def pages(self, core):
        return range(0x41200000, 0x41291000, 4096)

    def nonsecure(self, core):
        r = self.regs[core]
        return r['currentel'] < 12 and bool(r['scr_el3'] & 1)

    def memory(self, u, access, address, size, value, core):
        if access == UC_MEM_WRITE and 0x41200000 <= address < 0x41291000:
            assert not any(self.cleaned.values()), 'Any CPU/EL wrote owned RAM after cleanup began'
        # Same implemented-priority semantics as a733-supervised-scheduler.py.
        # The port probes one priority byte during xPortStartScheduler.
        for r in self.rdist:
            if r+0x10400 <= address and address+size <= r+0x10420:
                assert size in (1, 4)
                result = 0
                for byte in range(size):
                    a = address+byte; word = a & ~3; shift = (a & 3)*8
                    group1 = bool(self.mmio[r+0x10080] & (1 << (a-r-0x10400)))
                    original = self.mmio.get(word, 0); raw = (original >> shift) & 255
                    if access == UC_MEM_WRITE:
                        if not self.nonsecure(core) or group1:
                            v = (value >> (8*byte)) & 255
                            raw = (((v >> 1) | 128) if self.nonsecure(core) else v) & 0xf8
                            self.mmio[word] = (original & ~(255 << shift)) | (raw << shift)
                    else:
                        v = ((raw << 1) & 255 if group1 else 0) if self.nonsecure(core) else raw
                        result |= v << (8*byte)
                if access != UC_MEM_WRITE:
                    u.mem_write(address, result.to_bytes(size, 'little'))
                return
        if self.regs[core]['currentel'] == 0:
            if self.task_switch and 0x41200000 <= address < 0x41291000:
                assert address+size <= 0x41291000 and core in self.el1_enabled
                if access == UC_MEM_WRITE:
                    self.dirty[core].update(range(address & ~63, (address+size+63) & ~63, 64))
                    self.changed(address, size)
                return
            assert access != UC_MEM_WRITE and 0x41000000 <= address < 0x41200000
        return super().memory(u, access, address, size, value, core)

    def instruction(self, u, address, size, core):
        if address not in self.decoded:
            self.decoded[address] = next(self.decoder.disasm(bytes(u.mem_read(address, 4)), address))
        ins = self.decoded[address]; op, args = ins.mnemonic, ins.op_str.split(', ')
        r = self.regs[core]
        self.recent[core].append(f'{address:#x} {op} {ins.op_str}')
        if self.task_switch and address == self.symbols['vTaskSwitchContext']:
            assert r['currentel'] == 4 and (self.task_workload or core in self.idle_yields)
            self.selected.add(core)
        if r['currentel'] == 0 and self.task_switch:
            idle = self.symbols['prvIdleTask' if core == 0 else 'prvPassiveIdleTask']
            if address == idle:
                self.idle_entries.add(core)
            if op == 'svc' and args == ['#0x69']:
                if not self.task_workload or idle <= address < idle+(44 if core == 0 else 28):
                    assert core in self.idle_entries and idle <= address < idle+(44 if core == 0 else 28)
                    self.idle_yields.add(core)
            if address == self.symbols['worker']:
                assert core in self.selected and core not in self.entries
                assert self.reg(core, 'x0') == core and r['sctlr_el1'] == 0x30d41805
                tcb = self.q(self.symbols['pxCurrentTCBs']+core*8)
                stack = (self.symbols['stacks']+(core+1)*16384-8) & ~15
                assert tcb == self.symbols['tasks']+core*168 and u.reg_read(UC_ARM64_REG_SP) == stack
                self.entries[core] = {'pc':address, 'sp':stack, 'tcb':tcb, 'argument':core}
            assert self.task_workload or idle <= address < idle+(44 if core == 0 else 28) or address in (
                self.symbols['worker'],self.survey_hooks['identity_first_task_el0']), {
                    'core':core,'pc':hex(address),'selected':sorted(self.selected),
                    'entries':self.entries,'trace':list(self.recent[core])}
        elif r['currentel'] == 0:
            assert address in (self.symbols['prvIdleTask'], self.symbols['prvPassiveIdleTask'],
                               self.survey_hooks['identity_first_task_el0'])
            if core not in self.entries:
                assert address == self.symbols['prvIdleTask' if core == 0 else 'prvPassiveIdleTask']
                assert r['sctlr_el1'] == 0x30d41805
                self.entries[core] = {'pc': address, 'sp': u.reg_read(UC_ARM64_REG_SP),
                    'tcb': self.q(self.symbols['pxCurrentTCBs']+core*8), 'argument': self.reg(core, 'x0')}
        if self.fault == 'wrong-task-argument' and core == 6 and address == self.survey_hooks['identity_first_task_el0']:
            self.put(core, 'x0', 1)
        if op == 'svc':
            # Include IL=1, as specified for the AArch64 SVC syndrome.
            self.exception(core, 1, address+4, (0x15 << 26) | (1 << 25) | int(args[0][1:], 0))
            return
        if op == 'msr' and args[0] == 'icc_pmr_el1':
            value = self.reg(core, args[1])
            ns = self.nonsecure(core) and bool(r['scr_el3'] & 4)
            if ns:
                if self.secure_pmr[core] & 128:
                    self.secure_pmr[core] = ((value >> 1) | 128) & 0xf8
            else:
                self.secure_pmr[core] = value & 0xf8
            r['icc_pmr_el1'] = (self.secure_pmr[core] << 1) & 255 if self.secure_pmr[core] & 128 else 0
            u.reg_write(UC_ARM64_REG_PC, address+4); return
        if op == 'mrs' and args[1] == 'icc_pmr_el1':
            mask = self.secure_pmr[core]
            ns = self.nonsecure(core) and bool(r['scr_el3'] & 4)
            self.put(core, args[0], ((mask << 1) & 255 if mask & 128 else 0) if ns else mask)
            u.reg_write(UC_ARM64_REG_PC, address+4); return
        if op == 'msr' and args[0] in ('icc_ctlr_el1', 'icc_bpr1_el1'):
            assert r['currentel'] == 4 and self.reg(core, args[1]) == 0
            r[args[0]] = 0; u.reg_write(UC_ARM64_REG_PC, address+4); return
        if op == 'dc' and args[0] == 'civac':
            assert len(self.entries) == 8, 'Cache cleanup before every task entered'
        return super().instruction(u, address, size, core)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('image', 'bundle', 'prefix', 'output'):
        p.add_argument('--'+n, type=Path, required=True)
    p.add_argument('--case', choices=('success','wrong-task-argument','restore-el3'), default='success')
    p.add_argument('--task-switch', action='store_true')
    a = p.parse_args()
    machine = FirstTask(a.image, a.bundle)
    machine.task_switch = a.task_switch
    machine.local_manifest = json.loads((a.bundle/'manifest.json').read_text())
    assert machine.local_manifest['schema'] == ('a733-all-core-task-switch-probe-v1'
        if a.task_switch else 'a733-all-core-first-task-probe-v1')
    machine.overlay = (a.bundle/'overlay.bin').read_bytes()
    meta = json.loads(a.prefix.with_suffix('.json').read_text())
    assert digest(a.prefix) == meta['snapshot_sha256']
    for path, sha in meta['inputs'].items():
        assert digest(Path(path)) == sha
    prior = json.loads(next(Path(n).read_text() for n in meta['inputs'] if n.endswith('/manifest.json')))
    assert prior['primary_manifest'] == machine.local_manifest['primary_manifest']
    base = Path(prior['primary_manifest']['path']); bm = json.loads(base.read_text())
    checkpoint = bm['hooks']['primary_publication_return']-0x41020000
    assert machine.overlay[:checkpoint] == (base.parent/'overlay.bin').read_bytes()[:checkpoint]
    snapshot = pickle.loads(a.prefix.read_bytes())  # Hash-checked trusted local snapshot.
    machine.restore(snapshot, None if a.case=='success' else a.case)
    machine.execute()
    if a.case != 'success':
        assert machine.retained and not machine.returned
        assert 6 in machine.target_parked and 6 not in machine.stopped
        if a.case == 'wrong-task-argument':
            assert not machine.clean_complete and not machine.stopped
        else:
            assert machine.clean_complete == set(range(8))
        a.output.write_text(json.dumps({'physical_access':False,'pass':True,'case':a.case,
            'manifest_sha256':digest(a.bundle/'manifest.json'),'native_sha256':digest(a.image),
            'sources':{n:digest(ROOT/n) for n in (*barrier.SOURCES,'checks/all-core-first-task-probe.py')},
            'off':machine.stopped,'clean_complete':sorted(machine.clean_complete)},indent=2)+'\n')
        print('PASS retained',a.case)
        return
    assert machine.returned and not machine.retained, {
        'started':machine.started,'stopped':machine.stopped,'entries':machine.entries,
        'called':sorted(machine.called),'parked':sorted(machine.target_parked),
        'pcs':[hex(u.reg_read(UC_ARM64_REG_PC)) for u in machine.cpus],
        'clock':machine.clock,'deadline':machine.deadline,
        'trace':list(machine.recent[1]),
        'report':machine.read(0x42018100,128).hex()}
    assert machine.started == machine.stopped == list(range(1, 8))
    assert machine.called == machine.clean_complete == set(machine.entries) == set(range(8))
    raw = machine.read(0x42018000, 0x5200)
    assert raw[0x3200:0x4200] == machine.read(barrier.SHARED, 4096)
    for c in range(8):
        assert struct.unpack_from('<8Q', raw, 0x3000+c*64) == (3, c, machine.epoch, 1, 256, 9280, 255, 0)
        record = struct.unpack_from('<8Q', raw, 0x3200+c*64)
        entry = machine.entries[c]
        assert record[:4] == (machine.epoch, 1, machine.survey_hooks['identity_first_task_el0']+4, entry['sp'])
        assert record[4] & 15 == 0 and record[5:] == (entry['tcb'], entry['argument'], 0x30d41805)
        assert machine.secure_pmr[c] == 0
    for address, value in machine.original_mmio.items():
        if 0x03400000 <= address < 0x03600000 and address & 0xffff not in (0x180, 0x280, 0x380):
            assert machine.mmio[address] == value, ('Unrestored GIC', hex(address))
    assert not any(machine.el3_enabled | machine.el1_enabled)
    report = a.output.with_suffix('.bin'); report.write_bytes(raw)
    result = {'physical_access': False, 'pass': True, 'entries': machine.entries,
        'native_sha256': digest(a.image), 'manifest_sha256': digest(a.bundle/'manifest.json'),
        'report_sha256': digest(report), 'clean_lines_per_core': 9280, 'off': machine.stopped,
        'sources': {n: digest(ROOT/n) for n in (*barrier.SOURCES, 'checks/all-core-first-task-probe.py')},
        'limits': __doc__}
    if a.task_switch:
        assert machine.idle_entries == machine.idle_yields == machine.selected == set(range(8))
        result.update(idle_yield_cores=sorted(machine.idle_yields),
                      worker_selection_cores=sorted(machine.selected),worker_bodies_executed=False)
    a.output.write_text(json.dumps(result, indent=2)+'\n')
    print('PASS eight '+('idle-to-worker switches' if a.task_switch else 'first-task entries')+
          ', full writable-range cleaning and matched return')


if __name__ == '__main__':
    main()
