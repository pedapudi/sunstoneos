#!/usr/bin/env python3
"""Run the reviewed SRAM configuration trial without enabling a watchdog timer."""
import argparse
import fcntl
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / ".cache/sunstoneos"
IMAGE_HASH = "01a47f9bd1cc3c61353c988f0b34a9168d3311db591ba63ddd9b538009dd09eb"
FEL_HASH = "4a54cdc179baafbdecac47eb85714c3745d6d9952e05b482c7016afd4b22bf3e"
ROM_HASH = "a8e31821e8a1611d75867825914c58373c732742cb8f2235e8168c1d7e93b5aa"
REGISTERS = (0x02050014, 0x02050010, 0x02050018, 0x070501c0,
             0x070501c4, 0x070501d4, 0x070501d8, 0x07090108)
REGIONS = (("image", 0x52000, 0x800), ("mailbox", 0x52800, 64),
           ("stack", 0x53000, 0x1000), ("scratch", 0x71000, 128))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fel", type=Path, required=True)
    parser.add_argument("--image", type=Path,
                        default=CACHE / "build/watchdog-configuration/probe.bin")
    parser.add_argument("--rom", type=Path, required=True,
                        help="pinned physical board ROM capture")
    parser.add_argument("--evidence", type=Path,
                        default=CACHE / "evidence" / time.strftime("watchdog-configuration-%Y%m%d-%H%M%S"))
    args = parser.parse_args()
    image, rom = args.image.read_bytes(), args.rom.read_bytes()
    for name, data, expected in (("image", image, IMAGE_HASH), ("ROM", rom, ROM_HASH),
                                 ("FEL tool", args.fel.read_bytes(), FEL_HASH)):
        if hashlib.sha256(data).hexdigest() != expected:
            parser.error(f"{name} differs from the reviewed input")
    spec = importlib.util.spec_from_file_location("access", ROOT / "tools/usb-access.py")
    access = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(access)
    sys.path.insert(0, str(Path.home() / "git/mimus/tools"))
    import usb_guard
    usb_guard.check_health()
    args.evidence.mkdir(parents=True, exist_ok=False)
    record = dict(image_sha256=IMAGE_HASH, rom_sha256=ROM_HASH, tool_sha256=FEL_HASH,
                  transactions=[], complete=False)
    node, identity = None, None
    executing = False

    def save():
        (args.evidence / "session.json").write_text(json.dumps(record, indent=2) + "\n")

    def transaction(*arguments):
        if access.identity(node) != identity:
            raise RuntimeError("Device identity changed; no further requests")
        command = [str(args.fel), "-d", f"{int(node.parent.name)}:{int(node.name)}",
                   *map(str, arguments)]
        entry = {"command": command}
        record["transactions"].append(entry)
        save()
        result = subprocess.run(command, capture_output=True, text=True, timeout=15)
        entry.update(exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr)
        save()
        if result.returncode:
            raise RuntimeError(f"FEL request failed; stopped: {result.stderr.strip()}")
        return result.stdout

    def read_registers():
        words = transaction(*[arg for address in REGISTERS for arg in ("readl", hex(address))]).split()
        if len(words) != len(REGISTERS):
            raise RuntimeError("Unexpected register response length")
        return [int(word, 16) for word in words]

    try:
        with access.board_lock() as node:
            access.require_session(node)
            access.require_rom(node)
            identity = access.identity(node)
            record.update(node=str(node), node_identity=identity, port=access.target(node).name)
            save()
            if "soc=00001903(A733)" not in transaction("version"):
                raise RuntimeError("Unexpected SoC")
            transaction(*[arg for name, address, length in REGIONS for arg in
                          ("read", hex(address), length, args.evidence / f"{name}-before.bin")])
            transaction("memmove", "0x52000", "0", "756",
                        "read", "0x52000", "756", args.evidence / "rom-prefix.bin")
            if (args.evidence / "rom-prefix.bin").read_bytes() != rom[:0x2f4]:
                raise RuntimeError("Reset ROM prefix differs; diagnostic execution refused")
            before = read_registers()
            record["registers_before"] = dict(zip(map(hex, REGISTERS), before))
            save()
            if before[0] & 1 or before[3] or before[5]:
                raise RuntimeError("Watchdog or ROM dispatch already has an owner")
            request = args.evidence / "request.bin"
            request.write_bytes(struct.pack("<16I", 0x57444346, *([0] * 15)))
            transaction("write", "0x52000", args.image, "write", "0x52800", request,
                        "read", "0x52000", len(image), args.evidence / "uploaded.bin",
                        "read", "0x52800", "64", args.evidence / "uploaded-request.bin")
            if ((args.evidence / "uploaded.bin").read_bytes() != image or
                    (args.evidence / "uploaded-request.bin").read_bytes() != request.read_bytes()):
                raise RuntimeError("Upload mismatch; diagnostic execution refused")
            executing = True
            transaction("exe", "0x52000")
            transaction("read", "0x52800", "64", args.evidence / "result.bin")
            result = struct.unpack("<16I", (args.evidence / "result.bin").read_bytes())
            record["result"] = list(result)
            save()
            if (result[:4] != (0x57444346, 0x57525031, 0, 3) or
                    result[4:10] != tuple(before[:6]) or
                    result[10:14] != (0xfa50392f, 0x20, (before[1] & ~3) | 1, 2) or
                    result[14:] != (0, before[0])):
                raise RuntimeError("Diagnostic did not establish complete register restoration")
            after = read_registers()
            record["registers_after"] = dict(zip(map(hex, REGISTERS), after))
            save()
            if after != before:
                raise RuntimeError("Host register comparison failed")
            transaction(*[arg for name, address, _ in REGIONS for arg in
                          ("write", hex(address), args.evidence / f"{name}-before.bin")])
            transaction(*[arg for name, address, length in REGIONS for arg in
                          ("read", hex(address), length, args.evidence / f"{name}-after.bin")])
            if any((args.evidence / f"{name}-before.bin").read_bytes() !=
                   (args.evidence / f"{name}-after.bin").read_bytes() for name, _, _ in REGIONS):
                raise RuntimeError("SRAM restoration failed")
            record["complete"] = True
            save()
            print(f"PASS: ROM redirect and watchdog policy readback; register/SRAM restoration.\n{args.evidence}")
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        record.update(failure=str(error), diagnostic_execution_started=executing)
        save()
        # A partially completed configuration must not be silently retried.
        if node is not None and identity is not None:
            access.quarantine_instance(node, "Watchdog configuration trial stopped; inspect saved state before further access.", identity)
        raise SystemExit(f"Stopped: {error}\nEvidence: {args.evidence}") from error


if __name__ == "__main__":
    main()
