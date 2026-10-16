#!/usr/bin/env python3
"""Run selected GPU service integration cases with one compiled executable."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--nm", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cases", type=int, nargs="+", choices=(1, 2, 3), default=[1, 2, 3])
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    original = args.elf.read_bytes()
    if original[:6] != b"\x7fELF\x02\x01":
        raise ValueError("Expected a little-endian ELF64 image")
    symbols = subprocess.check_output([str(args.nm), "-S", str(args.elf)], text=True)
    symbol = [line.split() for line in symbols.splitlines()
              if line.split() and line.split()[-1] == "gpu_service_case"]
    if len(symbol) != 1 or int(symbol[0][1], 16) != 4:
        raise ValueError("Expected one four-byte gpu_service_case symbol")
    address = int(symbol[0][0], 16)
    phoff = struct.unpack_from("<Q", original, 32)[0]
    entry_size, count = struct.unpack_from("<HH", original, 54)
    offsets = []
    for index in range(count):
        kind, flags, offset, va, pa, filesz, memsz, align = struct.unpack_from(
            "<II6Q", original, phoff + index * entry_size)
        if kind == 1 and va <= address and address + 4 <= va + filesz:
            offsets.append(offset + address - va)
    if len(offsets) != 1 or struct.unpack_from("<I", original, offsets[0])[0] != 1:
        raise ValueError("Expected the default case in one file-backed segment")
    results = []
    for case in args.cases:
        image = bytearray(original)
        struct.pack_into("<I", image, offsets[0], case)
        path = args.output / f"case-{case}.elf"
        path.write_bytes(image)
        command = [str(args.qemu), "-machine", "virt,gic-version=3,secure=off,virtualization=off",
                   "-cpu", "cortex-a55", "-smp", "2", "-m", "128M", "-nographic",
                   "-monitor", "none", "-semihosting-config", "enable=on,target=native",
                   "-kernel", str(path)]
        (args.output / f"case-{case}-command.json").write_text(json.dumps(command, indent=2) + "\n")
        run = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=10)
        (args.output / f"case-{case}.log").write_text(run.stdout)
        marker = f"PASS GPU service: case 0x{case:016x};"
        result = {"case": case, "sha256": hashlib.sha256(image).hexdigest(),
                  "returncode": run.returncode, "marker_matches": marker in run.stdout,
                  "fixture_word_file_offset": offsets[0]}
        results.append(result)
        (args.output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print(run.stdout, end="")
        if run.returncode or not result["marker_matches"]:
            raise RuntimeError(f"GPU service case {case} failed")


if __name__ == "__main__":
    main()
