#!/usr/bin/env python3
"""Execute selected GPU boundaries in the full linked finite or OS image."""
import argparse
import hashlib
import json
from pathlib import Path
from elftools.elf.elffile import ELFFile
from harness import Image, skip_crumbs
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC

# Linked image sources whose paths are not named by the boundary model.
CHECK_INPUTS = ('drivers/gpu/gpu_queue.c',)
GATE_RUNS = [['--image', '{elf:ret-gpu}', '--profile', 'os', '--output', '{output}']]

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--image', type=Path, required=True)
p.add_argument('--profile', choices=('os', 'finite'), required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
names = ['board_application', 'a733_board_session', 'pvr_gpu_service_request', 'pvr_gpu_service_reply',
         'gpu_application', 'a733_gpu_backend', 'a733_gpu_platform', 'a733_gpu_reserved',
         'pvr_gpu_service', 'a733_gpu_backend_external', 'a733_gpu_queue', 'pvr_compute_queue']
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
    skip_crumbs(uc, image)
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
        uc.emu_start(image.address(name, unit), 0x80000000, count=2000000)
        assert uc.reg_read(UC_ARM64_REG_PC) == 0x80000000, 'instruction budget exhausted'
        return uc.reg_read(UC_ARM64_REG_X0)
    # The modeled service publishes progress at virtual times listed in 'events'. An event
    # wait returns at once when the sequence changed before it armed, wakes at the first
    # publication before its deadline, and otherwise returns EXPIRED at exactly the deadline.
    state = dict(time=100, live=1, waiting=0, sent=[], pending=0, collects=0,
                 sequence=0, phase=3, events=[], waits=0, wakes=0, expiries=0, untils=[])
    if a.profile == 'os':
        gpu = image.address('gpu', '/apps/os/gpu.c'); board = image.address('board')
        for name in ('started', 'opened'): setf(gpu, 'os_gpu_state', name, 1)
        setf(gpu, 'os_gpu_state', 'epoch', 1, 8)
        put(image.address('os_gpu_client_task', '/apps/os/gpu.c'), 0x1234, 8)
        component=field(field(gpu, 'os_gpu_state', 'session'), 'a733_gpu_queue', 'component')
        setf(gpu, 'os_gpu_state', 'component', component, 8)
        setf(component, 'pvr_compute_queue', 'completions', 0x8900, 8)
        setf(board, 'board_application', 'live_until', 3000000, 8)
        setf(field(board, 'board_application', 'session'), 'a733_board_session', 'deadline_us', 60000000, 8)
        service = field(gpu, 'os_gpu_state', 'service')
        hooks = {image.address(n): n for n in ('board_live', 'a733_smp_counter', 'xTaskGetCurrentTaskHandle',
            'xTaskGetTickCount', 'os_thermal_throttled', 'vTaskDelay', 'uxQueueMessagesWaiting', 'pvr_gpu_service_send_epoch',
            'pvr_gpu_service_collect', 'pvr_gpu_service_event_sequence', 'pvr_gpu_service_event_phase',
            'pvr_gpu_service_wait_event', 'pvr_gpu_service_close_ready')}
        def hook(machine, address, size, unused):
            name = hooks.get(address)
            if not name: return
            args = [machine.reg_read(UC_ARM64_REG_X0 + i) for i in range(5)]
            if name == 'board_live': ret(state['live'])
            elif name == 'a733_smp_counter': ret(state['time'] * 24)
            elif name == 'xTaskGetTickCount': ret(state['time'] // 1000)
            elif name == 'xTaskGetCurrentTaskHandle': ret(0x1234)
            elif name == 'os_thermal_throttled': ret(0)
            elif name == 'uxQueueMessagesWaiting': ret(0)
            # The owner waits only for publications and deadlines; a tick delay is polling.
            elif name == 'vTaskDelay': raise AssertionError('tick delay in the GPU owner')
            elif name == 'pvr_gpu_service_event_sequence': assert args[0] == service; ret(state['sequence'])
            elif name == 'pvr_gpu_service_event_phase': assert args[0] == service; ret(state['phase'])
            # The service stays busy in close-busy, so CLOSE never becomes ready there.
            elif name == 'pvr_gpu_service_close_ready': assert args[0] == service; ret(case != 'close-busy')
            elif name == 'pvr_gpu_service_wait_event':
                assert args[0] == service and state['time'] < args[2]
                # A BUSY CLOSE wait is visible to the queue's ownership producers.
                assert get(field(component, 'pvr_compute_queue', 'lifecycle_waiting')) == (case == 'close-busy')
                state['waits'] += 1; state['untils'].append(args[2])
                if args[1] != state['sequence']: ret(0)
                elif state['events'] and state['events'][0] < args[2]:
                    state['time'] = state['events'].pop(0); state['sequence'] += 1; state['wakes'] += 1
                    if state['phase'] == 4: state['phase'] = 5
                    ret(0)
                else: state['time'] = args[2]; state['expiries'] += 1; ret(1)
            elif name == 'pvr_gpu_service_send_epoch':
                assert args[0] == service and args[1] == 1 and args[3] == 0
                assert not state['pending'] and get(field(gpu, 'os_gpu_state', 'retired')) == 1 and get(field(gpu, 'os_gpu_state', 'waiting'))
                operation = get(field(args[2], 'pvr_gpu_service_request', 'operation'))
                assert operation == (2 if not state['sent'] or case == 'close-busy' else 0)
                # One final STATUS record follows the published STOPPED phase.
                assert operation == 2 or state['phase'] == 5
                state['sent'].append(operation); state['pending'] = len(state['sent']); put(args[4], state['pending'], 8)
                # CLOSE starts STOPPING; the service publishes STOPPED 2 ms later. In close-busy
                # the first CLOSE leaves one ownership publication that does not end the busy state.
                if operation == 2 and len(state['sent']) == 1:
                    if case != 'close-busy': state['phase'] = 4
                    state['events'].append(state['time'] + 2000)
                ret(1)
            else:
                assert args[0] == service and args[1] == state['pending'] and args[3] == 0
                state['collects'] += 1
                for key, value, width in [('ticket', args[1], 8), ('epoch', 1, 8), ('result', 3 if case == 'close-busy' else 0, 4),
                                          ('phase', state['phase'], 4)]:
                    setf(args[2], 'pvr_gpu_service_reply', key, value, width)
                if case == 'critical-during-close': state['live'] = 0
                state['pending'] = 0; ret(1)
        uc.hook_add(UC_HOOK_CODE, hook)
        result = call('os_gpu_queue_retire')
        assert result == (case == 'retire')
        assert get(field(gpu, 'os_gpu_state', 'retired')) == 1
        assert get(field(gpu, 'os_gpu_state', 'failed')) == (case != 'retire')
        assert get(field(gpu, 'os_gpu_state', 'parked')) == (case == 'retire')
        assert not get(field(component, 'pvr_compute_queue', 'lifecycle_waiting'))
        # retire waits once for STOPPED and reads one record. close-busy retries CLOSE once
        # after its one publication, then its second wait expires the window's end.
        # critical-during-close loses the board in the first reply and never waits.
        if case == 'retire':
            assert state['sent'] == [2, 0] and (state['waits'], state['wakes'], state['expiries']) == (1, 1, 0), state
        elif case == 'close-busy':
            assert state['sent'] == [2, 2] and (state['waits'], state['wakes'], state['expiries']) == (2, 1, 1), state
            assert state['untils'] == [1000100] * 2 and state['time'] == 1000100, state
        else: assert state['sent'] == [2] and not state['waits'], state
        before = list(state['sent']); state['live'] = 1
        assert call('os_gpu_queue_retire') == (case == 'retire')
        assert call('os_gpu_queue_thermal') == (0 if case == 'retire' else 0xffffffff)
        assert state['sent'] == before and get(field(gpu, 'os_gpu_state', 'waiting')) == (case != 'retire')
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
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text(json.dumps(dict(passed=True, image_sha256=identity, profile=a.profile, cases=results, physical_access=False,
    scope='Actual linked caller/backend boundaries; service replies, platform progress, lifetime and scheduler modeled. Device MMIO is unmapped and forbidden. No firmware or numerical execution.'), indent=2) + '\n')
print(f'PASS {len(results)} focused {a.profile} full-image boundary cases')
