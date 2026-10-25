#!/usr/bin/env python3
"""Execute the returning 32-round FreeRTOS workload with modeled hardware.

Actual workers, FP context checks, tick/SGI handlers and stop instructions run.
Timing, interrupt delivery and coherent cache effects remain modeled.

The modeled counter advances COUNTER_STEP ticks (24 MHz) per scheduling
quantum, in which each core executes 211 to 887 instructions, so a 1 ms tick
period gives CPU0 about 50,000 instructions. The image reprograms its tick as
the previous compare value plus the period, so a tick handled later than one
period fires again at once. At 1,000 ticks per quantum (about 12,500
instructions per period) CPU0's tick, SGI and context-switch work came close
to the whole period: inserting four or more instructions before EL1 entry
left CPU0's worker starved by back-to-back ticks. A core on the board
executes roughly a million instructions per tick period. The run therefore
fails at once, naming the model, if CPU0's enabled tick compare value falls
more than one period behind the modeled counter.
"""
import argparse
import importlib.util
import json
import pickle
import struct
from pathlib import Path
from unicorn import UC_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP

ROOT = Path(__file__).resolve().parents[1]
COUNTER_STEP = 250
TICK_PERIOD = 24000  # Counter ticks per FreeRTOS tick: 24 MHz at configTICK_RATE_HZ 1000.
spec = importlib.util.spec_from_file_location('first_task', ROOT/'checks/all-core-first-task-probe.py')
first = importlib.util.module_from_spec(spec); spec.loader.exec_module(first)
base = first.barrier.model
digest = first.digest


