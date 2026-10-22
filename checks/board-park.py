#!/usr/bin/env python3
"""Linked board PMIC task: a parked service suspends once instead of waking every tick.

The PMIC task in the linked board image runs under Unicorn from its entry.
a733_smp_counter, a733_pmic_poll, vTaskDelay and vTaskSuspend are modeled
boundaries; everything between them is the image's own code. Three cases:

- not-live: board.fatal is set, so the task parks. It must call
  vTaskSuspend(NULL), call it again if the kernel ever returns, and never
  reach vTaskDelay.
- fault: the provider reports EXPIRED. The task must close admission
  (fatal and watchdog_state bit 2) before it suspends the same way.
- pending: a live idle provider still sleeps one tick per poll, so the
  change leaves the live polling period unchanged.
"""
import argparse
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0

PENDING, EXPIRED = 0, 4
STACK = 0x50000000
SENTINEL = STACK - 0x1000


def run_case(image, case):
    board = image.address('board')
    fields = image.layouts['board_application']
    session = image.layouts['a733_board_session']
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(0x40000000, 0x8000000)
    uc.mem_map(STACK - 0x10000, 0x10000)
    for address, data in image.parts:
        uc.mem_write(address, bytes(data))
    # A live board: deadlines far beyond the modeled counter.
    uc.mem_write(board + fields['live_until'], struct.pack('<Q', 1 << 40))
    uc.mem_write(board + fields['session'] + session['deadline_us'], struct.pack('<Q', 1 << 40))
    uc.mem_write(board + fields['watchdog_state'], struct.pack('<I', 1))
    uc.mem_write(board + fields['fatal'], struct.pack('<I', 1 if case == 'not-live' else 0))
    events = []

    def returned(value=0):
        uc.reg_write(UC_ARM64_REG_X0, value)
        uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))

    def hook(machine, address, size, unused):
        name = names.get(address)
        if address == SENTINEL:
            raise AssertionError((case, 'pmic task returned'))
        if name == 'a733_smp_counter':
            returned(24 * 1000)
        elif name == 'a733_pmic_poll':
            events.append(('poll',))
            returned(EXPIRED if case == 'fault' else PENDING)
        elif name == 'vTaskDelay':
            events.append(('delay', machine.reg_read(UC_ARM64_REG_X0)))
            if case == 'pending':
                machine.emu_stop()
            else:
                returned()
        elif name == 'vTaskSuspend':
            events.append(('suspend', machine.reg_read(UC_ARM64_REG_X0)))
            # A second suspension shows that a return from the kernel parks
            # again rather than resuming the polling loop.
            if sum(e[0] == 'suspend' for e in events) == 2:
                machine.emu_stop()
            else:
                returned()

    # An image whose task never suspends may not link vTaskSuspend at all.
    names = {image.address(n): n for n in ('a733_smp_counter', 'a733_pmic_poll', 'vTaskDelay', 'vTaskSuspend')
             if n in image.symbols}
    harness.hook_addresses(uc, [*names, SENTINEL], hook)
    uc.reg_write(UC_ARM64_REG_SP, STACK - 0x100)
    uc.reg_write(UC_ARM64_REG_LR, SENTINEL)
    uc.reg_write(UC_ARM64_REG_X0, 0)
    uc.emu_start(image.address('pmic', unit='apps/board/main.c'), 0, count=200000)
    fatal, watchdog = (struct.unpack('<I', uc.mem_read(board + fields[n], 4))[0] for n in ('fatal', 'watchdog_state'))
    return events, fatal, watchdog


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--image', type=Path, required=True, help='linked sunstone-a733-board.elf')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    image = harness.Image(args.image, {'board_application', 'a733_board_session'})
    results = {}
    events, fatal, watchdog = run_case(image, 'not-live')
    assert events == [('suspend', 0), ('suspend', 0)], events
    results['not-live'] = events
    events, fatal, watchdog = run_case(image, 'fault')
    assert events == [('poll',), ('suspend', 0), ('suspend', 0)], events
    assert fatal == 1 and watchdog & 4, (fatal, watchdog)
    results['fault'] = events
    events, fatal, watchdog = run_case(image, 'pending')
    assert events == [('poll',), ('delay', 1)] and not fatal, events
    results['pending'] = events
    harness.save(args.output / 'result.json', {'passed': True, 'physical_access': False,
                                                'image': str(args.image.resolve()), 'cases': results})
    print('PASS parked and failed PMIC tasks suspend; a live idle PMIC task still sleeps one tick')


if __name__ == '__main__':
    main()
