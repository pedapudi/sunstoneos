#!/usr/bin/env python3
"""Build a returning SD0 read image from a configured central CMake build.

The selected build supplies platform objects and toolchain flags. The monitor
is compiled with counter restoration enabled. Providers and storage units compile
from current source; output records retain their hashes and dependencies.
No prior board run or separately approved object hash is required.
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
from returning_build import CENTRAL_PLATFORM, PROVIDER_UNITS, ReturningPlatform, digest, save
from host_evidence import new_directory, unique_directory  # noqa: E402

UNITS = ['apps/storage-read-returning/main.c', 'apps/storage-read-returning/return.c', 'drivers/storage/sunxi_mmc.c', 'drivers/storage/a733_mmc_platform.c',
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
    e = new_directory(e)
    apps = load('storage_read_returning_bba', ROOT / 'checks/board-bundles-apps.py')
    platform = ReturningPlatform(e, build=central)
    monitor = e / 'build/monitor-counter-option.obj'
    apps.compile_object(central, 'a733-power-probe', ROOT / 'platform/a733/monitor.c', monitor,
                        ('-DSUNSTONE_MONITOR_COUNTERS=1',), True)
    platform.replace_platform_object('platform/a733/monitor.c', monitor)
    platform.compile([ROOT / unit for unit in UNITS], [f'-I{ROOT}/apps/storage-read-returning'])
    platform.reuse(list(PROVIDER_UNITS))
    platform.link(options=['-Wl,--wrap=monitor_init','-Wl,--wrap=monitor_exception']); platform.binary(); platform.run()
    origins = {str(obj): {'sha256': digest(obj)} for obj in
               [*platform.platform_objects, *platform.provider_objects, *platform.objects]}
    if len(platform.platform_objects) != len(CENTRAL_PLATFORM):
        raise SystemExit('Platform object count changed')
    save(e, 'origins.json', origins)
    platform.record(__file__, {'origins_sha256': digest(e / 'origins.json')})
    for name in ('README.md',):
        source = ROOT / 'apps/storage-read-returning' / name
        if source.exists(): shutil.copy2(source, e / name)
    print(f'Built the returning SD0 read image: {e / "probe.bin"} ({digest(e / "probe.bin")})')


if __name__ == '__main__':
    main()
