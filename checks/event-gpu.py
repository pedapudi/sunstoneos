#!/usr/bin/env python3
"""Execute native GPU waits with modeled task delivery and no hardware access.

The worker's device step is replaced by a quiet admitted device. Its actual
idle wait, deadline selection and client arm/recheck code execute. This tests
software wake policy, not physical interrupt delivery or scheduler timing.
"""
import argparse
import hashlib
from functools import lru_cache
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_PSTATE
from harness import Image, machine, save

GATE_RUNS = [['--image', '{elf:ret-gpu}', '--output', '{output}']]
UNIT = '/drivers/gpu/pvr_gpu_service.c'
TYPES = {'pvr_gpu_service', 'pvr_gpu_service_config', 'pvr_gpu_service_client',
         'pvr_compute_queue', 'pvr_compute_fast', 'pvr_compute_fast_entry', 'pvr_gpu_start_ops',
         'fast_call', 'pvr_gpu_service_reply', 'pvr_gpu_service_request',
         'pvr_gpu_start', 'pvr_gpu_start_step', 'a733_gpu_wait', 'a733_gpu_backend',
         'a733_gpu_platform', 'a733_gpu_power'}


@lru_cache(maxsize=None)
def load(image_path):
    image = Image(image_path, TYPES)
    enums = {}
    performance = {}
    with image_path.open('rb') as stream:
        for cu in ELFFile(stream).get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                if die.tag == 'DW_TAG_enumerator':
                    enums[die.attributes['DW_AT_name'].value.decode()] = die.attributes['DW_AT_const_value'].value
                if die.tag == 'DW_TAG_structure_type' and die.attributes.get('DW_AT_name') and die.attributes['DW_AT_name'].value == b'pvr_gpu_service':
                    for member in die.iter_children():
                        if member.tag == 'DW_TAG_member' and member.attributes.get('DW_AT_name') and member.attributes['DW_AT_name'].value == b'performance':
                            typ = member.get_DIE_from_attribute('DW_AT_type')
                            performance = {m.attributes['DW_AT_name'].value.decode(): m.attributes['DW_AT_data_member_location'].value for m in typ.iter_children() if m.tag == 'DW_TAG_member'}

    return image, enums, performance


