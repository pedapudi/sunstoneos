#!/usr/bin/env python3
"""Build the register reader and stage tables for the DRAM training capture.

The capture reads the DRAM controller and the Synopsys-type DDR PHY after a
normal cold training (docs/designs/dram-fast-boot.md). Every range below is
bounded by addresses that the pinned boot0 (SHA-256 829ea8e3...) itself
accesses; the citation of each range names the boot0 load address that
shows it. Stages run in order of increasing risk, one FEL execution each,
so an access that stalls the bus loses only its own stage.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / ".cache/sunstoneos"
TABLE = 0x43000000
OUTPUT = 0x43100000
OUTPUT_STRIDE = 0x40000
READER = 0x85000
MAGIC = 0x50414344
H16, H32, PHY, UCCLK = 1, 2, 0x100, 0x200
PHY_BASE = 0x0a900000          # PHY CSR n is at PHY_BASE + 2 * n.


def halfwords(first, last):
    """Inclusive byte range of 16-bit PHY registers."""
    return (first, (last - first) // 2 + 1)


def words(first, last):
    return (first, (last - first) // 4 + 1)


# Vendor training-save window: boot0 0x576e4 clears MicroContMuxSel and reads
# two registers at 0x0a92009c + 0x2000 * byte lane block + 0x200 * lane for
# four blocks and nine lanes, then sets MicroContMuxSel to 1.
vendor_window = [(0x0a92009c + 0x2000 * block + 0x200 * lane, 2, H16 | PHY, f"dbyte{block} lane{lane} 0x9c-0x9e")
                 for block in range(4) for lane in range(9)]

STAGES = [
    {"name": "identity",
     "purpose": "Chip ID and thermal calibration words from the SID shadow, which has no clock gate.",
     "entries": [
         (*words(0x03006200, 0x0300620c), H32, "chip ID; boot0 compares these with the boot parameter region (0x5d9d8)"),
         (*words(0x03006244, 0x0300624c), H32, "thermal calibration fields (docs/thermal.md)"),
     ]},
    {"name": "vendor-window",
     "purpose": "The registers boot0's own training-save routine reads, with its MicroContMuxSel write only.",
     "entries": vendor_window},
    {"name": "controller",
     "purpose": "32-bit DRAM controller words over the extents boot0 programs.",
     "entries": [
         (*words(0x0a020000, 0x0a02022c), H32, "common block; boot0 literals 0x0a020010-0x0a020228"),
         (*words(0x0a021800, 0x0a02180c), H32, "common block; literals 0x0a021804-0x0a02180c"),
         (*words(0x0a021d00, 0x0a021d0c), H32, "common block; literals 0x0a021d04-0x0a021d0c"),
         (*words(0x0a030000, 0x0a03006c), H32, "literals 0x0a030008-0x0a030068"),
         (*words(0x0a110000, 0x0a110d00), H32, "channel 0; literals 0x0a110010-0x0a110d00, e.g. 0x57a20"),
         (*words(0x0a510000, 0x0a510d00), H32, "channel 1 (+0x400000); literals 0x0a510184-0x0a510b84, 0x4f51a"),
     ]},
    {"name": "phy-blocks",
     "purpose": "16-bit PHY registers over each block's boot0 extent, with the clock enable NXP uses.",
     "entries": [
         (*halfwords(0x0aaa0000, 0x0aaa01ce), H16 | PHY | UCCLK, "APBONLY (CSR 0xd0000); 0x4f47c"),
         (*halfwords(0x0aa80000, 0x0aa80242), H16 | PHY | UCCLK, "DRTUB (CSR 0xc0000)"),
         (*halfwords(0x0a940000, 0x0a940620), H16 | PHY | UCCLK, "MASTER (CSR 0x20000)"),
         (*halfwords(0x0a960000, 0x0a960660), H16 | PHY | UCCLK, "block CSR 0x30000, instance 0"),
         (*halfwords(0x0a962000, 0x0a962660), H16 | PHY | UCCLK, "block CSR 0x31000, instance 1"),
         *[(*halfwords(0x0a920000 + 0x2000 * block, 0x0a9210a0 + 0x2000 * block), H16 | PHY | UCCLK,
            f"DBYTE{block} (CSR 0x{0x10000 + 0x1000 * block:x})") for block in range(4)],
         (*halfwords(0x0a982000, 0x0a9838b0), H16 | PHY | UCCLK, "ACSM (CSR 0x41000)"),
         (*halfwords(0x0aa20000, 0x0aa21820), H16 | PHY | UCCLK, "INITENG (CSR 0x90000)"),
         # boot0 names offsets 0x2..0x1bec of this block literally and reaches
         # the copies of P-states 1 to 3 by adding 0x2000 per P-state (0x4bca8).
         *[(*halfwords(0x0aa40000 + 0x2000 * state, 0x0aa41bec + 0x2000 * state), H16 | PHY | UCCLK,
            f"CSR 0x{0xa0000 + 0x1000 * state:x}, P-state {state} copy") for state in range(4)],
     ]},
    {"name": "message-block",
     "purpose": "The training firmware's message block in PHY data memory, last because it is the least precedented.",
     "entries": [
         (*halfwords(0x0a9b0000, 0x0a9b09a0), H16 | PHY | UCCLK, "DMEM (CSR 0x58000); 0x59d00"),
     ]},
]


def build_tables():
    stages = []
    for index, stage in enumerate(STAGES):
        output = OUTPUT + OUTPUT_STRIDE * index
        table = bytearray(struct.pack("<8I", MAGIC, 1, len(stage["entries"]), output,
                                      0xffffffff, 0xffffffff, 0xffffffff, 0))
        layout = []
        offset = 0
        for address, count, flags, note in stage["entries"]:
            if not 1 <= count <= 0x10000:
                sys.exit(f"{stage['name']}: count {count} at {address:#x} is out of range")
            width = 2 if flags & 3 == H16 else 4
            if address % width or (flags & PHY and not 0x0a900000 <= address < 0x0ab00000):
                sys.exit(f"{stage['name']}: bad entry {address:#x}")
            table += struct.pack("<4I", address, count, flags, 0)
            layout.append({"address": address, "count": count, "width": width,
                           "phy": bool(flags & PHY), "ucclk": bool(flags & UCCLK),
                           "offset": offset, "note": note})
            offset = (offset + count * width + 3) & ~3
        if offset > OUTPUT_STRIDE or len(table) > 0x10000:
            sys.exit(f"{stage['name']}: output or table too large")
        stages.append({"name": stage["name"], "purpose": stage["purpose"],
                       "table": bytes(table), "output": output, "output_bytes": offset,
                       "entries": layout})
    return stages


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True,
                        help="new directory for the reader, tables and manifest")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    linker = shutil.which("ld.lld") or str(CACHE / "host/usr/lib/llvm-21/bin/ld.lld")
    objcopy = shutil.which("llvm-objcopy") or "/usr/lib/llvm-21/bin/llvm-objcopy"
    for name, address in (("ddr-capture", READER), ("cache-off", 0x71000)):
        source = ROOT / f"boot/a733/{name}.S"
        obj, elf = args.output / f"{name}.o", args.output / f"{name}.elf"
        subprocess.run(["clang", "--target=arm-none-eabi", "-march=armv8-a", "-c",
                        str(source), "-o", str(obj)], check=True)
        subprocess.run([linker, f"-Ttext={address:#x}", "-e", "_start", str(obj), "-o", str(elf)],
                       check=True)
        subprocess.run([objcopy, "-O", "binary", str(elf), str(args.output / f"{name}.bin")],
                       check=True)
    if (args.output / "ddr-capture.bin").stat().st_size > 0x400:
        sys.exit("Reader exceeds its 1 KiB SRAM interval.")
    files = ["ddr-capture.bin", "cache-off.bin"]
    manifest_stages = []
    for index, stage in enumerate(build_tables()):
        name = f"stage{index + 1}-{stage['name']}.bin"
        (args.output / name).write_bytes(stage["table"])
        files.append(name)
        manifest_stages.append({key: value for key, value in stage.items() if key != "table"}
                               | {"file": name})
    manifest = {
        "kind": "a733-ddr-capture-build",
        "reader": READER, "table": TABLE, "cache_off": 0x71000,
        "sources": {name: hashlib.sha256((ROOT / f"boot/a733/{name}.S").read_bytes()).hexdigest()
                    for name in ("ddr-capture", "cache-off")}
        | {"prepare-ddr-capture.py": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()},
        "stages": manifest_stages,
        "files": {name: {"size": (args.output / name).stat().st_size,
                         "sha256": hashlib.sha256((args.output / name).read_bytes()).hexdigest()}
                  for name in files},
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
