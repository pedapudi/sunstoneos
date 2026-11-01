#!/usr/bin/env python3
"""Build the finite USB client with compatible platform and provider objects.

The five TinyUSB units compile against the current headers. An optional monitor
object must match the central monitor bytes or have a recorded dependency
closure whose headers still match; a historical board run is not build input.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from returning_build import (CENTRAL_PLATFORM, PROVIDER_UNITS, ROOT, digest,
    object_closure, require_unchanged_headers, platform_arguments, platform_from, save)
from host_evidence import unique_directory

TINYUSB_UNITS = ('portable/sunxi/dcd_sunxi_musb.c', 'tusb.c', 'common/tusb_fifo.c',
                 'device/usbd.c', 'class/vendor/vendor_device.c')


def monitor_input(platform, obj):
    """Check the alternate monitor's actual object/header compatibility."""
    if not platform.build:
        raise ValueError('--base-monitor requires --platform-build')
    obj = obj.resolve()
    current = platform.platform_objects[CENTRAL_PLATFORM.index('platform/a733/monitor.c')]
    headers = {}
    if digest(obj) != digest(current):
        headers = require_unchanged_headers([object_closure(obj)], platform.compiler)
    platform.replace_platform_object('platform/a733/monitor.c', obj)
    return {'object': str(obj), 'sha256': digest(obj), 'headers': headers}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, required=True)
    platform_arguments(parser)
    parser.add_argument('--base-monitor', type=Path, help='optional compatible monitor object')
    parser.add_argument('--base-bundle', type=Path, help='optional historical provenance path, recorded only')
    parser.add_argument('--base-run', type=Path, help='optional historical provenance path, recorded only')
    parser.add_argument('--transport', type=Path, help='optional historical provenance path, recorded only')
    parser.add_argument('--transport-records', type=Path, help='optional historical provenance path, recorded only')
    parser.add_argument('--tinyusb', type=Path, default=Path.home() / '.cache/sunstoneos/sources/tinyusb-d668b0abb7219b9ce324fa88d46604bb9e869973/src',
                        help='TinyUSB source directory containing tusb.c')
    args = parser.parse_args(argv)
    if not args.platform_build and not args.baseline:
        parser.error('pass --platform-build or an explicitly selected --baseline')
    evidence = args.evidence.resolve()
    if evidence == ROOT or ROOT in evidence.parents:
        parser.error('evidence must be outside the repository')
    evidence = unique_directory(evidence)
    central = args.platform_build is not None
    platform = platform_from(args, evidence)
    monitor = monitor_input(platform, args.base_monitor) if args.base_monitor else None
    adapted = evidence / 'tinyusb'
    adapted.mkdir()
    adaptation = {}
    for name, patch in (('dcd_sunxi_musb.c', 'tinyusb-a733-dcd.patch'),
                        ('musb_def.h', 'tinyusb-a733-registers.patch')):
        source, patch_path = args.tinyusb / 'portable/sunxi' / name, ROOT / 'patches' / patch
        subprocess.run(['patch', '--silent', '--output', str(adapted / name), str(source)],
                       input=patch_path.read_text(), text=True, check=True)
        adaptation[name] = {'source': str(source), 'source_sha256': digest(source),
                            'patch': str(patch_path), 'patch_sha256': digest(patch_path)}
    flags = ['-DSUNSTONE_A733_USB=1', f'-I{ROOT}/platform/a733-storage-update',
             f'-I{ROOT}/drivers/usb', f'-I{args.tinyusb}', f'-I{adapted}']
    sources = [ROOT / name for name in ('apps/usb-returning/main.c', 'platform/a733/native_usb.c',
                'drivers/usb/a733_usb_platform.c', 'drivers/usb/a733_usb_platform_native.c')]
    sources.extend(adapted / 'dcd_sunxi_musb.c' if name.endswith('dcd_sunxi_musb.c')
                   else args.tinyusb / name for name in TINYUSB_UNITS)
    platform.compile(sources, flags)
    names = (list(PROVIDER_UNITS) if central else [Path(pin['path']).stem
             for pin in platform.accepted['new_objects'] if Path(pin['path']).name != '00-pmic-provider-probe.obj'])
    platform.reuse(names)
    platform.link(); platform.binary(); platform.run()
    provenance = {name: str(getattr(args, name)) for name in
                  ('base_bundle', 'base_run', 'transport', 'transport_records') if getattr(args, name)}
    platform.record(__file__, {'base_monitor': monitor, 'provenance': provenance,
                              'tinyusb_sources': [str(args.tinyusb / name) for name in TINYUSB_UNITS],
                              'tinyusb_adaptation': adaptation})
    platform.copy_helpers('usb-returning', ('host-check.py', 'check.py', 'prepare.py', 'README.md'))
    host_inputs = {}
    for name in ('tools/check-a733-usb-returning.py', 'tools/check-a733-kernel.py', 'tools/usb-access.py',
                 'tools/check-a733-smp.py', 'tools/usb_guard.py', 'tools/host_evidence.py'):
        source, target = ROOT / name, evidence / 'host' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        host_inputs[name] = digest(source)
    save(evidence, 'host-inputs.json', host_inputs)
    print(f'Built bounded USB returning image: {evidence}')


if __name__ == '__main__':
    main()
