#!/usr/bin/env python3
"""Execute the return-deadline workload profiles with modeled hardware.

The actual overlay, the patched frozen native image and the real FreeRTOS
workload run. The model adds the EL3 physical timer (CNTPS), Group 0 delivery
of its PPI 29 to EL3 as an FIQ, ICC_IAR0/EOIR0 and the deadline unit's
cleaning SMC. Cases:

- success: the deadline is armed, the workload completes, CPU0's completion
  SMC disarms the deadline before it fires, and the unchanged matched
  shutdown, GIC restoration and ROM return follow.
- stalled-core: resumes from the hash-checked point where all eight workers
  finished their rounds and no core has stopped its writers, then stops
  executing core 5, which therefore never publishes its completion record. The others wait for
  it until the startup deadline and park, at EL0 in the arrival barrier or,
  if core 5 had already arrived, at EL1 in identity_park; from identity_park
  CPU0 takes no tick and the deadline interrupt returns it, as in
  stalled-no-tick. Otherwise CPU0's next tick then
  takes the production IRQ-handler deadline exit, an SMC that the deadline
  unit captures: CPU0 records its state, cleans the owned writable interval
  from EL1 and returns to the ROM. Secondaries that take an interrupt after
  the deadline make the same exit and park in EL3, as on the board.
- stalled-no-tick: as stalled-core, and CPU0's tick timer also stops, so
  CPU0 takes no interrupt. The EL3 deadline interrupt then takes CPU0 from
  its wait, at EL0 or in identity_park at EL1, to the same capture, cleaning
  and return.
- restore-rwp: resumes from the same checkpoint with every core running.
  After CPU0's EL3 restoration rewrites its SGI/PPI priorities, the next
  read of its GICR_CTLR returns RWP set, as port 3-3 recorded on
  2026-10-25. The corrected restoration waits for RWP and the run returns
  normally.
- restore-rwp-unfixed: the same injection against an overlay without that
  wait. The restoration's comparison fails (cleanup error 3), the survey
  returns 0 and CPU0 returns through the deadline unit with reason 3, as on
  the board. Run it only against such an overlay; it reproduces the fault.
- restore-rwp-late and restore-rwp-late-secondary (with -unfixed
  variants): RWP reads clear at the first GICR_CTLR read after the priority
  write and set at the second, as port 3-3 recorded on CPU0 after the RWP
  wait was added. The corrected comparisons ignore GICR_CTLR's read-only
  status bits and the run returns normally; an overlay that compares them
  fails as in the -unfixed cases above.
- restore-rwp-secondary and restore-rwp-secondary-unfixed: the same
  injection on core 3's redistributor during its EL3 cleanup. Corrected,
  the cleanup waits for RWP and the run returns normally; uncorrected, its
  final comparison fails (cleanup error 6), core 3 parks, CPU0's survey
  times out waiting for it and CPU0 returns with reason 3.
- restore-pwrr and restore-pwrr-secondary (with -unfixed variants): the
  first GICR_PWRR read after the restoring priority write, on CPU0 or on
  core 3, returns both group power bits (RDGPO, RDGPD) flipped, as when the
  shared redistributor group changes power state. The corrected comparisons
  ignore those status bits and the run returns normally; an overlay that
  compares the whole word fails as in the matching RWP cases.

Timing, cache visibility and interrupt delivery remain modeled. The modeled
clock advances as in the workload model (its COUNTER_STEP, with the reason
there); after the stall it advances 20,000 ticks per scheduling quantum, so
that the 0.5 s deadline is reached in a bounded run time.
"""
import argparse
import importlib.util
import json
import pickle
import re
import struct
from pathlib import Path
from unicorn.arm64_const import UC_ARM64_REG_PC

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('workload', ROOT/'checks/all-core-task-workload-probe.py')
workload = importlib.util.module_from_spec(spec); spec.loader.exec_module(workload)
first = workload.first
base = workload.base
digest = workload.digest
MASK = (1 << 64)-1
# The overlay's own definitions (boot/a733/secondary-local-probe.h).
PROBE = {name: int(value, 0) for name, value in re.findall(
    r'^#define (\w+) (0x[0-9a-fA-F]+|\d+)\b', (ROOT/'boot/a733/secondary-local-probe.h').read_text(), re.M)}
