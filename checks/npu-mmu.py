#!/usr/bin/env python3
"""Build and run the NPU page-table check (checks/npu-mmu.c) against the production table builder.

The check walks the generated tables independently of the builder. It includes the page-zero case that the loader's
admission of unpatched pointer words below 0x1000 relies on (drivers/npu/nbg_assembly.c).
"""
import argparse, subprocess
from pathlib import Path
from harness import CLANG, SANITIZE, environment

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['checks/npu-mmu.c', 'drivers/npu/a733_npu_mmu.c', 'include/a733_npu_mmu.h', 'include/a733_range.h']
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
exe = args.output / 'npu-mmu'
subprocess.run([str(CLANG), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *SANITIZE, '-I' + str(ROOT / 'include'),
                str(ROOT / 'checks/npu-mmu.c'), str(ROOT / 'drivers/npu/a733_npu_mmu.c'), '-o', str(exe)], check=True,
               env=environment())
done = subprocess.run([str(exe)], capture_output=True, text=True, env=environment())
assert done.returncode == 0, done.stdout + done.stderr[-600:]
print(done.stdout.strip())
