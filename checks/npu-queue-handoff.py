#!/usr/bin/env python3
"""Execute native NPU queue handoff races with modeled RTOS and MMIO boundaries.

Relink an existing NPU OS build into host-only ELFs. Production queue, service,
controller and IRQ adapter instructions execute in Unicorn. DWARF supplies every
structure offset, size and enum. Scheduler notifications, hardware registers,
privilege/counter registers and already-established table integrity are modeled.
No image is frozen for a board and no device is accessed.
"""
import argparse
import hashlib
import json
import re
import shlex
import subprocess
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP
from harness import Image, environment, refused, skip_crumbs

ROOT = Path(__file__).resolve().parents[1]
UNITS = ('drivers/npu/npu_runtime_queue.c', 'drivers/npu/npu_service.c',
         'drivers/npu/npu_queue.c', 'drivers/npu/npu_idle.c')
TYPES = {'npu_runtime_queue', 'npu_runtime', 'npu_runtime_slot', 'npu_queue', 'npu_queue_ops',
         'npu_queue_command', 'npu_queue_job', 'npu_queue_fence', 'npu_service',
         'npu_service_direct', 'npu_service_runtime_state', 'npu_service_runtime',
         'npu_service_irq_ops', 'a733_npu_ops', 'a733_npu_controller', 'a733_npu_startup',
         'a733_npu_native', 'a733_npu_command', 'npu_runtime_counters'}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def layouts(path):
    image = Image(path, TYPES)
    image.sizes, image.enums = {}, {}
    with path.open('rb') as stream:
        elf = ELFFile(stream)
        image.memory = [(int(segment['p_vaddr']), int(segment['p_memsz']))
                        for segment in elf.iter_segments() if segment['p_type'] == 'PT_LOAD']
        for cu in elf.get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                name = die.attributes.get('DW_AT_name')
                if not name:
                    continue
                name = name.value.decode()
                if die.tag == 'DW_TAG_enumerator' and (name.startswith(('A733_NPU_', 'NPU_RUNTIME_')) or name == 'SERVICE_ACTIVE'):
                    value = die.attributes['DW_AT_const_value'].value
                    assert name not in image.enums or image.enums[name] == value, name
                    image.enums[name] = value
                if die.tag == 'DW_TAG_structure_type' and name in TYPES and 'DW_AT_byte_size' in die.attributes:
                    value = die.attributes['DW_AT_byte_size'].value
                    assert name not in image.sizes or image.sizes[name] == value, name
                    image.sizes[name] = value
    return image


