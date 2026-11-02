#!/usr/bin/env python3
"""Run checks/a733-mmc-dma-chain.c (the platform's descriptor-chain admission for the bounded DMA write)."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser()
ap.add_argument('--evidence', type=Path, required=True)
ap.add_argument('--platform', type=Path, default=root / 'drivers/storage/a733_mmc_platform.c', help='platform source (mutation checks pass an older copy)')
a = ap.parse_args()
a.evidence.mkdir(parents=True, exist_ok=True)
original = (root / 'checks/sunxi-mmc.c').read_text()
(a.evidence / 'mmc-card-model.h').write_text(original[original.index('struct model {'):original.index('static int barrier(void *ctx)')])
cc = shutil.which('clang-21') or shutil.which('clang')
test = (root / 'checks/a733-mmc-dma-chain.c').read_text().replace('"../drivers/storage/a733_mmc_platform.c"', '"%s"' % a.platform.resolve())
(a.evidence / 'dma-chain.c').write_text(test)
sources = ['drivers/storage/a733_storage_update.c', 'drivers/storage/storage_digest.c', 'drivers/storage/sunxi_mmc.c', 'drivers/clock/a733_clock_leases.c',
           'drivers/power/a733_rail_leases.c', 'drivers/power/a733_pmic.c', 'drivers/gpu/a733_gpu_resources.c', 'drivers/pinctrl/a733_pins.c',
           'drivers/pinctrl/a733_pin_tables.c']
binary = a.evidence / 'dma-chain'
done = subprocess.run([cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Iinclude',
                       '-I' + str(root / 'drivers/storage'), str(a.evidence / 'dma-chain.c'), *sources, '-o', str(binary)], cwd=root, capture_output=True, text=True)
if done.returncode:
    sys.exit(done.stderr)
done = subprocess.run([str(binary)], capture_output=True, text=True)
print(done.stdout, end='')
if done.returncode:
    sys.exit(done.stdout + done.stderr)
