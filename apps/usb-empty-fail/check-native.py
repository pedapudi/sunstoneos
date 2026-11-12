#!/usr/bin/env python3
"""Execute the linked application launcher and empty-port diagnostic.

The scheduler, clock and board liveness are modeled. The image's main_entry,
ss_live, grant/readiness checks, finish deadline, checkpoint predicate and
diagnostic execute. No USB device or register is accessed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'checks'))
from harness import Image, hook_addresses, machine
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--image', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
image = Image(args.image, {'board_application', 'a733_board_session', 'ss_grant'})
rows = []
cases = ['ready', 'no-checkpoints', 'host-only', 'typec-only', 'extra-checkpoint',
         'unprepared', 'usb-not-ready', 'ready-after-wake', 'expired-wait',
         'board-dead', 'deadline-registration-failed']
for case in cases:
    u = machine()
    u.mem_map(0x41000000, 0x1200000)
    u.mem_map(0x80000000, 0x20000)
    for address, data in image.parts:
        if data:
            u.mem_write(address, data)

    def put(address, value, size=4):
        u.mem_write(address, int(value).to_bytes(size, 'little'))

    def get(address, size=4):
        return int.from_bytes(u.mem_read(address, size), 'little')

    def local(name):
        return image.address(name, '/apps/os/application.c')

    grant = local('grant')
    board = image.address('board')
    session = board + image.layouts['board_application']['session']
    put(session + image.layouts['a733_board_session']['deadline_us'], 60000000, 8)
    put(grant + image.layouts['ss_grant']['devices'], 1 << 1)
    put(grant + image.layouts['ss_grant']['finish_by_us'], 55000000, 8)
    put(local('prepared'), case != 'unprepared')
    put(local('usb_ready'), case not in ('usb-not-ready', 'ready-after-wake', 'expired-wait'))
    checkpoints = {'no-checkpoints': 0, 'host-only': 1, 'typec-only': 2, 'extra-checkpoint': 7}.get(case, 3)
    put(image.address('usb_failure_checkpoints', '/apps/os/client.c'), checkpoints)
    state = {'clock_us': 1000000, 'registered': 0, 'waits': 0, 'delays': 0, 'terminal': None}
    names = ['board_service_deadline_register', 'board_live', 'ss_progress', 'a733_smp_counter',
             'os_session_inputs', 'application_wait_until_unless', 'vTaskDelay', 'ss_fail', 'ss_fail_value']
    hooks = {image.address(name): name for name in names}

    def ret(value=0):
        u.reg_write(UC_ARM64_REG_X0, value)
        u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))

    def stop(terminal):
        state['terminal'] = terminal
        u.emu_stop()

    def hook(uc, address, size, unused):
        name = hooks[address]
        arg = uc.reg_read(UC_ARM64_REG_X0)
        if name == 'board_service_deadline_register':
            state['registered'] += 1
            ret(case != 'deadline-registration-failed')
        elif name == 'board_live':
            ret(case != 'board-dead')
        elif name == 'ss_progress':
            ret()
        elif name == 'a733_smp_counter':
            ret(state['clock_us'] * 24)
        elif name == 'os_session_inputs':
            put(arg, 0)
            ret()
        elif name == 'application_wait_until_unless':
            state['waits'] += 1
            if case == 'ready-after-wake':
                put(local('usb_ready'), 1)
                ret()
            elif case == 'expired-wait':
                state['clock_us'] = 59990000
                ret()
            else:
                stop('readiness-wait')
        elif name == 'vTaskDelay':
            assert arg == 1
            state['delays'] += 1
            stop('checkpoint-wait')
        elif name == 'ss_fail':
            stop(f'failure-{arg:x}')
        elif name == 'ss_fail_value':
            state['detail'] = arg
            state['value'] = uc.reg_read(UC_ARM64_REG_X1)
            stop('injected')

    hook_addresses(u, hooks, hook)
    u.reg_write(UC_ARM64_REG_SP, 0x8001f000)
    u.reg_write(UC_ARM64_REG_LR, 0x80000000)
    u.emu_start(local('main_entry'), 0x80000000, count=100000)
    expected = ('readiness-wait' if case in ('unprepared', 'usb-not-ready') else
                'checkpoint-wait' if case in ('no-checkpoints', 'host-only', 'typec-only', 'extra-checkpoint') else
                'failure-750' if case in ('board-dead', 'expired-wait') else
                'failure-756' if case == 'deadline-registration-failed' else 'injected')
    assert state['registered'] == 1 and state['terminal'] == expected, (case, state)
    if expected in ('injected', 'checkpoint-wait'):
        assert get(local('finish_by'), 8) == 11000000, (case, 'run deadline missing')
        assert get(grant + image.layouts['ss_grant']['finish_by_us'], 8) == 11000000
    if expected == 'injected':
        assert state['detail'] == 0x790 and state['value'] == 3, (case, state)
    assert get(board + image.layouts['board_application']['result']) == 0, 'No fabricated attachment'
    rows.append({'case': case, 'passed': True, **state})

result = {'image': str(args.image), 'sha256': hashlib.sha256(args.image.read_bytes()).hexdigest(),
          'physical_access': False, 'scope': __doc__, 'cases': rows, 'passed': True}
(args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
print(f'PASS linked empty-port application entry: {len(rows)} cases')
