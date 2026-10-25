#!/usr/bin/env python3
"""Execute actual first-task restoration and the returning diagnostic overlay.

Register, MMIO and coherent-memory effects are modeled. No physical scheduling
or cache visibility follows from these instruction checks. The refusal cases
change one modeled GIC state before the pre-release admission and require a
return to the ROM with the matching code, before any secondary is released.
"""
from collections import deque
import argparse
import importlib.util
import json
import re
import pickle
import struct
from pathlib import Path
from unicorn import UC_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP

ROOT = Path(__file__).resolve().parents[1]
# SCTLR_EL1 that the native cached entry establishes (boot/a733/secondary-local-probe.h).
EL1_SCTLR = int(re.search(r'^#define NATIVE_EL1_SCTLR (0x[0-9a-fA-F]+)', (ROOT/'boot/a733/secondary-local-probe.h').read_text(), re.M).group(1), 16)
spec = importlib.util.spec_from_file_location('barrier', ROOT/'checks/all-core-pre-scheduler-probe.py')
barrier = importlib.util.module_from_spec(spec); spec.loader.exec_module(barrier)
digest = barrier.digest


REPORT = 0x42018100
# Refusal case: injected GIC words, then the expected code, address, value read and core.
IDLE_RESUME_CORE = 3
REFUSALS = {
    'refuse-gic-unreadable': (None, (0x10, 0x03400004, 0, 0)),
    'refuse-cpu0-pending': ({0x03470200: 1 << 29}, (0x34, 0x03470200, 1 << 29, 0)),
}
# Words the admission reads in other cores' redistributor frames before release.
ADMISSION_OFFSETS = (0x0, 0x14, 0x24, 0x10100, 0x10200, 0x10300)


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
        self.idle_resumed = False
        self.recent = [deque(maxlen=24) for _ in range(8)]
        if fault in REFUSALS:
            words, _ = REFUSALS[fault]
            for address in [a for a in self.mmio if 0x03400000 <= a < 0x03600000]:
                if words is None:
                    self.mmio[address] = 0  # Every GIC word reads zero.
                elif address in words:
                    self.mmio[address] = words[address]
                self.original_mmio[address] = self.mmio[address]

    def pages(self, core):
        return range(0x41200000, 0x41291000, 4096)

    def nonsecure(self, core):
        r = self.regs[core]
        return r['currentel'] < 12 and bool(r['scr_el3'] & 1)

    def memory(self, u, access, address, size, value, core):
        # Pre-release admission: CPU0 reads, never writes, other frames' words.
        if (core == 0 and access != UC_MEM_WRITE and not self.started and size == 4 and
                0x03480000 <= address < 0x03560000 and (address-0x03460000) % 0x20000 in ADMISSION_OFFSETS):
            return barrier.model.Machine.memory(self, u, access, address, size, value, core)
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
        if op == 'msr' and args[0] == 'rmr_el3' and self.q(REPORT+48) == 3:
            # Admission refusal: return before any release, with no GIC write.
            assert core == 0 and self.reg(core, args[1]) == 2 and not self.started and not any(self.active[1:])
            self.returned = True; u.emu_stop(); return
        if self.task_switch and address == self.symbols['vTaskSwitchContext']:
            assert r['currentel'] == 4 and (self.task_workload or core in self.idle_yields)
            self.selected.add(core)
        if r['currentel'] == 0 and self.task_switch:
            idle = self.symbols['prvIdleTask' if core == 0 else 'prvPassiveIdleTask']
            if address == idle:
                self.idle_entries.add(core)
            if op == 'svc' and args == ['#0x69']:
                if not self.task_workload or idle <= address < idle+self.sizes['prvIdleTask' if core == 0 else 'prvPassiveIdleTask']:
                    assert core in self.idle_entries and idle <= address < idle+self.sizes['prvIdleTask' if core == 0 else 'prvPassiveIdleTask']
                    self.idle_yields.add(core)
            if address == self.symbols['worker']:
                assert core in self.selected and core not in self.entries
                assert self.reg(core, 'x0') == core and r['sctlr_el1'] == EL1_SCTLR
                tcb = self.q(self.symbols['pxCurrentTCBs']+core*8)
                stack = (self.symbols['stacks']+(core+1)*16384-8) & ~15
                assert tcb == self.symbols['tasks']+core*168 and u.reg_read(UC_ARM64_REG_SP) == stack
                self.entries[core] = {'pc':address, 'sp':stack, 'tcb':tcb, 'argument':core}
            check = self.symbols['prvCheckTasksWaitingTermination']
            assert self.task_workload or idle <= address < idle+self.sizes['prvIdleTask' if core == 0 else 'prvPassiveIdleTask'] or (
                check <= address < check+self.sizes['prvCheckTasksWaitingTermination']) or address in (
                self.symbols['worker'],self.survey_hooks['identity_first_task_el0']), {
                    'core':core,'pc':hex(address),'selected':sorted(self.selected),
                    'entries':self.entries,'trace':list(self.recent[core])}
        elif r['currentel'] == 0:
            assert address in (self.symbols['prvIdleTask'], self.symbols['prvPassiveIdleTask'],
                               self.survey_hooks['identity_first_task_el0'])
            if core not in self.entries:
                assert address == self.symbols['prvIdleTask' if core == 0 else 'prvPassiveIdleTask']
                assert r['sctlr_el1'] == EL1_SCTLR
                self.entries[core] = {'pc': address, 'sp': u.reg_read(UC_ARM64_REG_SP),
                    'tcb': self.q(self.symbols['pxCurrentTCBs']+core*8), 'argument': self.reg(core, 'x0')}
        if (self.fault == 'idle-resumes' and core == IDLE_RESUME_CORE and r['currentel'] == 0 and op == 'svc' and
                args == ['#0x69'] and not self.idle_resumed):
            # The scheduler selects this idle task again: its first yield
            # returns at once, and the idle loop continues.
            assert self.task_switch and not self.task_workload
            self.idle_resumed = True
            u.reg_write(UC_ARM64_REG_PC, address+4); return
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
        if op == 'dc' and args[0] == 'civac' and not self.production_table_clean(address):
            # The overlay's owned-memory cleanup follows every task entry.
            assert len(self.entries) == 8, ('Cache cleanup before every task entered', core, hex(address), sorted(self.entries))
        return super().instruction(u, address, size, core)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('image', 'bundle', 'prefix', 'output'):
        p.add_argument('--'+n, type=Path, required=True)
    p.add_argument('--case', choices=('success','wrong-task-argument','restore-el3','idle-resumes', *REFUSALS), default='success')
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
    if a.case in REFUSALS:
        assert machine.returned and not machine.retained and machine.started == machine.stopped == []
        assert machine.security == [], machine.security  # Refused before the EL3 security setup.
        assert all(address in (0x08001004, 0x08001008) for address, _, _ in machine.mmio_writes), machine.mmio_writes
        assert machine.mmio == machine.original_mmio | {0x08001004: 0x20, 0x08001008: 0}
        raw = machine.read(0x42018000, 0x5200)
        header = struct.unpack_from('<32Q', raw, 0x100)
        code, address, observed, core = REFUSALS[a.case][1]
        assert header[:8] == (0x4137335052455331, 1, 0x5100, machine.epoch, machine.deadline, 0, 3, core), header[:8]
        assert header[12:15] == (code, address, observed) and not any(header[8:12]+header[15:])
        assert not any(raw[0x200:])
        report = a.output.with_suffix('.bin'); report.write_bytes(raw)
        a.output.write_text(json.dumps({'physical_access':False,'pass':True,'case':a.case,
            'manifest_sha256':digest(a.bundle/'manifest.json'),'native_sha256':digest(a.image),
            'sources':{n:digest(ROOT/n) for n in (*barrier.SOURCES,'checks/all-core-first-task-probe.py')},
            'returned':True,'started':[],'off':[],'clean_complete':[],'refusal':{'code':code,'address':address,
            'value':observed,'core':core},'report_sha256':digest(report)},indent=2)+'\n')
        print('PASS returned refusal',a.case,hex(code))
        return
    if a.case not in ('success', 'idle-resumes'):
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
    assert machine.security == barrier.model.el3_security_writes(), machine.security
    assert machine.returned and not machine.retained, {
        'started':machine.started,'stopped':machine.stopped,'entries':machine.entries,
        'called':sorted(machine.called),'parked':sorted(machine.target_parked),
        'pcs':[hex(u.reg_read(UC_ARM64_REG_PC)) for u in machine.cpus],
        'clock':machine.clock,'deadline':machine.deadline,
        'trace':list(machine.recent[1]),
        'report':machine.read(0x42018100,128).hex()}
    assert machine.started == machine.stopped == list(range(1, 8))
    assert machine.called == machine.clean_complete == set(machine.entries) == set(range(8))
    # A resumed idle task must still reach its worker without an interrupt.
    assert a.case != 'idle-resumes' or machine.idle_resumed
    raw = machine.read(0x42018000, 0x5200)
    assert raw[0x3200:0x4200] == machine.read(barrier.SHARED, 4096)
    for c in range(8):
        assert struct.unpack_from('<8Q', raw, 0x3000+c*64) == (3, c, machine.epoch, 1, 256, 9280, 255, 0)
        record = struct.unpack_from('<8Q', raw, 0x3200+c*64)
        entry = machine.entries[c]
        assert record[:4] == (machine.epoch, 1, machine.survey_hooks['identity_first_task_el0']+4, entry['sp'])
        assert record[4] & 15 == 0 and record[5:] == (entry['tcb'], entry['argument'], EL1_SCTLR)
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
                      worker_selection_cores=sorted(machine.selected),worker_bodies_executed=False,
                      case=a.case, idle_resumed_core=IDLE_RESUME_CORE if machine.idle_resumed else None)
    a.output.write_text(json.dumps(result, indent=2)+'\n')
    print('PASS eight '+('idle-to-worker switches' if a.task_switch else 'first-task entries')+
          ', full writable-range cleaning and matched return')


if __name__ == '__main__':
    main()
