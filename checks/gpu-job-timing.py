#!/usr/bin/env python3
"""Run the diagnostic producer with modeled architectural counter/barriers."""
import argparse
import json
import os
import pathlib
import shlex
import subprocess
from harness import CLANG

parser = argparse.ArgumentParser()
parser.add_argument("compile_commands", type=pathlib.Path)
parser.add_argument("--output", type=pathlib.Path, required=True)
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[1]
commands = json.loads(args.compile_commands.read_text())
command = next(c for c in commands if c["file"].endswith("/apps/os/gpu.c"))
flags = [f for f in shlex.split(command["command"]) if f.startswith(("-I", "-D"))]
# The host calls the diagnostic crumb callback directly. The board's SVC
# transport is AArch64-only and is covered by the native build and queued run.
flags = [f for f in flags if "newlib" not in f.lower()
         and not f.startswith("-DSUNSTONE_CRUMBS")]
flags.insert(0, "-I" + str(root / "include"))
args.output.mkdir(parents=True, exist_ok=True)
os.environ["TMPDIR"] = str(args.output)
source = (root / "apps/os/gpu-timing.c").read_text()
old = 'uint64_t t; __asm__ volatile("isb; mrs %0, cntpct_el0":"=r"(t)::"memory"); return t;'
assert source.count(old) == 1
source = source.replace(old, 'static uint64_t t; return ++t;')
for barrier in ("oshld", "oshst"):
    source = source.replace(f'__asm__ volatile("dmb {barrier}":::"memory");', '')
source += '\n#include "' + str(root / 'checks/gpu-job-timing.c') + '"\n'
model = args.output / "gpu-job-timing-host.c"
model.write_text(source)
binary = args.output / "gpu-job-timing-host"
subprocess.run([str(CLANG), "-DSUNSTONE_GPU_TIMING=1", *flags, "-UNDEBUG", "-O1", "-g",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                str(model), "-o", str(binary)], check=True)
subprocess.run([str(binary)], check=True)
print("GPU job timing producer, lifetime, cache filter, IRQ classification and capacity passed")
