#!/usr/bin/env python3
"""Actual GPU client/USB task ordering; transport and scheduler are modeled."""
import argparse
import hashlib
import importlib.util
import json
import struct
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_PSTATE

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('gpu_image_layout', ROOT / 'checks/storage-read-native.py')
layout = importlib.util.module_from_spec(spec)
spec.loader.exec_module(layout)


class Machine:
    def __init__(self, image):
        self.image = image
        self.cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.cpu.mem_map(0x41000000, 0x1200000)
        self.cpu.mem_map(0x43000000, 0x1000000)
        self.cpu.reg_write(UC_ARM64_REG_PSTATE, 5)
        for address, data in image.parts:
            if data:
                self.cpu.mem_write(address, data)
        self.app = image.address('app')
        self.fields = image.layouts['gpu_application']
        self.shared = self.app + self.fields['shared']
        self.write(self.shared + image.layouts['a733_native_usb']['pmic_ready'], 1)
        session = self.app + self.fields['session']
        self.write(session + image.layouts['a733_board_session']['deadline_us'], 61000000, 8)
        self.write(session + image.layouts['a733_board_session']['epoch'], 123, 8)
        self.now, self.limit, self.health = 1000000, 40, 1
        self.request, self.available = None, 4
        self.tx, self.stop, self.opened = [], None, False
        self.usb_running = False
        names = ['a733_smp_counter', 'vTaskDelay', 'pvr_gpu_service_send',
                 'a733_native_usb_step', 'a733_native_usb_health', 'tusb_rhport_init',
                 'tud_task_ext', 'tud_vendor_n_available', 'tud_vendor_n_read',
                 'tud_vendor_n_write', 'tud_vendor_n_write_flush']
        self.hooks = {image.address(n): n for n in names}
        self.cpu.hook_add(UC_HOOK_CODE, self.hook)

    def write(self, address, value, size=4):
        self.cpu.mem_write(address, int(value).to_bytes(size, 'little'))

    def field(self, name):
        return int.from_bytes(self.cpu.mem_read(self.app + self.fields[name], 4), 'little')

    def ret(self, value=0):
        self.cpu.reg_write(UC_ARM64_REG_X0, value)
        self.cpu.reg_write(UC_ARM64_REG_PC, self.cpu.reg_read(UC_ARM64_REG_LR))

    def hook(self, cpu, address, size, unused):
        if address == 0x43fff000:
            self.stop = 'returned'
            cpu.emu_stop()
            return
        name = self.hooks.get(address)
        if not name:
            return
        args = [cpu.reg_read(UC_ARM64_REG_X0 + n) for n in range(4)]
        if name == 'a733_smp_counter':
            self.ret(self.now * 24)
        elif name == 'vTaskDelay':
            if self.usb_running and not self.field('fatal') and self.now < 61000000:
                self.ret()
            else:
                self.stop = 'delay'
                cpu.emu_stop()
        elif name == 'pvr_gpu_service_send':
            assert int.from_bytes(cpu.mem_read(args[1], 4), 'little') == 1  # OPEN
            self.opened, self.stop = True, 'open'
            cpu.emu_stop()
        elif name in ('a733_native_usb_step', 'a733_native_usb_health'):
            self.ret(self.health & ((1 << 64) - 1))
        elif name in ('tusb_rhport_init', 'tud_task_ext'):
            self.ret(1)
        elif name == 'tud_vendor_n_available':
            self.ret(self.available if self.request is not None else 0)
        elif name == 'tud_vendor_n_read':
            cpu.mem_write(args[1], struct.pack('<I', self.request))
            self.request = None
            self.ret(4)
        elif name == 'tud_vendor_n_write':
            count = min(args[2], self.limit)
            self.tx.append(bytes(cpu.mem_read(args[1], count)))
            self.ret(count)
        elif name == 'tud_vendor_n_write_flush':
            self.ret()
            self.stop = 'queued'
            cpu.emu_stop()
        else:
            raise AssertionError(name)

    def run(self, name, *args, usb=False):
        self.stop = None
        self.usb_running = usb
        self.cpu.reg_write(UC_ARM64_REG_SP, 0x43ffe000 if usb else 0x43ffd000)
        self.cpu.reg_write(UC_ARM64_REG_LR, 0x43fff000)
        for n, value in enumerate(args):
            self.cpu.reg_write(UC_ARM64_REG_X0 + n, value)
        unit = '/apps/gpu-render/main.c' if name == 'client' else None
        self.cpu.emu_start(self.image.address(name, unit), 0, count=100000)
        assert self.stop, (name, hex(self.cpu.reg_read(UC_ARM64_REG_PC)))
        if usb:
            self.usb_context = self.cpu.context_save()

    def queue(self):
        self.request = 0
        self.run('gpu_application_usb', 0, usb=True)

    def resume_usb(self):
        self.usb_running = True
        self.cpu.context_restore(self.usb_context)
        self.stop = None
        self.cpu.emu_start(self.cpu.reg_read(UC_ARM64_REG_PC), 0, count=100000)
        assert self.stop
        self.usb_context = self.cpu.context_save()

    def complete(self, count, interface=0):
        self.run('tud_vendor_tx_cb', interface, count)

    def client(self):
        self.run('client', 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    image = layout.Image(args.image)
    cases = []
    def passed(name):
        cases.append(name)
        (args.output / 'completed-cases.json').write_text(json.dumps(cases, indent=2) + '\n')

    m = Machine(image)
    m.client()
    assert m.stop == 'delay' and not m.opened
    m.queue()
    assert m.stop == 'queued' and len(b''.join(m.tx)) == 40
    frame = b''.join(m.tx)
    assert struct.unpack_from('<3I', frame) == (0x43555047, 1, 0)
    assert not m.field('usb_observed')
    m.client()
    assert m.stop == 'delay' and not m.opened
    m.complete(40)
    assert m.field('usb_observed') == 1
    m.client()
    assert m.opened
    passed('complete-pending-transfer-before-open')

    m = Machine(image)
    m.limit = 20
    m.queue()
    m.complete(20)
    m.client()
    assert not m.field('usb_observed') and not m.opened
    m.resume_usb()
    m.complete(20)
    m.client()
    assert m.field('usb_observed') and m.opened
    passed('partial-queue-and-completion-retain-gate')

    for variant in ('zero-write', 'invalid-length', 'invalid-index', 'overcount',
                    'wrong-interface', 'expired-completion', 'fatal-completion',
                    'shared-provider-fault', 'fatal-after-publication'):
        m = Machine(image)
        if variant == 'zero-write': m.limit = 0
        if variant == 'invalid-length': m.available = 3
        if variant == 'shared-provider-fault': m.health = -1
        if variant == 'invalid-index':
            m.request = 2
            m.run('gpu_application_usb', 0, usb=True)
        else:
            m.queue()
        if variant == 'zero-write': m.complete(0)
        elif variant == 'overcount': m.complete(41)
        elif variant == 'wrong-interface': m.complete(40, 1)
        elif variant == 'expired-completion':
            m.now = 61000000
            m.complete(40)
        elif variant == 'fatal-completion':
            m.write(m.app + m.fields['fatal'], 1)
            m.complete(40)
        elif variant == 'fatal-after-publication':
            m.complete(40)
            assert m.field('usb_observed')
            m.write(m.app + m.fields['fatal'], 1)
        m.client()
        assert not m.opened, variant
        if variant != 'fatal-after-publication': assert not m.field('usb_observed'), variant
        if variant not in ('zero-write',): assert m.field('fatal'), variant
        passed(variant + '-cannot-open')

    result = {'passed': True, 'physical_access': False, 'cases': cases,
              'image_sha256': hashlib.sha256(args.image.read_bytes()).hexdigest(),
              'scope': 'Actual linked GPU client, USB task and completion callback; scheduler, providers and endpoint transfers modeled. No GPU arithmetic/startup replay or physical claim.'}
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS actual GPU application ordering:', len(cases), 'focused cases')


if __name__ == '__main__':
    main()