RECORD = PROBE['DEADLINE_RECORD']
RECORD_MAGIC = PROBE['DEADLINE_RECORD_MAGIC']
DEADLINE_TICKS = PROBE['DEADLINE_TICKS']
STALLED_CORE = 5
# Fault: (core, redistributor register, bits changed, index of the read of
# that register after the core's priority write: 0 the first, 1 the second).
# GICR_CTLR reads RWP set; GICR_PWRR reads RDGPO and RDGPD inverted.
RWP, PWRR = (0, 8), (0x24, PROBE['GICR_PWRR_STATUS'])
RWP_TARGET = {'restore-rwp': (0, *RWP, 0), 'restore-rwp-unfixed': (0, *RWP, 0),
              'restore-rwp-secondary': (3, *RWP, 0), 'restore-rwp-secondary-unfixed': (3, *RWP, 0),
              'restore-rwp-late': (0, *RWP, 1), 'restore-rwp-late-unfixed': (0, *RWP, 1),
              'restore-rwp-late-secondary': (3, *RWP, 1), 'restore-rwp-late-secondary-unfixed': (3, *RWP, 1),
              'restore-pwrr': (0, *PWRR, 0), 'restore-pwrr-unfixed': (0, *PWRR, 0),
              'restore-pwrr-secondary': (3, *PWRR, 0), 'restore-pwrr-secondary-unfixed': (3, *PWRR, 0)}
COMPLETIONS = 0x41243000  # CPU0 EL1 stack page: one 64-byte completion record per core.
SOURCES = (*first.barrier.SOURCES, 'checks/all-core-first-task-probe.py',
           'checks/all-core-task-workload-probe.py', 'checks/workload-bisect-probe.py')


