#!/usr/bin/env python3
"""Build the monitor-only cold-entry recorder in a new ordinary directory."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--clang", default="/usr/bin/clang-21")
    parser.add_argument("--lld", default="/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21")
    parser.add_argument("--objcopy", default="/usr/lib/llvm-21/bin/llvm-objcopy")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    obj, elf, binary = (args.output / name for name in ("cold-entry.o", "cold-entry.elf", "monitor.bin"))
    commands = [
        [args.clang, "--target=aarch64-none-elf", "-g", "-ffreestanding",
         "-I", str(ROOT / "include"), "-c", str(ROOT / "boot/a733/cold-entry.S"), "-o", str(obj)],
        [args.lld, "-T", str(ROOT / "boot/a733/cold-entry.ld"), str(obj), "-o", str(elf)],
        [args.objcopy, "-O", "binary", str(elf), str(binary)],
    ]
    for command in commands:
        subprocess.run(command, check=True)
    raw = binary.read_bytes()
    word = struct.unpack_from("<I", raw)[0]
    if not (word & 0xfc000000 == 0x14000000 and
            0x108 <= (word & 0x3ffffff) * 4 < len(raw) and
            raw[4:12] == b"monitor\0" and
            struct.unpack_from("<I", raw, 0x2c)[0] == 0x47000000 and
            len(raw) <= 0x8000):
        raise ValueError("Cold monitor header or extent differs.")
    sources = ["boot/a733/cold-entry.S", "boot/a733/cold-entry.ld",
               "include/a733_cpu_entry.h", "tools/prepare-cold-entry.py"]
    manifest = {
        "schema": 1, "kind": "a733-microsd-monitor-container-input",
        "monitor": {"format": "sunstone-a733-flat-monitor-v1", "path": "monitor.bin",
                    "bytes": len(raw), "sha256": sha(binary)},
    }
    (args.output / "input.json").write_text(json.dumps(manifest, indent=2) + "\n")
    evidence = {
        "physical_access": False, "physical_boot_qualified": False,
        "application_admission": False, "commands": commands,
        "sources": {name: sha(ROOT / name) for name in sources},
        "outputs": {path.name: sha(path) for path in (obj, elf, binary)},
        "scope": "Primary EL3 entry recorder. Captures boot0 metadata and CPU state, then parks. No FreeRTOS entry, remote-writer exclusion, peripheral access or recovery.",
    }
    (args.output / "build.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(binary)


if __name__ == "__main__":
    main()
