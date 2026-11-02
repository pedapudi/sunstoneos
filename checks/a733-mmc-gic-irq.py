#!/usr/bin/env python3
"""Run checks/a733-mmc-gic-irq.c: the SD0 platform against the strict controller model with INTID 193 enabled, masked and in irq mode.

Usage: a733-mmc-gic-irq.py --evidence DIR [--trace]   (--trace also prints the register sequences of the polled and the interrupt read)"""
import argparse
import pathlib
import subprocess
import shutil
import sys

root = pathlib.Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser()
ap.add_argument('--evidence', type=pathlib.Path, required=True)
ap.add_argument('--trace', action='store_true')
a = ap.parse_args()
a.evidence.mkdir(parents=True, exist_ok=True)
original = (root / 'checks/sunxi-mmc.c').read_text()
model = original[original.index('struct model {'):original.index('static int barrier(void *ctx)')]
(a.evidence / 'mmc-card-model.h').write_text(model)
cc = shutil.which('clang-21') or shutil.which('clang')
sources = ['drivers/storage/a733_storage_update.c', 'drivers/storage/storage_digest.c', 'drivers/storage/a733_mmc_platform.c', 'drivers/storage/sunxi_mmc.c',
           'drivers/clock/a733_clock_leases.c', 'drivers/power/a733_rail_leases.c', 'drivers/power/a733_pmic.c', 'drivers/gpu/a733_gpu_resources.c',
           'drivers/pinctrl/a733_pins.c', 'drivers/pinctrl/a733_pin_tables.c', 'checks/a733-mmc-gic-irq.c']
binary = a.evidence / 'gic-irq'
build = [cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
         '-DA733_MMC_LOCAL_CLOCK_RETAIN=1', '-Iinclude', '-Ichecks', '-I' + str(a.evidence), *sources, '-o', binary]
done = subprocess.run([str(c) for c in build], cwd=root, capture_output=True, text=True)
if done.returncode:
    sys.exit(done.stderr)
done = subprocess.run([str(binary)] + (['--trace'] if a.trace else []), cwd=root, capture_output=True, text=True)
print(done.stdout, end='')
if done.returncode:
    sys.exit(done.stderr)