class Bisect(workload.Workload):
    def restore(self, snapshot, fault):
        super().restore(snapshot, fault)
        for r in self.regs:
            r.setdefault('cntps_ctl_el1', 0); r.setdefault('cntps_cval_el1', MASK)
            r.setdefault('icc_igrpen0_el1', 0); r.setdefault('elr_el1', 0); r.setdefault('spsr_el1', 0)
            r.setdefault('spsr_el3', 0)
        self.fiq_active = False
        self.fiq_taken = []
        self.armed = []
        self.disarmed_at_completion = False
        self.helper_lines = 0
        self.clock_step = workload.COUNTER_STEP
        self.rwp_injected = False
        self.rwp_reads = self.rwp_seen = 0
        self.cpu0_parked = False

    def in_hook(self, name, address, length):
        start = self.survey_hooks[name]
        return start <= address < start+length

    def timer_asserted(self):
        r = self.regs[0]
        return r['cntps_ctl_el1'] & 3 == 1 and self.clock >= r['cntps_cval_el1']

    def fiq_pending(self):
        g = self.rdist[0]+0x10000
        return (self.timer_asserted() and self.regs[0]['icc_igrpen0_el1'] & 1 and
                not self.mmio[g+0x80] & (1 << 29) and self.mmio[g+0x100] & (1 << 29))

    def memory(self, u, access, address, size, value, core):
        target, register, bits, skip = RWP_TARGET.get(self.fault, (None, 0, 0, 0))
        if core == target and self.regs[core]['currentel'] == 12:
            watched = self.rdist[core]+register
            if access == workload.UC_MEM_WRITE and address == self.rdist[core]+0x10400 and not self.rwp_injected:
                self.rwp_injected = True; self.rwp_reads = 1; self.rwp_skip = skip
            elif access != workload.UC_MEM_WRITE and address == watched and self.rwp_reads and self.rwp_skip:
                self.rwp_skip -= 1
            elif access != workload.UC_MEM_WRITE and address == watched and self.rwp_reads:
                self.rwp_reads -= 1; self.rwp_seen += 1
                changed = self.mmio[address] ^ bits if register else self.mmio[address] | bits
                u.mem_write(address, changed.to_bytes(4, 'little'))
                return
        # The capture reads every redistributor's pending, active and enable
        # words and every core's power status; nothing else changes there.
        pc = self.cpus[core].reg_read(UC_ARM64_REG_PC)
        capture = self.survey_hooks['identity_deadline_capture']
        if core == 0 and capture <= pc < self.survey_hooks['identity_deadline_el1'] and address < 0x41000000:
            assert access != workload.UC_MEM_WRITE and size == 4 and self.regs[0]['currentel'] == 12
            words = {0x07052008+c*0x1000: self.mmio[0x07052008+c*0x1000] for c in range(8)}
            for target, r in enumerate(self.rdist):
                pending = set(self.pending[target])
                if target == 0 and self.timer_asserted():
                    pending.add(29)
                words[r+0x10200] = sum(1 << irq for irq in pending)
                words[r+0x10300] = 0 if self.irq_active[target] is None else 1 << self.irq_active[target]
                words[r+0x10100] = self.mmio[r+0x10100]
            assert address in words, hex(address)
            u.mem_write(address, int(words[address]).to_bytes(4, 'little'))
            return
        return super().memory(u, access, address, size, value, core)

    def instruction(self, u, address, size, core):
        r = self.regs[core]
        if core == 0 and r['currentel'] < 12 and not self.fiq_active and self.fiq_pending():
            # SCR_EL3.FIQ routes Group 0 to EL3; PSTATE masks at EL0/EL1 do not apply.
            assert r['scr_el3'] & 4
            self.exception(0, 3, address)
            u.reg_write(UC_ARM64_REG_PC, self.regs[0]['vbar_el3']+0x500)
            self.fiq_active = True; self.fiq_taken.append(self.clock)
            return
        if address not in self.decoded:
            self.decoded[address] = next(self.decoder.disasm(bytes(u.mem_read(address, 4)), address))
        ins = self.decoded[address]; op, args = ins.mnemonic, ins.op_str.split(', ')
        if address == self.survey_hooks['identity_park'] and self.fault in ('stalled-core', 'stalled-no-tick'):
            # identity_park waits in WFI with interrupts masked. A secondary
            # stays there. CPU0 waits too: SCR_EL3.FIQ routes the deadline
            # interrupt to EL3, which takes it whatever the EL1 masks (the
            # check above), from this wait as from an EL0 one.
            assert r['currentel'] == 4 and r['daif'] & 0x40
            if core:
                self.target_parked.add(core); self.active[core] = False
            else:
                self.cpu0_parked = True
            u.emu_stop(); return
        if self.in_hook('identity_deadline_arm', address, 0x58):
            assert r['currentel'] == 12
            if op == 'msr' and args[0] in ('icc_igrpen0_el1', 'cntps_cval_el1', 'cntps_ctl_el1'):
                value = self.reg(core, args[1])
                if core:
                    assert args[0] == 'icc_igrpen0_el1' and value == 0
                r[args[0]] = value
                if args[0] == 'cntps_cval_el1':
                    assert core == 0 and value == self.deadline+DEADLINE_TICKS
                    self.armed.append(value)
                u.reg_write(UC_ARM64_REG_PC, address+4); return
        if self.in_hook('identity_deadline_smc', address, 0x48) and op == 'msr':
            assert core == 0 and self.reg(core, args[1]) == 0
            if args[0] == 'cntps_ctl_el1':
                assert not self.fiq_taken and len(self.completed_workers) == 8
                self.disarmed_at_completion = True
            r[args[0]] = 0
            u.reg_write(UC_ARM64_REG_PC, address+4); return
        capture = self.survey_hooks['identity_deadline_capture']
        if capture <= address < self.survey_hooks['identity_deadline_el1']:
            assert core == 0 and r['currentel'] == 12
            if op == 'mrs' and args[1] == 'icc_iar0_el1':
                assert self.fiq_pending()
                self.put(core, args[0], 29); u.reg_write(UC_ARM64_REG_PC, address+4); return
            if op == 'msr' and args[0] in ('icc_eoir0_el1', 'icc_igrpen0_el1', 'cntps_ctl_el1'):
                value = self.reg(core, args[1])
                if args[0] == 'icc_eoir0_el1':
                    assert value == 29 and self.fiq_active; self.fiq_active = False
                else:
                    assert value == 0; r[args[0]] = 0
                u.reg_write(UC_ARM64_REG_PC, address+4); return
            if op == 'mrs' and args[1] in ('cntps_cval_el1', 'spsr_el3', 'elr_el1', 'spsr_el1', 'far_el1'):
                self.put(core, args[0], r.get(args[1], 0)); u.reg_write(UC_ARM64_REG_PC, address+4); return
        helper = self.survey_hooks['identity_deadline_el1']
        if helper <= address < self.survey_hooks['identity_deadline_finish']:
            assert core == 0 and r['currentel'] == 4 and r['daif'] == 0x3c0
            if op == 'msr':
                assert self.reg(core, args[1]) == 0; r[args[0]] = 0
                u.reg_write(UC_ARM64_REG_PC, address+4); return
            if op == 'dc':
                line = self.reg(core, args[1])
                assert line == 0x41200000+64*self.helper_lines
                self.helper_lines += 1
                u.reg_write(UC_ARM64_REG_PC, address+4); return
            if op == 'smc':
                assert args == ['#0x736'] and self.helper_lines == 9280
                self.exception(core, 3, address+4, (0x17 << 26) | (1 << 25) | 0x736); return
        if op == 'smc' and args != ['#0x733']:
            if core:
                # A secondary's deadline exit: el1_return_checkpoint refuses it
                # and the core parks in EL3 with interrupts masked.
                assert self.clock >= self.deadline and self.fault in ('stalled-core', 'stalled-no-tick')
                self.target_parked.add(core); self.active[core] = False
                u.emu_stop(); return
            self.exception(core, 3, address+4, (0x17 << 26) | (1 << 25) | int(args[0][1:], 0)); return
        if core == 0 and self.fault == 'stalled-no-tick' and op == 'msr' and args[0] == 'cntp_ctl_el0':
            u.reg_write(UC_ARM64_REG_PC, address+4); return
        if op == 'msr' and args[0] == 'rmr_el3' and self.q(RECORD) == RECORD_MAGIC:
            assert core == 0 and self.reg(core, args[1]) == 2
            self.returned = True; u.emu_stop(); return
        return super().instruction(u, address, size, core)

    def execute(self):
        for quantum in range(200000):
            for index in range(8):
                core = (index+quantum) % 8
                if not self.active[core]:
                    continue
                u = self.cpus[core]
                u.emu_start(u.reg_read(UC_ARM64_REG_PC), 0, count=(211, 347, 509, 673, 887)[(quantum+core) % 5])
                if self.returned or self.retained:
                    return
            if self.clock_step == workload.COUNTER_STEP:
                # Until a stall case advances the clock in large steps.
                self.check_tick_lag()
            self.clock += self.clock_step
        raise AssertionError(('Return-deadline instruction budget', self.clock,
                              [hex(u.reg_read(UC_ARM64_REG_PC)) for u in self.cpus]))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'bundle', 'output'):
        p.add_argument('--'+name, type=Path, required=True)
    p.add_argument('--prefix', type=Path, help='Primary-return snapshot (success case)')
    p.add_argument('--resume-workload', type=Path, help='Completed-worker checkpoint of this bundle (stalled-core case)')
    p.add_argument('--case', choices=('success', 'stalled-core', 'stalled-no-tick', *RWP_TARGET), required=True)
    a = p.parse_args()
    manifest = json.loads((a.bundle/'manifest.json').read_text())
    assert manifest['schema'] == 'a733-all-core-task-workload-probe-v1' and 'workload_bisect' in manifest
    machine = Bisect(a.image, a.bundle)
    machine.local_manifest = manifest
    machine.overlay = (a.bundle/'overlay.bin').read_bytes()
    machine.native_hash, machine.bundle_hash = digest(a.image), digest(a.bundle/'manifest.json')
    if a.case == 'success':
        meta = json.loads(a.prefix.with_suffix('.json').read_text())
        assert digest(a.prefix) == meta['snapshot_sha256']
        assert all(digest(Path(path)) == sha for path, sha in meta['inputs'].items())
        prior = json.loads(next(Path(path).read_text() for path in meta['inputs'] if path.endswith('/manifest.json')))
        assert prior['primary_manifest'] == manifest['primary_manifest']
        primary = Path(prior['primary_manifest']['path'])
        checkpoint = json.loads(primary.read_text())['hooks']['primary_publication_return']-0x41020000
        assert machine.overlay[:checkpoint] == (primary.parent/'overlay.bin').read_bytes()[:checkpoint]
        machine.restore(pickle.loads(a.prefix.read_bytes()), None)
        machine.last_announced_rounds = (0,)*8
        machine.checkpoint = a.output.parent/'workload-complete.pickle'
    else:
        saved = json.loads(a.resume_workload.with_suffix('.json').read_text())
        assert saved['snapshot_sha256'] == digest(a.resume_workload)
        assert saved['native_sha256'] == machine.native_hash and saved['manifest_sha256'] == machine.bundle_hash
        assert all(digest(ROOT/name) == value for name, value in saved['sources'].items())
        base.Machine.restore(machine, a.resume_workload)
        assert machine.completed_workers == set(range(8)) and not machine.stopped_writers
        assert machine.armed == [machine.deadline+DEADLINE_TICKS] and not machine.fiq_taken
        machine.checkpoint = None
        machine.fault = a.case
        machine.rwp_injected, machine.rwp_reads, machine.rwp_seen = False, 0, 0
        machine.cpu0_parked = False
        assert all(machine.q(COMPLETIONS+c*64) != machine.epoch for c in range(8))
        if a.case.startswith('stalled'):
            machine.stalled = STALLED_CORE
            machine.active[machine.stalled] = False
            machine.clock_step = 20000
        if a.case == 'stalled-no-tick':
            machine.regs[0]['cntp_ctl_el0'] = 0
    try:
        machine.execute()
        raw = machine.read(0x42018000, 0x5200)
        record = struct.unpack_from('<64Q', raw, RECORD-0x42018000)
        assert machine.returned and not machine.retained
        if a.case in RWP_TARGET:
            assert machine.rwp_injected and machine.rwp_seen == 1
        if a.case in ('restore-rwp-secondary-unfixed', 'restore-rwp-late-secondary-unfixed',
                      'restore-pwrr-secondary-unfixed'):
            local = struct.unpack_from('<37Q', raw, 0xa00+3*512)  # Words 17-26 before, 27-36 after.
            assert record[:4] == (RECORD_MAGIC, 1, 3, 1) and record[62] == 1, record[:4]
            if 'pwrr' in a.case:
                assert local[11] == 6 and local[36] ^ local[26] == PWRR[1] and local[17:26] == local[27:36], local
            else:
                assert local[11] == 6 and local[27] == 8 and local[17] == 0, local
            # CPU0 may have powered off earlier secondaries before core 3 parked.
            assert 3 in machine.target_parked and 3 not in machine.stopped
            results = [struct.unpack_from('<5Q', machine.read(0x41201080+c*128, 40)) for c in range(8)]
        elif a.case in ('restore-rwp-unfixed', 'restore-rwp-late-unfixed', 'restore-pwrr-unfixed'):
            primary_record = struct.unpack_from('<34Q', raw, 0xa00)  # Words 10-19 before, 20-29 after.
            assert record[:4] == (RECORD_MAGIC, 1, 3, 1) and record[62] == 1, record[:4]
            if 'pwrr' in a.case:
                assert primary_record[30] == 3 and primary_record[29] ^ primary_record[19] == PWRR[1], primary_record
                assert primary_record[10:19] == primary_record[20:29], primary_record
            else:
                assert primary_record[30] == 3 and primary_record[20] == 8, primary_record
            assert machine.started == list(range(1, 8)) and not machine.stopped
            results = [struct.unpack_from('<5Q', machine.read(0x41201080+c*128, 40)) for c in range(8)]
        elif a.case in ('success', 'restore-rwp', 'restore-rwp-secondary',
                        'restore-rwp-late', 'restore-rwp-late-secondary',
                        'restore-pwrr', 'restore-pwrr-secondary'):
            assert machine.armed == [machine.deadline+DEADLINE_TICKS] and machine.disarmed_at_completion
            # The completed survey copies CPU0's EL1 record page over this area.
            assert not machine.fiq_taken and record[0] != RECORD_MAGIC
            assert machine.started == machine.stopped == list(range(1, 8))
            assert machine.clean_complete == machine.stopped_writers == set(range(8))
            assert not any(machine.pending) and all(irq is None for irq in machine.irq_active)
            assert machine.regs[0]['cntps_ctl_el1'] == 0 and machine.regs[0]['icc_igrpen0_el1'] == 0
            for address, value in machine.original_mmio.items():
                if 0x03400000 <= address < 0x03600000 and address & 0xffff not in (0x180, 0x280, 0x380):
                    assert machine.mmio[address] == value, ('Unrestored GIC', hex(address))
            results = [struct.unpack_from('<5Q', raw, 0x4280+c*128) for c in range(8)]
            for rounds, background, sgis, failed, ticks in results:
                assert rounds == 32 and background > 0 and sgis > 0 and failed == 0
            assert results[0][4] >= 32
        else:
            if a.case == 'stalled-no-tick' or machine.cpu0_parked:
                # CPU0 took no interrupt after the deadline: without a tick, or
                # waiting in identity_park with interrupts masked because the
                # stalled core had already arrived. The deadline FIQ returns it.
                assert len(machine.fiq_taken) == 1 and machine.fiq_taken[0] >= machine.deadline+DEADLINE_TICKS
                assert record[:4] == (RECORD_MAGIC, 1, 1, 3) and record[19] == 29, record[:4]
            else:
                assert not machine.fiq_taken and record[:4] == (RECORD_MAGIC, 1, 2, 3), record[:4]
                assert record[8] == 0x5e000000 and record[9] == 3 and record[10] in (0, 30) and record[19] == 1023
            assert record[62] == 1 and record[5] == machine.deadline+DEADLINE_TICKS
            assert record[25] == 9280 and machine.helper_lines == 9280
            assert record[4] <= record[26]
            assert not machine.stopped and not machine.active[machine.stalled]
            assert all(record[29+4*c+3] == 8 for c in range(1, 8)), 'Secondary power status differs'
            assert machine.q(COMPLETIONS+machine.stalled*64) != machine.epoch
            results = [struct.unpack_from('<5Q', machine.read(0x41201080+c*128, 40)) for c in range(8)]
    except BaseException:
        a.output.with_suffix('.failure.json').write_text(json.dumps({
            'clock': machine.clock, 'completed_workers': sorted(machine.completed_workers),
            'stopped_writers': sorted(machine.stopped_writers), 'fiq_taken': machine.fiq_taken,
            'pcs': [hex(u.reg_read(UC_ARM64_REG_PC)) for u in machine.cpus],
            'traces': [list(trace) for trace in machine.recent], 'registers': machine.regs}, indent=2, default=str)+'\n')
        raise
    report = a.output.with_suffix('.bin'); report.write_bytes(raw)
    a.output.write_text(json.dumps({
        'pass': True, 'case': a.case, 'physical_access': False, 'profile': manifest['workload_bisect']['profile'],
        'native_sha256': machine.native_hash, 'manifest_sha256': machine.bundle_hash,
        'report_sha256': digest(report), 'sources': {name: digest(ROOT/name) for name in SOURCES},
        'workload_checkpoint': str(a.resume_workload) if a.resume_workload else None,
        'deadline': machine.deadline+DEADLINE_TICKS, 'fiq_taken': machine.fiq_taken,
        'stalled_core': getattr(machine, 'stalled', None), 'cpu0_parked': machine.cpu0_parked, 'injected_reads': machine.rwp_seen, 'parked_secondaries': sorted(getattr(machine, 'target_parked', ())),
        'workload_results': results, 'off': machine.stopped,
        'clean_complete': sorted(machine.clean_complete), 'helper_lines': machine.helper_lines,
        'limits': __doc__}, indent=2)+'\n')
    print('PASS return-deadline workload:', a.case, flush=True)


if __name__ == '__main__':
    main()
