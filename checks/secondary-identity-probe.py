#!/usr/bin/env python3
"""Execute the sequential survey using native power/entry code and modeled I/O."""
import argparse
import copy
import ctypes
import hashlib
import json
import pickle
from pathlib import Path
import struct
import importlib.util
from unicorn import UC_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_PC

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('primary_check', ROOT/'checks/primary-publication-probe.py')
primary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(primary)
model = primary.publication.model
REPORT, SLOTS = 0x42018100, 0x42018200
# The top-operating-point relock's read-only entry words with port 3-3's
# recorded values (bundles 14 and 15): the oscillator, the SID word, the
# S_TWI0 gate and reset, PL0 and PL1, APBS0 and APBS1, and S_TWI0 idle in the
# PMIC provider's baseline with a 216 kHz divider. The AXP8191 registers hold
# the values that bundle 25 leaves.
TWI = 0x07083000
TOP_OPP_WORDS = {0x07090160: 0x983fb0f7, 0x0300624c: 0x496, 0x0701019c: 0x10001, 0x07025000: 0x22,
                 0x0701000c: 0, 0x07010010: 0, TWI+0x08: 0, TWI+0x0c: 0x40, TWI+0x10: 0xf8, TWI+0x14: 0x29,
                 TWI+0x18: 0, TWI+0x1c: 0, TWI+0x20: 0x30, TWI+0x200: 0, TWI+0x210: 0xc0, TWI+0x214: 0,
                 TWI+0x218: 0, TWI+0x21c: 0, TWI+0x314: 0, TWI+0x318: 0, TWI+0x31c: 0}
TOP_OPP_PMIC = {0xff: 0, 0x0e: 3, 0x0f: 0, 0x10: 0xff, 0x14: 0xb7, 0x16: 0xb2}
TOP_OPP_PMIC_READS = [0xff, 0x0e, 0x0f, 0x10, 0x14, 0x16, 0xff]


