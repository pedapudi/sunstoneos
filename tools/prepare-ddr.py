#!/usr/bin/env python3
"""Build a RAM-only DDR wrapper for the pinned Orange Pi Zero 3W boot0."""
import argparse
import hashlib
import importlib.util
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
VARIANTS = {"pstate0": (0, 0), "init-only": (0x1fff0000, 0x00010000), "restore": None}
parser.add_argument("--variant", choices=sorted(VARIANTS))
parser.add_argument("--restore-set", type=Path, help="reviewed restore-set-v3.json; required for restore")
args = parser.parse_args()
if (args.restore_set is not None) != (args.variant == "restore"):
    sys.exit("--restore-set goes with, and only with, --variant restore.")
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
        if args.variant == "restore":
            defines = ["-Wa,--defsym,DDR_RESTORE=1"]
        else:
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
restore_manifest = None
if args.variant == "restore":
    spec = importlib.util.spec_from_file_location("training_record", ROOT / "tools/training-record.py")
    training = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(training)
    profile = training.profile(training.restore_set(args.restore_set))
    generated = (f"#define BOOT0_CHECKSUM {struct.unpack_from('<I', blob, 12)[0]:#x}u\n"
                 f"#define STRAP_CRC {training.crc(blob[0x574:0x5f4]):#x}u\n"
                 f"#define PROFILE_COUNT {len(profile)}u\n"
                 "static const struct { uint32_t address; uint16_t mask; } restore_profile[] = {\n")
    generated += "".join(f"{{{address:#x}u, {mask:#x}u}},\n" for address, mask in profile) + "};\n"
    (args.output / "restore-profile.h").write_text(generated)
    objects = []
    for name in ("ddr-restore.c", "ddr-restore-entry.S"):
        obj = args.output / (name + ".o")
        subprocess.run(["clang", "--target=arm-none-eabi", "-march=armv8-a", "-marm", "-Os",
                        "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-mno-unaligned-access",
                        "-Wall", "-Wextra", "-Werror", "-I", str(args.output), "-c",
                        str(ROOT / "boot/a733" / name), "-o", str(obj)], check=True)
        objects.append(str(obj))
    script = args.output / "restore.ld"
    script.write_text("SECTIONS { . = 0x92000; .text : { *(.entry) *(.text*) *(.rodata*) } "
                      "/DISCARD/ : { *(.ARM.exidx*) *(.comment*) } }\n")
    subprocess.run([linker, "-T", str(script), "-e", "restore_entry", *objects,
                    "-o", str(args.output / "ddr-restore.elf")], check=True)
    subprocess.run([objcopy, "-O", "binary", str(args.output / "ddr-restore.elf"),
                    str(args.output / "ddr-restore.bin")], check=True)
    # Raw uploads do not initialize NOLOAD/BSS. Inspect the linked allocatable
    # sections, not only the binary length, before declaring SRAM ownership.
    linked = (args.output / "ddr-restore.elf").read_bytes()
    if linked[:7] != b"\x7fELF\x01\x01\x01":
        sys.exit("Restore helper is not a little-endian ELF32 image.")
    section_offset = struct.unpack_from("<I", linked, 32)[0]
    section_size, section_count, names_index = struct.unpack_from("<3H", linked, 46)
    sections = [struct.unpack_from("<10I", linked, section_offset + i*section_size)
                for i in range(section_count)]
    names = sections[names_index]
    strings = linked[names[4]:names[4]+names[5]]
    allocated = []
    for section in sections:
        name, kind, flags, address, offset, size = section[:6]
        if flags & 2 and size:
            if kind != 1 or not 0x92000 <= address < address+size <= 0x9a000:
                sys.exit("Restore helper has uninitialized or out-of-reservation allocated sections.")
            allocated.append({"name": strings[name:].split(b"\0",1)[0].decode(),
                              "address": address, "size": size})
    if (args.output / "ddr-restore.bin").stat().st_size > 0x8000:
        sys.exit("Restore helper exceeds its SRAM interval.")
    hook = next(int(line.split()[0], 16) for line in symbols.splitlines() if line.split()[-1] == "restore_hook")
    patch_source = args.output / "restore-call.S"
    # Keep the halfword-aligned call at its actual address. ELF .text defaults
    # to four-byte alignment, so a section requested at 0x56142 can move.
    patch_source.write_text(".syntax unified\n.arch armv8-a\n.thumb\n.text\nnop\nblx restore_hook\n")
    subprocess.run(["clang", "--target=arm-none-eabi", "-c", str(patch_source), "-o",
                    str(args.output / "restore-call.o")], check=True)
    subprocess.run([linker, "-Ttext=0x56140", "-e", "0x56142", f"--defsym=restore_hook={hook:#x}",
                    str(args.output / "restore-call.o"), "-o", str(args.output / "restore-call.elf")], check=True)
    subprocess.run([objcopy, "-O", "binary", str(args.output / "restore-call.elf"),
                    str(args.output / "restore-call.bin")], check=True)
    patch_image = (args.output / "restore-call.bin").read_bytes()
    patch = patch_image[2:]
    if len(patch_image) != 6 or blob[0xf142:0xf146].hex() != "f5f7b1fd":
        sys.exit("Unexpected original or replacement timing call.")
    blob[0xf142:0xf146] = patch
    restore_manifest = {"set_sha256": training.RESTORE_SET_SHA256, "helper": 0x92000,
                        "helper_limit": 0x8000, "record": 0x87000, "record_limit": training.RECORD_LIMIT,
                        "stack": [0x9b000, 0x9f000], "hook": hook, "phy_registers": len(profile),
                        "sample_bytes": 8192, "sample_read_bytes": 278528,
                        "allocated_sections": allocated,
                        "profile_sha256": hashlib.sha256(generated.encode()).hexdigest(),
                        "errors": {"geometry": 1, "header_crc": 2, "payload_crc": 3, "chip": 4,
                                   "boot0": 5, "strap": 6, "readback": 7, "size": 8,
                                   "memory": 9, "hook": 10, "entry": 11}}
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
if restore_manifest:
    manifest["restore"] = restore_manifest
    manifest["sources"] = {name: hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in
                           ("boot/a733/ddr-entry.S", "boot/a733/ddr-restore.c",
                            "boot/a733/ddr-restore-entry.S", "boot/a733/cache-off.S",
                            "tools/prepare-ddr.py", "tools/training-record.py")}
for name in (*payloads, "ddr-entry.bin", "cache-off.bin", *(("ddr-restore.bin",) if restore_manifest else ())):
    data = (args.output / name).read_bytes()
    manifest["files"][name] = {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
(args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(args.output)
