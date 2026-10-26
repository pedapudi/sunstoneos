#!/usr/bin/env python3
"""Build a RAM-only DDR wrapper for the pinned Orange Pi Zero 3W boot0."""
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
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", type=Path, default=CACHE / "build/a733-ddr")
# Capture variants (docs/designs/dram-training-restore.md). Without
# --variant the wrapper is byte-identical to the reviewed one.
VARIANTS = {"pstate0": (0, 0), "init-only": (0x1fff0000, 0x00010000)}
parser.add_argument("--variant", choices=sorted(VARIANTS))
args = parser.parse_args()
if args.variant and args.output == CACHE / "build/a733-ddr":
    sys.exit("A variant needs its own --output directory.")
args.output.mkdir(parents=True, exist_ok=True)
blob_path = Path(subprocess.check_output(
    ["python3", str(ROOT / "tools/fetch-sources.py"), "board-boot0"], text=True).strip())
blob = bytearray(blob_path.read_bytes())
expected = json.loads((ROOT / "sources.lock.json").read_text())["board-boot0"]["sha256"]
# Function addresses and SRAM layout were reviewed for this exact binary.
# Updating the dependency pin alone must not authorize another firmware image.
reviewed = "829ea8e3b65d71a02f2d0e094bc073de3a10869f948f62db49dbf023d07fabe3"
if expected != reviewed or hashlib.sha256(blob).hexdigest() != reviewed:
    sys.exit("boot0 differs from the reviewed DDR layout; inspect it before updating this guard.")
if (len(blob) != 0x3c000 or blob[4:12] != b"eGON.BT0"
        or struct.unpack_from("<I", blob, 16)[0] != 0x3c000
        or struct.unpack_from("<II", blob, 0xb5c) != (0x82dd0, 0x83c28)):
    sys.exit("boot0 header or BSS layout differs from the reviewed binary.")

linker = shutil.which("ld.lld") or str(CACHE / "host/usr/lib/llvm-21/bin/ld.lld")
objcopy = shutil.which("llvm-objcopy") or "/usr/lib/llvm-21/bin/llvm-objcopy"
nm = shutil.which("llvm-nm") or "/usr/lib/llvm-21/bin/llvm-nm"
for name, address in (("ddr-entry", 0x85000), ("cache-off", 0x71000)):
    source = ROOT / f"boot/a733/{name}.S"
    obj = args.output / f"{name}.o"
    elf = args.output / f"{name}.elf"
    defines = []
    if args.variant and name == "ddr-entry":
        clear, value = VARIANTS[args.variant]
        defines = [f"-Wa,--defsym,DDR_PARAM_PATCH=1", f"-Wa,--defsym,TPR10_CLEAR={clear:#x}",
                   f"-Wa,--defsym,TPR10_SET={value:#x}"]
    subprocess.run(["clang", "--target=arm-none-eabi", "-march=armv8-a", "-c", *defines,
                    str(source), "-o", str(obj)], check=True)
    subprocess.run([linker, f"-Ttext={address:#x}", "-e", "_start", str(obj),
                    "-o", str(elf)], check=True)
    subprocess.run([objcopy, "-O", "binary", str(elf),
                    str(args.output / f"{name}.bin")], check=True)
symbols = subprocess.check_output([nm, str(args.output / "ddr-entry.elf")], text=True)
log_address = next(int(line.split()[0], 16) for line in symbols.splitlines()
                   if line.split()[-1] == "log_character")
# Replace the Thumb UART character output with an interworking literal load.
offset = 0x4a540 - 0x47000
if blob[offset:offset + 8].hex() != "034b1b685a695206":
    sys.exit("Unexpected UART output function; refusing to patch.")
blob[offset:offset + 8] = bytes.fromhex("dff800f0") + struct.pack("<I", log_address)
payloads = {"boot0-head.bin": bytes(blob[:0x3000]),
            "boot0-body.bin": bytes(blob[0x3000:])}
for name, data in payloads.items():
    (args.output / name).write_bytes(data)
if ((args.output / "ddr-entry.bin").stat().st_size > 0x1000
        or (args.output / "cache-off.bin").stat().st_size > 64):
    sys.exit("Wrapper exceeds its reserved SRAM interval.")
manifest = {"boot0_sha256": expected, "load": 0x47000, "head_shadow": 0x8e000,
            "wrapper": 0x85000, "mailbox": 0x86000, "log": 0xe0000,
            "log_character": log_address, "files": {}}
if args.variant:
    manifest["variant"] = args.variant
for name in (*payloads, "ddr-entry.bin", "cache-off.bin"):
    data = (args.output / name).read_bytes()
    manifest["files"][name] = {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
(args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(args.output)
