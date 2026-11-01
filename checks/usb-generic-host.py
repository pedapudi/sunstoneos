#!/usr/bin/env python3
"""Run the generic TinyUSB/xHCI classes against a modeled USB peripheral."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
from harness import CACHE, CLANG, SANITIZE, environment

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r'static (?:int|void) ' + name + r'\([^)]*\)\s*\{', source)
    assert match, name
    opening = source.index('{', match.start())
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return match.start(), opening, end


def replace_function(source, name, body):
    _, opening, end = function(source, name)
    return source[:opening + 1] + '\n' + body + '\n}' + source[end:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--services', type=Path, default=ROOT,
                        help='Source checkout supplying the OS broker')
    parser.add_argument('--resume', action='store_true')
    parser.add_argument('--alias', action='store_true', help='Model the real application RO alias and compile its actual TX adapter')
    parser.add_argument('--cases', nargs='+', default=['hid', 'serial', 'iso', 'bulk', 'mjpeg', 'format-refusal'],
                        choices=['hid', 'serial', 'serial-direct', 'serial-stale', 'iso', 'bulk', 'mjpeg', 'format-refusal'])
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=args.resume)
    services = args.services.resolve()
    tiny = CACHE / 'sources/tinyusb-d668b0abb7219b9ce324fa88d46604bb9e869973/src'
    logs = json.loads((out / 'commands.json').read_text()) if args.resume else []

    def run(label, command):
        command = list(map(str, command))
        prior = next((row for row in reversed(logs) if row['label'] == label), None)
        if prior and prior['exit'] == 0 and prior['command'] == command and prior.get('inputs'):
            if all(Path(p).is_file() and hashlib.sha256(Path(p).read_bytes()).hexdigest() == value
                   for p, value in prior['inputs'].items()):
                return
        result = subprocess.run(command, text=True, capture_output=True, env=environment())
        attempt = sum(row['label'] == label for row in logs) + 1
        log = label + (f'-attempt{attempt}' if attempt > 1 else '') + '.log'
        (out / log).write_text(result.stdout + result.stderr)
        inputs = set(Path(arg) for arg in command if Path(arg).is_file())
        if label.startswith('compile-') and result.returncode == 0:
            dep = Path(command[command.index('-MF') + 1])
            inputs.update(Path(p) for p in dep.read_text().replace('\\\n', ' ').split(':', 1)[1].split())
        logs.append({'label': label, 'command': command, 'exit': result.returncode,
                     'log': log, 'inputs': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}})
        (out / 'commands.json').write_text(json.dumps(logs, indent=2) + '\n')
        if result.returncode:
            raise RuntimeError(f'{label} failed; inspect {out / log}')

    run('tinyusb', [sys.executable, ROOT / 'drivers/usb/host/superspeed.py',
                    '--source', tiny, '--output', out / 'tinyusb'])
    run('hub', ['patch', '--silent', '--output', out / 'hub.c', tiny / 'host/hub.c',
                ROOT / 'drivers/usb/host/hub.patch'])
    run('cdc', [sys.executable, ROOT / 'drivers/usb/host/serial-source.py',
                '--source', tiny / 'class/cdc/cdc_host.c', '--output', out / 'cdc.c'])
    run('phy', [sys.executable, ROOT / 'drivers/usb/host/cadence-source.py',
                '--source', CACHE / 'sources/pcie/kernel/bsp/drivers/phy/sunxi-cadence-combophy.c',
                '--output', out / 'ss-cadence.inc'])
    # Reuse the existing provider/MMIO model, replacing only its USB peripheral
    # traffic producer. The legacy test main is compiled but never executed.
    base = (ROOT / 'checks/a733-usb2-host.c').read_text()
    base = replace_function(base, 'control_data', '    generic_control_data(p,count);')
    base = replace_function(base, 'wire', '    generic_wire();')
    if args.alias:
        old = 'static _Alignas(4096) uint8_t dma[A733_XHCI_MEMORY_BYTES];'
        assert old in base
        base = base.replace(old, 'static uint8_t (*dma_pages)[A733_XHCI_MEMORY_BYTES];\n#define dma (*dma_pages)')
        base = base.replace('0x79000000', 'A733_USB_MEMORY_BASE')
    (out / 'usb-generic-base.c').write_text(base)
    client = (services / 'apps/os/client.c').read_text()
    begin, _, end = function(client, 'usb_write')
    (out / 'usb-client-write.inc').write_text(client[begin:end] + '\n')
    public = out / 'service-include'
    public.mkdir(exist_ok=True)
    for name in ('sunstone_app.h', 'sunstone_usb_service.h', 'sunstone_usb_jpeg.h'):
        path = services / 'include' / name
        if path.exists():
            shutil.copy2(path, public / name)
    units = [ROOT / p for p in [
        'checks/usb-generic-host.c', 'drivers/usb/a733_usb2_platform.c',
        'drivers/usb/a733_xhci.c', 'drivers/usb/a733_xhci_tinyusb.c',
        'drivers/usb/a733_usb3_phy.c', 'drivers/usb/host/hid.c',
        'drivers/usb/host/serial.c', 'drivers/usb/host/serial-native.c',
        'drivers/usb/host/uvc.c', 'drivers/power/a733_pmic.c',
        'drivers/power/a733_rail_leases.c', 'drivers/clock/a733_clock_leases.c',
        'drivers/pinctrl/a733_pins.c', 'drivers/pinctrl/a733_pin_tables.c',
        'drivers/gpu/a733_gpu_resources.c', 'drivers/usb/a733_typec.c',
        'drivers/usb/a733_typec_pd.c', 'drivers/i2c/a733_twi.c',
        'runtime/device_memory_pool.c']]
    units += [services / 'apps/os/usb-service.c', out / 'hub.c', out / 'cdc.c']
    units += [out / 'tinyusb' / p for p in [
        'tusb.c', 'host/usbh.c', 'common/tusb_fifo.c',
        'class/msc/msc_host.c', 'class/hid/hid_host.c']]
    includes = [out, out / 'tinyusb', ROOT / 'drivers/usb/host', public,
                ROOT / 'include', ROOT / 'checks', tiny]
    flags = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-function', '-DSUNSTONE_USB_HOST_GENERIC=1', *SANITIZE]
    flags += ['-I' + str(path) for path in includes]
    if args.alias:
        flags += ['-DSUNSTONE_USB_APP_ALIAS=1']
    objects, dependencies = [], set()
    for number, unit in enumerate(units):
        obj, dep = out / f'unit-{number}.o', out / f'unit-{number}.d'
        local = ['-iquote' + str(out / 'tinyusb/host')] if unit == out / 'hub.c' else []
        if unit == out / 'cdc.c':
            local += ['-iquote' + str(tiny / 'class/cdc')]
        run(f'compile-{number}', [CLANG, *flags, *local, '-MMD', '-MF', dep, '-c', unit, '-o', obj])
        objects.append(obj)
        text = dep.read_text().replace('\\\n', ' ')
        dependencies.update(Path(p) for p in text.split(':', 1)[1].split())
    exe = out / 'classes'
    run('link', [CLANG, *SANITIZE, *objects, '-o', exe])
    for case in args.cases:
        run(case, [exe, case])
    dependencies.update([Path(__file__), ROOT / 'checks/a733-usb2-host.c', services / 'apps/os/client.c',
                         ROOT / 'drivers/usb/host/serial-source.py',
                         ROOT / 'drivers/usb/host/serial-control.inc',
                         ROOT / 'drivers/usb/host/hub.patch'])
    record = {'result': 'PASS', 'hardware_access': False, 'cases': args.cases, 'application_alias': args.alias,
              'boundary': 'Actual classes, adapter, xHCI and resource providers; modeled controller, peripherals, Type-C checkpoint and IRQ delivery. No CPU startup or physical timing proof.',
              'inputs': {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in sorted(dependencies)},
              'executable_sha256': hashlib.sha256(exe.read_bytes()).hexdigest()}
    (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print('PASS: generic TinyUSB HID, serial and UVC completion paths:', ', '.join(args.cases))


if __name__ == '__main__':
    main()
