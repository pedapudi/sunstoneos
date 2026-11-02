#!/usr/bin/env python3
"""Run the native adapter's real register filter on the host: every CMD25 and write-threshold rule of the bounded writer,
in a write-capable build and in a read-only build (checks/a733-mmc-native-filter.c, drivers/storage/a733_mmc_platform_native.c)."""
import argparse
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
KERNEL = Path.home() / '.cache/sunstoneos/sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    a = ap.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    for name, define in (('write-capable', ['-DA733_MMC_BOUNDED_WRITER']), ('without-writer', []), ('read-only', ['-DSUNXI_MMC_READ_ONLY'])):
        binary = out / name
        r = subprocess.run([str(harness.CLANG), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *harness.SANITIZE,
                            '-DA733_MMC_NATIVE_HOST_TEST', *define, '-I', str(ROOT / 'include'), '-I', str(ROOT / 'checks/host'),
                            '-I', str(KERNEL / 'include'), str(ROOT / 'checks/a733-mmc-native-filter.c'),
                            str(ROOT / 'drivers/storage/a733_mmc_platform_native.c'),
                            str(ROOT / 'drivers/storage/a733_storage_bounds.c'), str(ROOT / 'drivers/storage/storage_digest.c'),
                            '-o', str(binary)], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        r = subprocess.run([str(binary)], capture_output=True, text=True, env=harness.environment())
        assert r.returncode == 0, r.stdout + r.stderr
        print(name + ':', r.stdout.strip())


if __name__ == '__main__':
    main()
