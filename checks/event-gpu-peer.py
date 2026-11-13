#!/usr/bin/env python3
"""Execute GPU peer notification selection, startup and precise fence waits.

The actual application selector, runtime deadline wait and GPU notification
publishers execute. Kernel delivery and timeline values are modeled. The four-second primary/peer ordering
models the failed lifecycle handshake, not a physical latency prediction.
"""
import argparse
import hashlib
from functools import lru_cache
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP
from harness import Image, machine, save, skip_crumbs

GATE_RUNS = [['--image', '{elf:ret-gpu}', '--output', '{output}']]
CHECK_INPUTS = ['apps/os/application.c', 'runtime/deadline.c', 'drivers/gpu/gpu_queue.c',
                'drivers/gpu/pvr_compute_fast.c', 'include/pvr_compute_fast.h', 'include/sunstone_notify.h',
                'apps/gpu-queue-peer/main.c', 'apps/gpu-lifecycle/main.c']
APP = '/apps/os/application.c'
TYPES = {'board_application', 'a733_board_session', 'ss_application', 'ss_manifest', 'ss_grant',
         'pvr_gpu_service', 'pvr_gpu_service_config', 'pvr_compute_queue', 'pvr_compute_fast', 'fast_call',
         'pvr_mips_buffer', 'pvr_fw_buffer'}


@lru_cache(maxsize=None)
def load(path):
    image = Image(path, TYPES)
    with path.open('rb') as stream:
        for cu in ELFFile(stream).get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                if (die.tag == 'DW_TAG_structure_type' and die.attributes.get('DW_AT_name') and
                        die.attributes['DW_AT_name'].value == b'pvr_mips_buffer' and
                        'DW_AT_byte_size' in die.attributes):
                    image.mips_buffer_bytes = die.attributes['DW_AT_byte_size'].value
    return image


