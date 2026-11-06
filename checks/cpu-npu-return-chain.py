#!/usr/bin/env python3
"""Compose the checked application receipt with native close, return and host cleanup.

The application/GEMM producer executes on host threads. One native machine
then executes the public SDK mailbox, client copy/count/report code, CLOSE,
worker park, bootstrap local-stop coordination, FINISHED and owner retirement.
Queue/device replies, CPU-clock completion, 40 C sensor samples and local
platform completion are modeled effects. Its
unmodified OS/application/NPU RAM continues into the actual eight-core terminal;
architectural startup/GIC state and contiguous cache-maintenance are modeled.
The raw native reports then pass through the production runner's restoration
and approved post-ROM helper method with a modeled USB transport/helper result.
No modeled NPU output or timing is physical accelerator evidence.
"""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
from functools import lru_cache
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import types
from unittest.mock import patch
from elftools.elf.elffile import ELFFile
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP
from harness import Image, environment, save

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/os/npu-report.py', 'apps/cpu-npu-concurrency/main.c', 'apps/cpu-npu-concurrency/gemm.h',
 'apps/cpu-npu-concurrency/host.py', 'apps/cpu-npu-concurrency/receipt.h',
 'apps/os/npu-observe.c', 'apps/os/npu-client.c', 'apps/os/npu.c', 'apps/os/npu-declaration.c',
 'apps/os/application.c', 'apps/os/cpu-return.c', 'apps/os/cpu-return-host.py',
 'apps/os/cpu-return-followup.py', 'apps/os/cpu-return/terminal.patch',
 'apps/os/cpu-return/thermal.patch', 'drivers/npu/npu_service.c',
 'drivers/npu/a733_npu_runtime.c', 'runtime/npu_app.c',
 'checks/cpu-npu-concurrency/producer.c', 'checks/os-cpu-return-services.py',
 'checks/os-cpu-return.py', 'apps/os/thermal.c', 'apps/os/thermal-report.py',
 'apps/os/cpu-return-thermal.py', 'tools/board.py', 'tools/board_images.py']
GATE_RUNS = [['--elf', '{elf:ret-npu-load}', '--out', '{output}']]
MAIN, CLIENT, BOOTSTRAP, WORKER = 0x5000, 0x3000, 0x4000, 0x9000
APPLICATION_WORKER = 0x5800   # a second task of the application, also at EL0 on the board
POOL, POOL_BYTES = 0x47400000, 0x800000  # the model's own NPU reservation; the OS publishes the range it takes from the ledger


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


owners = module('chain_return_owners', ROOT/'checks/os-cpu-return-services.py')
terminal = module('chain_return_terminal', ROOT/'checks/os-cpu-return.py')
receipt = module('chain_receipt_decoder', ROOT/'apps/cpu-npu-concurrency/host.py')
prior = owners.prior


@lru_cache(maxsize=None)
def type_size(path, name):
    with path.open('rb') as stream:
        sizes = {d.attributes['DW_AT_byte_size'].value
                 for cu in ELFFile(stream).get_dwarf_info().iter_CUs() for d in cu.iter_DIEs()
                 if d.tag == 'DW_TAG_structure_type' and 'DW_AT_name' in d.attributes and
                 d.attributes['DW_AT_name'].value.decode() == name and 'DW_AT_byte_size' in d.attributes}
    assert len(sizes) == 1, (name, sizes)
    return sizes.pop()


