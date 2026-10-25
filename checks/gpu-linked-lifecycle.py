#!/usr/bin/env python3
"""Execute selected GPU boundaries in the full linked finite or OS image."""
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
p.add_argument('--profile', choices=('os', 'finite'), required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
names = ['board_application', 'a733_board_session', 'pvr_gpu_service_request', 'pvr_gpu_service_reply',
         'gpu_application', 'a733_gpu_backend', 'a733_gpu_platform', 'a733_gpu_reserved',
         'pvr_gpu_service', 'a733_gpu_backend_external']
image = Image(a.image, layout_names=names)
layouts = image.layouts
if a.profile == 'os':
    # Resolve the anonymous private state by its defining unit and actual type.
    with a.image.open('rb') as f:
        matches = []
        for cu in ELFFile(f).get_dwarf_info().iter_CUs():
            if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith('/apps/os/gpu.c'): continue
            for die in cu.iter_DIEs():
                if die.tag == 'DW_TAG_variable' and die.attributes.get('DW_AT_name') and die.attributes['DW_AT_name'].value == b'gpu':
                    typ = die.get_DIE_from_attribute('DW_AT_type')
                    matches.append({c.attributes['DW_AT_name'].value.decode(): c.attributes['DW_AT_data_member_location'].value
                                    for c in typ.iter_children() if c.tag == 'DW_TAG_member'})
        assert len(matches) == 1
        layouts['os_gpu_state'] = matches[0]
identity = hashlib.sha256(a.image.read_bytes()).hexdigest()
results = []
for case in (('retire', 'close-busy', 'critical-during-close') if a.profile == 'os' else
             ('power-pending', 'power-failed', 'power-ready')):
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(0x41000000, 0x1200000); uc.mem_map(0x80000000, 0x10000)
    for address, data in image.parts:
        if data: uc.mem_write(address, data)
    def put(address, value, size=4): uc.mem_write(address, int(value).to_bytes(size, 'little'))
    def get(address, size=4): return int.from_bytes(uc.mem_read(address, size), 'little')
    def field(base, typ, name): return base + layouts[typ][name]
    def setf(base, typ, name, value, size=4): put(field(base, typ, name), value, size)
    def ret(value=0):
        uc.reg_write(UC_ARM64_REG_X0, value & ((1 << 64) - 1))
        uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))
    def call(name, *args, unit=None):
        uc.reg_write(UC_ARM64_REG_SP, 0x8000f000); uc.reg_write(UC_ARM64_REG_LR, 0x80000000)
        for i, value in enumerate(args): uc.reg_write(UC_ARM64_REG_X0 + i, value)
        uc.emu_start(image.address(name, unit), 0x80000000, count=100000)
        assert uc.reg_read(UC_ARM64_REG_PC) == 0x80000000, 'instruction budget exhausted'
        return uc.reg_read(UC_ARM64_REG_X0)
    state = dict(time=100, live=1, waiting=0, sent=[], pending=0, collects=0)
    if a.profile == 'os':
        gpu = image.address('gpu', '/apps/os/gpu.c'); board = image.address('board')
        for name in ('started', 'opened'): setf(gpu, 'os_gpu_state', name, 1)
        setf(gpu, 'os_gpu_state', 'epoch', 1, 8)
        put(image.address('os_recognizer'), 0x1234, 8)
        setf(board, 'board_application', 'live_until', 3000000, 8)
        setf(field(board, 'board_application', 'session'), 'a733_board_session', 'deadline_us', 60000000, 8)
        hooks = {image.address(n): n for n in ('board_live', 'a733_smp_counter', 'xTaskGetCurrentTaskHandle',
            'os_thermal_throttled', 'os_dispatch_waiting', 'vTaskDelay', 'pvr_gpu_service_send_epoch', 'pvr_gpu_service_collect')}
        def hook(machine, address, size, unused):
            name = hooks.get(address)
            if not name: return
            args = [machine.reg_read(UC_ARM64_REG_X0 + i) for i in range(5)]
            if name == 'board_live': ret(state['live'])
            elif name == 'a733_smp_counter': ret(state['time'] * 24)
            elif name == 'xTaskGetCurrentTaskHandle': ret(0x1234)
            elif name == 'os_thermal_throttled': ret(0)
            elif name == 'os_dispatch_waiting': state['waiting'] = args[0]; ret()
            elif name == 'vTaskDelay': assert args[0] == 1; state['time'] += 1000; ret()
            elif name == 'pvr_gpu_service_send_epoch':
                assert args[0] == field(gpu, 'os_gpu_state', 'service') and args[1] == 1 and args[3] == 10
                assert not state['pending'] and get(field(gpu, 'os_gpu_state', 'retired')) == 1 and state['waiting']
                operation = get(field(args[2], 'pvr_gpu_service_request', 'operation'))
                assert operation == (2 if not state['sent'] else 0)
                state['sent'].append(operation); state['pending'] = len(state['sent']); put(args[4], state['pending'], 8); ret(1)
            else:
                assert args[1] == state['pending'] and args[3] == 10
                state['collects'] += 1
                for key, value, width in [('ticket', args[1], 8), ('epoch', 1, 8), ('result', 3 if case == 'close-busy' else 0, 4),
                                          ('phase', 4 if state['collects'] == 1 else 5, 4)]:
                    setf(args[2], 'pvr_gpu_service_reply', key, value, width)
                if case == 'critical-during-close': state['live'] = 0
                state['pending'] = 0; ret(1)
        uc.hook_add(UC_HOOK_CODE, hook)
        result = call('a733_gpu_mimus_retire')
        assert result == (case == 'retire')
        assert get(field(gpu, 'os_gpu_state', 'retired')) == 1
        assert get(field(gpu, 'os_gpu_state', 'failed')) == (case != 'retire')
        assert get(field(gpu, 'os_gpu_state', 'parked')) == (case == 'retire')
        before = list(state['sent']); state['live'] = 1
        assert call('a733_gpu_mimus_open') == 0
        assert call('a733_gpu_mimus_begin', 0) == (0 if case == 'retire' else 0xffffffff)
        assert call('a733_gpu_mimus_thermal') == (0 if case == 'retire' else 0xffffffff)
        assert state['sent'] == before and state['waiting'] == (case != 'retire')
    else:
        app = image.address('app'); gpu = field(app, 'gpu_application', 'backend')
        power = field(app, 'gpu_application', 'power'); service = field(app, 'gpu_application', 'service')
        for key, value, width in [('service', service, 8), ('power', power, 8), ('initialized', 1, 4), ('phase', 2, 4), ('deadline', 10000, 8)]:
            setf(gpu, 'a733_gpu_backend', key, value, width)
        # The counter/output guard and platform transition are the modeled
        # boundary. The linked backend's phase/token/lease gate remains real.
        hooks = {image.address('now', '/platform/a733/gpu_backend.c'): 'now', image.address('a733_gpu_platform_poll'): 'poll'}
        def hook(machine, address, size, unused):
            name = hooks.get(address)
            if name == 'now':
                put(machine.reg_read(UC_ARM64_REG_X0 + 1), state['time'], 8); ret(1)
            elif name == 'poll':
                state['sent'].append('platform_poll')
                if case == 'power-ready':
                    setf(power, 'a733_gpu_platform', 'phase', 3); setf(power, 'a733_gpu_platform', 'token', 17, 8)
                    setf(power, 'a733_gpu_platform', 'delegated', 1); ret(1)
                else: ret(0 if case == 'power-pending' else -1)
        uc.hook_add(UC_HOOK_CODE, hook)
        result = call('power_poll', gpu, unit='/platform/a733/gpu_backend.c')
        assert result == (0xffffffff if case == 'power-failed' else 0)
        assert get(field(gpu, 'a733_gpu_backend', 'phase')) == (3 if case == 'power-ready' else 2)
        assert get(field(gpu, 'a733_gpu_backend', 'identity_step')) == 0
        assert get(field(gpu, 'a733_gpu_backend', 'error')) == (7 if case == 'power-failed' else 0)
    results.append(dict(case=case, passed=True, model=state))
a.output.write_text(json.dumps(dict(passed=True, image_sha256=identity, profile=a.profile, cases=results, physical_access=False,
    scope='Actual linked caller/backend boundaries; service replies, platform progress, lifetime and scheduler modeled. Device MMIO is unmapped and forbidden. No firmware or numerical execution.'), indent=2) + '\n')
print(f'PASS {len(results)} focused {a.profile} full-image boundary cases')
