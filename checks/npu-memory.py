#!/usr/bin/env python3
"""Build and run the NPU memory layer check (checks/npu-memory.c) against the production memory layer, table builder and controller.

The check gives the layer separate CPU and device copies of every buffer and walks the device's copy of the tables with a
walker of its own. It covers private copies of the buffer list, the inspection of every table byte before each
publication, cache maintenance per job and per session group, controller ownership, fault retention, and refusals at
admission that leave the memory object as it was.
"""
import argparse, subprocess
from pathlib import Path
from harness import CLANG, SANITIZE, environment

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ['drivers/npu/a733_npu_memory.c', 'drivers/npu/a733_npu_mmu.c', 'drivers/npu/a733_npu_controller.c']
CHECK_INPUTS = ['checks/npu-memory.c', 'drivers/npu/a733_npu_memory.c', 'drivers/npu/a733_npu_mmu.c',
                'drivers/npu/a733_npu_controller.c', 'include/a733_npu_memory.h', 'include/a733_npu_mmu.h',
                'include/a733_npu_controller.h', 'include/a733_range.h']
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
exe = args.output / 'npu-memory'
subprocess.run([str(CLANG), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *SANITIZE, '-I' + str(ROOT / 'include'),
                str(ROOT / 'checks/npu-memory.c'), *(str(ROOT / s) for s in SOURCES), '-o', str(exe)], check=True,
               env=environment())
done = subprocess.run([str(exe)], capture_output=True, text=True, env=environment())
assert done.returncode == 0, done.stdout + done.stderr[-600:]
print(done.stdout.strip())