class Workload(first.FirstTask):
    task_switch = True
    task_workload = True

    def restore(self, snapshot, fault):
        super().restore(snapshot, fault)
        self.pending = [set() for _ in range(8)]
        self.irq_active = [None]*8
        self.delivered = [[] for _ in range(8)]
        self.completed_workers = set()
        self.stopped_writers = set()
        self.wait_cores = set()
        self.last_rounds = None
        self.last_progress_tick = 0

    def memory(self, u, access, address, size, value, core):
        r = self.rdist[core]
        if address == r+0x10200 and access != UC_MEM_WRITE:
            self.mmio[address] = sum(1 << irq for irq in self.pending[core])
        if address == r+0x10300 and access != UC_MEM_WRITE:
            self.mmio[address] = 0 if self.irq_active[core] is None else 1 << self.irq_active[core]
        if address == r+0x10280 and access == UC_MEM_WRITE:
            self.pending[core].difference_update(i for i in range(32) if value & (1 << i))
        if core and self.regs[core]['currentel'] == 4 and address < 0x41000000:
            assert address in (r, r+0x10180, r+0x10280, r+0x10200, r+0x10300)
            if access == UC_MEM_WRITE:
                assert address in (r+0x10180, r+0x10280) and value == 0x40000001
                assert self.stopped_writers == set(range(8))
            return first.barrier.local.Local.memory(self, u, access, address, size, value, core)
        return super().memory(u, access, address, size, value, core)

    def instruction(self, u, address, size, core):
        r = self.regs[core]
        if core == 0:
            timer_asserted = r['cntp_ctl_el0'] & 3 == 1 and self.clock >= r['cntp_cval_el0']
            if timer_asserted and self.irq_active[core] != 30:
                self.pending[core].add(30)
            elif not timer_asserted:
                self.pending[core].discard(30)
        if (r['currentel'] <= 4 and not r['daif'] & 0x80 and
                r['icc_igrpen1_el1'] & 1 and r['icc_pmr_el1'] > 0xe0 and
                self.pending[core] and self.irq_active[core] is None):
            self.exception(core, 1, address, irq=True)
            return
        if address not in self.decoded:
            self.decoded[address] = next(self.decoder.disasm(bytes(u.mem_read(address, 4)), address))
        ins = self.decoded[address]; op, args = ins.mnemonic, ins.op_str.split(', ')
        if address == self.survey_hooks['identity_workload_complete']:
            assert r['currentel'] == 0 and self.reg(core, 'x19') == core
            assert self.q(self.symbols['a733_smp_core_results']+core*128) == 32
            self.completed_workers.add(core)
            if len(self.completed_workers) == 8 and self.checkpoint is not None:
                assert not self.stopped_writers and not any(self.cleaned.values())
                path = self.checkpoint
                self.checkpoint = None
                base.Machine.save(self, path)
                path.with_suffix('.json').write_text(json.dumps({
                    'snapshot_sha256': digest(path), 'native_sha256': self.native_hash,
                    'manifest_sha256': self.bundle_hash,
                    'sources': {name: digest(ROOT/name) for name in first.barrier.SOURCES},
                    'checker_sha256': digest(Path(__file__)),
                    'scope': 'All eight workers completed 32 rounds; no writer has stopped. Reuse for stop/cleanup cases only.'}, indent=2)+'\n')
        if address == self.survey_hooks['identity_first_task_el0']:
            assert self.completed_workers == set(range(8))
            self.entries[core]['sp'] = u.reg_read(UC_ARM64_REG_SP)
            self.entries[core]['argument'] = self.reg(core, 'x0')
        if op == 'wfe' and r['currentel'] == 0:
            assert r['sctlr_el1'] & (1 << 18)
            self.wait_cores.add(core)
        if op == 'msr' and args[0] in ('icc_igrpen1_el1', 'icc_sgi1r_el1', 'icc_eoir1_el1'):
            name, value = args[0], self.reg(core, args[1])
            assert r['currentel'] == 4
            if name == 'icc_igrpen1_el1':
                assert value in (0, 1)
                if not value:
                    assert core in self.completed_workers and not r['cntp_ctl_el0'] & 3
                    self.stopped_writers.add(core)
                r[name] = value
            elif name == 'icc_sgi1r_el1':
                target = (value >> 16) & 255
                assert target < 8 and value & 0xffff == 1 and (value >> 24) & 15 == 0
                assert core not in self.stopped_writers
                self.pending[target].add(0)
            else:
                assert self.irq_active[core] == value and not r.get('icc_ctlr_el1', 0) & 2
                self.irq_active[core] = None
            u.reg_write(UC_ARM64_REG_PC, address+4)
            return
        if op == 'mrs' and args[1] == 'icc_iar1_el1':
            irq = min(self.pending[core]) if self.pending[core] else 1023
            if irq < 1020:
                assert irq in (0, 30) and self.irq_active[core] is None
                address_priority = self.rdist[core]+0x10400+irq
                priority = (self.mmio[address_priority & ~3] >> (8*(address_priority & 3))) & 255
                assert (priority << 1) & 255 == 0xe0
                assert self.mmio[self.rdist[core]+0x10100] & (1 << irq)
                self.pending[core].remove(irq); self.irq_active[core] = irq
                self.delivered[core].append(irq)
            self.put(core, args[0], irq)
            u.reg_write(UC_ARM64_REG_PC, address+4)
            return
        if op == 'dc' and args[0] == 'civac' and not self.production_table_clean(address):
            assert self.stopped_writers == set(range(8))
            assert not self.pending[core] and self.irq_active[core] is None
        if self.fault == 'wrong-rounds' and core == 6 and address == self.survey_hooks['identity_first_task_el0']:
            at = self.symbols['a733_smp_core_results']+core*128
            u.mem_write(at, struct.pack('<Q', 31)); self.changed(at, 8)
        return super().instruction(u, address, size, core)

    def check_tick_lag(self):
        """Fail if CPU0's tick has fallen more than one period behind the
        modeled counter: the model is saturated, and the run says so instead
        of reporting starved workers."""
        r = self.regs[0]
        lag = self.clock - r['cntp_cval_el0']
        if r['cntp_ctl_el0'] & 3 == 1 and lag > TICK_PERIOD:
            raise AssertionError(('Model saturated: CPU0 tick compare value is more than one period '
                                  'behind the modeled counter; reduce COUNTER_STEP', self.clock, lag))

    def execute(self):
        for quantum in range(100000):
            for index in range(8):
                core = (index+quantum) % 8
                if not self.active[core]:
                    continue
                u = self.cpus[core]
                u.emu_start(u.reg_read(UC_ARM64_REG_PC), 0, count=(211, 347, 509, 673, 887)[(quantum+core) % 5])
                if self.returned or self.retained:
                    return
            if len(self.entries) == 8 and not self.stopped_writers:
                result = self.symbols['a733_smp_core_results']
                rounds = tuple(self.q(result+c*128) for c in range(8))
                ticks = self.q(result+32)
                if rounds != self.last_rounds:
                    self.last_rounds, self.last_progress_tick = rounds, ticks
                    if min(rounds) // 8 != min(self.last_announced_rounds) // 8:
                        print('Worker rounds:', rounds, 'ticks:', ticks, flush=True)
                    self.last_announced_rounds = rounds
                elif ticks-self.last_progress_tick >= 256:
                    raise AssertionError(('No worker progress for 256 ticks', rounds, ticks))
            self.check_tick_lag()
            self.clock += COUNTER_STEP
        raise AssertionError(('Returning workload instruction budget', self.last_rounds))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'bundle', 'prefix', 'output'):
        p.add_argument('--'+name, type=Path, required=True)
    p.add_argument('--case', choices=('success', 'wrong-rounds'), default='success')
    p.add_argument('--resume-workload', type=Path,
                   help='Reuse hash-checked completed workers for changed stop/cleanup checks only')
    a = p.parse_args()
    machine = Workload(a.image, a.bundle)
    machine.local_manifest = json.loads((a.bundle/'manifest.json').read_text())
    assert machine.local_manifest['schema'] == 'a733-all-core-task-workload-probe-v1'
    machine.overlay = (a.bundle/'overlay.bin').read_bytes()
    meta = json.loads(a.prefix.with_suffix('.json').read_text())
    assert digest(a.prefix) == meta['snapshot_sha256']
    assert all(digest(Path(path)) == sha for path, sha in meta['inputs'].items())
    prior = json.loads(next(Path(path).read_text() for path in meta['inputs'] if path.endswith('/manifest.json')))
    assert prior['primary_manifest'] == machine.local_manifest['primary_manifest']
    primary = Path(prior['primary_manifest']['path'])
    checkpoint = json.loads(primary.read_text())['hooks']['primary_publication_return']-0x41020000
    assert machine.overlay[:checkpoint] == (primary.parent/'overlay.bin').read_bytes()[:checkpoint]
    machine.restore(pickle.loads(a.prefix.read_bytes()), None if a.case == 'success' else a.case)
    machine.last_announced_rounds = (0,)*8
    machine.native_hash, machine.bundle_hash = digest(a.image), digest(a.bundle/'manifest.json')
    machine.checkpoint = a.output.parent/'workload-complete.pickle'
    if a.resume_workload:
        saved = json.loads(a.resume_workload.with_suffix('.json').read_text())
        assert saved['snapshot_sha256'] == digest(a.resume_workload)
        assert saved['native_sha256'] == machine.native_hash and saved['manifest_sha256'] == machine.bundle_hash
        assert all(digest(ROOT/name) == value for name, value in saved['sources'].items())
        base.Machine.restore(machine, a.resume_workload)
        assert machine.completed_workers == set(range(8)) and not machine.stopped_writers
        assert not any(machine.cleaned.values())
        machine.fault = None if a.case == 'success' else a.case
    try:
        machine.execute()
        if a.case == 'success':
            assert machine.returned and not machine.retained
            assert machine.security == base.el3_security_writes(), machine.security
            assert machine.started == machine.stopped == list(range(1, 8))
            assert machine.clean_complete == machine.stopped_writers == set(range(8))
            assert not any(machine.pending) and all(irq is None for irq in machine.irq_active)
            assert not machine.el3_enabled and not machine.el1_enabled
            for address, value in machine.original_mmio.items():
                if 0x03400000 <= address < 0x03600000 and address & 0xffff not in (0x180, 0x280, 0x380):
                    assert machine.mmio[address] == value, ('Unrestored GIC', hex(address))
        else:
            assert machine.retained and not machine.returned and not machine.clean_complete
            assert not machine.stopped and machine.completed_workers == set(range(8))
    except BaseException:
        a.output.with_suffix('.failure.json').write_text(json.dumps({
            'clock': machine.clock, 'rounds': machine.last_rounds,
            'completed_workers': sorted(machine.completed_workers),
            'stopped_writers': sorted(machine.stopped_writers),
            'pending': [sorted(pending) for pending in machine.pending],
            'traces': [list(trace) for trace in machine.recent],
            'registers': machine.regs}, indent=2)+'\n')
        raise
    raw = machine.read(0x42018000, 0x5200)
    report = a.output.with_suffix('.bin'); report.write_bytes(raw)
    results = [struct.unpack_from('<5Q', raw, 0x4280+c*128) for c in range(8)]
    if a.case == 'success':
        for rounds, background, sgis, failed, ticks in results:
            assert rounds == 32 and background > 0 and sgis > 0 and failed == 0
        assert results[0][4] >= 32
    sources = (*first.barrier.SOURCES, 'checks/all-core-first-task-probe.py',
               'checks/all-core-task-workload-probe.py')
    a.output.write_text(json.dumps({'pass': True, 'case': a.case, 'physical_access': False,
        'native_sha256': digest(a.image), 'manifest_sha256': digest(a.bundle/'manifest.json'),
        'report_sha256': digest(report), 'sources': {name: digest(ROOT/name) for name in sources},
        'workload_checkpoint': str(a.resume_workload) if a.resume_workload else None,
        'workload_results': results, 'interrupt_counts': [len(v) for v in machine.delivered],
        'off': machine.stopped, 'clean_complete': sorted(machine.clean_complete),
        'el0_wait_cores': sorted(machine.wait_cores), 'limits': __doc__}, indent=2)+'\n')
    print('PASS returning task workload:', a.case, flush=True)


if __name__ == '__main__':
    main()
