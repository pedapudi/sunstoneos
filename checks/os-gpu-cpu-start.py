#!/usr/bin/env python3
"""Check actual combined OS GPU startup ordering and owner exclusion; no inference or MMIO."""
import argparse
import hashlib
import json
from pathlib import Path
from harness import Image
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_INTR
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--image', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--npu-owners-only', action='store_true', help='only combined NPU owner and PCK exclusions')
a = parser.parse_args()
image = Image(a.image, layout_names=['board_application', 'os_session', 'a733_cpu_clock_grant_record', 'os_npu_session'])
unit = '/os-image/recognition-gpu.cc'
results = []
cases = ('npu-firmware-owner', 'npu-session-owner', 'npu-pck-client', 'distinct-owner') if a.npu_owners_only else (
    'ready', 'delayed', 'expired', 'revoked', 'firmware-owner', 'session-owner', 'distinct-owner')
for case in cases:
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
    def ret(value=0):
        uc.reg_write(UC_ARM64_REG_X0, value)
        uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))
    ownership = a.npu_owners_only or case.endswith('owner')
    state = dict(hashes=0, waits=0, ready_reads=0, opens=0, providers=0, suspends=0)
    hooks = {image.address(n, '/apps/mimus/parallel_runtime.cc' if n == 'mimus_parallel_bind' else None): n for n in (
        'mimus_parallel_bind', 'mimus_board_live', 'mimus_board_session_live',
        'mimus_board_progress', 'storage_digest_init', 'storage_digest_add', 'storage_digest_end',
        'os_cpu_clock_ready', 'vTaskDelay', 'os_gpu_open', 'platform_puts', 'vTaskSuspend',
        'xTaskGetSchedulerState', 'xTaskGetCurrentTaskHandle', 'os_session_admitted', 'a733_gpu_platform_native_init')}
    def hook(machine, address, size, unused):
        name = hooks.get(address)
        if not name:
            return
        x = [machine.reg_read(UC_ARM64_REG_X0 + i) for i in range(3)]
        if name == 'storage_digest_end':
            expected = image.address('mimus_asset_sha256', unit) + state['hashes'] * 32
            uc.mem_write(x[1], bytes(uc.mem_read(expected, 32)))
            state['hashes'] += 1
            ret(1)
        elif name == 'os_cpu_clock_ready':
            assert state['hashes'] == 3 and not state['opens']
            state['ready_reads'] += 1
            ret(case == 'ready' or case == 'delayed' and state['waits'] == 3)
        elif name == 'mimus_board_live':
            ret(not (case == 'revoked' and state['ready_reads'] and state['waits'] == 2))
        elif name == 'vTaskDelay':
            assert x[0] == 1 and state['ready_reads'] and not state['opens']
            state['waits'] += 1
            ret()
        elif name == 'os_gpu_open':
            assert state['hashes'] == 3 and case in ('ready', 'delayed')
            state['opens'] += 1
            uc.emu_stop()
        elif name == 'vTaskSuspend':
            state['suspends'] += 1
            if state['suspends'] == 1:
                ret()  # An attempted resume must suspend again, without OPEN.
            else:
                uc.emu_stop()
        elif name == 'xTaskGetSchedulerState':
            assert ownership
            ret(1)  # taskSCHEDULER_NOT_STARTED
        elif name == 'os_session_admitted':
            assert ownership
            ret(0x80002000)
        elif name == 'a733_gpu_platform_native_init':
            assert ownership
            state['providers'] += 1
            uc.emu_stop()
        else:
            ret(1)
    uc.hook_add(UC_HOOK_CODE, hook)
    uc.hook_add(UC_HOOK_INTR, lambda machine, number, unused: None)  # taskYIELD SVC, modeled scheduler
    uc.reg_write(UC_ARM64_REG_SP, 0x8000f000)
    uc.reg_write(UC_ARM64_REG_LR, 0x80000000)
    if ownership:
        owner = {'firmware-owner': 0x4f53475055464d31, 'session-owner': 0x4f534731}.get(case,0x43505531)
        if a.npu_owners_only:
            npu = 0x80002000 + image.layouts['os_session']['npu']
            npu_owner = {'npu-firmware-owner':0x4f53475055464d31,'npu-session-owner':0x4f534731}.get(case,19)
            put(npu+image.layouts['os_npu_session']['owner'],npu_owner,8)
            put(npu+image.layouts['os_npu_session']['pck_client'],7 if case=='npu-pck-client' else 1)
        offset = image.layouts['os_session']['cpu_clock'] + image.layouts['a733_cpu_clock_grant_record']['owner']
        put(0x80002000 + offset, owner, 8)
        put(image.address('os_recognizer'), 1, 8)
        uc.reg_write(UC_ARM64_REG_X0, 6)
        uc.emu_start(image.address('a733_gpu_mimus_start'), 0x80000000, count=1000000)
        assert state['providers'] == (case == 'distinct-owner'), state
        if case != 'distinct-owner':
            # Pre-provider refusal preserves the existing startup convention:
            # the OS can start with GPU OPEN disabled. Host admission rejects
            # these aliases before a combined image is launched.
            assert uc.reg_read(UC_ARM64_REG_PC) == 0x80000000 and uc.reg_read(UC_ARM64_REG_X0) == 1
            uc.reg_write(UC_ARM64_REG_LR, 0x80000000)
            uc.emu_start(image.address('a733_gpu_mimus_open'), 0x80000000, count=1000)
            assert uc.reg_read(UC_ARM64_REG_PC) == 0x80000000 and uc.reg_read(UC_ARM64_REG_X0) == 0
    else:
        uc.emu_start(image.address('recognize', unit), 0x80000000, count=2000000)
        assert state['hashes'] == 3
        assert state['waits'] == {'ready': 0, 'delayed': 3, 'expired': 1000, 'revoked': 2}[case], state
        if case in ('ready', 'delayed'):
            assert state['opens'] == 1 and not state['suspends']
        else:
            assert state['suspends'] == 2 and not state['opens']
            assert get(image.address('state', unit)) == 2 and get(image.address('stopped', unit), 1) == 1
            assert get(image.address('board') + image.layouts['board_application']['fatal']) == 1
    results.append(dict(case=case, passed=True, observed=state))
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(json.dumps(dict(passed=True, physical_access=False,
    image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(), cases=results,
    scope='Actual generated recognizer and native GPU startup; authentication digests, scheduler, CPU readiness and admitted-session accessor modeled. Stops before GPU provider construction or OPEN execution. No inference, firmware or hardware qualification.'), indent=2) + '\n')
print('PASS combined CPU/GPU startup:', len(results), 'cases')
