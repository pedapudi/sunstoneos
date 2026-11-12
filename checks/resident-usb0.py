#!/usr/bin/env python3
"""Execute grant-controlled USB0 task creation and application readiness.

Session admission, task allocation, pins, PMIC/GPU binding and power-domain
construction are modeled. The linked board start, native provider binding,
application readiness and service health selection execute. No hardware access.
"""
import argparse
import importlib.util
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

from harness import Image

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['{elf:resident-stop}', '--out', '{output}']]
CHECK_INPUTS = ['apps/os/main.patch', 'apps/os/application.c', 'apps/os/session.c',
                'apps/os/services.c', 'apps/os/os-services.h', 'platform/a733/native_usb.c',
                'include/sunstone_app.h', 'include/a733_native_usb.h', 'apps/os/grants.c',
                'tools/session.py', 'checks/resident-linked.py', 'apps/board/main.c',
                'apps/board/application.h', 'apps/os/start.c']
spec = importlib.util.spec_from_file_location('resident_linked', ROOT / 'checks/resident-linked.py')
linked = importlib.util.module_from_spec(spec)
spec.loader.exec_module(linked)
USB0_GRANT, USB_DEVICE = 1 << 6, 1 << 9


class Boot(linked.Model):
    def __init__(self, image, layout, grant, lifetime):
        super().__init__(image, layout)
        self.lifetime(lifetime, None, lifetime)
        self.created, self.usb_initializations, self.binds = [], 0, []
        self.put32(layout.address('granted_word', 'apps/os/grants.c'), 0x80000000 | grant)
        self.put32(self.session + layout.layouts['os_session']['granted'], grant)
        runtime = self.s['a733_cached_runtime']
        self.put64(runtime + layout.layouts['a733_cached_runtime']['startup_deadline'], 24000000)
        self.put64(runtime + layout.layouts['a733_cached_runtime']['diagnostic_deadline'], (1 << 64) - 1)
        self.hooks.update({self.s[name]: lambda: 1 for name in
                          ('a733_smp_device_preparation_admitted', 'a733_board_session_valid',
                           'board_application_pins', 'a733_pmic_native_init', 'a733_pmic_init',
                           'a733_gpu_resources_native_use_pmic', 'a733_power_domains_native_init',
                           'a733_power_domains_init', 'board_application_start')})
        self.hooks[self.s['board_session_snapshot']] = lambda: self.session
        self.hooks[self.s['vTaskCoreAffinitySet']] = lambda: 0
        self.hooks[self.s['a733_native_usb_init']] = self.native_init
        self.hooks[self.s['a733_usb_platform_init']] = self.usb_init
        self.hooks[self.s['xTaskCreateStatic']] = self.create

    def arg(self, n):
        return self.uc.reg_read(linked.prior.fault.REGS[n])

    def create(self):
        p = self.arg(1)
        name = bytes(self.uc.mem_read(p, 32)).split(b'\0')[0].decode()
        self.created.append(name)
        return 0x41e00000 + 256 * len(self.created)

    def native_init(self):
        p = self.arg(0)
        nf = self.layout.layouts['a733_native_usb']
        self.put32(p + nf['used'], 1)
        return 1

    def usb_init(self):
        self.usb_initializations += 1
        return 1

    def code(self, uc, address, size, unused):
        if address == self.s['platform_panic']:
            raise AssertionError(('unexpected startup panic', self.arg(1)))
        if address == self.s['a733_native_usb_bind']:
            self.binds.append((self.arg(1), self.arg(2)))
        if address == self.s['vTaskStartScheduler']:
            self.stop = 'scheduler-start'
            uc.emu_stop()
            return
        super().code(uc, address, size, unused)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    names = {'board_application', 'a733_board_session', 'os_session', 'board_provider',
             'a733_native_usb', 'a733_cached_runtime', 'ss_grant', 'ss_manifest', 'ss_application'}
    layout, rows = Image(args.elf, names), []
    image = linked.prior.fault.Image(args.elf)
    # Use the real producer for framing, facts, input records and CPU grant.
    # Only the optional device declaration and its matching digest are model
    # mutations; an undeclared device must still refuse native decoding.
    produced = args.out / ('produced-session-' + hashlib.sha256(args.elf.read_bytes()).hexdigest()[:16])
    if not produced.exists():
        data = args.out / 'input.bin'
        data.write_bytes(b'USB0 grant fixture\n')
        done = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/session.py'), 'build',
                               str(args.elf), '--out', str(produced), '--clock-grant',
                               '--input', str(data)], capture_output=True, text=True)
        assert done.returncode == 0, done.stderr
    raw = (produced / 'template/session.bin').read_bytes()
    for declared, granted in ((False, False), (True, False), (True, True), (False, True)):
        m = linked.Model(image, layout)
        mf = layout.layouts['ss_manifest']
        manifest = m.u64(m.s['ss_application'] + layout.layouts['ss_application']['manifest'])
        if declared:
            m.put32(manifest + mf['devices_optional'], USB_DEVICE)
        wire = bytearray(raw)
        manifest_bytes = m.u32(manifest + mf['bytes'])
        wire[592 + 32:592 + 64] = hashlib.sha256(bytes(m.uc.mem_read(manifest, manifest_bytes))).digest()
        struct.pack_into('<I', wire, 592 + 24, USB_DEVICE if granted else 0)
        m.uc.mem_write(0x41f00000, bytes(wire))
        admitted = m.call(0, 'os_session_decode_why',
                          (0x41f00000, len(wire), 0x42100000, 0x42101000), budget=500000)
        assert admitted == (not granted or declared), (declared, granted, admitted, m.u32(0x42101000))
        if admitted:
            actual = m.u32(0x42100000 + layout.layouts['os_session']['granted'])
            assert bool(actual & USB0_GRANT) == granted
        else:
            assert m.u32(0x42101000) == 2  # A733_DECODE_MANIFEST: device was not declared.
        rows.append({'case': 'native-session-decode', 'declared': declared, 'granted': granted,
                     'admitted': bool(admitted)})
    m = linked.Model(image, layout)
    assert m.call(0, 'os_composition_refused', (USB0_GRANT, 3)) != 0
    assert m.call(0, 'os_composition_refused', (USB0_GRANT, 2)) == 0
    rows.append({'case': 'returning-usb0-requires-shutdown'})
    for lifetime in (2, 3):
        for grant in (0, USB0_GRANT):
            m = Boot(image, layout, grant, lifetime)
            m.call(0, 'a733_smp_diagnostic_start', budget=500000)
            assert m.stop == 'scheduler-start'
            assert ('board-usb0' in m.created) == bool(grant), (lifetime, grant, m.created)
            assert {'board-pmic', 'board-watchdog'} <= set(m.created)
            assert m.usb_initializations == bool(grant), (grant, m.usb_initializations)
            assert len(m.binds) == 1 and bool(m.binds[0][1]) == bool(grant)
            # The real binding stays one-shot even without a USB owner.
            assert m.call(0, 'a733_native_usb_bind',
                          (layout.address('providers'), m.binds[0][0], m.binds[0][1])) == 0
            assert not any(0x04100000 <= a < 0x04200000 for _, a, _ in m.loads)
            assert not any(0x04100000 <= a < 0x04200000 or 0x02003300 <= a < 0x02003344
                           for _, a, _, _ in m.writes)
            rows.append({'case': 'task-selection', 'lifetime': lifetime, 'grant': bool(grant),
                         'tasks': m.created, 'usb_initializations': m.usb_initializations})
    # Exercise actual ss_live with an admitted application and no USB-ready
    # publication. Only the USB0 device grant requires that publication.
    for device in (0, USB_DEVICE):
        m = linked.Model(image, layout)
        m.put32(m.finished, 0)
        m.put32(layout.address('prepared', 'apps/os/application.c'), 1)
        grant = layout.address('grant', 'apps/os/application.c')
        m.put32(grant + layout.layouts['ss_grant']['devices'], device)
        board = layout.address('board', 'os-image/main.c')
        m.put32(board + layout.layouts['board_application']['result'], 3)
        assert m.call(0, 'ss_live') == (0 if device else 1)
        m.call(0, 'os_application_usb_ready')
        assert m.call(0, 'ss_live') == 1
        rows.append({'case': 'readiness', 'usb_device': bool(device)})
    # A silent absent USB0 owner must not prevent resident renewal.
    m = linked.Model(image, layout)
    seen = layout.address('seen', 'apps/os/services.c')
    m.put64(seen + 8, 0)
    assert m.call(0, 'board_resident_renew', (0, 4 * 24000000)) == 0
    assert m.uc.mem_read(m.completion, 1) == b'\1'
    rows.append({'case': 'ungranted-usb0-not-supervised'})
    (args.out / 'results.json').write_text(json.dumps({'physical_access': False, 'cases': rows}, indent=2) + '\n')
    print('PASS %d linked USB0 grant cases; no device access' % len(rows))


if __name__ == '__main__':
    main()
