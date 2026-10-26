#!/usr/bin/env python3
"""Load the reviewed A733 DDR wrapper, return to FEL, and check two RAM pages."""
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
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--fel", type=Path, required=True)
parser.add_argument("--build", type=Path, default=CACHE / "build/a733-ddr")
parser.add_argument("--evidence", type=Path,
                    default=CACHE / "evidence" / time.strftime("ddr-%Y%m%d-%H%M%S"))
parser.add_argument("--allow-untrained", action="store_true",
                    help="init-only capture variant: accept a wrapper return without trained DRAM")
parser.add_argument("--record", type=Path, help="training record for the restore variant")
args = parser.parse_args()
manifest = json.loads((args.build / "manifest.json").read_text())
if args.allow_untrained != (manifest.get("variant") == "init-only"):
    sys.exit("--allow-untrained goes with, and only with, an init-only variant build.")
restore = manifest.get("variant") == "restore"
if (args.record is not None) != restore:
    sys.exit("--record goes with, and only with, a restore variant build.")
training_bytes = None
if restore:
    training_bytes = args.record.read_bytes()
    if not 128 <= len(training_bytes) <= 0x6000:
        sys.exit("Training record exceeds its SRAM reservation or lacks a header.")
    if len(training_bytes) != 128 + struct.unpack_from("<I", training_bytes, 16)[0]:
        sys.exit("Training record length differs from its header.")
    spec = importlib.util.spec_from_file_location("training_record", ROOT / "tools/training-record.py")
    training = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(training)
    try:
        training.parse(training_bytes)
    except (ValueError, struct.error) as error:
        sys.exit(f"Invalid training record: {error}")
for name, expected in manifest["files"].items():
    content = (args.build / name).read_bytes()
    if len(content) != expected["size"] or hashlib.sha256(content).hexdigest() != expected["sha256"]:
        sys.exit(f"Payload mismatch: {name}")
if restore and not 0 < manifest["files"]["ddr-restore.bin"]["size"] <= 0x8000:
    sys.exit("Restore helper exceeds its SRAM reservation.")
args.evidence.mkdir(parents=True, exist_ok=False)
record = {"manifest": manifest, "tool_sha256": hashlib.sha256(args.fel.read_bytes()).hexdigest(),
          "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
          "transactions": [], "complete": False, "launch_attempted": False,
          "device_access_attempted": False}
if restore:
    record["training_record_sha256"] = hashlib.sha256(training_bytes).hexdigest()
    (args.evidence / "training-record.bin").write_bytes(training_bytes)
    (args.evidence / "training-record-upload.bin").write_bytes(training_bytes + bytes(0x6000-len(training_bytes)))


def save():
    (args.evidence / "session.json").write_text(json.dumps(record, indent=2) + "\n")


spec = importlib.util.spec_from_file_location("usb_access", ROOT / "tools/usb-access.py")
access = importlib.util.module_from_spec(spec)
spec.loader.exec_module(access)
spec = importlib.util.spec_from_file_location("ddr_base", ROOT / "tools/check-a733-smp.py")
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)
sys.path.insert(0, str(Path.home() / "git/mimus/tools"))
import usb_guard


def transaction(*commands):
    if access.identity(device) != identity:
        raise RuntimeError("Device node changed; stopped.")
    command = [str(args.fel), "-d", f"{int(device.parent.name)}:{int(device.name)}", *map(str, commands)]
    entry = {"command": command}
    record["transactions"].append(entry)
    save()
    try:
        record["device_access_attempted"] = True
        record["launch_attempted"] |= "exe" in commands
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired as error:
        entry["failure"] = "Host timeout; no further device access."
        save()
        raise RuntimeError(entry["failure"]) from error
    entry.update(exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr)
    save()
    if result.returncode:
        raise RuntimeError(f"FEL command failed; stopped: {result.stdout}{result.stderr}")
    return result.stdout


