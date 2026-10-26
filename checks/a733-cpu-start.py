#!/usr/bin/env python3
"""Compare bounded native CPU power-on with isolated published firmware helpers."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_MEM_READ
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE_SHA = "8af16b86ae8e2eff634a3ebca76a8cdf3c10b3540387573d46c3d785545b2632"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def reference(data, core):
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(0x48000000, 0x20000)
    uc.mem_write(0x48000000, data)
    uc.mem_map(0x07050000, 0x10000)
    uc.mem_map(0x08000000, 0x10000)
    uc.mem_map(0x49000000, 0x2000)
    control = 0x07050200 + core * 4
    mode = 0x07050220 + core * 4
    power = 0x07052008 + core * 0x1000
    arch = 0x08001000 + core * 0x1000
    allowed = {arch, arch + 4, arch + 8, control, mode, power}
    for addr, value in [(arch, 0xa0), (control, 0x42), (mode, 0x80), (power, 0x12340000)]:
        uc.mem_write(addr, value.to_bytes(4, "little"))
    writes = []
    waits = 0

    def access(machine, access_type, address, size, value, user):
        nonlocal waits
        if 0x49000000 <= address < 0x49002000:
            return
        assert address in allowed and size == 4, (address, size)
        if access_type == UC_MEM_READ:
            if address == power and int.from_bytes(machine.mem_read(control, 4), "little") & 1:
                waits += 1
                if waits >= 2:
                    machine.mem_write(power, (0x12340008).to_bytes(4, "little"))
        else:
            writes.append((address, value))

    uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, access)
    for offset, argument in [(0xc4a0, 0x41000000 + core * 0x4000), (0xc298, 1), (0xc314, 0)]:
        uc.reg_write(UC_ARM64_REG_X0, 0)
        uc.reg_write(UC_ARM64_REG_X1, core)
        uc.reg_write(UC_ARM64_REG_X2, argument)
        uc.reg_write(UC_ARM64_REG_SP, 0x49001000)
        uc.reg_write(UC_ARM64_REG_LR, 0x49000000)
        uc.emu_start(0x48000000 + offset, 0x49000000, count=1000)
        assert uc.reg_read(UC_ARM64_REG_PC) == 0x49000000
    assert len(writes) == 7 and waits == 2
    return writes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--resident", action="store_true", help="only cold static-OFF release cases; no firmware replay")
    args = parser.parse_args()
    evidence = args.evidence.resolve()
    assert ROOT not in [evidence, *evidence.parents]
    assert digest(args.firmware) == FIRMWARE_SHA
    evidence.mkdir(parents=True, exist_ok=True)
    sources = ["checks/a733-cpu-start.c", "platform/a733/cpu_start.c"]
    command = ["clang-21", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(ROOT / "include"),
               *[str(ROOT / x) for x in sources], "-o", str(evidence / "check")]
    if args.resident: command.insert(1, "-DSUNSTONE_BOARD_RESIDENT=1")
    build = subprocess.run(command, capture_output=True, text=True)
    (evidence / "compile.log").write_text(build.stdout + build.stderr)
    build.check_returncode()
    run = subprocess.run([str(evidence / "check")], capture_output=True, text=True)
    (evidence / "run.log").write_text(run.stdout + run.stderr)
    run.check_returncode()
    traces = {c: [] for c in range(1, 8)}
    for line in run.stdout.splitlines():
        if line.startswith("TRACE "):
            _, c, a, v = line.split()
            traces[int(c)].append((int(a, 16), int(v, 16)))
    if not args.resident:
        for core, trace in traces.items():
            assert trace == reference(args.firmware.read_bytes(), core), core
    result = {"passed": True, "resident": args.resident, "compile_command": command, "run_returncode": run.returncode,
              "firmware_sha256": FIRMWARE_SHA, "firmware_helpers": ["0xc4a0", "0xc298", "0xc314"],
              "scope": "Modeled register accesses only; seven secondary targets; no CPU execution/power or physical board qualification",
              "traces": traces, "inputs": {p: digest(ROOT / p) for p in sources + ["include/a733_cpu_start.h", "checks/a733-cpu-start.py"]},
              "executable_sha256": digest(evidence / "check")}
    (evidence / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print("PASS resident bounded static-OFF release and immediate wake" if args.resident else
          "PASS native retained-failure checks and complete writes match isolated board firmware for cores1..7")


if __name__ == "__main__":
    main()