class Native(terminal.KernelPages, owners.Model):
    """An admitted runtime before OPEN; no completed/parked/stop flag is seeded."""
    def __init__(self, image, layout, payload):
        super().__init__(image, layout)
        self.payload = payload
        self.words = struct.unpack('<216Q', payload)
        self.current_task, self.scheduler = MAIN, 1
        self.time = self.words[8]
        self.n = layout.address('npu', 'apps/os/npu.c')
        self.nf = layout.layouts['os_npu_application']
        self.device = terminal.npu_device(self, self.s, layout, self.n)
        self.df = layout.layouts['os_npu_device']
        self.kernel_only(self.device, self.device+type_size(image.path, 'os_npu_device'))
        self.rt = self.n+self.nf['runtime']
        self.rf = layout.layouts['npu_runtime']
        self.service = self.rt+self.rf['service']
        self.sf = layout.layouts['npu_service']
        self.slotf = layout.layouts['npu_runtime_slot']
        # The runtime's tables are not arrays inside the runtime: it points to them (npu_runtime.slot, .plans, .commands, ...).
        # On the board they are EL1 metadata in kernel ledger pages; this model puts them in the pool from 0x200000 to 0x400000,
        # clear of the tensor windows, and marks that range kernel-only.
        self.tables = 0x300000
        self.kernel_only(POOL+0x200000, POOL+0x400000)
        self.slot = POOL+0x300000+0x20000
        self.startup = self.device+self.df['startup']
        self.report = layout.address('os_npu_published_report')
        self.ops, self.ticket, self.operation, self.submits = [], 0, 0, 0
        self.stop_calls, self.failures = 0, []
        self.contexts, self.notifications = {}, {}
        self.pending_completion, self.delivered = 0, False
        self.put64(self.thermal+layout.layouts['os_thermal_service']['next_read'],self.time//24)
        self.put64(self.thermal+layout.layouts['os_thermal_service']['handle'], 0x6000)
        self.notify_wait = layout.address('ulTaskGenericNotifyTake')
        self.uc.mem_map(POOL, POOL_BYTES)
        self.input_data = layout.address('cpu_npu_input')
        self.input_bytes = self.u64(layout.address('cpu_npu_input_bytes'))
        self.reference = bytes(self.uc.mem_read(layout.address('cpu_npu_reference'),
                                               self.u64(layout.address('cpu_npu_reference_bytes'))))
        self.output = layout.address('output', 'apps/cpu-npu-concurrency/main.c')
        self.payload_address = layout.address('receipt', 'apps/cpu-npu-concurrency/main.c')
        self.input_window, self.output_window = POOL, POOL+0x40000
        self.session = layout.address('decoded', 'apps/os/session.c')
        self.uc.mem_write(self.session, bytes(type_size(image.path, 'os_session')))
        self.put64(self.n+self.nf['client'], CLIENT)
        self.put64(self.n+self.nf['bootstrap'], BOOTSTRAP)
        self.put64(layout.address('main_task', 'apps/os/application.c'), MAIN)
        self.put64(layout.address('grant', 'apps/os/application.c')+
                   layout.layouts['ss_grant']['finish_by_us'], 55000000)
        admitted_epoch=self.u64(self.board+layout.layouts['board_application']['session']+
                                  layout.layouts['a733_board_session']['epoch'])
        self.put32(0x07090100,0x5c0d0000|(admitted_epoch&0xffff)) # modeled launcher tag
        # Cached SMP startup metadata is an architectural admission input.
        # The terminal completion/arrival flags remain zero until native code
        # publishes them; no terminal barrier result is supplied here.
        cached=layout.address('a733_cached_runtime')
        cached_size=type_size(image.path,'a733_cached_runtime')
        cached_fields=layout.layouts['a733_cached_runtime']
        session_address=self.board+layout.layouts['board_application']['session']
        admitted_deadline=self.u64(session_address+layout.layouts['a733_board_session']['deadline_us'])*24
        for core in range(8):
            self.put64(cached+core*cached_size+cached_fields['epoch'],admitted_epoch)
            self.put64(cached+core*cached_size+cached_fields['diagnostic_deadline'],admitted_deadline)
            self.put64(cached+core*cached_size+cached_fields['redistributor'],0x03460000+core*0x20000)
        self.put32(self.n+self.nf['started'], 1)
        self.put64(self.n+self.nf['end'], 55000000)
        self.put64(self.n+self.nf['initial_deadline'], 55000000)
        self.put64(self.n+self.nf['gate'], (55000000<<3)|1)
        self.put64(self.rt+self.rf['startup'], self.startup)
        self.put64(self.startup+layout.layouts['a733_npu_startup']['task'], BOOTSTRAP)
        self.put32(self.rt+self.rf['prepared'], 1)
        self.put32(self.rt+self.rf['bound'], 1)
        self.put32(self.rt+self.rf['count'], 1)
        # The image links no graph; the model holds one registered graph. The SDK back end describes it from the runtime's
        # plan (sdk_info), which this model keeps empty: the application's request is compared with the tensor windows.
        self.put64(self.rt+self.rf['plans'], POOL+0x3f0000)
        self.put64(layout.address('os_npu_runtime_for_sdk', 'apps/os/npu-client.c'), self.rt)
        self.put32(self.rt+self.rf['slots'], 1)
        self.put64(self.rt+self.rf['parent_cpu'], POOL)
        self.put64(self.rt+self.rf['parent_bytes'], POOL_BYTES)
        self.put64(self.rt+self.rf['slot'], self.slot)
        self.put64(self.slot+self.slotf['client'], CLIENT)
        self.put32(self.slot+self.slotf['state'], 2)
        self.put64(layout.address('instance', 'drivers/npu/npu_service.c'), self.service)
        self.put32(self.service+self.sf['initialized'], 1)
        self.put32(self.service+self.sf['shared'], 1)
        self.put32(self.service+self.sf['irq_bound'], 1)
        self.put64(self.service+self.sf['worker'], WORKER)
        self.put64(self.service+self.sf['activation']+layout.layouts['npu_service_activation']['bootstrap'], BOOTSTRAP)
        self.hooks[layout.address('xTaskGetCurrentTaskHandle')] = lambda: self.current_task
        self.el1_hook(layout)
        self.hooks[layout.address('xTaskGetSchedulerState')] = lambda: self.scheduler
        self.hooks[layout.address('os_session')] = lambda: self.session
        self.hooks[layout.address('os_granted')] = lambda: int(self.uc.reg_read(prior.fault.REGS[0]) == 1)  # the modeled session grants the NPU (bit 0) alone; admission is not under test
        self.hooks[layout.address('a733_counter_clock_now')] = self.clock_now
        self.hooks[layout.address('npu_runtime_startup_poll')] = lambda: 0
        self.hooks[layout.address('npu_service_activate')] = self.activate
        self.hooks[layout.address('npu_runtime_send')] = self.send
        self.hooks[layout.address('npu_runtime_collect')] = self.collect
        self.hooks[layout.address('a733_npu_startup_stop')] = self.platform_stop
        # The plans are a table that the runtime's reservation holds and the runtime points to (npu_runtime.plans), not an
        # array inside the runtime: put one in the pool, clear of the tensor windows.
        plan = POOL+0x200000
        self.put64(self.rt+self.rf['plans'], plan)
        pf = layout.layouts['nbg_assembly_layout']
        self.put32(plan+pf['inputs'], 1)
        self.put32(plan+pf['outputs'], 1)
        requirement = type_size(image.path, 'nbg_allocation_requirement')
        table = POOL+0x220000                      # the layout's table of allocations (buffers, capacity)
        self.put64(plan+pf['buffers'], table); self.put32(plan+pf['capacity'], 8)
        self.put32(table+4*requirement, self.input_bytes)
        self.put32(table+5*requirement, len(self.reference))
        self.uc.mem_write(self.slot+self.slotf['layout'],
                          bytes(self.uc.mem_read(plan, type_size(image.path, 'nbg_assembly_layout'))))
        tf = layout.layouts['npu_runtime_tensor']
        tensor_size = type_size(image.path, 'npu_runtime_tensor')
        self.put64(self.slot+self.slotf['tensors'], POOL+0x300000+0x40000); self.put32(self.slot+self.slotf['tensor_capacity'], 4)
        for index, (address, count) in enumerate(((self.input_window, self.input_bytes),
                                                  (self.output_window, len(self.reference)))):
            tensor = POOL+0x300000+0x40000+index*tensor_size
            self.put64(tensor+tf['data'], address)
            self.put64(tensor+tf['bytes'], count)
        hooks = layout.address('hooks', 'drivers/npu/a733_npu_runtime.c')
        assert self.api('npu_service_runtime', self.service, hooks, self.rt) == 1
        assert self.api('os_npu_sdk_bind', self.rt) == 1
        self.scheduler = 2
        for name, unit in [('bootstrap', 'apps/os/npu.c'), ('runtime_worker', 'drivers/npu/npu_service.c')]:
            self.s['chain_'+name] = layout.address(name, unit)

    def api(self, name, *args, task=MAIN, core=6):
        self.current_task = task
        self.el0 = task in (MAIN, APPLICATION_WORKER)   # the application's tasks (checks/os-cpu-return.py KernelPages)
        try:
            result = self.call(core, name, args)
        finally:
            self.el0 = False
        self.el0_touched(name)
        return result

    def sample_thermal(self):
        policy = self.thermal+self.layout.layouts['os_thermal_service']['policy']
        pf = self.layout.layouts['a733_thermal_policy']
        now = self.time//24
        self.put32(policy+pf['cpu_critical_millicelsius'], 70000)
        self.put32(policy+pf['seen'], 31)
        self.put32(policy+pf['sequence'], self.u32(policy+pf['sequence'])+1)
        for index in range(5):
            self.put32(policy+pf['millicelsius']+index*4, 40000)
            self.put64(policy+pf['seen_us']+index*8, now)
        self.s['chain_thermal_reading'] = self.layout.address('record_reading', 'return-thermal.c')
        self.api('chain_thermal_reading', now, 0, task=0x6000, core=0)
        readings=self.layout.address('readings','return-thermal.c')
        self.put64(self.thermal+self.layout.layouts['os_thermal_service']['next_read'],
                   self.u64(readings+self.layout.layouts['os_thermal_readings']['next']))

    def advance_thermal(self, target):
        readings = self.layout.address('readings', 'return-thermal.c')
        next_field = self.layout.layouts['os_thermal_readings']['next']
        while self.u64(readings+next_field)*24 <= target:
            self.time = max(self.time, self.u64(readings+next_field)*24)
            self.sample_thermal()
        self.time = max(self.time, target)

    def notify(self):
        handle=self.uc.reg_read(prior.fault.REGS[0])
        self.notifications[handle]=self.notifications.get(handle,0)+1
        return super().notify()

    def delay(self):
        self.time+=max(1,self.uc.reg_read(prior.fault.REGS[0]))*24000
        return 0

    def clock_now(self):
        self.put64(self.uc.reg_read(prior.fault.REGS[1]), self.time//24)
        return 1

    def activate(self):
        self.put32(self.service+self.sf['activation_state'], 2)
        self.put32(self.service+self.sf['controller']+
                   self.layout.layouts['a733_npu_controller']['state'], 2)
        return 0

    def send(self):
        assert self.current_task == CLIENT
        self.operation = self.uc.reg_read(prior.fault.REGS[2])
        self.ticket += 1
        self.put64(self.uc.reg_read(prior.fault.REGS[6]), self.ticket)
        self.ops.append(dict(operation=self.operation, ticket=self.ticket))
        if self.operation == 2:
            assert bytes(self.uc.mem_read(self.input_window, self.input_bytes)) == bytes(
                self.uc.mem_read(self.input_data, self.input_bytes))
            self.pending_completion = self.words[80+34*self.submits+9]
            self.delivered = False
        if self.operation == 5:
            assert self.submits == 4
            self.put32(self.service+self.sf['controller']+
                       self.layout.layouts['a733_npu_controller']['state'], 9)
            self.put32(self.slot+self.slotf['state'], 8)
            self.put32(self.slot+self.slotf['closed'], 1)
            self.put32(self.service+self.sf['irq_stopped'], 1)
        return 1

    def collect(self):
        if self.operation==2 and not self.delivered:
            assert self.time>=self.pending_completion
            self.uc.mem_write(self.output_window,self.reference) # modeled completed DMA
            self.submits+=1;self.delivered=True
        out = self.uc.reg_read(prior.fault.REGS[3])
        self.uc.mem_write(out, bytes(type_size(self.image.path, 'npu_service_reply')))
        self.put64(out, self.ticket)
        self.put32(out+self.layout.layouts['npu_service_reply']['state'],
                   {1:2, 7:2, 2:6, 5:9}[self.operation])
        return 1

    def platform_stop(self):
        assert self.u32(self.service+self.sf['runtime_parked']) == 1
        assert self.u32(self.service+self.sf['irq_stopped']) == 1
        self.stop_calls += 1
        return 2 if self.stop_calls < 3 else 0

    def code(self, uc, address, size, unused):
        if address == self.layout.address('board_fail_at'):
            self.failures.append((uc.reg_read(prior.fault.REGS[0]), uc.reg_read(prior.fault.REGS[1])))
            self.stop = 'failure';uc.emu_stop();return
        if address==self.layout.address('npu_runtime_collect') and self.operation==2 and self.time<self.pending_completion:
            self.stop='device-wait';uc.emu_stop();return
        if address == self.notify_wait:
            self.stop = 'blocked';uc.emu_stop();return
        super().code(uc, address, size, unused)

    def task(self, name, *, resume=False, device=False):
        self.current_task = BOOTSTRAP if name == 'bootstrap' else CLIENT
        self.core, self.stop = 1, None
        if resume:
            self.uc.context_restore(self.contexts[name])
            if not device:
                notified=self.notifications.pop(self.current_task,0)
                if not notified:self.time+=24000 # expiry of the existing 1 ms client wait
                self.uc.reg_write(prior.fault.REGS[0], notified)
                self.uc.reg_write(UC_ARM64_REG_PC, self.uc.reg_read(UC_ARM64_REG_LR))
        else:
            index = 0 if name == 'bootstrap' else 1
            self.uc.reg_write(UC_ARM64_REG_SP, self.device+self.df['stacks']+(index+1)*4096*8)
            self.uc.reg_write(UC_ARM64_REG_LR, prior.fault.SENTINEL)
            self.uc.reg_write(UC_ARM64_REG_PC, self.layout.address(
                'bootstrap' if name == 'bootstrap' else 'os_npu_client_task',
                'apps/os/npu.c' if name == 'bootstrap' else 'apps/os/npu-client.c'))
        self.uc.emu_start(self.uc.reg_read(UC_ARM64_REG_PC), 0, count=2000000)
        assert self.stop in ('blocked', 'device-wait', 'suspended', 'failure'), (name, self.stop)
        self.contexts[name] = self.uc.context_save()


class Terminal(terminal.Model):
    def from_owner(self, owner):
        epoch = owner.u64(owner.board+owner.layout.layouts['board_application']['session']+
                          owner.layout.layouts['a733_board_session']['epoch'])
        terminal.prior.EPOCH = epoch
        deadline = owner.u64(owner.board+owner.layout.layouts['board_application']['session']+
                             owner.layout.layouts['a733_board_session']['deadline_us'])*24
        # Architectural admission and original local GIC state are modeled,
        # using the real monitor boot/local constructors before terminal entry.
        assert self.call(0, 'a733_qualification_boot', (epoch, deadline)) == 1
        self.call(0, 'os_cpu_return_spi_save')
        for core in range(8):
            assert self.call(core, 'a733_qualification_local', (core, epoch)) == 1
        for index in range(512):
            self.put64(self.s['a733_cached_el3_l2']+index*8, (0x40000000+index*0x200000)|0x725)
            self.put64(self.s['a733_cached_el3_l3']+index*8, (0x42000000+index*4096)|(1<<54)|0x443)
        self.put64(self.s['a733_cached_el3_l2']+128, self.s['a733_cached_el3_l3']|3)
        self.uc.mem_map(POOL, POOL_BYTES)
        # The OS published the reservation before the run: the word is part of the image RAM that the phases share.
        owner.put64(self.s['os_device_ranges_head'], 0x70000000)
        self.uc.mem_write(0x41200000, bytes(owner.uc.mem_read(0x41200000, 0xe00000)))
        self.uc.mem_write(POOL, bytes(owner.uc.mem_read(POOL, POOL_BYTES)))
        self.uc.mem_write(0x07090000,bytes(owner.uc.mem_read(0x07090000,4096)))
        # The NPU's reservation is a published device range (apps/os/device-ranges.h): the terminal cleans the ranges of the
        # list at EL3, whose tables map all DRAM.
        # The page is mapped by the terminal model (checks/os-cpu-return.py Model.pages).
        self.put64(0x70000000, 0); self.put64(0x70000008, 1); self.put64(0x70000010, POOL); self.put64(0x70000018, POOL_BYTES)
        for g in range(2, 13):
            self.put64(self.s['a733_cached_el3_l1']+g*8, (g<<30)|(1<<54)|0x765)
        self.time = owner.time
        # Exact cross-phase copies, including readiness, park, local stop,
        # immutable report and copied application envelope; no healthy flag seeds.
        assert bytes(self.uc.mem_read(0x41200000, 0xe00000)) == bytes(owner.uc.mem_read(0x41200000, 0xe00000))
        contexts = []
        for core in range(8):
            self.core = core
            self.reg[core].update(scr_el3=0x531, sctlr_el3=0x1005,
                ttbr0_el3=self.s['a733_cached_el3_l1'], icc_sre_el3=15, icc_igrpen1_el3=0)
            self.set('tpidr_el1', core);self.set('sctlr_el1', 0x30d01805)
            self.set('tcr_el1', 0x80803519);self.set('icc_sre_el1', 1);self.set('icc_sre_el2', 9)
            assert self.u64(self.local['arrived']+core*8)==0
            self.uc.mem_write(self.s['a733_cached_records']+core*128, struct.pack('<3Q', epoch, core, 3))
            rdist = 0x03460000+core*0x20000
            self.put32(rdist+0x14, 0);self.put32(rdist+0x10100, 0x40000001)
            self.put32(rdist+0x10200, 0x40000001)
            if core:self.put32(0x07052008+core*0x1000, 8)
            self.uc.reg_write(prior.fault.REGS[0], core)
            self.uc.reg_write(UC_ARM64_REG_SP, self.s['a733_cpu_el1_stacks']+(core+1)*32768)
            self.uc.reg_write(UC_ARM64_REG_PC, self.s['a733_qualification_worker_done'])
            contexts.append(self.uc.context_save())
        return epoch, contexts


def producer(args):
    directory = args.out/'producer';directory.mkdir()
    adapter = directory/'npu-observe.c'
    shutil.copyfile(ROOT/'apps/os/npu-observe.c', adapter)
    executable = directory/'producer'
    command = [args.cc, '-std=c11', '-O2', '-ffp-contract=off', '-pthread',
         '-DCPU_NPU_WITH_NPU=1',
        '-I'+str(ROOT/'checks/cpu-npu-concurrency'), '-I'+str(ROOT/'include'),
        '-I'+str(ROOT/'apps/cpu-npu-concurrency'), str(ROOT/'apps/cpu-npu-concurrency/main.c'),
        str(adapter), str(ROOT/'checks/cpu-npu-concurrency/producer.c'), '-o', str(executable)]
    subprocess.run(command, check=True, env=environment(), capture_output=True)
    payload = directory/'payload.bin'
    subprocess.run([str(executable), 'actual', str(payload)], check=True, env=environment(), timeout=20)
    save(directory/'commands.json', [command, [str(executable), 'actual', str(payload)]])
    return payload.read_bytes()


def ownership(image, layout, payload, *, fault=False, fault_task=MAIN):
    native = Native(image, layout, payload)
    assert native.api('os_cpu_return_start') == 1
    native.task('bootstrap');assert native.stop == 'blocked'
    assert native.u32(native.n+native.nf['active']) == 1
    assert not native.u32(native.n+native.nf['stopped'])
    native.task('client');assert native.stop == 'blocked'
    native.sample_thermal()
    native.api('ss_activity_begin')
    for job in range(4):
        arguments = (0, native.input_data, native.input_bytes, native.output, len(native.reference), 100000)
        assert native.api('ss_npu_run', *arguments) == 0
        native.task('client', resume=True)
        if native.stop=='device-wait':
            native.advance_thermal(native.pending_completion)
            native.task('client',resume=True,device=True)
        assert native.stop=='blocked'
        assert native.api('ss_npu_run', *arguments) == 1
        native.sample_thermal()
        assert bytes(native.uc.mem_read(native.output, len(native.reference))) == native.reference
    native.advance_thermal(native.words[9]+24)
    native.api('ss_activity_end', 1)
    assert native.api('ss_npu_close') == 0
    native.task('client', resume=True);assert native.stop == 'blocked'
    assert native.api('ss_npu_close') == 1
    assert native.u32(native.n+native.nf['closed']) == 1
    assert not native.u32(native.service+native.sf['runtime_parked'])
    assert not native.u32(native.n+native.nf['stopped'])
    assert not native.u32(native.report+12)
    if fault:
        notify = next(name for name in ('vTaskGenericNotifyGiveFromISR', 'vTaskNotifyGiveFromISR')
                      if name in layout.symbols)
        native.hooks[layout.address(notify)] = lambda: 0
        native.api('npu_service_irq_notify_from_isr', native.service, 1, native.output, task=CLIENT, core=1)
        assert native.u32(native.service+native.sf['irq_pending']) == 3
        native.uc.mem_write(native.payload_address, payload)
        if fault_task == MAIN:
            native.api('ss_finish', native.payload_address, len(payload))
        else:
            # Another task of the application reaches the same retirement failure (fail_at in apps/os/npu.c): it is at EL0
            # too, and must not read the slot table.
            native.api('os_npu_retirement_check', task=fault_task)
        assert native.stop == 'failure' and native.failures[0][0]&0xf000 == 0xa000
        assert not native.u32(native.layout.address('os_npu_first_failure')+
                              native.layout.layouts['os_npu_failure']['slot_state'])
        assert native.failures[0][1]&3 == 3
        assert not native.u32(layout.address('finished', 'apps/os/application.c'))
        assert not native.u32(native.flags['ready']) and not native.u32(native.flags['closing'])
        return native
    native.api('chain_runtime_worker', native.service, task=WORKER, core=1)
    assert native.stop == 'blocked' and native.u32(native.service+native.sf['runtime_parked']) == 1
    native.task('bootstrap', resume=True)
    assert native.stop == 'suspended' and native.stop_calls == 3
    assert native.u32(native.n+native.nf['stopped']) == 1
    assert not native.u32(native.report+12), 'Bootstrap cannot publish the client result.'
    native.task('client', resume=True);assert native.stop == 'suspended'
    assert native.u32(native.report+12) == 1 and native.u32(native.report+56) == 1
    assert native.u32(native.report+40) == 4 and native.u32(native.report+52) == 0
    assert native.api('os_npu_complete') == 1
    native.uc.mem_write(native.payload_address, payload)
    native.api('ss_finish', native.payload_address, len(payload));assert native.stop == 'suspended'
    assert native.u32(layout.address('finished', 'apps/os/application.c')) == 1
    assert native.u32(native.flags['closing']) == 1
    native.api('os_thermal_task', task=0x6000, core=0)
    assert native.stop == 'suspended' and native.u32(native.flags['thermal_done']) == 1
    native.api('os_cpu_return_supervise', task=0x7000, core=0)
    assert not native.u32(native.flags['ready']) and native.u32(native.flags['pmic_stop']) == 1
    native.api('os_cpu_return_pmic', task=0x8000, core=0)
    assert native.stop == 'suspended' and native.u32(native.flags['pmic_done']) == 1
    native.api('os_cpu_return_supervise', task=0x7000, core=0)
    assert native.stop == 'suspended' and native.u32(native.flags['ready']) == 1
    assert not native.failures
    return native


def runner_cleanup(args, machine, geometry_host, epoch, generation, frozen):
    """Run the production runner against exact RAM exported by the native model."""
    sys.path.insert(0, str(args.tool_tree/'tools'))
    board = module('chain_production_board', args.tool_tree/'tools/board.py')
    images = board.images
    assert callable(getattr(images,'npu_on_required',None)), 'Current PCK4 admission runner required.'
    geo = geometry_host.geometry(args.elf)
    output = args.out/'runner'
    parser = argparse.ArgumentParser()
    board.run_options(parser)
    options = parser.parse_args([str(args.elf), '--port', '3-1', '--restore-rails', '--preserve-dram',
                                 '--decoder', str(frozen), '--host-tree', str(args.tool_tree)])
    options.evidence = output
    uploads = []
    for index, (address, data) in enumerate(machine.image.segments):
        path = args.out/f'upload-segment-{index}.bin';path.write_bytes(data)
        uploads.append((address, path))
    plan = dict(format='generic', geometry=geo, return_host=geometry_host,
                image_ram=geometry_host.IMAGE_RAM, input_regions=[], uploads=uploads,
                launch=['exe','0x52000'], watchdog=dict(mode=0xb1,session_seconds=60),
                epoch=epoch,generation=generation)
    plan['preserve_regions'] = images.preservation_regions(plan)
    owners_seen = []

    class Transport(board.Live):
        def __init__(self, *values):
            super().__init__(*values)
            self.events, self.clock, self.helper_running = [], 0, False
            self.record['physical_access']=False
            self.record['modeled_transport']=True
            self.identity=('modeled-current-instance',)
            self.memory = [(address, bytearray(bytes([17+index])*size)) for index,(address,size)
                           in enumerate(plan['preserve_regions'])]
            self.saved = {address:bytes(data) for address,data in self.memory}
            # Not preserved, only read: the first page of the published device range list, which the OS took from the ledger at run
            # time. FEL reads any DRAM; the OS cleans the page to DRAM when it publishes (apps/os/large.c).
            self.readable = [(0x70000000, bytes(machine.uc.mem_read(0x70000000, 4096)))]
            self.elapsed_now = lambda:self.clock
            owners_seen.append(self)
        @contextlib.contextmanager
        def locked(self):
            yield self
        def sanity(self):self.events.append('modeled-current-instance')
        def reset(self):self.events.append('modeled-fresh-ROM')
        def dram(self):self.events.append('modeled-DDR-authority')
        def journal(self):pass
        def prepare(self, info):
            if images.npu_on_required(info):self.npu_on_state()
            return plan
        def quarantine(self, reason):self.quarantined=True;self.record['quarantined']=True
        def read(self, address, size, path):
            for begin,data in self.memory+self.readable:
                if begin<=address and address+size<=begin+len(data):
                    raw=bytes(data[address-begin:address-begin+size]);path.write_bytes(raw);return raw
            raise AssertionError(('read outside preserved native geometry',hex(address),size))
        def upload(self, address, path, *, expected_sha256=None):
            data=Path(path).read_bytes()
            assert expected_sha256 is None or hashlib.sha256(data).hexdigest()==expected_sha256
            self.put(address,data)
            self.events.append(('upload',address,len(data)))
        def put(self, address, raw):
            for begin,data in self.memory:
                if begin<=address and address+len(raw)<=begin+len(data):
                    data[address-begin:address-begin+len(raw)]=raw;return
            raise AssertionError(('write outside preserved native geometry',hex(address),len(raw)))
        def arm(self,*values,**kwargs):self.armed=True
        def wait(self,*values):self.clock+=1
        def live_state(self,*values):
            return dict(words={hex(0x08871000+4096*domain+offset):value for domain in range(3)
                for offset,value in ((0,board.state.BOOT_PLL[domain]),(0x1c,board.state.BOOT_MUX[domain]))})
        def rails(self, phase='rails'):
            if phase=='rails':
                self.events.append('modeled-entry-DCDC3-DCDC5-800mV');return
            assert phase=='restore-rails'
            assert self.record['native_retirement_validated'] and self.record['dram_restored']
            assert all(bytes(data)==self.saved[address] for address,data in self.memory)
            self.events.append('actual-post-helper-admission')
            board.Live.rails(self,phase)
            assert all(bytes(data)==self.saved[address] for address,data in self.memory)
            self.events.append('actual-post-helper-restoration')
        def tx(self,*values):
            if values and all(values[i]=='readl' for i in range(0,len(values),2)):
                modeled={0x07090160:0x10008002,board.usb_power.CFG:1,
                         board.usb_power.DAT:0,board.usb_power.PULL:0,**images.NPU_ENTRY_GATES,
                         **{address:8 for address in images.NPU_PCK4}}
                return ' '.join(hex(modeled[int(values[i],0)]) for i in range(1,len(values),2))
            if values==('exe','0x52000'):
                for address,data in self.memory:
                    self.put(address,bytes(machine.uc.mem_read(address,len(data))))
                self.events.append('native-RAM-import');return ''
            assert values==('reset64','0x41000000'),values
            self.helper_running=True
            self.put(board.POST_RAIL_HELPER_DRAM[0],bytes([109])*board.POST_RAIL_HELPER_DRAM[1])
            followup=images.load('chain_post_helper_parser',args.tool_tree/'apps/os/cpu-return-followup.py')
            lines=[f'{name} {phase} pll={pll:#x} mux={mux:#x} mhz={mhz} pat0=0 pat1=0 lfm=0'
                   for name,pll,mux,mhz in board.state.BOOT_ROWS for phase in ('before','after')]
            lines+=['SUPPLY dcdc5_uv=1000000 dcdc3_uv=800000 bin=4 dcxo_hz=26000000',
                    'VOLT dcdc5_selector=30 dcdc5_uv=800000',followup.COMPLETE]
            data=('\n'.join(lines)+'\n').encode();raw=bytearray(16384)
            struct.pack_into('<4Q',raw,0,0x534b4552,5,0,len(data));raw[256:256+len(data)]=data
            self.put(0x42000000,raw);return ''
        def returned(self):
            self.armed=False
            if self.helper_running:
                self.helper_running=False;return 0xb1,0,{}
            offsets={'tag':0x100,'last':0x104,'time':0x10c,'fault':0x110,
                     'esr':0x114,'elr':0x118,'far':0x11c}
            words={name:machine.u32(0x07090000+offset) for name,offset in offsets.items()}
            return machine.u32(0x02050014),0,words

    with contextlib.redirect_stdout(io.StringIO()):
        result = board.run_image(options, Transport)
    assert result['outcome']=='returned',result
    assert result['native_retirement_validated'] and result['dram_restored'] and result['rails_restored']
    assert result['decoded']['jobs']==4 and result['decoded']['overlap_mask']==15
    assert result['rail_runs']['restore-rails']['image_sha256']==board.POST_RAIL_HELPER_SHA256
    transport=owners_seen[0]
    assert all(bytes(data)==transport.saved[address] for address,data in transport.memory)
    for filename in ('return-report.bin','output-record.bin','npu-stop.bin'):
        assert (output/filename).read_bytes()==(args.out/filename).read_bytes(),filename
    save(args.out/'runner-events.json',transport.events)
    return dict(result_sha256=images.sha(output/'result.json'),outcome=result['outcome'],
                dram_restored=True,post_helper_sha256=board.POST_RAIL_HELPER_SHA256,
                decoder_image_sha256=result['image_sha256'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--tool-tree', type=Path, default=ROOT)
    parser.add_argument('--cc', default='/usr/bin/cc')
    parser.add_argument('--payload', type=Path, help='Reuse an unchanged successful actual producer payload.')
    args = parser.parse_args();args.out.mkdir(parents=True, exist_ok=False)
    payload = args.payload.read_bytes() if args.payload else producer(args)
    names = {'board_application', 'a733_board_session', 'board_provider', 'a733_native_usb',
        'a733_pmic', 'a733_pmic_slot', 'a733_platform_locks', 'os_thermal_service',
        'os_npu_application', 'os_npu_device', 'os_npu_failure', 'npu_runtime', 'npu_runtime_slot', 'npu_service',
        'a733_npu_controller', 'a733_npu_startup', 'ss_grant', 'npu_service_reply',
        'os_session', 'os_npu_session', 'nbg_assembly_layout', 'npu_runtime_tensor',
        'a733_thermal_policy', 'os_thermal_readings', 'npu_service_activation', 'a733_cached_runtime'}
    layout = Image(args.elf, names)
    image = prior.fault.Image(args.elf)
    native = ownership(image, layout, payload)
    normal = bytes(native.uc.mem_read(0x41200000, 0xe00000))
    pool = bytes(native.uc.mem_read(POOL, POOL_BYTES))
    (args.out/'ownership-normal.bin').write_bytes(normal)
    (args.out/'ownership-pool.bin').write_bytes(pool)
    returns = Terminal(terminal.prior.fault.Image(args.elf))
    epoch, contexts = returns.from_owner(native)
    assert returns.execute(contexts, budget=4000000) == 'rom'
    assert not returns.postclean
    assert all(returns.u64(returns.local['arrived']+core*8)==epoch for core in range(8))
    # The caches are cleaned by set/way, one sweep per core (checks/os-cpu-return.py checks every operand); the published NPU
    # range is walked and validated by each core, whose counters are in image RAM.
    assert [len(x) for x in returns.sweeps] == [18944]+[2560]*5+[5120]*2
    walk = image.symbols['os_cpu_return_walk']
    assert all(returns.u64(walk+16*core) and returns.u64(walk+16*core+8) >= returns.u64(walk+16*core) for core in range(8))
    record = layout.address('ss_result_record')
    envelope = bytes(returns.uc.mem_read(record, 56+len(payload)))
    stop = bytes(returns.uc.mem_read(native.report, 104))
    cpu_report = bytes(returns.uc.mem_read(returns.report, 8880))
    assert envelope == bytes(native.uc.mem_read(record, len(envelope)))
    assert stop == bytes(native.uc.mem_read(native.report, 104))
    (args.out/'output-record.bin').write_bytes(envelope)
    (args.out/'npu-stop.bin').write_bytes(stop)
    (args.out/'return-report.bin').write_bytes(cpu_report)
    frozen = args.out/'decoder.py'
    binding = receipt.freeze_decoder(args.elf, frozen, 1)
    decoder = module('chain_frozen_decoder', frozen)
    host = module('chain_native_return_host', args.tool_tree/'apps/os/cpu-return-host.py')
    generation = native.u64(native.board+layout.layouts['board_application']['session']+
                            layout.layouts['a733_board_session']['generation'])
    assert host.host.decode_result(envelope, epoch, generation, 1728) == payload
    assert decoder.decode(payload)['jobs'] == 4
    host.mandatory_return({'return-report':cpu_report, 'npu-stop':stop}, {'npu_stop':{}}, epoch, generation)
    cleanup = runner_cleanup(args, returns, host, epoch, generation, frozen)
    failed = ownership(image, layout, payload, fault=True)
    ownership(image, layout, payload, fault=True, fault_task=APPLICATION_WORKER)
    save(args.out/'result.json', dict(passed=True, physical_access=False, model_boundary=__doc__,
        image_sha256=hashlib.sha256(args.elf.read_bytes()).hexdigest(), binding=binding,
        epoch=epoch, generation=generation, sdk_operations=native.ops, sdk_jobs=native.submits,
        local_stop_calls=native.stop_calls, sweep_operations=[len(x) for x in returns.sweeps], runner_cleanup=cleanup,
        stages=['actual-host-GEMM-receipt','actual-public-SDK-mailbox-and-copy',
                'actual-CLOSE','actual-worker-park','actual-bootstrap-local-stop',
                'actual-immutable-report','actual-ss_finish','actual-owner-retirement',
                'unmodified-state-to-eight-core-worker-done-arrival-barrier','actual-native-reports',
                'production-runner-RAM-restoration','approved-post-ROM-helper-admission-and-RAM-restoration'],
        upstream_fault=dict(first_failure=failed.failures[0], terminal_entered=False),
        artifacts={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in
                   (args.out/'output-record.bin',args.out/'npu-stop.bin',args.out/'return-report.bin')},
        sources={name:hashlib.sha256(((ROOT if name.startswith('checks/') else args.tool_tree)/name).read_bytes()).hexdigest() for name in CHECK_INPUTS}))
    print('PASS composed CPU/NPU native return chain and upstream late-IRQ refusal')


if __name__ == '__main__':
    main()
