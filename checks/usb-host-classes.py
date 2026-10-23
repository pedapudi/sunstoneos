#!/usr/bin/env python3
"""USB2 hub tiers, hub power budgets and audio capture against a ring-level model; no board access."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

CASES = ['nested-msc', 'nested-hid-ls', 'nested-mtt-hid-ls', 'nested-unplug', 'nested-leaf-unplug',
         'nested-refused', 'third-tier',
         'bus-hub', 'bus-self-tier', 'bus-refused', 'bus-budget', 'bus-leaf-power', 'bus-status-mismatch',
         'bus-bus-tier', 'bus-self-tier-heavy',
         'audio-fs', 'audio-16k', 'audio-stereo', 'audio-hub', 'audio-hs', 'audio-missed', 'audio-missed-batch',
         'audio-missed-null', 'audio-missed-silent', 'audio-overrun', 'audio-overrun-refill', 'audio-overrun-no-trb',
         'audio-overrun-ring-full', 'audio-late-poll',
         'audio-stall', 'audio-unplug', 'audio-disabled', 'audio-large', 'audio-hid-interface']


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--sources', type=Path, default=Path.home()/'.cache/sunstoneos/sources')
    p.add_argument('--port', type=Path, required=True, help='FreeRTOS AArch64 port directory for native compiles')
    p.add_argument('--cases', nargs='+', choices=CASES, default=CASES)
    a = p.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = a.output.resolve()
    out.mkdir(parents=True)
    compiler = Path(shutil.which('clang-21')).resolve()
    tiny = a.sources/'tinyusb-d668b0abb7219b9ce324fa88d46604bb9e869973/src'
    freertos = a.sources/'FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2/include'
    units = [root/u for u in ['checks/usb-host-classes.c', 'drivers/usb/a733_usb2_platform.c', 'drivers/usb/a733_xhci.c',
        'drivers/usb/a733_xhci_tinyusb.c', 'drivers/usb/a733_usb3_phy.c', 'drivers/power/a733_pmic.c',
        'drivers/power/a733_rail_leases.c', 'drivers/clock/a733_clock_leases.c', 'drivers/pinctrl/a733_pins.c',
        'drivers/pinctrl/a733_pin_tables.c', 'drivers/gpu/a733_gpu_resources.c', 'runtime/device_memory_pool.c',
        'drivers/usb/a733_typec.c', 'drivers/usb/a733_typec_pd.c', 'drivers/i2c/a733_twi.c']]
    classes = ['tusb.c', 'host/usbh.c', 'common/tusb_fifo.c', 'class/msc/msc_host.c', 'class/hid/hid_host.c',
               'class/audio/audio_host.c']
    inputs = {}
    for path in sorted(set(units+[root/'checks/a733-pmic.c', root/'drivers/usb/host/tusb_config.h',
            root/'drivers/usb/host/hub.patch', root/'drivers/usb/host/superspeed.py', root/'drivers/usb/host/cadence-source.py', Path(__file__).resolve(),
            tiny/'host/hub.c', *[tiny/c for c in classes]]+list((root/'include').glob('*.h')))):
        inputs[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
    (out/'inputs.json').write_text(json.dumps(inputs, indent=2)+'\n')
    steps = {}

    def run(label, command):
        command = [str(x) for x in command]
        r = subprocess.run(command, capture_output=True, text=True)
        (out/(label+'.log')).write_text(r.stdout+r.stderr)
        steps[label] = {'command': command, 'exit': r.returncode, 'output': r.stdout.strip()}
        (out/'steps.json').write_text(json.dumps(steps, indent=2)+'\n')
        if r.returncode:
            raise SystemExit(f'{label} failed; see {out/(label+".log")}')

    # The pinned TinyUSB tree with the board's SuperSpeed adaptation and hub patch.
    run('stack', [sys.executable, root/'drivers/usb/host/superspeed.py', '--source', tiny, '--output', out/'tinyusb-ss'])
    cadence = a.sources/'pcie/kernel/bsp/drivers/phy/sunxi-cadence-combophy.c'
    run('ss-source', [sys.executable, root/'drivers/usb/host/cadence-source.py', '--source', cadence, '--output', out/'ss-cadence.inc'])
    run('hub-patch', ['patch', '--silent', '--output', out/'hub-native.c', tiny/'host/hub.c', root/'drivers/usb/host/hub.patch'])
    includes = [root/'drivers/usb/host', out/'tinyusb-ss', root/'include', root/'checks', tiny/'host', out]
    flags = [compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
             '-DSUNSTONE_USB_HOST_AUDIO=1']+['-I'+str(i) for i in includes]
    run('host-build', flags+units+[out/'hub-native.c']+[out/'tinyusb-ss'/c for c in classes]+['-o', out/'classes'])
    env = {'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
    for case in a.cases:
        r = subprocess.run([str(out/'classes'), case], capture_output=True, text=True, env=env)
        (out/(case+'.log')).write_text(r.stdout+r.stderr)
        steps[case] = {'exit': r.returncode, 'output': r.stdout.strip()}
        (out/'steps.json').write_text(json.dumps(steps, indent=2)+'\n')
        if r.returncode:
            raise SystemExit(f'{case} failed; see {out/(case+".log")}')
    # The same audio-enabled adapter, core and class compile for the native target.
    native = [compiler, '--target=aarch64-none-elf', '-march=armv8-a', '-mstrict-align', '-mgeneral-regs-only',
              '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-ffreestanding', '-fno-builtin',
              '-DSUNSTONE_USB_HOST_AUDIO=1']+['-I'+str(i) for i in includes+[root/'runtime/include', root/'platform/a733', freertos, a.port]]
    for i, unit in enumerate([root/'drivers/usb/a733_xhci.c', root/'drivers/usb/a733_xhci_tinyusb.c',
                              out/'tinyusb-ss/class/audio/audio_host.c']):
        run('native-'+str(i), native+['-c', unit, '-o', out/('native-'+str(i)+'.o')])
    result = {'result': 'PASS', 'cases': a.cases, 'hardware_access': False,
              'compiler': subprocess.check_output([str(compiler), '--version'], text=True),
              'binary_sha256': hashlib.sha256((out/'classes').read_bytes()).hexdigest(),
              'scope': 'Actual USB2 platform, xHCI core, TinyUSB adapter and pinned hub, MSC, HID and audio classes '
                       'against a ring-level controller and USB device model. Electrical power, isochronous timing, '
                       'DMA visibility and physical devices are not established.'}
    (out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k: result[k] for k in ('result', 'cases')}, indent=2))


if __name__ == '__main__':
    main()
