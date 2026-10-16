#!/usr/bin/env python3
"""Build pinned newlib with dynamic task state and retargetable locks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
cache = Path.home() / ".cache/sunstoneos"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--toolchain", type=Path, default=cache / "host/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf")
parser.add_argument("--jobs", type=int, default=8)
args = parser.parse_args()
if args.jobs < 1:
    parser.error("jobs must be positive")
source = Path(subprocess.check_output([sys.executable, str(root / "tools/fetch-sources.py"), "newlib"], text=True).strip())


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


flags = "-O2 -g -march=armv8-a -mstrict-align -ffunction-sections -fdata-sections -D__DYNAMIC_REENT__ -DREENTRANT_SYSCALLS_PROVIDED"
patch = root / "patches/newlib-reclaim.patch"
options = ["--target=aarch64-none-elf", "--disable-multilib", "--disable-libgloss",
           "--disable-newlib-supplied-syscalls", "--enable-newlib-multithread",
           "--enable-newlib-retargetable-locking", "--enable-newlib-reent-check-verify",
           "--enable-newlib-io-long-long", "--enable-newlib-io-c99-formats",
           "--enable-newlib-register-fini", "--enable-newlib-mb"]
# Source content and tools identify this build independently of Git commits.
identity = {"source": {str(p.relative_to(source)): digest(p) for p in sorted(source.rglob("*")) if p.is_file()},
            "tools": {name: digest(args.toolchain / "bin" / ("aarch64-none-elf-" + name))
                      for name in ("gcc", "as", "ar", "ranlib")},
            "cc1": digest(args.toolchain / "libexec/gcc/aarch64-none-elf/14.3.1/cc1"),
            "flags": flags, "options": options, "builder": digest(__file__), "patch": digest(patch)}
key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:16]
build = cache / "build/newlib-smp" / key
prefix = cache / "host" / ("newlib-smp-" + key)
build.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, PATH=str(args.toolchain.resolve() / "bin") + os.pathsep + os.environ["PATH"],
           CFLAGS_FOR_TARGET=flags)
patched = build / "source"
if not patched.exists():
    shutil.copytree(source, patched)
    subprocess.run(["patch", "--batch", "--forward", "-p1", "-i", str(patch)], cwd=patched,
                   stdout=subprocess.DEVNULL, check=True)
commands = [[str(patched / "configure"), *options, "--prefix=" + str(prefix)],
            ["make", "-j", str(args.jobs), "all-target-newlib"],
            ["make", "install-target-newlib"]]
record = prefix / "build.json"
if not record.exists():
    with (build / "build.log").open("w") as log:
        for command in commands:
            result = subprocess.run(command, cwd=build, env=env, stdout=log, stderr=subprocess.STDOUT)
            if result.returncode:
                sys.exit(f"Newlib build failed; inspect {build / 'build.log'}")
    outputs = {str(p.relative_to(prefix)): digest(p) for p in sorted(prefix.rglob("*")) if p.is_file()}
    record.write_text(json.dumps({"identity": identity, "commands": commands, "outputs": outputs}, indent=2) + "\n")
else:
    previous = json.loads(record.read_text())
    if any(digest(prefix / name) != value for name, value in previous["outputs"].items()):
        sys.exit(f"Installed newlib files changed: {prefix}")
print(prefix)