try:
    usb_guard.check_health()
    with access.board_lock() as device:
        access.require_rom(device)
        identity = access.identity(device)
        record.update(device=str(device), node_identity=identity)
        try:
            version = transaction("version")
            if "soc=00001903(A733)" not in version:
                raise RuntimeError(f"Unexpected SoC: {version}")
            # Save all SRAM regions this attempt uses. The live FEL interval is
            # restored by the wrapper while interrupts are disabled, not by USB.
            ranges = [("fel", 0x47000, 0x3000), ("body", 0x4a000, 0x39c28),
                      ("wrapper", 0x85000, 0x1100), ("stack", 0x9b000 if restore else 0x89000, 0x4000),
                      ("shadow", 0x8e000, 0x3000), ("log", 0xe0000, 0x4000)]
            if restore:
                ranges.extend([("record", 0x87000, 0x6000), ("restore", 0x92000, 0x8000)])
            commands = []
            for name, address, size in ranges:
                commands.extend(["read", hex(address), str(size), args.evidence / f"{name}-before.bin"])
            transaction(*commands)
            transaction("write", "0x71000", args.build / "cache-off.bin", "exe", "0x71000")
            transaction("write", "0x8e000", args.build / "boot0-head.bin",
                        "write", "0x4a000", args.build / "boot0-body.bin",
                        "write", "0x85000", args.build / "ddr-entry.bin")
            if restore:
                transaction("write", "0x87000", args.evidence / "training-record-upload.bin",
                            "write", "0x92000", args.build / "ddr-restore.bin")
                transaction("read", "0x87000", "24576", args.evidence / "record-upload.bin",
                            "read", "0x92000", str(manifest["files"]["ddr-restore.bin"]["size"]),
                            args.evidence / "restore-upload.bin")
                for actual, expected in ((args.evidence / "record-upload.bin", args.evidence / "training-record-upload.bin"),
                                         (args.evidence / "restore-upload.bin", args.build / "ddr-restore.bin")):
                    if actual.read_bytes() != expected.read_bytes():
                        raise RuntimeError("Restore upload comparison failed: " + actual.name)
            # A bounded upload readback establishes the complete payload before
            # invoking power/clock setup. No firmware runs during this comparison.
            transaction("read", "0x8e000", "12288", args.evidence / "head-upload.bin",
                        "read", "0x4a000", "233472", args.evidence / "body-upload.bin",
                        "read", "0x85000", str(manifest["files"]["ddr-entry.bin"]["size"]),
                        args.evidence / "entry-upload.bin")
            for short, source in (("head", "boot0-head.bin"), ("body", "boot0-body.bin"),
                                  ("entry", "ddr-entry.bin")):
                if (args.evidence / f"{short}-upload.bin").read_bytes() != (args.build / source).read_bytes():
                    raise RuntimeError(f"Upload comparison failed: {short}")
            print("Executing the DDR wrapper once.", flush=True)
            transaction("exe", "0x85000")
            transaction("read", "0x86000", "128", args.evidence / "mailbox.bin",
                        "read", "0xe0000", "16384", args.evidence / "log.bin",
                        "read", "0x831f8", "128", args.evidence / "dram-parameters.bin")
            values = struct.unpack("<7I", (args.evidence / "mailbox.bin").read_bytes()[:28])
            record["result"] = dict(zip(("magic", "stage", "dram_mib", "board_result",
                                         "entry_cpsr", "entry_sctlr", "log_length"), values))
            log = (args.evidence / "log.bin").read_bytes()[:min(values[6], 16384)].decode(errors="replace")
            (args.evidence / "firmware.log").write_text(log)
            print(log, end="", flush=True)
            save()
            if restore:
                restored = struct.unpack_from("<7I", (args.evidence / "mailbox.bin").read_bytes(), 28)
                record["restore_result"] = dict(zip(("error", "phy_writes", "hook_calls", "sample_read_bytes",
                                                     "failure_address", "expected", "actual"), restored))
                save()
                if restored[:4] != (0, 1016, 1, 278528) or values[2] != 12288:
                    raise RuntimeError(f"DDR restore did not succeed: {record['restore_result']}")
            if args.allow_untrained:
                # The init-only variant leaves DRAM untrained; the capture that
                # follows reads only the PHY and controller.
                if values[0] != 0x53524444 or values[1] != 5 or values[3] != 0:
                    raise RuntimeError(f"Wrapper did not return normally: {record['result']}")
                record.update(untrained=True, complete=True)
                save()
                print(f"PASS: init-only wrapper returned, DRAM untrained ({values[2]} MiB reported). Evidence: {args.evidence}")
            else:
                if values[0] != 0x53524444 or values[1] != 5 or values[3] != 0 or values[2] < 64:
                    raise RuntimeError(f"DDR initialization did not succeed: {record['result']}")
                pages = [(0x41000000, 0xa55a5aa5), (0x42000000, 0x73307330)]
                commands = []
                for address, seed in pages:
                    payload = struct.pack("<1024I", *((seed ^ address ^ (i * 0x10201)) & 0xffffffff for i in range(1024)))
                    path = args.evidence / f"ram-{address:x}.bin"
                    path.write_bytes(payload)
                    commands.extend(["write", hex(address), path])
                for address, _ in pages:
                    commands.extend(["read", hex(address), "4096", args.evidence / f"ram-{address:x}-read.bin"])
                transaction(*commands)
                for address, _ in pages:
                    if (args.evidence / f"ram-{address:x}.bin").read_bytes() != (args.evidence / f"ram-{address:x}-read.bin").read_bytes():
                        raise RuntimeError(f"RAM page comparison failed: {address:#x}")
                record["complete"] = True
                save()
                print(f"PASS: DDR reports {values[2]} MiB; both 4 KiB pages match. Evidence: {args.evidence}")
        except BaseException as error:
            if record["device_access_attempted"]:
                try:
                    base.stop_after_failure(record, save, lambda: access.quarantine_instance(
                        device, "DDR attempt stopped; no retry; coordinator recovery required.", identity), error)
                finally:
                    if not record["launch_attempted"]:
                        base.hold_ownership()
            raise

except (OSError, RuntimeError, ValueError) as error:
    record["failure"] = str(error)
    save()
    sys.exit(f"Stopped: {error}\nEvidence: {args.evidence}")
