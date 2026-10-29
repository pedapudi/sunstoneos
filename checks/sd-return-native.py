#!/usr/bin/env python3
"""Actual SD task/close and linked private monitor guard; modeled hardware only."""
import argparse
import binascii
import copy
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'checks'))
import harness
import importlib.util
spec = importlib.util.spec_from_file_location('sd_return_previous', ROOT / 'checks/storage-read-returning.py')
old = importlib.util.module_from_spec(spec); spec.loader.exec_module(old)
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC

RECEIPT = 0x42101000


class Machine(old.Card):
    def __init__(self, image):
        super().__init__(image, 2)
        self.monitor = False; self.terminal = None; self.release = []; self.timer_writes = []
        self.bus_closed = False
        self.interrupt_dma = False; self.interrupted_at = None
        self.bad_receipt_mapping = False
        self.stuck_close = False; self.close_attempts = 0
        self.regs.update({0x0701019c: 0x10001, 0x07025000: 0x22, 0x07025014: 0x11,
                          0x07025024: 5, 0x0708300c: 0x40, 0x07083010: 0xf8, 0x07083020: 0x30})
        self.hooks.update({image.address(n): n for n in ('monitor_init', 'monitor_exception',
                          'a733_pins_release_banked', 'a733_rail_release', 'a733_mmc_platform_close')})
        self.esr = 0x17 << 26

    def code(self, cpu, a, size, unused):
        if self.interrupt_dma and not self.monitor and self.regs.get(0x04020080, 0)&0x80:
            self.monitor = True; self.interrupted_at = len(self.trace)
            for i, v in enumerate((0, 0, 3)): cpu.reg_write(UC_ARM64_REG_X0+i, v)
            cpu.reg_write(UC_ARM64_REG_PC, self.image.address('__wrap_monitor_exception')); return
        instruction = int.from_bytes(cpu.mem_read(a, 4), 'little')
        if self.monitor:
            special = instruction & ~31
            values = {0xd5384240: 12, 0xd53e1000: 0, 0xd53e5200: self.esr,
                      0xd53be020: (self.time+1)*24}
            if special in values:
                self.time += special == 0xd53be020
                cpu.reg_write(UC_ARM64_REG_X0+(instruction & 31), values[special])
                cpu.reg_write(UC_ARM64_REG_PC, a+4); return
            if instruction == 0xd503205f:  # actual retained loop's WFE
                self.terminal = 'park'; self.finished = True; cpu.emu_stop(); return
            if instruction & 0xfff00000 == 0xd5100000:  # modeled EL3 timer/DAIF writes
                self.timer_writes.append(instruction); cpu.reg_write(UC_ARM64_REG_PC, a+4); return
        name = self.hooks.get(a)
        args = [cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(6)]
        if name == 'a733_mmc_platform_close':
            self.close_attempts += 1
            if self.stuck_close: self.write(args[0]+self.image.layouts['a733_mmc_platform']['busy'], 1)
            return  # observe/inject serialization; execute the actual API
        if name == 'aarch64_device_translate' and self.bad_receipt_mapping and args[0] == RECEIPT:
            cpu.mem_write(args[2], struct.pack('<Q2I', args[0], 0xff, 3)); self.ret(1); return
        if name == 'monitor_init': self.ret(); return  # unchanged qualified body, not replayed
        if name == 'monitor_exception':
            self.terminal = 'qualified-monitor'; self.exit = args[1]; self.finished = True; cpu.emu_stop(); return
        if name == 'platform_exit':
            self.monitor = True
            for i, v in enumerate((0x733, args[0], 1)): cpu.reg_write(UC_ARM64_REG_X0+i, v)
            cpu.reg_write(UC_ARM64_REG_PC, self.image.address('__wrap_monitor_exception')); return
        if name == 'vTaskDelay':
            self.time += 1000
            if self.field('pmic_stop'): self.write(self.base+self.layout['pmic_parked'], 1)
            self.ret(); return
        if name == 'a733_pins_release_banked':
            assert args[2] == 1 and not self.regs.get(0x02002d0c, 0)&0x10001
            self.release.append('pins'); self.cpu.mem_write(args[1], bytes(72)); self.ret(0); return
        if name == 'a733_rail_release':
            assert self.release == ['pins']; self.release.append('rail'); self.ret(0); return
        super().code(cpu, a, size, unused)

    def read_mmio(self, cpu, access, a, size, value, unused):
        assert not (self.bus_closed and 0x04020000 <= a < 0x04021000), 'SDC read after bus disable'
        if self.monitor:
            assert size == 4
            self.write(a, self.regs.get(a, 0)); self.trace.append(['monitor-read', a, self.regs.get(a, 0)]); return
        super().read_mmio(cpu, access, a, size, value, unused)

    def write_mmio(self, cpu, access, a, size, value, unused):
        assert not self.monitor, ('unexpected monitor MMIO write', hex(a))
        assert not (self.bus_closed and 0x04020000 <= a < 0x04021000), 'SDC write after bus disable'
        if a == 0x02002d0c and value == 0x10000: self.bus_closed = True
        if a in (0x02002d00, 0x02002d0c) and not value & (1 << 31):
            self.trace.append(['write', a, value]); self.regs[a] = value; return
        super().write_mmio(cpu, access, a, size, value, unused)

    def start(self, plan=None):
        self.monitor = True; self.call('__wrap_monitor_init'); self.monitor = False
        assert self.read(RECEIPT+8, 8) == 1
        raw = old.host.input_bytes(2, 0x1234567, old.FIXTURE, plan or [(0, 8)], ids=(7, 9, 11))
        self.cpu.mem_write(0x43000000, raw)
        assert self.call('sdrt_setup') == 1 and self.call('sdrt_bind', 2) == 1 and self.call('sdrt_arm') == 1
        return raw