class Machine:
    # Scratch and stacks lie above the full OS device pool, including its tail.
    BASE, END, MMIO = 0x90000000, 0x903ff000, 0x03600000

    def __init__(self, image, disassembly):
        self.image = image
        self.u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.u.mem_map(self.BASE, 0x400000)
        self.u.mem_map(self.MMIO, 4096)
        mapped = set()
        for address, length in image.memory:
            for page in range(address & ~4095, (address + length + 4095) & ~4095, 4096):
                if page not in mapped:
                    self.u.mem_map(page, 4096)
                    mapped.add(page)
        for address, data in image.parts:
            if data:
                self.u.mem_write(address, data)
        self.contexts, self.notifications, self.trace = {}, {}, []
        self.current, self.reason, self.daif = 'caller', None, 0
        self.pause_read, self.paused_read, self.pause_notify, self.stop_after_notify = False, False, False, False
        self.mask_fault, self.mask_runtime_fault, self.clear_daif, self.guard_after_first = False, False, False, False
        self.reads, self.mmio, self.arms, self.masks, self.acks = 0, [], 0, 0, []
        self.bridge_calls, self.default_notices = [], 0
        self.clock = 24000000
        self.q, self.rt, self.startup, self.runtime = self.BASE + 0x1000, self.BASE + 0x10000, self.BASE + 0x90000, self.BASE + 0xa0000
        self.task, self.worker = self.BASE + 0xb0000, self.BASE + 0xb1000
        self.s = self.field(self.rt, 'npu_runtime', 'service')
        self.queue = self.field(self.q, 'npu_runtime_queue', 'queue')
        self.ctrl = self.field(self.s, 'npu_service', 'controller')
        self.slot = self.field(self.rt, 'npu_runtime', 'slot')
        self.callbacks = {self.BASE + 0xc0000 + 16 * n: name for n, name in enumerate(
            ('mask', 'rearm', 'read', 'clock', 'step', 'finished', 'bridge'))}
        cb = {name: address for address, name in self.callbacks.items()}
        self.set(self.rt, 'npu_runtime', 'bound', 1)
        self.set(self.rt, 'npu_runtime', 'slots', 1)
        self.set(self.rt, 'npu_runtime', 'startup', self.startup, 8)
        self.set(self.startup, 'a733_npu_startup', 'window_open', 1)
        self.set(self.slot, 'npu_runtime_slot', 'opened', 1)
        self.set(self.slot, 'npu_runtime_slot', 'client', self.task, 8)
        self.set(self.slot, 'npu_runtime_slot', 'state', image.enums['NPU_RUNTIME_READY'])
        for name, value, width in [('runtime', self.runtime, 8), ('worker', self.worker, 8),
                                   ('requests', self.BASE + 0xb2000, 8), ('irq_bound', 1, 4),
                                   ('irq_masked', 1, 4), ('initialized', 1, 4), ('affinity', 2, 4),
                                   ('running', 8, 4), ('activation_state', image.enums['SERVICE_ACTIVE'], 4)]:
            self.set(self.s, 'npu_service', name, value, width)
        self.set(self.ctrl, 'a733_npu_controller', 'state', image.enums['A733_NPU_READY'])
        hooks = self.field(self.runtime, 'npu_service_runtime_state', 'hooks')
        self.set(hooks, 'npu_service_runtime', 'step', cb['step'], 8)
        self.set(hooks, 'npu_service_runtime', 'finished', cb['finished'], 8)
        ops = self.field(self.s, 'npu_service', 'irq_ops')
        self.set(ops, 'npu_service_irq_ops', 'mask', cb['mask'], 8)
        self.set(ops, 'npu_service_irq_ops', 'rearm', cb['rearm'], 8)
        for base, typ in ((self.s, 'npu_service'), (self.ctrl, 'a733_npu_controller')):
            ops = self.field(base, typ, 'ops')
            self.set(ops, 'a733_npu_ops', 'read', cb['read'], 8)
            self.set(ops, 'a733_npu_ops', 'microseconds', cb['clock'], 8)
        self.put(image.address('instance', unit='/drivers/npu/npu_service.c'), self.s, 8)
        native = self.field(self.rt, 'npu_runtime', 'native')
        self.set(native, 'a733_npu_native', 'registers', self.MMIO, 8)
        if not hasattr(image, 'system_sites'):
            image.system_sites = {}
            for line in disassembly.splitlines():
                match = re.match(r'\s*([0-9a-f]+):\s+(mrs|msr)\s+(.*)', line)
                if match:
                    image.system_sites[int(match[1], 16)] = (match[2], match[3].strip().lower())
        self.sys = image.system_sites
        self.functions = {}
        for name in ('xTaskGetSchedulerState', 'xTaskGetCurrentTaskHandle', 'xTaskGenericNotify',
                     'ulTaskGenericNotifyTake', 'uxQueueMessagesWaiting', 'xQueueReceive',
                     'npu_runtime_tables_intact', 'vTaskNotifyGiveFromISR', 'vTaskGenericNotifyGiveFromISR'):
            values = image.symbols.get(name, set()) - {0}
            if values:
                assert len(values) == 1, name
                self.functions[next(iter(values))] = name
        self.crumbs, self.failures = [], []
        skip_crumbs(self.u, image, self.crumbs, self.failures)
        self.u.hook_add(UC_HOOK_CODE, self.code)
        self.u.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, self.device, begin=self.MMIO, end=self.MMIO + 4095)
        direct_address = self.field(self.s, 'npu_service', 'direct')
        self.u.hook_add(UC_HOOK_MEM_WRITE, self.handoff_write, begin=direct_address, end=direct_address + 7)

    def field(self, base, typ, name):
        return base + self.image.layouts[typ][name]

    def set(self, base, typ, name, value, width=4):
        self.put(self.field(base, typ, name), value, width)

    def getfield(self, base, typ, name, width=4):
        return self.get(self.field(base, typ, name), width)

    def put(self, address, value, width=4):
        self.u.mem_write(address, int(value).to_bytes(width, 'little'))

    def get(self, address, width=4):
        return int.from_bytes(self.u.mem_read(address, width), 'little')

    def ret(self, value=0):
        self.u.reg_write(UC_ARM64_REG_X0, value & ((1 << 64) - 1))
        self.u.reg_write(UC_ARM64_REG_PC, self.u.reg_read(UC_ARM64_REG_LR))

    def stop(self, reason):
        self.reason = reason
        self.u.emu_stop()

    def device(self, cpu, access, address, size, value, unused):
        self.mmio.append((access, address - self.MMIO, size))
        if self.guard_after_first and len(self.mmio) == 1:
            self.set(self.rt, 'npu_runtime', 'failed', 1)

    def handoff_write(self, cpu, access, address, size, value, unused):
        if self.current == 'caller' and value == 0:
            assert self.daif & 0x80, 'return cleared direct ownership with IRQs enabled'

    def code(self, cpu, address, size, unused):
        if address == self.END:
            self.stop('returned')
            return
        # Preempt after the release store, before notification arguments load.
        # Stopping at xTaskGenericNotify entry would hide an uncaptured-owner bug.
        if self.current == 'worker' and self.pause_notify and self.getfield(self.s, 'npu_service', 'direct_ready') == 1:
            self.pause_notify = False
            self.stop('before-worker-notify')
            return
        instruction = self.sys.get(address)
        if instruction:
            op, operands = instruction
            if op == 'mrs':
                register, system = operands.split(', ')
                values = {'currentel': 4, 'mpidr_el1': 0x100, 'cntfrq_el0': 24000000,
                          'cntpct_el0': self.clock, 'daif': self.daif}
                if system in values:
                    self.u.reg_write(UC_ARM64_REG_X0 + int(register[1:]), values[system])
                    self.u.reg_write(UC_ARM64_REG_PC, address + 4)
                    return
            elif operands.startswith('daifset,'):
                self.daif |= 0x80
                self.trace.append((self.current, 'lock'))
                self.u.reg_write(UC_ARM64_REG_PC, address + 4)
                return
            elif operands.startswith('daif, x'):
                self.daif = self.u.reg_read(UC_ARM64_REG_X0 + int(operands.split('x')[1]))
                self.trace.append((self.current, 'unlock'))
                self.u.reg_write(UC_ARM64_REG_PC, address + 4)
                return
        args = [self.u.reg_read(UC_ARM64_REG_X0 + n) for n in range(7)]
        name = self.callbacks.get(address) or self.functions.get(address)
        if name is None:
            return
        self.trace.append((self.current, name))
        if name == 'xTaskGetSchedulerState':
            self.ret(2)  # taskSCHEDULER_RUNNING is the public FreeRTOS macro.
        elif name == 'xTaskGetCurrentTaskHandle':
            self.ret(self.worker if self.current == 'worker' else self.task)
        elif name == 'npu_runtime_tables_intact':
            self.ret(1)
        elif name == 'xTaskGenericNotify':
            assert args[0] in (self.worker, self.task), 'worker notified an invalid handoff owner'
            if self.current == 'worker':
                ready = self.getfield(self.s, 'npu_service', 'direct_ready')
                self.acks.append(ready)
            self.notifications[args[0]] = self.notifications.get(args[0], 0) + 1
            self.ret(1)
            if self.current == 'worker' and self.stop_after_notify:
                self.stop('notified')
        elif name == 'ulTaskGenericNotifyTake':
            task = self.worker if self.current == 'worker' else self.task
            if self.notifications.get(task, 0):
                count = self.notifications.pop(task)
                self.ret(count)
            else:
                self.stop('waiting')  # Resume at this actual wait after a notification.
        elif name in ('uxQueueMessagesWaiting', 'xQueueReceive'):
            self.ret(0)
        elif name == 'clock':
            self.clock += 24
            self.ret(self.clock // 24)
        elif name == 'read':
            assert self.current == 'worker' and args[1] == 0x10
            if self.pause_read and not self.paused_read:
                self.paused_read = True
                self.stop('worker-before-event-read')
                return
            assert not self.getfield(self.q, 'npu_runtime_queue', 'claimed'), 'worker MMIO overlapped armed direct owner'
            self.reads += 1
            self.put(args[2], 0)
            self.ret(1)
        elif name == 'step':
            self.ret(1)  # No ordinary job work; return to the safe loop entry.
        elif name == 'finished':
            self.ret()
        elif name == 'bridge':
            assert args[0] == self.q and args[1] == self.BASE + 0xd0000
            failed = self.getfield(self.queue, 'npu_queue', 'failed')
            if failed:
                assert self.getfield(self.rt, 'npu_runtime', 'failed') == 1
                assert self.getfield(self.s, 'npu_service', 'irq_pending') & 2
            self.bridge_calls.append({'failed': failed, 'completed': self.getfield(self.queue, 'npu_queue', 'completed', 8)})
            self.notifications[self.task] = self.notifications.get(self.task, 0) + 1
            self.ret()
        elif name == 'rearm':
            assert self.getfield(self.s, 'npu_service', 'direct_ready') == 1, 'claim armed before worker ACK'
            self.arms += 1
            self.ret(1)
        elif name == 'mask':
            self.masks += 1
            if self.clear_daif:
                self.daif = 0  # Provider exits its RTOS critical section with IRQs enabled.
            if self.mask_runtime_fault:
                self.set(self.rt, 'npu_runtime', 'failed', 1)
            if self.mask_fault:
                self.mask_fault = False
                self.ret(1)
                # Execute the real adapter ISR as an interrupt, preserving task registers.
                context = self.u.context_save()
                self.contexts['interrupted'] = context
                direct = self.field(self.q, 'npu_runtime_queue', 'direct')
                self.u.reg_write(UC_ARM64_REG_X0, self.q)
                self.u.reg_write(UC_ARM64_REG_X0 + 1, self.BASE + 0xd0000)
                self.u.reg_write(UC_ARM64_REG_LR, self.END)
                self.u.reg_write(UC_ARM64_REG_PC, self.getfield(direct, 'npu_service_direct', 'irq', 8))
                self.current = 'irq'
                return
            self.ret(1)
        elif name in ('vTaskNotifyGiveFromISR', 'vTaskGenericNotifyGiveFromISR'):
            self.default_notices += 1
            self.ret()
        else:
            raise AssertionError(name)

    def run(self, name, function=None, *args):
        self.current, self.reason = name, None
        if function is not None:
            self.u.reg_write(UC_ARM64_REG_SP, self.BASE + (0x3c0000 if name == 'worker' else 0x380000))
            self.u.reg_write(UC_ARM64_REG_LR, self.END)
            self.u.reg_write(UC_ARM64_REG_PC, function if isinstance(function, int) else self.image.address(function))
            for n, value in enumerate(args):
                self.u.reg_write(UC_ARM64_REG_X0 + n, value)
        else:
            self.u.context_restore(self.contexts[name])
        for _ in range(2):
            self.u.emu_start(self.u.reg_read(UC_ARM64_REG_PC), 0, count=200000)
            if self.current == 'irq' and self.reason == 'returned':
                self.u.context_restore(self.contexts.pop('interrupted'))
                self.current, self.reason = name, None
                continue
            break
        assert self.reason is not None, ('execution budget', name, hex(self.u.reg_read(UC_ARM64_REG_PC)))
        self.contexts[name] = self.u.context_save()
        result = self.u.reg_read(UC_ARM64_REG_X0) & 0xffffffff
        return self.reason, result - (1 << 32) if result & (1 << 31) else result

    def pause_worker(self):
        self.pause_read = True
        self.set(self.s, 'npu_service', 'irq_pending', 1)
        result, _ = self.run('worker', self.image.address('runtime_worker', unit='/drivers/npu/npu_service.c'), self.s)
        assert result == 'worker-before-event-read'
        assert not self.getfield(self.s, 'npu_service', 'irq_pending')

    def begin_claim(self):
        result, _ = self.run('caller', 'npu_runtime_queue_claim', self.q, self.rt)
        assert result == 'waiting' and self.arms == 0, 'claim armed before worker ACK'

    def acknowledge(self):
        result, _ = self.run('worker')
        assert result == 'waiting' and self.reads == 1
        assert self.getfield(self.s, 'npu_service', 'direct_ready') == 1, 'worker did not acknowledge safe entry'
        assert self.getfield(self.s, 'npu_service', 'direct', 8), 'worker did not publish direct ownership'

    def claim(self):
        self.pause_worker()
        self.begin_claim()
        self.acknowledge()
        assert self.run('caller') == ('returned', 1)
        assert self.arms == 1 and self.getfield(self.q, 'npu_runtime_queue', 'claimed') == 1


def cases(image, disassembly, only=None):
    rows = []

    def record(label, machine):
        rows.append({'case': label, 'passed': True, 'arms': machine.arms, 'worker_reads': machine.reads,
                     'masks': machine.masks, 'trace': machine.trace, 'mmio': machine.mmio,
                     'crumbs': machine.crumbs, 'failures': machine.failures})
        print('PASS', label, flush=True)

    for outcome in ('success', 'event-error', 'idle-missing', 'runtime-lost'):
        label = 'bridge-notifies-after-' + outcome
        if only not in (None, label):
            continue
        m = Machine(image, disassembly)
        bridge = next(address for address, name in m.callbacks.items() if name == 'bridge')
        m.set(m.q, 'npu_runtime_queue', 'progress_from_isr', bridge, 8)
        m.set(m.q, 'npu_runtime_queue', 'progress_cookie', m.q, 8)
        m.claim()
        m.notifications.clear()
        job = m.BASE + 0xd1000
        m.set(m.queue, 'npu_queue', 'head', job, 8)
        m.set(m.queue, 'npu_queue', 'tail', job, 8)
        m.set(m.queue, 'npu_queue', 'submitted', 1, 8)
        m.set(m.queue, 'npu_queue', 'held', 1, 8)
        fence = m.field(job, 'npu_queue_job', 'fence')
        m.set(fence, 'npu_queue_fence', 'queue', m.queue, 8)
        m.set(fence, 'npu_queue_fence', 'value', 1, 8)
        m.set(job, 'npu_queue_job', 'held', 1)
        m.set(m.q, 'npu_runtime_queue', 'started', m.clock - 24, 8)
        m.set(m.q, 'npu_runtime_queue', 'timeout_ticks', 24000, 8)
        m.put(m.MMIO + 0x10, 2 if outcome == 'event-error' else 1)
        m.put(m.MMIO + 4, 0 if outcome == 'idle-missing' else 0x7fffffff)
        if outcome == 'runtime-lost':
            m.set(m.rt, 'npu_runtime', 'failed', 1)
        function = image.address('irq', unit='/drivers/npu/npu_runtime_queue.c')
        assert m.run('bridge-irq', function, m.q, m.BASE + 0xd0000)[0] == 'returned'
        good = outcome == 'success'
        assert m.bridge_calls == [{'failed': int(not good), 'completed': int(good)}]
        assert m.default_notices == 0 and m.notifications.get(m.task) == 1
        assert m.getfield(job, 'npu_queue_job', 'held') == 1
        assert m.getfield(m.queue, 'npu_queue', 'held', 8) == 1
        assert all(offset != 0x3a4 for unused, offset, width in m.mmio)
        if not good:
            assert m.getfield(m.queue, 'npu_queue', 'head', 8) == job
        record(label, m)

    if only in (None, 'worker-resumes-before-safe-entry-ack'):
        m = Machine(image, disassembly)
        m.claim()
        record('worker-resumes-before-safe-entry-ack', m)

    for failure in ('runtime', 'startup-window'):
        label = 'claim-waiting-for-ack-' + failure + '-loss'
        if only not in (None, label):
            continue
        m = Machine(image, disassembly)
        m.pause_worker()
        m.begin_claim()
        if failure == 'runtime':
            m.set(m.rt, 'npu_runtime', 'failed', 1)
        else:
            m.set(m.startup, 'a733_npu_startup', 'window_open', 0)
        m.acknowledge()
        assert m.run('caller') == ('returned', -1), 'claim accepted lost lifecycle authority'
        assert m.arms == 0 and m.getfield(m.queue, 'npu_queue', 'failed') == 1
        assert m.getfield(m.rt, 'npu_runtime', 'failed') == 1
        record(label, m)

    for fault in ('healthy', 'late-isr-fault', 'runtime-fault'):
        label = 'return-provider-enables-irqs-' + fault
        if only not in (None, label):
            continue
        m = Machine(image, disassembly)
        m.claim()
        m.clear_daif, m.mask_fault, m.mask_runtime_fault = True, fault == 'late-isr-fault', fault == 'runtime-fault'
        result = m.run('caller', 'npu_runtime_queue_return', m.q)
        if fault != 'healthy':
            assert result == ('returned', -1), 'return released after late fault'
            assert m.getfield(m.s, 'npu_service', 'direct', 8), 'return cleared retained direct owner'
            assert m.getfield(m.q, 'npu_runtime_queue', 'claimed') == 1
            assert m.getfield(m.queue, 'npu_queue', 'failed') == 1
            assert m.getfield(m.rt, 'npu_runtime', 'failed') == 1
        else:
            assert result == ('returned', 1)
            assert not m.getfield(m.s, 'npu_service', 'direct', 8)
            assert not m.getfield(m.q, 'npu_runtime_queue', 'claimed')
            mask_at = max(i for i, row in enumerate(m.trace) if row == ('caller', 'mask'))
            assert ('caller', 'lock') in m.trace[mask_at + 1:], 'return lacked final raw lock after provider callback'
        assert m.masks == 1 and m.daif == 0
        record(label, m)

    label = 'ack-publication-before-notify-owner-remains-stable'
    if only in (None, label):
        m = Machine(image, disassembly)
        m.pause_worker()
        m.begin_claim()
        m.pause_notify = True
        assert m.run('worker')[0] == 'before-worker-notify'
        assert m.getfield(m.s, 'npu_service', 'direct_ready') == 1
        m.notifications[m.task] = 1  # Unrelated notification wins the ACK-notify race.
        assert m.run('caller') == ('returned', 1)
        assert m.run('caller', 'npu_runtime_queue_return', m.q) == ('returned', 1)
        assert not m.getfield(m.s, 'npu_service', 'direct_owner', 8)
        m.stop_after_notify = True
        assert m.run('worker')[0] == 'notified'
        assert m.notifications.get(m.task) == 1, 'worker lost captured handoff owner'
        record(label, m)

    label = 'returned-queue-refuses-submission-while-peer-owns'
    if only in (None, label):
        m = Machine(image, disassembly)
        m.claim()
        # A previously completed fence survives the retirement operation.
        m.set(m.queue, 'npu_queue', 'submitted', 1, 8)
        m.set(m.queue, 'npu_queue', 'completed', 1, 8)
        assert m.run('caller', 'npu_runtime_queue_return', m.q) == ('returned', 1)
        assert m.getfield(m.queue, 'npu_queue', 'retired') == 1
        old_bytes = bytes(m.u.mem_read(m.q, image.sizes['npu_runtime_queue']))
        peer = m.BASE + 0xe0000
        assert m.run('caller', 'npu_runtime_queue_claim', peer, m.rt)[0] == 'waiting'
        assert m.run('worker')[0] == 'waiting'
        assert m.run('caller') == ('returned', 1)
        assert m.getfield(peer, 'npu_runtime_queue', 'claimed') == 1
        owner = m.field(peer, 'npu_runtime_queue', 'direct')
        assert m.getfield(m.s, 'npu_service', 'direct', 8) == owner
        job, command, out = m.BASE + 0xd1000, m.BASE + 0xd2000, m.BASE + 0xd3000
        m.set(job, 'npu_queue_job', 'next', 0x12345678, 8)
        m.set(command, 'npu_queue_command', 'owner', m.queue, 8)
        hardware = m.field(command, 'npu_queue_command', 'hardware')
        m.set(hardware, 'a733_npu_command', 'device', 0x80000000)
        m.set(hardware, 'a733_npu_command', 'readable_bytes', 4096, 8)
        m.set(hardware, 'a733_npu_command', 'token', m.BASE + 0xd4000, 8)
        before_job = bytes(m.u.mem_read(job, image.sizes['npu_queue_job']))
        before_out = bytes([0xa5]) * image.sizes['npu_queue_fence']
        m.u.mem_write(out, before_out)
        assert m.run('caller', 'npu_queue_submit', m.queue, job, command, 0, 0, 100, out) == ('returned', 0)
        assert bytes(m.u.mem_read(job, len(before_job))) == before_job, 'retired submission changed the caller record'
        assert bytes(m.u.mem_read(out, len(before_out))) == before_out, 'retired submission changed the output fence'
        assert bytes(m.u.mem_read(m.q, len(old_bytes))) == old_bytes, 'retired submission mutated the old queue'
        assert not m.mmio and not m.getfield(m.rt, 'npu_runtime', 'failed')
        assert m.getfield(m.s, 'npu_service', 'direct', 8) == owner
        assert m.run('caller', 'npu_queue_fence_status', m.queue, m.queue, 1) == ('returned', 1)
        record(label, m)

    for operation in ('start', 'complete'):
        for failure in ('runtime', 'startup-window', 'between-accesses'):
            label = operation + '-guard-' + failure
            if only not in (None, label):
                continue
            m = Machine(image, disassembly)
            m.claim()
            if failure == 'runtime':
                m.set(m.rt, 'npu_runtime', 'failed', 1)
            elif failure == 'startup-window':
                m.set(m.startup, 'a733_npu_startup', 'window_open', 0)
            else:
                m.guard_after_first = True
            command = m.BASE + 0xd1000
            m.set(command, 'a733_npu_command', 'device', 0x80000000)
            m.set(command, 'a733_npu_command', 'readable_bytes', 4096, 8)
            m.set(command, 'a733_npu_command', 'token', m.BASE + 0xd2000, 8)
            function = image.address(operation, unit='/drivers/npu/npu_runtime_queue.c')
            if operation == 'start':
                result = m.run('caller', function, m.q, command, 100)
            else:
                result = m.run('caller', function, m.q)
            assert result == ('returned', 0), 'direct adapter accepted lost lifecycle authority'
            expected = 1 if failure == 'between-accesses' else 0
            assert len(m.mmio) == expected, 'direct adapter touched MMIO after lifecycle authority loss'
            record(label, m)
    return rows


def build_images(build, output, variants):
    """Reuse the accepted graph and rebuild the queue's compiled closure."""
    cmake = (build / 'CMakeCache.txt').read_text()
    source = Path(re.search(r'^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$', cmake, re.M)[1])
    flags_path = build / 'CMakeFiles/sunstone-a733-os-npu.dir/flags.make'
    flags = flags_path.read_text()
    options = []
    for name in ('C_DEFINES', 'C_INCLUDES', 'C_FLAGS'):
        options += shlex.split(re.search(r'^' + name + r' = (.*)$', flags, re.M)[1].replace(str(source), str(ROOT)))
    cc = re.search(r'^CMAKE_C_COMPILER:(?:STRING|FILEPATH)=(.*)$', cmake, re.M)[1]
    base_link = shlex.split((build / 'CMakeFiles/sunstone-a733-os.dir/link.txt').read_text())
    commands, inputs = [], {str(flags_path): sha(flags_path)}
    for token in base_link:
        candidates = [token]
        if token.startswith('-Wl,-T,'):
            candidates = [token.removeprefix('-Wl,-T,')]
        elif token.startswith('--ld-path='):
            candidates = [token.removeprefix('--ld-path=')]
        for candidate in candidates:
            path = Path(candidate)
            if not path.is_absolute():
                path = build / path
            if path.is_file():
                inputs[str(path)] = sha(path)
    env = dict(environment(), TMPDIR=str(output / 'tmp'))

    def run(command, log):
        commands.append(list(map(str, command)))
        (output / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        result = subprocess.run(command, cwd=build, env=env, capture_output=True)
        log.write_bytes(result.stdout + result.stderr)
        assert result.returncode == 0, ('build failed', log)

    units = {unit: (ROOT / unit, options) for unit in UNITS}
    # Cached objects may predate a private structure change. Recompile every
    # linked consumer with its own target flags instead of mixing old offsets
    # with new adapters or suppressing conflicting DWARF layouts.
    headers = ('include/npu_runtime.h', 'include/npu_runtime_queue.h',
               'include/npu_service.h', 'include/npu_queue.h')
    for token in base_link:
        if not token.endswith('.obj') or '.dir/' not in token:
            continue
        dependency = build / (token + '.d')
        if not dependency.is_file() or not any(h in dependency.read_text() for h in headers):
            continue
        unit = token.split('.dir/', 1)[1][:-4]
        path = ROOT / unit
        if not path.is_file():
            path = build / unit
        assert path.is_file(), ('missing cached compilation input', path)
        target_flags = build / (token.split('.dir/', 1)[0] + '.dir/flags.make')
        text = target_flags.read_text()
        own_options = []
        for name in ('C_DEFINES', 'C_INCLUDES', 'C_FLAGS'):
            own_options += shlex.split(re.search(r'^' + name + r' = (.*)$', text, re.M)[1].replace(str(source), str(ROOT)))
        units[unit] = (path, own_options)
        inputs[str(target_flags)] = sha(target_flags)
    sources = {unit: path.read_text() for unit, (path, unused) in units.items()}
    for path, unused in units.values():
        inputs[str(path)] = sha(path)
    images = {}
    for variant in variants:
        directory = output / variant
        directory.mkdir()
        replacements = {}
        for unit, original in sources.items():
            text = original
            if unit.endswith('npu_runtime_queue.c'):
                if variant == 'without-handshake':
                    start = text.index('    rt->service.direct_owner = q->task;')
                    end = text.index('    const uint64_t armed_saved', start)
                    text = text[:start] + '    rt->service.direct = &q->direct;\n    npu_runtime_queue_unlock(saved);\n' + text[end:]
                if variant == 'without-return-recheck':
                    start = text.index('    if (!npu_queue_idle(&q->queue) ||')
                    end = text.index('    if (result == 1)', start)
                    text = text[:start] + text[end:]
                if variant == 'without-window-guards':
                    assert text.count('    if (!window(q)) return 0;') == 4
                    text = text.replace('    if (!window(q)) return 0;\n', '')
            elif unit.endswith('npu_service.c') and variant == 'without-owner-capture':
                text = text.replace('            TaskHandle_t owner = s->direct_owner;\n', '')
                text = text.replace('            xTaskNotifyGive(owner);', '            xTaskNotifyGive(s->direct_owner);')
                assert text != original
            generated = directory / unit
            generated.parent.mkdir(parents=True, exist_ok=True)
            obj, dep = (directory / (unit.replace('/', '-') + suffix) for suffix in ('.o', '.d'))
            # Keep real production units as the compilation input in the nominal build.
            generated.write_text(text)
            compile_source = units[unit][0] if variant == 'actual' else generated
            run([cc, '--target=aarch64-none-elf', *units[unit][1], '-MD', '-MF', str(dep), '-c', str(compile_source), '-o', str(obj)], directory / (obj.stem + '-compile.log'))
            for token in shlex.split(dep.read_text().replace('\\\n', ' ').split(':', 1)[1]):
                path = Path(token)
                if path.is_file():
                    inputs[str(path)] = sha(path)
            replacements[unit + '.obj'] = obj
        link, used = [], set()
        for token in base_link:
            substitute = next((obj for unit, obj in replacements.items() if token.endswith('/' + unit)), None)
            if substitute:
                used.add(substitute)
                token = str(substitute)
            elif token.startswith('-Wl,-Map,'):
                token = '-Wl,-Map,' + str(directory / 'fixture.map')
            link.append(token)
        # A reused build can predate a new portable dependency. Include each
        # newly compiled unit even when no previous object occupies its place.
        link += [str(obj) for obj in replacements.values() if obj not in used]
        assert '-o' in link
        link[link.index('-o') + 1] = str(directory / 'fixture.elf')
        link += ['-Wl,--undefined=' + name for name in ('npu_runtime_queue_claim', 'npu_runtime_queue_return',
                                                       'npu_queue_submit', 'npu_queue_fence_status')]
        # Use the linker's configured invocation path without resolving its symlink.
        run(link, directory / 'link.log')
        elf = directory / 'fixture.elf'
        disassembly = subprocess.check_output(['/usr/bin/llvm-objdump-21', '-d', '--no-show-raw-insn', str(elf)], text=True)
        images[variant] = (elf, disassembly)
    return images, inputs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True, help='Existing CMake NPU OS build, read only')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', action='append', help='Run only this named nominal case; repeat to select several')
    parser.add_argument('--skip-mutations', action='store_true', help='Reuse separately recorded mutation evidence')
    parser.add_argument('--mutation', action='append', choices=('without-handshake', 'without-return-recheck',
                        'without-owner-capture', 'without-window-guards'), help='Run only these regression mutations')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / 'tmp').mkdir()
    assert not (args.skip_mutations and args.mutation), 'Select mutations or skip them'
    mutation_names = [] if args.skip_mutations else args.mutation or ['without-handshake', 'without-return-recheck',
                                                                     'without-owner-capture', 'without-window-guards']
    variants = ['actual', *mutation_names]
    images, inputs = build_images(args.build_dir.resolve(), output, variants)
    image = layouts(images['actual'][0])
    rows = []
    for selected in args.case or [None]:
        batch = cases(image, images['actual'][1], selected)
        assert batch, ('unknown nominal case', selected)
        rows.extend(batch)
    mutations = []
    expected = {'without-handshake': 'claim armed before worker ACK',
                'without-return-recheck': 'return released after late fault',
                'without-owner-capture': 'worker notified an invalid handoff owner',
                'without-window-guards': 'direct adapter accepted lost lifecycle authority'}
    for variant in mutation_names:
        reason = expected[variant]
        elf, disassembly = images[variant]
        target = {'without-handshake': 'worker-resumes-before-safe-entry-ack',
                  'without-return-recheck': 'return-provider-enables-irqs-runtime-fault',
                  'without-owner-capture': 'ack-publication-before-notify-owner-remains-stable',
                  'without-window-guards': 'start-guard-runtime'}[variant]
        message = refused(lambda: cases(layouts(elf), disassembly, target), reason, AssertionError)
        mutations.append({'variant': variant, 'caught': message})
    inputs[str(Path(__file__).resolve())] = sha(__file__)
    result = {'passed': True, 'physical_access': False, 'cases': rows, 'mutations': mutations,
              'images': {name: sha(elf) for name, (elf, unused) in images.items()}, 'inputs': inputs,
              'scope': __doc__}
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS', len(rows), 'native handoff/lifecycle cases and', len(mutations), 'caught regressions')


if __name__ == '__main__':
    main()