def run(image_path, case):
    image, enums, performance = load(image_path)
    u = machine()
    u.mem_map(0x41000000, 0x2000000)
    u.mem_map(0x80000000, 0x40000)
    for address, data in image.parts:
        if data:
            u.mem_write(address, data)
    service, queue, task = 0x80001000, 0x80010000, 0x80024000
    state = {'now': 100, 'wakes': 0, 'waits': [], 'blocked': False, 'index': None, 'samples': 0}
    measured_idle = case in ('idle-measured', 'idle-immediate', 'idle-clock-reversal', 'idle-duration-overflow')

    def put(address, value, width=8):
        u.mem_write(address, int(value).to_bytes(width, 'little'))

    def get(address, width=8):
        return int.from_bytes(u.mem_read(address, width), 'little')

    def field(base, typ, name):
        return base + image.layouts[typ][name]

    def set_field(base, typ, name, value, width=8):
        put(field(base, typ, name), value, width)

    put(image.address('instance', UNIT), service)
    set_field(service, 'pvr_gpu_service', 'initialized', 1, 4)
    set_field(service, 'pvr_gpu_service', 'phase', enums['PVR_GPU_SERVICE_READY'], 4)
    set_field(service, 'pvr_gpu_service', 'worker', task)
    set_field(service, 'pvr_gpu_service', 'requests', 0x8800)
    if case == 'idle-duration-overflow':
        set_field(service, 'pvr_gpu_service', 'idle_wait_us', 0xfffffffffffffff0)
    config = field(service, 'pvr_gpu_service', 'config')
    set_field(config, 'pvr_gpu_service_config', 'compute_queue', queue)
    set_field(config, 'pvr_gpu_service_config', 'job_irq_ack', 0x80000200)
    start_ops = field(config, 'pvr_gpu_service_config', 'start')
    set_field(start_ops, 'pvr_gpu_start_ops', 'now', 0x80000100)
    irq_ops = field(config, 'pvr_gpu_service_config', 'irq')
    put(irq_ops, 0x80000200)
    fast = field(queue, 'pvr_compute_queue', 'fast')
    set_field(fast, 'pvr_compute_fast', 'enabled', 1, 4)
    client = field(service, 'pvr_gpu_service', 'clients')
    set_field(client, 'pvr_gpu_service_client', 'task', task)
    if case in ('job-deadline', 'completed-job', 'during-arm'):
        entry = field(fast, 'pvr_compute_fast', 'entry')
        set_field(entry, 'pvr_compute_fast_entry', 'serial', 1, 4)
        set_field(entry, 'pvr_compute_fast_entry', 'deadline', 12345)
        if case == 'completed-job':
            set_field(fast, 'pvr_compute_fast', 'completed', 1, 4)
    if case == 'close-performance-busy':
        put(field(service, 'pvr_gpu_service', 'performance') + performance['ticket'], 1)
    if case == 'owner-lock':
        set_field(fast, 'pvr_compute_fast', 'locked', 1, 4)
    direct_case = case in ('direct-during-arm', 'direct-expiry', 'subtick-close', 'direct-wait-refused', 'close-wait-refused')
    if direct_case:
        state['now'] = 12000
        put(image.address('instance', '/drivers/gpu/gpu_queue.c'), queue)
        set_field(queue, 'pvr_compute_queue', 'service', service)
        set_field(queue, 'pvr_compute_queue', 'generation', 1, 4)
        set_field(queue, 'pvr_compute_queue', 'create_done', 1, 4)
        set_field(queue, 'pvr_compute_queue', 'client_owed', 1, 4)
        set_field(queue, 'pvr_compute_queue', 'completions', 0x8801)
        set_field(fast, 'pvr_compute_fast', 'pending', 1, 4)
        set_field(service, 'pvr_gpu_service', 'event_phase', enums['PVR_GPU_SERVICE_READY'], 4)
    lifecycle_case = case in ('worker-start-wait', 'lifecycle-retry-overhead', 'platform-retry-overhead')
    if lifecycle_case:
        set_field(service, 'pvr_gpu_service', 'phase', enums['PVR_GPU_SERVICE_STARTING'], 4)
        set_field(service, 'pvr_gpu_service', 'deadline', 12345)
        start = field(service, 'pvr_gpu_service', 'start')
        set_field(start, 'pvr_gpu_start', 'phase', enums['PVR_GPU_START_RUNNING'], 4)
        set_field(start, 'pvr_gpu_start', 'deadline', 12345)
        ops = field(start, 'pvr_gpu_start', 'ops')
        set_field(ops, 'pvr_gpu_start_ops', 'now', 0x80000100)
        set_field(ops, 'pvr_gpu_start_ops', 'started', 0x80000110)
        step = field(start, 'pvr_gpu_start', 'steps')
        set_field(step, 'pvr_gpu_start_step', 'kind', enums['WAIT_BOOT'], 4)
        if case == 'lifecycle-retry-overhead':
            wait = field(start, 'pvr_gpu_start', 'wait')
            set_field(wait, 'a733_gpu_wait', 'next_wait_us', 90)
            set_field(wait, 'a733_gpu_wait', 'kind', 1, 4)
        elif case == 'platform-retry-overhead':
            set_field(service, 'pvr_gpu_service', 'phase', enums['PVR_GPU_SERVICE_ENABLING'], 4)
            backend, platform = 0x80027000, 0x80029000
            set_field(config, 'pvr_gpu_service_config', 'context', backend)
            set_field(config, 'pvr_gpu_service_config', 'next_wait', image.address('next_wait', '/platform/a733/gpu_backend.c'))
            set_field(backend, 'a733_gpu_backend', 'power', platform)
            power = field(platform, 'a733_gpu_platform', 'power')
            set_field(power, 'a733_gpu_power', 'phase', enums['A733_GPU_POWER_STARTING'], 4)
            set_field(power, 'a733_gpu_power', 'deadline', 12345)
            wait = field(power, 'a733_gpu_power', 'wait')
            set_field(wait, 'a733_gpu_wait', 'next_wait_us', 90)
            set_field(wait, 'a733_gpu_wait', 'kind', 1, 4)
    if case == 'client-notify-phase':
        # A client publishes while the worker is between phases: the published
        # phase stays the worker's last publication; the sequence advances.
        set_field(service, 'pvr_gpu_service', 'phase', enums['PVR_GPU_SERVICE_STOPPING'], 4)
        set_field(service, 'pvr_gpu_service', 'event_phase', enums['PVR_GPU_SERVICE_READY'], 4)
        set_field(service, 'pvr_gpu_service', 'event_sequence', 5)
        set_field(client, 'pvr_gpu_service_client', 'event_waiting', 1, 4)
        set_field(client, 'pvr_gpu_service_client', 'event_index', 2, 4)
    state['notified'] = 0
    client_case = case in ('before-arm', 'during-arm', 'expiry')
    if client_case:
        assert 'event_sequence' in image.layouts['pvr_gpu_service'], 'native event waits absent'
        set_field(service, 'pvr_gpu_service', 'event_sequence', 2 if case == 'before-arm' else 1)
    hooks = {}
    for name in ('sunstone_deadline_register', 'sunstone_deadline_wait_until', 'sunstone_deadline_now_us',
                 'sunstone_deadline_notify_index', 'ulTaskGenericNotifyTake', 'xQueueReceive',
                 'uxQueueMessagesWaiting', 'xTaskGetSchedulerState', 'xTaskGetCurrentTaskHandle',
                 'vTaskEnterCritical', 'vTaskExitCritical', 'xTaskGenericNotify', 'platform_panic',
                 'pvr_gpu_call', 'pvr_gpu_service_send_epoch', 'pvr_gpu_service_collect'):
        if name in image.symbols:
            hooks[image.address(name)] = name
    if case != 'worker-start-wait':
        hooks[image.address('step', UNIT)] = 'step'
    hooks[0x80000100] = 'clock'
    hooks[0x80000110] = 'started'
    hooks[0x80000200] = 'mask'

    def ret(value=0):
        u.reg_write(UC_ARM64_REG_X0, value)
        u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))

    def hook(uc, address, size, unused):
        name = hooks.get(address)
        if name is None:
            return
        args = [uc.reg_read(UC_ARM64_REG_X0 + i) for i in range(5)]
        if name in ('vTaskEnterCritical', 'vTaskExitCritical', 'step'):
            ret()
        elif name == 'mask':
            ret(1)
        elif name == 'started':
            state['samples'] += 1
            put(args[1], 0, 4)
            ret(1)
        elif name == 'platform_panic':
            raise AssertionError(('native assertion', case, args))
        elif name == 'clock':
            put(args[1], state['now'])
            ret(1)
        elif name == 'sunstone_deadline_now_us':
            ret(state['now'])
        elif name == 'sunstone_deadline_register':
            ret(0 if case == 'close-wait-refused' else 1)
        elif name == 'sunstone_deadline_notify_index':
            ret(2)
        elif name == 'xTaskGetCurrentTaskHandle':
            ret(task)
        elif name == 'xTaskGetSchedulerState':
            ret(2)
        elif name == 'uxQueueMessagesWaiting':
            ret()
        elif name == 'xTaskGenericNotify':
            state['notified'] += 1
            ret(1)
        elif name == 'xQueueReceive':
            wait = args[2]
            if wait:
                if wait in (0xffffffff, 0xffffffffffffffff):
                    state['blocked'] = True
                    uc.emu_stop()
                else:
                    state['now'] += wait * 1000
                    state['wakes'] += 1
                    if state['now'] >= 1000100:
                        uc.emu_stop()
                    else:
                        ret()
            else:
                ret()
        elif name == 'ulTaskGenericNotifyTake':
            if args[2]:
                assert args[2] in (0xffffffff, 0xffffffffffffffff), ('tick timeout', args)
                if measured_idle:
                    state['now'] += -1 if case == 'idle-clock-reversal' else 0 if case == 'idle-immediate' else 1000000
                    ret(1)
                else:
                    state['blocked'] = True
                    uc.emu_stop()
            else:
                state['index'] = args[0]
                ret()
        elif name == 'pvr_gpu_service_send_epoch':
            state['operation'] = get(field(args[2], 'pvr_gpu_service_request', 'operation'), 4)
            put(args[4], 1)
            ret(1)
        elif name == 'pvr_gpu_service_collect' and case == 'close-wait-refused':
            ret(0)
        elif name == 'pvr_gpu_service_collect':
            phase = 'PVR_GPU_SERVICE_STOPPED' if state['operation'] == enums['PVR_GPU_SERVICE_CLOSE'] else 'PVR_GPU_SERVICE_READY'
            set_field(args[2], 'pvr_gpu_service_reply', 'result', enums['PVR_GPU_SERVICE_OK'], 4)
            set_field(args[2], 'pvr_gpu_service_reply', 'phase', enums[phase], 4)
            ret(1)
        elif name == 'pvr_gpu_call':
            assert direct_case and args[0] == 2
            record = field(args[1], 'fast_call', 'record')
            put(record, 7, 4)
            put(record + 4, enums['GPU_QUEUE_OK'], 2)
            set_field(queue, 'pvr_compute_queue', 'client_owed', 0, 4)
            set_field(queue, 'pvr_compute_queue', 'slot_state', enums['PVR_COMPUTE_QUEUE_IDLE'], 4)
            ret(enums['GPU_QUEUE_OK'])
        elif name == 'sunstone_deadline_wait_until':
            state['waits'].append(args[0])
            assert not get(field(fast, 'pvr_compute_fast', 'locked'), 4), 'owner slept while the transport lock remained held'
            if lifecycle_case:
                assert args[0] == 150, ('mature retry did not release the CPU', state)
                uc.emu_stop()
            elif direct_case:
                assert args[0] == 12345, ('absolute deadline rounded to a tick', args)
                if case == 'direct-wait-refused':
                    ret(0xffffffffffffffff)
                    return
                state['now'] = args[0] if case == 'direct-expiry' else args[0] - 1
                if case != 'direct-expiry':
                    set_field(fast, 'pvr_compute_fast', 'completed', 1, 4)
                ret(1 if case == 'direct-expiry' else 0)
            elif client_case:
                assert get(field(client, 'pvr_gpu_service_client', 'event_waiting'), 4) == 1
                assert get(field(client, 'pvr_gpu_service_client', 'event_index'), 4) == 2
                if case == 'during-arm':
                    set_field(service, 'pvr_gpu_service', 'event_sequence', 2)
                    set_field(client, 'pvr_gpu_service_client', 'event_gave', 1, 4)
                    ret(0)
                else:
                    state['now'] = args[0]
                    ret(1)
            else:
                uc.emu_stop()
        else:
            raise AssertionError(name)

    u.hook_add(UC_HOOK_CODE, hook)
    u.reg_write(UC_ARM64_REG_PSTATE, 4)
    u.reg_write(UC_ARM64_REG_SP, 0x8003f000)
    u.reg_write(UC_ARM64_REG_LR, 0x80000000)
    u.reg_write(UC_ARM64_REG_X0, service)
    if measured_idle:
        u.reg_write(UC_ARM64_REG_X0 + 1, 0x80026000)
        u.reg_write(UC_ARM64_REG_X0 + 2, 0)
        function = image.address('wait_request', UNIT)
    elif case == 'close-performance-busy':
        function = image.address('pvr_gpu_service_close_ready')
    elif direct_case:
        if case in ('subtick-close', 'close-wait-refused'):
            u.reg_write(UC_ARM64_REG_X0, 1)
            u.reg_write(UC_ARM64_REG_X0 + 1, 12345)
            u.reg_write(UC_ARM64_REG_X0 + 2, enums['PVR_GPU_SERVICE_CLOSE'])
            function = image.address('lifetime_until', '/drivers/gpu/gpu_queue.c')
        else:
            u.reg_write(UC_ARM64_REG_X0, queue)
            u.reg_write(UC_ARM64_REG_X0 + 1, 1)
            u.reg_write(UC_ARM64_REG_X0 + 2, 0x80025000)
            u.reg_write(UC_ARM64_REG_X0 + 3, 12345)
            u.reg_write(UC_ARM64_REG_X0 + 4, 0)
            function = image.address('direct_wait_until', '/drivers/gpu/gpu_queue.c')
    elif case == 'client-notify-phase':
        function = image.address('pvr_gpu_service_notify_clients')
    elif client_case:
        u.reg_write(UC_ARM64_REG_X0 + 1, 1)
        u.reg_write(UC_ARM64_REG_X0 + 2, 12345)
        function = image.address('pvr_gpu_service_wait_event')
    else:
        function = image.address('worker', UNIT)
    u.emu_start(function, 0x80000000, count=10000000)
    if measured_idle:
        assert u.reg_read(UC_ARM64_REG_PC) == 0x80000000
        values = [get(field(service, 'pvr_gpu_service', name)) for name in
                  ('event_wakes', 'deadline_wakes', 'idle_wakes', 'idle_wait_us')]
        duration = 0xffffffffffffffff if case in ('idle-clock-reversal', 'idle-duration-overflow') else 0 if case == 'idle-immediate' else 1000000
        assert values == [1, 0, 1, duration], (case, values)
        state['owner_counters'] = values
    elif case == 'close-performance-busy':
        assert u.reg_read(UC_ARM64_REG_PC) == 0x80000000
        assert u.reg_read(UC_ARM64_REG_X0) == 0 and not state['waits']
    elif direct_case:
        assert u.reg_read(UC_ARM64_REG_PC) == 0x80000000, (case, 'execution budget')
        assert state['waits'] == [12345], state
        assert state['index'] == 2, 'direct waiter cleanup used an application-owned index'
        # A refused deadline wait is a fault: no retry can succeed.
        expected = {'direct-expiry': 'GPU_QUEUE_TIMEOUT', 'direct-wait-refused': 'GPU_QUEUE_FAULT',
                    'close-wait-refused': 'GPU_QUEUE_FAULT'}.get(case, 'GPU_QUEUE_OK')
        assert u.reg_read(UC_ARM64_REG_X0) == enums[expected], (case, 'result', u.reg_read(UC_ARM64_REG_X0), enums[expected])
        if case in ('direct-expiry', 'direct-wait-refused'):
            assert get(field(fast, 'pvr_compute_fast', 'pending'), 4) == 1
            assert get(field(queue, 'pvr_compute_queue', 'client_owed'), 4) == 1
    elif case == 'client-notify-phase':
        assert u.reg_read(UC_ARM64_REG_PC) == 0x80000000, (case, 'execution budget')
        published = get(field(service, 'pvr_gpu_service', 'event_phase'), 4)
        assert published == enums['PVR_GPU_SERVICE_READY'], ('client published the worker phase', published)
        assert get(field(service, 'pvr_gpu_service', 'event_sequence')) == 6
        assert get(field(client, 'pvr_gpu_service_client', 'event_gave'), 4) == 1 and state['notified'] == 1, state
    elif client_case:
        assert u.reg_read(UC_ARM64_REG_PC) == 0x80000000, (case, 'execution budget')
        assert u.reg_read(UC_ARM64_REG_X0) == (1 if case == 'expiry' else 0)
        assert state['waits'] == ([] if case == 'before-arm' else [12345])
        assert get(field(client, 'pvr_gpu_service_client', 'event_waiting'), 4) == 0
        if case == 'during-arm':
            assert state['index'] == 2, 'late event hint leaked from runtime wait'
    elif lifecycle_case:
        assert state['waits'] == [150], state
        assert state['samples'] == (1 if case == 'worker-start-wait' else 0), state
    elif case == 'job-deadline':
        assert state['waits'] == [12345], state
    else:
        assert state['blocked'] and not state['waits'], (case, 'unsolicited idle wake', state)
    return {'case': case, **state}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline-image', type=Path)
    args = parser.parse_args()
    rows = [run(args.image, case) for case in ('idle', 'idle-measured', 'idle-immediate', 'idle-clock-reversal', 'idle-duration-overflow', 'completed-job', 'job-deadline', 'owner-lock',
                                              'before-arm', 'during-arm', 'expiry', 'direct-during-arm', 'direct-expiry', 'subtick-close', 'close-performance-busy',
                                              'client-notify-phase', 'direct-wait-refused', 'close-wait-refused',
                                              'worker-start-wait', 'lifecycle-retry-overhead', 'platform-retry-overhead')]
    baseline = None
    if args.baseline_image:
        try:
            run(args.baseline_image, 'idle')
        except AssertionError as failure:
            baseline = str(failure)
            assert 'unsolicited idle wake' in baseline, baseline
        else:
            raise AssertionError('baseline unexpectedly blocks without an event')
    args.output.mkdir(parents=True, exist_ok=False)
    save(args.output / 'results.json', {'physical_access': False, 'cases': rows,
         'image_sha256': hashlib.sha256(args.image.read_bytes()).hexdigest(),
         'baseline_failure': baseline, 'idle_wakes_per_second': 0})
    print('PASS native GPU event waits:', len(rows), 'cases; modeled idle wakes 0/s')


if __name__ == '__main__':
    main()