class Survey(primary.Probe):
    def __init__(self, image, bundle):
        manifest = json.loads((bundle/'manifest.json').read_text())
        base = Path(manifest['primary_manifest']['path'])
        assert primary.probe.digest(base) == manifest['primary_manifest']['sha256']
        super().__init__(image, base.parent)
        assert manifest['native_sha256'] == primary.probe.digest(image)
        assert all(primary.probe.digest(ROOT/n) == h for n, h in manifest['sources'].items())
        self.survey_hooks = manifest['hooks']
        for payload in manifest['payloads'] + [dict(manifest['overlay'], name='overlay.bin')]:
            data = (bundle/payload['name']).read_bytes()
            assert hashlib.sha256(data).hexdigest() == payload['sha256']
            ctypes.memmove(self.ram_pointer+payload['address']-0x41000000, data, len(data))
        self.in_survey = False
        self.boundary = False
        self.fault = None
        self.started = []
        self.stopped = []
        self.parked = []
        self.shutdown_steps = [[] for _ in range(8)]
        for c in range(1, 8):
            self.regs[c].update(s3_0_c15_c2_7=0x100, sctlr_el1=0x30d00800+(c<<16),
                                tcr_el1=(c<<32)|0x123400, s3_0_c15_c3_1=0x41,
                                s3_0_c15_c3_0=0x073fc237)
            # Nonzero, distinct original words detect partial or cross-core restoration.
            self.mmio[0x08001000+c*0x1000] = 0xa0+c*2
            self.mmio[0x08001004+c*0x1000] = 0x81200000+c*0x1000
            self.mmio[0x08001008+c*0x1000] = c
            self.mmio[0x07050200+c*4] = 0x20+c*4
            self.mmio[0x07050220+c*4] = 0x40+c*2
        # Top-operating-point profile: CPU PLL, pattern and mux words with port
        # 3-3's recorded boot values (docs/designs/cpu-top-opp.md).
        self.top_opp = manifest.get('top_opp')
        self.init_top_opp()
        self.original_mmio = copy.deepcopy(self.mmio)
        self.epoch = self.q(0x42004000+24)
        self.deadline = self.q(0x42004000+88)

    def init_top_opp(self):
        """Board 3-3's boot words of the three CPU clock domains; called again
        after a prefix restore replaces the modeled MMIO."""
        self.pll_state, self.pll_order, self.pll_written, self.pll_lowered = {}, [], False, False
        self.twi = dict(stage='idle', pending=None, countdown=0, stopping=0, data=0, reg=None, regs=[])
        if not self.top_opp:
            return
        self.mmio.update(TOP_OPP_WORDS)
        for d, (pll, mux) in {1: (0xf8802600, 0x03010000), 2: (0xf8802700, 0x03000000),
                              3: (0xf8802600, 0x03010105)}.items():
            base = 0x08870000+0x1000*d
            self.mmio.update({base: pll, base+4: 0, base+8: 0x30000000, base+0x18: 0x40000001, base+0x1c: mux})
            self.pll_state[d] = dict(update_reads=0, lock_reads=0, run=0, last=None, third=None, park=None)

    def top_opp_address(self, address):
        return bool(self.top_opp) and (0x08871000 <= address < 0x08874000 or address in TOP_OPP_WORDS)

    def twi_access(self, u, access, address, size, value, core):
        """One entry-check access of the relock outside the CPU PLLs. The model
        is the AXP8191 behind S_TWI0 as the PMIC provider drives it: each bus
        event sets INT_FLAG at the second control read after it starts, with
        its status; a STOP clears M_STP at the second read. It requires CPU0
        before any secondary, only the data and control words written, each
        event started after the previous one completed, the provider's phase
        order, and every read finished before the first clock write."""
        assert self.in_survey and core == 0 and not self.started and size == 4
        t, control = self.twi, TWI+0x0c
        if access == UC_MEM_WRITE:
            assert address in (TWI+0x08, control) and not self.pll_written, ('relock entry write', hex(address))
            if address == TWI+0x08:
                t['data'] = value & 255
                return
            assert t['pending'] is None and not t['stopping'], 'bus event started before the last completed'
            stage = t['stage']
            if value == 0x58:
                assert stage == 'received', 'STOP outside a completed read'
                self.mmio[control] = 0x50
                t.update(stage='idle', stopping=2)
                return
            if value == 0x68:
                assert stage in ('idle', 'register') and (stage != 'idle' or self.mmio[TWI+0x10] == 0xf8)
                status, stage = (0x08, 'start') if stage == 'idle' else (0x10, 'restart')
                self.mmio[control] = 0x60
            elif value == 0x48:
                if stage == 'start':
                    assert t['data'] == 0x6c, 'write address'
                    status, stage = 0x18, 'address'
                elif stage == 'address':
                    t['reg'] = t['data']
                    status, stage = 0x28, 'register'
                elif stage == 'restart':
                    assert t['data'] == 0x6d, 'read address'
                    status, stage = 0x40, 'read'
                else:
                    assert stage == 'read', ('data phase out of order', stage)
                    self.mmio[TWI+0x08] = TOP_OPP_PMIC[t['reg']]
                    t['regs'].append(t['reg'])
                    status, stage = 0x58, 'received'
                self.mmio[control] = 0x40
            else:
                raise AssertionError(('S_TWI0 control word', hex(value)))
            t.update(stage=stage, pending=status, countdown=2)
            return
        if address == control:
            if t['pending'] is not None:
                t['countdown'] -= 1
                if not t['countdown']:
                    self.mmio[control] = (self.mmio[control] & ~0x20) | 8
                    self.mmio[TWI+0x10], t['pending'] = t['pending'], None
            elif t['stopping']:
                t['stopping'] -= 1
                if not t['stopping']:
                    self.mmio[control], self.mmio[TWI+0x10] = 0x40, 0xf8
        u.mem_write(address, int(self.mmio[address]).to_bytes(4, 'little'))

    def pll_access(self, access, address, size, value, core):
        """One CPU PLL access of the top-operating-point relock. The update bit
        clears at the second read after it is set and lock sets at the second
        read after that. The model requires the CPU clock driver's sequence:
        CPU0 at EL3 before any secondary starts; a PLL write only while its mux
        is parked and 20 us after the park; the PLL selected again only after
        three lock reads at least 3 us apart and 20 us after the third, with
        P = 0 written first; and the domains in the order CPU-B, CPU-L, DSU."""
        d, offset = (address-0x08870000) >> 12, address & 0xfff
        if self.started:
            # The completed path's CPU-B lowering (top_opp_lower_b): CPU0, CPU-B
            # only, and only once every started secondary is observed OFF.
            assert core == 0 and size == 4 and d == 2 and sorted(self.stopped) == sorted(self.started) and \
                not any(self.mmio[0x07052008+c*0x1000] & 15 for c in self.started), \
                ('CPU PLL access while a secondary runs', hex(address))
            self.pll_lowered = True
        else:
            assert self.in_survey and core == 0 and size == 4 and d in (1, 2, 3)
        assert offset in (0, 4, 8, 0x18, 0x1c), ('unexpected CPU PLL word', hex(address))
        pll, sel, st = 0x08870000+0x1000*d, 0x08870000+0x1000*d+0x1c, self.pll_state[d]
        parked = not (self.mmio[sel] >> 24) & 7
        if access != UC_MEM_WRITE:
            if offset == 0 and parked:
                word = self.mmio[pll]
                if word & (1 << 26):
                    st['update_reads'] += 1
                    if st['update_reads'] >= 2:
                        self.mmio[pll] = word & ~(1 << 26)
                elif not word & (1 << 28):
                    st['lock_reads'] += 1
                    if st['lock_reads'] >= 2:
                        self.mmio[pll] = word | (1 << 28)
                word = self.mmio[pll]
                if not word & (1 << 26):
                    if not word & (1 << 28):
                        st['run'] = 0
                    elif not st['run'] or self.clock >= st['last']+72:
                        st['last'] = self.clock
                        st['run'] += 1
                        if st['run'] == 3:
                            st['third'] = self.clock
            return
        assert offset in (0, 0x1c), ('CPU PLL write outside PLL and mux', hex(address))
        assert self.twi['regs'] == TOP_OPP_PMIC_READS and self.twi['stage'] == 'idle' and not self.twi['stopping'], \
            'clock write before the relock read both supplies'
        self.pll_written = True
        if offset == 0:
            assert parked and self.clock >= st['park']+480, 'PLL written while selected or within 20 us of the park'
            self.mmio[pll] = value & ~(1 << 28)
            st.update(update_reads=0, lock_reads=0, run=0)
            return
        was, now = (self.mmio[sel] >> 24) & 7, (value >> 24) & 7
        if was and not now:
            st['park'] = self.clock
            self.pll_order.append(d)
        if now == 3 and not was:
            assert st['run'] >= 3 and self.clock >= st['third']+480, 'PLL selected before three lock reads and 20 us'
            assert not (value >> 16) & 3 and not (self.mmio[sel] >> 16) & 3, 'P written with the select'

    def memory(self, u, access, address, size, value, core):
        if self.top_opp_address(address):
            if not 0x08871000 <= address < 0x08874000:
                return self.twi_access(u, access, address, size, value, core)
            self.pll_access(access, address, size, value, core)
            return model.Machine.memory(self, u, access, address, size, value, core)
        if not self.in_survey:
            return primary.Probe.memory(self, u, access, address, size, value, core)
        if address < 0x41000000:
            assert not 0x03400000 <= address < 0x03600000, 'Unexpected GIC access'
            allowed = {0x08001004, 0x08001008}
            for c in range(2, 8):
                allowed.update((0x08001000+c*0x1000, 0x08001004+c*0x1000,
                                0x08001008+c*0x1000, 0x07050200+c*4,
                                0x07050220+c*4, 0x07052008+c*0x1000))
            assert address in allowed and size == 4, ('Unexpected device access', core, hex(address))
            if access == UC_MEM_WRITE:
                self.mmio_writes.append((address, size, value))
                if 0x07050208 <= address <= 0x0705021c and value & 1:
                    target = (address-0x07050200)//4
                    assert core == 0 and not any(self.active[1:]), 'Overlapping secondary lifetimes'
                    assert target == len(self.started)+2
                    assert self.mmio[0x08001004+target*0x1000] == self.symbols['a733_cpu_secondary_entry']
                    assert self.mmio[0x08001008+target*0x1000] == 0
                    self.started.append(target)
                if core and address == 0x07050200+core*4:
                    assert self.shutdown_steps[core] == ['power', 'dsb', 'isb']
                    self.shutdown_steps[core].append('control')
                if core == 0 and address in (0x08001000+c*0x1000 for c in self.started):
                    target = (address-0x08001000)//0x1000
                    if value == self.original_mmio[address]:
                        assert target in self.stopped and self.q(SLOTS+target*256+136) == self.epoch
                        if self.fault == 'restore-readback' and target == 2:
                            value ^= 2
        return model.Machine.memory(self, u, access, address, size, value, core)

    def instruction(self, u, address, size, core):
        if address == self.survey_hooks['identity_primary_gate'] and not self.in_survey:
            assert core == 0 and self.maintained == self.expected and not any(self.active[1:])
            primary.probe.decode(self.read(primary.probe.REPORT[0], primary.probe.REPORT[1]), self.nonce)
            self.in_survey = True
            self.boundary = True
            u.emu_stop()
            return
        if not self.in_survey:
            return primary.Probe.instruction(self, u, address, size, core)
        if address == self.survey_hooks['identity_park']:
            self.parked.append(core)
            self.retained = True
            u.emu_stop()
            return
        assert address != self.symbols['a733_smp_stop'], 'Unexpected native stop'
        if address not in self.decoded:
            self.decoded[address] = next(self.decoder.disasm(bytes(u.mem_read(address, 4)), address))
        ins = self.decoded[address]
        op, args = ins.mnemonic, ins.op_str.split(', ')
        if op == 'msr':
            name, src = args
            if name == 'rmr_el3':
                assert core == 0 and self.reg(core, src) == 2 and not any(self.active[1:])
                self.returned = True
                u.emu_stop()
                return
            assert name not in ('sctlr_el3', 'sctlr_el1', 'tcr_el1', 'tcr_el3'), 'Unexpected cache/translation transition'
            assert not name.startswith('icc_'), 'Unexpected interrupt interface access'
            if name == 's3_0_c15_c2_7':
                assert core and self.reg(core, src) == 0x101
                self.shutdown_steps[core].append('power')
        if core and op == 'dsb' and self.shutdown_steps[core] == ['power']:
            self.shutdown_steps[core].append('dsb')
        if core and op == 'isb' and self.shutdown_steps[core] == ['power', 'dsb']:
            self.shutdown_steps[core].append('isb')
        if core and op == 'wfi' and address == self.survey_hooks['identity_target_terminal']+4:
            assert self.shutdown_steps[core] == ['power', 'dsb', 'isb', 'control']
            assert self.q(SLOTS+core*256+128) == self.q(SLOTS+core*256+136) == self.epoch
            assert self.mmio[0x07050200+core*4] == self.original_mmio[0x07050200+core*4] | 2
            assert self.mmio[0x07050220+core*4] == self.original_mmio[0x07050220+core*4]
            if self.fault == 'ack-without-off':
                self.clock = self.deadline+1
            else:
                self.mmio[0x07052008+core*0x1000] = 0
                self.active[core] = False
                self.stopped.append(core)
            u.emu_stop()
            return
        return model.Machine.instruction(self, u, address, size, core)

    def snapshot(self):
        return (bytes(self.ram), [u.context_save() for u in self.cpus],
                copy.deepcopy({n:getattr(self,n) for n in ('regs','mmio','clock','active','versions','exclusive')}))

    def restore(self, snapshot, fault):
        ram, contexts, state = snapshot
        ctypes.memmove(self.ram_pointer, ram, len(ram))
        for u, context in zip(self.cpus, contexts): u.context_restore(context)
        for n, value in copy.deepcopy(state).items(): setattr(self,n,value)
        self.fault = fault
        self.started, self.stopped, self.parked, self.mmio_writes = [], [], [], []
        self.shutdown_steps = [[] for _ in range(8)]
        self.returned = self.retained = False

    def execute(self):
        for tick in range(20000):
            for core in range(8):
                if self.active[core]:
                    u = self.cpus[core]
                    u.emu_start(u.reg_read(UC_ARM64_REG_PC), 0, count=500)
                    if self.returned or self.retained: return
            self.clock += 1000
        raise AssertionError(('Survey did not terminate', self.started, self.stopped,
                              [hex(u.reg_read(UC_ARM64_REG_PC)) for u in self.cpus]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'bundle', 'output'): parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--resume', action='store_true', help='Reuse the preserved actual-boot prefix')
    parser.add_argument('--case', choices=('success','ack-without-off','restore-readback'), action='append')
    args = parser.parse_args()
    machine = Survey(args.image, args.bundle)
    saved = args.output.parent/'primary-prefix.pickle'
    metadata = saved.with_suffix('.json')
    inputs = {str(p):primary.probe.digest(p) for p in (
        args.image,args.bundle/'manifest.json',ROOT/'checks/primary-publication-probe.py',
        ROOT/'checks/a733-supervised-publication.py',ROOT/'checks/a733-smp-cached.py')}
    if args.resume:
        record = json.loads(metadata.read_text())
        assert record['inputs'] == inputs and record['snapshot_sha256'] == primary.probe.digest(saved)
        snapshot = pickle.loads(saved.read_bytes())  # Trusted local snapshot, hash checked above.
        machine.in_survey = machine.boundary = True
    else:
        assert not saved.exists(), 'Preserved prefix exists; use --resume'
        machine.cpus[0].emu_start(machine.symbols['a733_smp_entry'], 0, count=3000000)
        assert machine.boundary and not machine.returned and not machine.retained
        snapshot = machine.snapshot()
        saved.write_bytes(pickle.dumps(snapshot))
        metadata.write_text(json.dumps({'inputs':inputs,'snapshot_sha256':primary.probe.digest(saved),
            'check_sha256':primary.probe.digest(Path(__file__))},indent=2)+'\n')
    results = []
    for case in args.case or ('success','ack-without-off','restore-readback'):
        fault = None if case == 'success' else case
        machine.restore(snapshot, fault)
        machine.execute()
        header = struct.unpack('<32Q', machine.read(REPORT, 256))
        if fault is None:
            assert machine.returned and not machine.retained
            assert machine.started == machine.stopped == list(range(2, 8))
            assert header[:8] == (0x4137334944454e31,1,0x900,machine.epoch,machine.deadline,0xfc,2,7)
            for c in range(2, 8):
                slot = struct.unpack('<32Q', machine.read(SLOTS+c*256, 256))
                assert slot[:4] == (machine.epoch,c,machine.regs[c]['midr_el1'],machine.regs[c]['mpidr_el1'])
                assert slot[4:7] == tuple(machine.regs[c][n] for n in ('sctlr_el3','sctlr_el1','tcr_el1'))
                assert slot[11:13] == (0x100,0x101)
                assert slot[15:18] == (1,machine.epoch,machine.epoch) and slot[23:27] == (0,1,2,2)
                for address in (0x08001000+c*0x1000,0x08001004+c*0x1000,
                                0x08001008+c*0x1000,0x07050200+c*4,0x07050220+c*4):
                    assert machine.mmio[address] == machine.original_mmio[address]
        else:
            assert machine.retained and not machine.returned and machine.parked == [0]
            assert machine.started == [2] and header[5] == 0 and header[6] == 1
            assert machine.q(SLOTS+2*256+192) == 0
            assert not any(a in (0x08001004,0x08001008) for a,_,_ in machine.mmio_writes)
            if fault == 'ack-without-off':
                assert machine.stopped == [] and machine.active[2]
                assert machine.mmio[0x08003004] == machine.symbols['a733_cpu_secondary_entry']
            else: assert machine.stopped == [2]
        results.append({'case':fault or 'sequential-six-core-entry-off-restore-return', 'pass':True,
                        'started':machine.started, 'off':machine.stopped,
                        'returned':machine.returned, 'retained':machine.retained})
        (args.output.parent/('case-'+case+'.json')).write_text(json.dumps(
            dict(results[-1],check_sha256=primary.probe.digest(Path(__file__))),indent=2)+'\n')
    output={'native_sha256':primary.probe.digest(args.image),
        'manifest_sha256':primary.probe.digest(args.bundle/'manifest.json'), 'physical_access':False,
        'primary_prefix_reused':args.resume,'sources':{n:primary.probe.digest(ROOT/n) for n in (
            'checks/secondary-identity-probe.py','checks/primary-publication-probe.py',
            'checks/a733-supervised-publication.py','checks/a733-smp-cached.py')},'cases':results,
        'limits':'Actual native instructions with modeled CPU registers, power controller and shared RAM. No physical entry, OFF, cache visibility or ROM-return qualification.'}
    args.output.write_text(json.dumps(output,indent=2)+'\n')
    print('PASS six-core sequential entry/OFF/restoration and two retained failures; primary prefix executed once')


if __name__ == '__main__': main()
