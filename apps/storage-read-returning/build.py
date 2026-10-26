#!/usr/bin/env python3
"""Build the returning SD0 read image on the returning platform.

usage: build.py --platform-build CENTRAL --evidence NEW_DIRECTORY

CENTRAL is a central CMake build configured from this tree with
-DCMAKE_EXPORT_COMPILE_COMMANDS=ON. The image links:

- the returning platform objects of its a733-power-probe target
  (tools/returning_build.py, CENTRAL_PLATFORM), except platform/a733/monitor.c,
  which is compiled with the monitor's counter option and must equal, byte for
  byte, the monitor object of the kernel check that passed on port 3-3 with the
  EL3 security writes and the MBUS restore (board-bundles-20261025-mbus-qual-2,
  board-session-20261025-24, run bb2-run-2, which logged
  "MBUS 0x84000000 0x81000000 0");
- the fourteen provider units of tools/returning_build.py, compiled from this
  tree with the PMIC provider probe's FreeRTOS configuration;
- its own units: apps/storage-read-returning/main.c, the SD0 driver and
  provider (drivers/storage/sunxi_mmc.c, a733_mmc_platform.c,
  a733_mmc_platform_native.c, a733_storage_update.c) and
  arch/aarch64/device_memory.c. No USB unit is linked.

It records, in origins.json, how every platform and provider object compares
with rank-9-usb-v4's object of the same unit (byte-identical, identical apart
from debug sections, or different) and which board run covers the monitor and
platform objects. No step contacts a device.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from returning_build import CENTRAL_PLATFORM, PROVIDER_UNITS, ReturningPlatform, digest, save  # noqa: E402

CACHE = Path.home() / '.cache/sunstoneos'
V4 = CACHE / 'evidence/board-bundles-20261023/rank-9-usb-v4'
# The kernel check with the EL3 security writes, the MBUS restore and the
# counter option, which passed on port 3-3 (docs/handoff.md, "MBUS restore
# qualified").
QUALIFIED = CACHE / 'evidence/board-bundles-20261025-mbus-qual-2/port-3-3/kernel-check-counter-option/bundle'
QUALIFIED_RUN = CACHE / 'evidence/board-session-20261025-24/port-3-3/bb2-run-2'
QUALIFIED_MONITOR = 'd86d9743f3b2445de089619a49552c7993815acf35668f5a4f17d6c5e67b9656'
UNITS = ['apps/storage-read-returning/main.c', 'drivers/storage/sunxi_mmc.c', 'drivers/storage/a733_mmc_platform.c',
         'drivers/storage/a733_mmc_platform_native.c', 'drivers/storage/a733_storage_update.c',
         'arch/aarch64/device_memory.c']


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--platform-build', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    e = args.evidence.resolve(); central = args.platform_build.resolve()
    e.mkdir(parents=True, exist_ok=False)
    apps = load('storage_read_returning_bba', ROOT / 'checks/board-bundles-apps.py')
    qualified = json.loads((QUALIFIED / 'equivalence.json').read_text())
    run = json.loads((QUALIFIED_RUN / 'session.json').read_text())
    if not (qualified['objects']['platform/a733/monitor.c.obj']['sha256'] == QUALIFIED_MONITOR and
            run.get('complete') is True and run['result']['status'] == 0 and
            run.get('image_sha256') == digest(QUALIFIED / 'probe.bin')):
        raise SystemExit('The qualified kernel check record does not name its monitor or its passing run')
    platform = ReturningPlatform(e, build=central)
    monitor = e / 'build/monitor-counter-option.obj'
    apps.compile_object(central, 'a733-power-probe', ROOT / 'platform/a733/monitor.c', monitor,
                        ('-DSUNSTONE_MONITOR_COUNTERS=1',), True)
    if digest(monitor) != QUALIFIED_MONITOR:
        raise SystemExit('This tree\'s monitor.c with the counter option is not the qualified monitor object')
    platform.replace_platform_object('platform/a733/monitor.c', monitor)
    platform.compile([ROOT / unit for unit in UNITS], [f'-I{ROOT}/apps/storage-read-returning'])
    platform.reuse(list(PROVIDER_UNITS))
    platform.link(); platform.binary(); platform.run()
    # Origins against rank-9-usb-v4 and the qualified kernel check.
    v4_platform, v4_provider, _ = apps.v4_parts(V4)
    work = e / 'build/compare'; work.mkdir()
    origins = {}
    for (rel, v4_obj), obj in zip(v4_platform, platform.platform_objects):
        mine, theirs = digest(obj), digest(v4_obj)
        how = ('rank-9-usb-v4, byte-identical' if mine == theirs else
               'rank-9-usb-v4, identical apart from debug sections'
               if apps.without_debug(obj, work) == apps.without_debug(v4_obj, work) else 'differs from rank-9-usb-v4')
        record = qualified['objects'].get(rel)
        if record and record['sha256'] == mine:
            how += '; byte-identical to the qualified kernel check of board-session-20261025-24'
        origins[rel] = {'sha256': mine, 'origin': how}
    for unit, obj in zip(PROVIDER_UNITS.values(), platform.provider_objects):
        mine, theirs = digest(obj), digest(v4_provider[unit])
        origins[unit + '.obj'] = {'sha256': mine, 'origin': 'rank-9-usb-v4, byte-identical' if mine == theirs else
            'rank-9-usb-v4, identical apart from debug sections'
            if apps.without_debug(obj, work) == apps.without_debug(v4_provider[unit], work) else
            'differs from rank-9-usb-v4' + ('; declared new since then in checks/board-bundles-apps.py'
                                            if unit in apps.PROVIDER_NEW else '')}
    for unit, obj in zip(UNITS, platform.objects[:len(UNITS)]):
        origins[unit + '.obj'] = {'sha256': digest(obj), 'origin': 'new unit of this image'}
    if len(platform.platform_objects) != len(CENTRAL_PLATFORM):
        raise SystemExit('Platform object count changed')
    save(e, 'origins.json', origins)
    platform.record(__file__, {'origins_sha256': digest(e / 'origins.json'),
                               'qualified_monitor': {'sha256': QUALIFIED_MONITOR, 'bundle': str(QUALIFIED),
                                                     'run': str(QUALIFIED_RUN)}})
    for name in ('README.md',):
        source = ROOT / 'apps/storage-read-returning' / name
        if source.exists(): shutil.copy2(source, e / name)
    print(f'Built the returning SD0 read image: {e / "probe.bin"} ({digest(e / "probe.bin")})')


if __name__ == '__main__':
    main()