class SentinelMachine(Machine):
    def __init__(self, image, skip=False):
        super().__init__(image); self.skip = skip; self.prefill_seen = False; self.expected_payload = None

    def dma_cache(self, operation, buffer, size):
        if operation == 0:
            assert size == 65536 and bytes(self.cpu.mem_read(buffer, size)) == bytes([0xa5])*size
            self.prefill_seen = True
        else:
            # Model the known weak reference shape: only the first descriptor
            # has nonzero data. Suppress one descriptor's device publication
            # while keeping the existing modeled OWN/drain completion intact.
            self.expected_payload = self.dma['data'][:4096]+bytes(15*4096)
            published = bytearray(self.expected_payload)
            if self.skip:
                published[7*4096:8*4096] = self.cpu.mem_read(buffer+7*4096, 4096)
            self.dma['data'] = bytes(published)
        return super().dma_cache(operation, buffer, size)


def run(elf, out, inflight_only=False, admission_only=False, busy_only=False, chain_only=False, sentinel_only=False):
    out.mkdir(parents=True, exist_ok=False)
    image = harness.Image(elf, old.LAYOUTS)
    # Decode actual branches, not source-name assumptions: 16 vector slots and
    # monitor_entry must name wrappers; original bodies remain separately linked.
    dis = subprocess.check_output(['llvm-objdump-21', '-d', '--no-show-raw-insn', str(elf)], text=True)
    (out / 'link-disassembly.txt').write_text(dis)
    def word(address):
        return next(int.from_bytes(data[address-base:address-base+4], 'little')
                    for base, data in image.parts if base <= address < base+len(data)-3)
    for i in range(16):
        address = image.address('monitor_vectors')+128*i+4; instruction = word(address)
        assert instruction & 0xfc000000 == 0x14000000
        displacement = instruction & 0x3ffffff
        if displacement & (1 << 25): displacement -= 1 << 26
        assert address+4*displacement == image.address('__wrap_monitor_exception')
    entry = dis[dis.index('<monitor_entry>:'):dis.index('<monitor_vectors>:')]
    assert '<__wrap_monitor_init>' in entry
    results = {}

    def save(name, m, **extra):
        (out / (name+'.log')).write_text(m.log)
        results[name] = dict(terminal=m.terminal, receipt=m.read(RECEIPT+8, 8), **extra)
        (out / 'results.json').write_text(json.dumps(results, indent=2)+'\n')

    if sentinel_only:
        for name, skip in (('sentinel-chain', False), ('skipped-descriptor-write', True)):
            m = SentinelMachine(image, skip); raw = m.start([(0, 128)])
            m.log = ('SD_RETURN_VERSION=0x0000000000000001\n'
                     'SD_PROFILE=0x0000000000000002\nSD_GENERATION=0x0000000001234567\n')
            m.call('sd_task', 0)
            assert m.prefill_seen and m.terminal == 'qualified-monitor' and m.exit == 0
            assert m.read(RECEIPT+8, 8) == 5 and m.release == ['pins', 'rail']
            assert len(m.dma_reads) == 1 and len(m.dma_reads[0]['descriptors']) == 16
            buffer = image.address('buffer')
            assert m.cache_events == [('clean', buffer, 65536), ('invalidate', buffer, 65536)]
            value = old.host.decode_log(m.log, raw, 0)
            reference = dict(complete=True, status=0, card=copy.deepcopy(value['card']), reads=[
                dict(first=i*8, blocks=8, bytes=4096, crc32=binascii.crc32(m.expected_payload[i*4096:(i+1)*4096]))
                for i in range(16)])
            terminal = dict(magic=0x534b4552, stage=5, status=0)
            if skip:
                assert value['chunk_crc32'][7] == binascii.crc32(bytes([0xa5])*4096)
                rejection = harness.refused(lambda: old.host.validate_successful_return(
                    value, terminal, raw, reference), 'chain-read reference CRC mismatch')
            else:
                assert old.host.validate_successful_return(value, terminal, raw, reference)['reference_crc']['matched_bytes'] == 65536
                rejection = None
            assert all(value['chunk_crc32'][i] == reference['reads'][i]['crc32'] for i in range(16) if not skip or i != 7)
            (out/(name+'.input.bin')).write_bytes(raw)
            save(name, m, prefill_seen=m.prefill_seen, descriptors=16, cache_events=m.cache_events,
                 skipped_descriptor=7 if skip else None, strict_rejection=rejection, chunk_crc32=value['chunk_crc32'])
        return

    if chain_only:
        for name, blocks in (('nominal', 8), ('chain', 128)):
            m = Machine(image); raw = m.start([(0, blocks)])
            m.log = ('SD_RETURN_VERSION=0x0000000000000001\n'
                     'SD_PROFILE=0x0000000000000002\nSD_GENERATION=0x0000000001234567\n')
            m.call('sd_task', 0)
            assert m.terminal == 'qualified-monitor' and m.exit == 0 and m.read(RECEIPT+8, 8) == 5
            assert m.release == ['pins', 'rail'] and m.field('done') == 1 and not m.pf_writes
            assert len(m.dma_reads) == 1 and len(m.dma_reads[0]['descriptors']) == blocks // 8
            data = m.dma_reads[0]['data']; buffer = image.address('buffer')
            assert m.cache_events == [('clean', buffer, blocks*512), ('invalidate', buffer, blocks*512)]
            value = old.host.decode_log(m.log, raw, 0)
            assert value['complete'] and value['reads'][0]['crc32'] == binascii.crc32(data)
            if blocks == 128:
                assert value['chunk_crc32'] == [binascii.crc32(data[i:i+4096]) for i in range(0, 65536, 4096)]
            else: assert 'chunk_crc32' not in value
            command = next(i for i, t in enumerate(m.trace) if t[0] == 'write' and t[1] == 0x04020018 and t[2]&63 == 18)
            assert not any(t[0] == 'read' and t[1] == 0x04020200 for t in m.trace[command:])
            assert not any(t[0] == 'write' and t[1] == 0x04020018 and t[2] & (1 << 10) for t in m.trace)
            assert not m.regs.get(0x03400118, 0)&2 and not m.regs.get(0x0402008c, 0)
            (out / (name+'.input.bin')).write_bytes(raw)
            save(name, m, cache_events=m.cache_events, descriptors=len(m.dma_reads[0]['descriptors']),
                 read_crc=value['reads'][0]['crc32'], chunk_crc32=value.get('chunk_crc32'), close_release=m.release)
        return

    def inflight():
        n = Machine(image); n.start(); n.interrupt_dma = True
        for _ in range(20000):
            r = n.call('sdrt_step')
            if n.terminal: break
            assert r >= 0
            if not r: n.time += 1000
        else: raise AssertionError('DMA was never started')
        core = n.base+n.layout['platform']+image.layouts['a733_mmc_platform']['core']
        assert n.terminal == 'park' and n.interrupted_at is not None
        assert n.read(core+image.layouts['sunxi_mmc']['borrowed'], 8)
        assert not any(t[0] == 'write' for t in n.trace[n.interrupted_at:])
        save('exception-during-dma', n, borrowed=True)

    if inflight_only:
        inflight(); return
    def admission():
        for name in ('pre-task-refusal', 'uninitialized', 'bad-receipt-mapping', 'malformed-magic'):
            n = Machine(image); n.monitor = True
            if name != 'uninitialized': n.call('__wrap_monitor_init')
            if name == 'bad-receipt-mapping':
                n.monitor = False; n.bad_receipt_mapping = True
                assert n.call('sdrt_arm') == 0 and n.read(RECEIPT+8, 8) == 1
                n.monitor = True
            if name == 'malformed-magic': n.write(RECEIPT, 0, 8)
            n.call('__wrap_monitor_exception', 0x733, 1, 1)
            assert n.terminal == ('park' if name in ('uninitialized', 'malformed-magic') else 'qualified-monitor')
            save(name, n)
    if admission_only:
        admission(); return
    if busy_only:
        n = Machine(image); n.start(); n.stuck_close = True
        deadline = n.field('start', 8)+1750000
        n.call('sd_task', 0)
        assert n.terminal == 'park' and n.field('reason') == 10 and n.field('done') == 1
        assert n.close_attempts and deadline <= n.time < deadline+2000 and not n.release
        save('stuck-close', n, close_attempts=n.close_attempts, deadline=deadline, stopped_at=n.time)
        return

    m = Machine(image); raw = m.start(); m.log = 'SD_RETURN_VERSION=0x0000000000000001\n'
    m.call('sd_task', 0)
    save('nominal', m, status=m.exit, reason=m.field('reason'), release=m.release, trace_tail=m.trace[-30:])
    assert m.terminal == 'qualified-monitor' and m.exit == 0 and m.read(RECEIPT+8, 8) == 5
    assert m.release == ['pins', 'rail'] and m.field('done') == 1
    assert [e[0] for e in m.cache_events] == ['clean', 'invalidate']
    value = old.host.decode_log(m.log, raw, 0); assert value['complete']
    (out / 'nominal.input.bin').write_bytes(raw)
    save('nominal', m, close_release=m.release, read_crc=value['reads'][0]['crc32'])

    # Resume independent cases from the actual completed lifetime, before its
    # guard consumes READY. Only guard-owned state differs after this checkpoint.
    saved = image.address('saved', unit='apps/storage-read-returning/return.c')
    base_saved = bytes(m.cpu.mem_read(saved, 80)); regs = copy.deepcopy(m.regs)
    for name, kind, status, state, change in (
        ('timer-after-close', 2, 0, 4, None), ('exception-after-close', 3, 0, 4, None),
        ('armed-failure', 1, 1, 3, None), ('malformed-receipt', 1, 0, 99, None),
        ('duplicate', 1, 0, 5, None), ('changed-pf', 1, 0, 4, 0x02000300),
        ('pmic-not-idle', 1, 0, 4, 0x07083010)):
        m.cpu.mem_write(saved, base_saved); m.write(saved+76, 0)
        m.regs = copy.deepcopy(regs); m.write(RECEIPT+8, state, 8)
        if change is not None: m.regs[change] = m.regs.get(change, 0)^1
        m.terminal = None; m.timer_writes = []; m.call('__wrap_monitor_exception', 0x733, status, kind)
        assert m.terminal == 'park'
        if m.terminal == 'park': assert m.read(RECEIPT+8, 8) == 6 and len(m.timer_writes) >= 3
        save(name, m)
    n = Machine(image); n.start(); before = len(n.trace); n.monitor = True
    n.call('__wrap_monitor_exception', 0, 0, 2)
    assert n.terminal == 'park' and not any(t[0] == 'write' for t in n.trace[before:])
    save('timer-before-dma', n)
    admission()
    inflight()
    (out / 'result.json').write_text(json.dumps(dict(result='pass', elf_sha256=hashlib.sha256(elf.read_bytes()).hexdigest(),
        physical=False, cases=len(results), reused='qualified monitor body; arithmetic/transfer models',
        modeled='MMIO, card DMA, cache callbacks, PMIC and pin/rail services, task scheduling, EL3 registers and SMC dispatch'), indent=2)+'\n')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__); p.add_argument('--image', type=Path, required=True)
    p.add_argument('--inflight-only', action='store_true', help='only the in-flight interrupt injection')
    p.add_argument('--admission-only', action='store_true', help='only initialized refusal and receipt-mapping admission')
    p.add_argument('--busy-only', action='store_true', help='only the caller deadline under stuck serialization')
    p.add_argument('--chain-only', action='store_true', help='one sixteen-descriptor read and legacy first-read nominal')
    p.add_argument('--sentinel-only', action='store_true', help='native prefill, complete chain and skipped descriptor payload')
    p.add_argument('--output', type=Path, required=True); a = p.parse_args(); run(a.image, a.output, a.inflight_only, a.admission_only, a.busy_only, a.chain_only, a.sentinel_only)
