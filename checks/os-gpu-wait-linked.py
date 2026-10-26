#!/usr/bin/env python3
"""Run GPU timeout through the actual linked hook, dispatcher and recognizer failure path."""
import argparse
import hashlib
import json
from pathlib import Path
from elftools.elf.elffile import ELFFile
from harness import Image
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--image', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
image = Image(a.image, layout_names=['board_application', 'a733_board_session', 'pvr_mimus_request',
                                    'os_dispatch_record', 'Call'])
with a.image.open('rb') as stream:
    matches = []
    for cu in ELFFile(stream).get_dwarf_info().iter_CUs():
        if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith('/apps/os/gpu.c'):
            continue
        for die in cu.iter_DIEs():
            if die.tag == 'DW_TAG_variable' and die.attributes.get('DW_AT_name') and die.attributes['DW_AT_name'].value == b'gpu':
                typ = die.get_DIE_from_attribute('DW_AT_type')
                matches.append({c.attributes['DW_AT_name'].value.decode(): c.attributes['DW_AT_data_member_location'].value
                                for c in typ.iter_children() if c.tag == 'DW_TAG_member'})
    assert len(matches) == 1, 'GPU private state type must be unambiguous'
    image.layouts['os_gpu_state'] = matches[0]

results = []
for case in ('missing-job-reply', 'late-job-reply', 'accepted-open-no-reply', 'invalid-job-reply', 'timely-record'):
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(0x41000000, 0x1200000)
    uc.mem_map(0x80000000, 0x10000)
    for address, data in image.parts:
        if data:
            uc.mem_write(address, data)

    def put(address, value, size=4):
        uc.mem_write(address, int(value).to_bytes(size, 'little'))

    def get(address, size=4):
        return int.from_bytes(uc.mem_read(address, size), 'little')

    def field(base, typ, name):
        return base + image.layouts[typ][name]

    def setf(base, typ, name, value, size=4):
        put(field(base, typ, name), value, size)

    def ret(value=0):
        uc.reg_write(UC_ARM64_REG_X0, value)
        uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))

    def call(name, *args, unit=None, terminal=False):
        uc.reg_write(UC_ARM64_REG_SP, 0x8000f000)
        uc.reg_write(UC_ARM64_REG_LR, 0x80000000)
        for i, value in enumerate(args):
            uc.reg_write(UC_ARM64_REG_X0 + i, value)
        uc.emu_start(image.address(name, unit), 0x80000000, count=1000000)
        if terminal:
            assert state['suspends'] == 2 and uc.reg_read(UC_ARM64_REG_PC) != 0x80000000, 'Owner returned or budget exhausted'
        else:
            assert uc.reg_read(UC_ARM64_REG_PC) == 0x80000000, 'Instruction budget exhausted'
        return uc.reg_read(UC_ARM64_REG_X0)

    gpu = image.address('gpu', '/apps/os/gpu.c')
    board = image.address('board')
    hook_call = image.address('call', '/apps/os/gpu_hook.cc')
    record = image.address('os_dispatch_record')
    state = dict(time=100, receives=0, submissions=0, requests=0, suspends=0, late_available=False,
                 counter_reads=0, tick_reads=0, task_queries=0)
    setf(gpu, 'os_gpu_state', 'started', 1)
    setf(gpu, 'os_gpu_state', 'opened', case != 'accepted-open-no-reply')
    setf(gpu, 'os_gpu_state', 'component', 0x80001000, 8)
    put(image.address('os_recognizer'), 0x1234, 8)
    setf(board, 'board_application', 'live_until', 3000000, 8)
    setf(field(board, 'board_application', 'session'), 'a733_board_session', 'deadline_us', 60000000, 8)
    # These represent the engine's borrowed input and output descriptor.
    put(0x80003000, 0x123456789abcdef0, 8)
    put(0x80002000, 0xfedcba9876543210, 8)
    hooks = {image.address(n): n for n in ('a733_smp_counter', 'xTaskGetTickCount', 'xTaskGetCurrentTaskHandle',
             'pvr_mimus_service_submit', 'pvr_mimus_service_receive', 'pvr_gpu_service_send_epoch',
             'pvr_gpu_service_collect', 'vTaskSuspend')}

    def hook(machine, address, size, unused):
        name = hooks.get(address)
        if not name:
            return
        args = [machine.reg_read(UC_ARM64_REG_X0 + i) for i in range(5)]
        if name == 'a733_smp_counter':
            state['counter_reads'] += 1
            ret(state['time'] * 24)
        elif name == 'xTaskGetTickCount':
            state['tick_reads'] += 1
            ret(state['time'] // 1000)
        elif name == 'xTaskGetCurrentTaskHandle':
            state['task_queries'] += 1
            ret(0x1234)
        elif name == 'pvr_mimus_service_submit':
            assert not state['submissions'] and not state['suspends']
            state['submissions'] += 1
            ret(1)
        elif name == 'pvr_mimus_service_receive':
            assert args[2] == 10 and not state['suspends']
            state['receives'] += 1
            if case in ('late-job-reply', 'invalid-job-reply', 'timely-record'):
                if case == 'late-job-reply':
                    state['time'] = 2000100
                put(args[1], 1)
                put(args[1] + 4, 99 if case == 'invalid-job-reply' else 0)
                ret(1)
            else:
                state['time'] += 10000
                ret(0)
        elif name == 'pvr_gpu_service_send_epoch':
            assert case == 'accepted-open-no-reply' and not state['requests'] and args[3] == 10
            state['requests'] += 1
            put(args[4], 77, 8)
            ret(1)
        elif name == 'pvr_gpu_service_collect':
            assert args[1] == 77 and args[3] == 10 and not state['suspends']
            state['time'] += 10000
            ret(0)
        elif name == 'vTaskSuspend':
            assert args[0] == 0
            state['suspends'] += 1
            if state['suspends'] == 1:
                # An unexpected task wake with a newly available late reply
                # cannot escape the real generated failed() suspension loop.
                state['late_available'] = True
                ret()
            else:
                machine.emu_stop()

    uc.hook_add(UC_HOOK_CODE, hook)
    if case == 'accepted-open-no-reply':
        call('os_dispatch_open', terminal=True)
        assert get(field(record, 'os_dispatch_record', 'open_failed')) == 0
        assert get(field(gpu, 'os_gpu_state', 'accepted_ticket'), 8) == 77
    else:
        job = 0x80004000
        setf(job, 'pvr_mimus_request', 'rows', 1)
        assert call('a733_gpu_mimus_begin', job) == 1
        if case == 'timely-record':
            assert call('a733_gpu_mimus_finish', 0x80005000) == 1
            assert get(0x80005000) == 1 and get(0x80005004) == 0
            assert get(field(gpu, 'os_gpu_state', 'job_pending')) == 0
            assert get(field(gpu, 'os_gpu_state', 'failed')) == 0
            assert (state['counter_reads'], state['tick_reads'], state['task_queries']) == (9, 3, 2)
            assert state['submissions'] == state['receives'] == 1 and not state['requests']
            results.append(dict(case=case, passed=True, model=state))
            continue
        setf(hook_call, 'Call', 'rows', 1, 8)
        setf(hook_call, 'Call', 'in_flight', 1, 1)
        call('Finish', 0, 0x80002000, unit='/apps/os/gpu_hook.cc', terminal=True)
        assert get(field(hook_call, 'Call', 'in_flight'), 1) == 1
        assert get(field(gpu, 'os_gpu_state', 'job_pending')) == 1
        assert state['submissions'] == 1 and not state['requests']
    assert get(field(record, 'os_dispatch_record', 'failure')) == (2 if case == 'invalid-job-reply' else 3)
    assert get(image.address('os_dispatch_active')) == 0
    assert get(field(board, 'board_application', 'fatal')) == 1
    assert get(image.address('state', '/os-image/recognition-gpu.cc')) == 2
    assert get(image.address('stopped', '/os-image/recognition-gpu.cc'), 1) == 1
    assert get(0x80003000, 8) == 0x123456789abcdef0 and get(0x80002000, 8) == 0xfedcba9876543210
    results.append(dict(case=case, passed=True, model=state))
a.output.write_text(json.dumps(dict(passed=True, physical_access=False,
    image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(), cases=results,
    scope='Actual full-image caller, hook, dispatch and generated failed() path; service queues and scheduler modeled. '
          'No GPU MMIO, firmware, model inference, drain or physical recovery qualification.'), indent=2) + '\n')
print('PASS 5 full-image GPU reply cases: retained failures, timely record and counted caller overhead')