def run(path, case, ready_index=None, completion_index=None):
    image = load(path)
    u = machine()
    u.mem_map(0x41000000, 0x2000000)
    u.mem_map(0x80000000, 0x40000)
    for address, data in image.parts:
        if data:
            u.mem_write(address, data)
    skip_crumbs(u, image)
    task, peer, service, queue = 0x80001000, 0x80001100, 0x80003000, 0x80010000
    current = peer if 'peer' in case or case.startswith('startup-') else task
    state = {'now': 1100, 'wait_indices': [], 'callback': False, 'publisher_index': None}

    def put(address, value, width=8):
        u.mem_write(address, int(value).to_bytes(width, 'little'))

    def get(address, width=8):
        return int.from_bytes(u.mem_read(address, width), 'little')

    def field(base, typ, name):
        return base + image.layouts[typ][name]

    put(image.address('peer_task', APP), peer)
    put(image.address('main_task', APP), task)
    board = image.address('board')
    session = field(board, 'board_application', 'session')
    put(field(session, 'a733_board_session', 'deadline_us'), 60000000)
    put(image.address('finish_by', APP), 60000000)
    fast = field(queue, 'pvr_compute_queue', 'fast')
    put(field(fast, 'pvr_compute_fast', 'enabled'), 1, 4)
    put(field(fast, 'pvr_compute_fast', 'pending') + 4, 2, 4)
    put(field(fast, 'pvr_compute_fast', 'completed'), 1, 4)
    put(field(queue, 'pvr_compute_queue', 'service'), service)
    if case in ('fast-irq-publisher', 'fast-task-publisher'):
        put(field(fast, 'pvr_compute_fast', 'task') + 8, peer)
        if case == 'fast-irq-publisher':
            put(field(fast, 'pvr_compute_fast', 'submitted'), 2, 4)
            put(field(fast, 'pvr_compute_fast', 'waiter') + 4, 2, 4)
            timeline_buffer = field(queue, 'pvr_compute_queue', 'firmware') + 5 * image.mips_buffer_bytes
            put(field(field(timeline_buffer, 'pvr_mips_buffer', 'storage'), 'pvr_fw_buffer', 'cpu'), 0x80026000)
            put(0x80026000, 2, 4)
            config = field(service, 'pvr_gpu_service', 'config')
            put(field(config, 'pvr_gpu_service_config', 'job_now'), 0x80000200)
        else:
            put(field(fast, 'pvr_compute_fast', 'lock_waiter') + 4, 1, 4)
            put(field(fast, 'pvr_compute_fast', 'locked'), 1, 4)
    if case == 'startup-before-arm':
        put(image.address('peer_ready', APP), 1, 4)
    if case == 'ready-publisher':
        current = task
        app = image.address('ss_application')
        manifest = 0x80020000
        put(field(app, 'ss_application', 'manifest'), manifest)
        grant = image.address('grant', APP)
        put(field(grant, 'ss_grant', 'finish_by_us'), 60000000)

    hooks = {}
    for name in ('xTaskGetCurrentTaskHandle', 'xTaskGetApplicationTaskTag', 'vTaskEnterCritical',
                 'vTaskExitCritical', 'sunstone_deadline_now_us', 'sunstone_deadline_kick',
                 'ulTaskGenericNotifyTake', 'pvr_gpu_call', 'ss_live', 'ss_fail',
                 'gpu_queue_peer_main', 'board_service_deadline_register', 'application_entry_ready',
                 'os_session_inputs', 'a733_smp_counter', 'xTaskGenericNotify', 'vTaskGenericNotifyGiveFromISR'):
        if name in image.symbols:
            hooks[image.address(name)] = name
    hooks[0x80000200] = 'job-clock'

    def ret(value=0):
        u.reg_write(UC_ARM64_REG_X0, value)
        u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))

    def hook(uc, address, size, unused):
        name = hooks.get(address)
        if name is None:
            return
        args = [uc.reg_read(UC_ARM64_REG_X0 + i) for i in range(5)]
        if name == 'xTaskGetCurrentTaskHandle':
            ret(current)
        elif name == 'xTaskGetApplicationTaskTag':
            ret(0x80022000 if case == 'main-selector' else 0)
        elif name in ('vTaskEnterCritical', 'vTaskExitCritical', 'sunstone_deadline_kick'):
            ret()
        elif name == 'sunstone_deadline_now_us':
            ret(state['now'])
        elif name == 'a733_smp_counter':
            ret(state['now'] * 24)
        elif name in ('ss_live', 'board_service_deadline_register', 'application_entry_ready'):
            ret(1)
        elif name == 'os_session_inputs':
            put(args[0], 0, 4)
            ret()
        elif name == 'ss_fail':
            raise AssertionError(('unexpected application failure', case, args[0]))
        elif name == 'gpu_queue_peer_main':
            state['callback'] = True
            uc.emu_stop()
        elif name == 'job-clock':
            put(args[1], state['now'])
            ret(1)
        elif name in ('xTaskGenericNotify', 'vTaskGenericNotifyGiveFromISR'):
            assert args[0] == peer
            if case == 'ready-publisher':
                assert get(image.address('peer_ready', APP), 4) == 1, 'Ready flag must precede notification'
            elif case == 'fast-irq-publisher':
                assert name == 'vTaskGenericNotifyGiveFromISR' and args[2] == 0x80025000
                assert get(field(fast, 'pvr_compute_fast', 'waiter') + 4, 4) == 0
                assert get(field(fast, 'pvr_compute_fast', 'completed'), 4) == 2
            else:
                assert case == 'fast-task-publisher' and name == 'xTaskGenericNotify'
                assert get(field(fast, 'pvr_compute_fast', 'lock_waiter') + 4, 4) == 0
            state['publisher_index'] = args[1]
            if case == 'ready-publisher':
                uc.emu_stop()
            else:
                ret()
        elif name == 'ulTaskGenericNotifyTake':
            if not args[2]:
                ret()
                return
            state['wait_indices'].append(args[0])
            if case == 'direct-peer-irq':
                assert completion_index is not None
                put(field(fast, 'pvr_compute_fast', 'completed'), 2, 4)
                state['now'] = 1200 if args[0] == completion_index else 4001100
            else:
                assert case == 'startup-during-arm'
                assert ready_index is not None
                put(image.address('peer_ready', APP), 1, 4)
                state['now'] = 1200 if args[0] == ready_index else 60000000
            ret(1)
        elif name == 'pvr_gpu_call':
            assert case == 'direct-peer-irq' and args[0] == 2
            assert get(field(fast, 'pvr_compute_fast', 'completed'), 4) == 2
            record = field(args[1], 'fast_call', 'record')
            put(record, 1, 4)
            ret(0)
        else:
            raise AssertionError(name)

    u.hook_add(UC_HOOK_CODE, hook)

    def invoke(name, *args, unit=None):
        u.reg_write(UC_ARM64_REG_SP, 0x8003f000)
        u.reg_write(UC_ARM64_REG_LR, 0x80000000)
        for i, value in enumerate(args):
            u.reg_write(UC_ARM64_REG_X0 + i, value)
        u.emu_start(image.address(name, unit), 0x80000000, count=2000000)
        assert state['callback'] or state['publisher_index'] is not None or u.reg_read(UC_ARM64_REG_PC) == 0x80000000, (case, 'execution budget')
        return u.reg_read(UC_ARM64_REG_X0)

    if case.endswith('-selector'):
        selected = invoke('sunstone_deadline_notify_index')
        state['selected_index'] = selected
        expected = 0 if case == 'service-selector' else 2 if case == 'main-selector' else completion_index
        assert selected == expected, (case, state)
    elif case.startswith('startup-'):
        invoke('peer_entry', 0, unit=APP)
        assert state['callback'] and state['now'] < 60000000, (case, 'ready notification lost', state)
        assert state['wait_indices'] == ([] if case == 'startup-before-arm' else [ready_index]), (case, state)
    elif case == 'ready-publisher':
        invoke('main_entry', 0, unit=APP)
        if completion_index is not None:
            assert state['publisher_index'] == completion_index, (case, state)
    elif case in ('fast-irq-publisher', 'fast-task-publisher'):
        invoke('pvr_compute_fast_snapshot' if case == 'fast-irq-publisher' else 'pvr_compute_fast_unlock',
               queue, 0x80025000, 0)
        assert u.reg_read(UC_ARM64_REG_PC) == 0x80000000 and state['publisher_index'] is not None
    elif case == 'direct-peer-irq':
        assert invoke('sunstone_deadline_register') == 1
        result = invoke('direct_wait_until', queue, 1 | 1 << 32, 0x80023000, 4001100, 0, 0,
                        unit='/drivers/gpu/gpu_queue.c')
        assert result == 0, (case, result, state)
        # Main starts its four-second peer wait 100 us before the peer starts
        # this fence wait. A lost IRQ produces two collected jobs but publishes
        # peer completion after the main application's check11 deadline.
        state['primary_peer_check_11'] = int(state['now'] >= 4001000)
        assert not state['primary_peer_check_11'], (case, 'check11 peer timeout after successful collection', state)
        assert state['wait_indices'] == [completion_index], (case, state)
    return {'case': case, **state}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--baseline-image', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    rows = [run(args.image, case) for case in ('fast-irq-publisher', 'fast-task-publisher', 'ready-publisher')]
    irq_index, task_index, ready_index = [row['publisher_index'] for row in rows]
    assert irq_index == task_index == ready_index, ('linked producers disagree', rows)
    cases = ('main-selector', 'peer-selector', 'service-selector', 'startup-before-arm',
             'startup-during-arm', 'direct-peer-irq')
    rows += [run(args.image, case, ready_index, irq_index) for case in cases]
    rejected = []
    wrong_index = (irq_index + 1) % 3
    for case, image, ready, signal in [('startup-during-arm', args.image, wrong_index, irq_index),
                                     ('direct-peer-irq', args.image, ready_index, wrong_index)] + (
            [('peer-selector', args.baseline_image, ready_index, irq_index),
             ('ready-publisher', args.baseline_image, ready_index, irq_index),
             ('direct-peer-irq', args.baseline_image, ready_index, irq_index)] if args.baseline_image else []):
        try:
            run(image, case, ready, signal)
        except AssertionError as error:
            rejected.append({'case': case, 'ready_index': ready, 'completion_index': signal,
                             'image': str(image), 'failure': str(error)})
        else:
            raise AssertionError(('negative control passed', case, ready))
    save(args.output/'results.json', {'physical_access': False, 'cases': rows,
         'negative_controls': rejected, 'image_sha256': hashlib.sha256(args.image.read_bytes()).hexdigest()})
    print('PASS GPU peer notification routing:', len(rows), 'cases;', len(rejected), 'negative controls')


if __name__ == '__main__':
    main()
