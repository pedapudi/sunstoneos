#!/usr/bin/env python3
"""Run a physical FreeRTOS or forced-stall check and collect the FEL result."""
import argparse
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / ".cache/sunstoneos"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--fel", type=Path, required=True)
parser.add_argument("--image", type=Path, default=CACHE / "build/qemu-virt/a733-kernel-check.bin")
parser.add_argument("--evidence", type=Path,
                    default=CACHE / "evidence" / time.strftime("kernel-%Y%m%d-%H%M%S"))
parser.add_argument("--mode", choices=("kernel", "timeout"), default="kernel")
parser.add_argument("--expect", default="PASS kernel:",
                    help="Required success text for a bounded runtime check")
parser.add_argument("--ram-input", type=Path, help="Input bundle loaded at 0x43000000; at most 32 MiB")
parser.add_argument("--ram-input-present", action="store_true", help="Verify existing RAM input instead of uploading it")
parser.add_argument("--return-timeout", type=float, default=10)
args = parser.parse_args()
if not 5 <= args.return_timeout <= 600:
    sys.exit("Return timeout must be between 5 and 600 seconds.")
if args.ram_input_present and not args.ram_input:
    sys.exit("--ram-input-present requires --ram-input.")
ram_input = args.ram_input.read_bytes() if args.ram_input else None
if ram_input is not None and not 0 < len(ram_input) <= 32 * 1024 * 1024:
    sys.exit("RAM input exceeds its reserved interval.")
image = args.image.read_bytes()
if not image or len(image) > 0x100000:
    sys.exit("Kernel image exceeds the reserved image interval.")
args.evidence.mkdir(parents=True, exist_ok=False)
record = dict(image_sha256=hashlib.sha256(image).hexdigest(),
              tool_sha256=hashlib.sha256(args.fel.read_bytes()).hexdigest(),
              mode=args.mode, expected_text=args.expect, transactions=[], complete=False)
spec = importlib.util.spec_from_file_location("access", ROOT / "tools/usb-access.py")
access = importlib.util.module_from_spec(spec)
spec.loader.exec_module(access)
sys.path.insert(0, str(Path.home() / "git/mimus/tools"))
import usb_guard


def save():
    (args.evidence / "session.json").write_text(json.dumps(record, indent=2) + "\n")


def transaction(*arguments, timeout=30):
    if access.identity(node) != node_identity:
        raise RuntimeError("Device identity changed before a protocol request.")
    command = [str(args.fel), "-d", f"{int(node.parent.name)}:{int(node.name)}",
               *map(str, arguments)]
    item = dict(command=command)
    record["transactions"].append(item)
    save()
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        item["failure"] = "FEL timeout; no further device requests."
        save()
        raise RuntimeError(item["failure"]) from error
    item.update(exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr)
    save()
    if result.returncode:
        raise RuntimeError(f"FEL request failed; stopped: {result.stdout}{result.stderr}")
    return result.stdout


try:
    usb_guard.check_health()
    with access.board_lock() as node:
        access.require_session(node)
        access.require_rom(node)
        node_identity = access.identity(node)
        port = access.target(node)
        config = dict(port=port.name, sysfs=str(port.resolve()))
        record.update(initial_node=str(node), initial_identity=node_identity, port=port.name)
        if "soc=00001903(A733)" not in transaction("version"):
            raise RuntimeError("Target is not the verified A733.")
        # The forced-FEL reset entry must still match the inspected ROM words.
        words = transaction("readl", "0x20", "readl", "0x24", "readl", "0x28",
                            "readl", "0x3c", "readl", "0x40")
        if [int(word, 16) for word in words.split()] != [
                0xeb00008f, 0xe3a00911, 0xe3a0193d, 0xe3a0605c, 0xea00004d]:
            raise RuntimeError("Forced-FEL ROM entry differs from the reviewed instructions.")
        transaction("read", "0x42000000", "128", args.evidence / "before.bin")
        signature = struct.unpack_from("<I", (args.evidence / "before.bin").read_bytes())[0]
        if signature not in (0x41363450, 0x454c3150, 0x534b4552):
            raise RuntimeError("No prior AArch64 probe signature; establish initialized DDR first.")
        if ram_input is not None:
            record["ram_input_sha256"] = hashlib.sha256(ram_input).hexdigest()
            record["ram_input_reused"] = args.ram_input_present
            arguments = [] if args.ram_input_present else ["write", "0x43000000", args.ram_input]
            transaction(*arguments, "read", "0x43000000", len(ram_input),
                        args.evidence / "input-readback.bin", timeout=120)
            if (args.evidence / "input-readback.bin").read_bytes() != ram_input:
                raise RuntimeError("RAM input differs; execution refused.")
        blank = bytearray(16384)
        struct.pack_into("<Q", blank, 80, args.mode == "timeout")
        (args.evidence / "blank.bin").write_bytes(blank)
        transaction("write", "0x41000000", args.image,
                    "read", "0x41000000", len(image), args.evidence / "uploaded.bin")
        if (args.evidence / "uploaded.bin").read_bytes() != image:
            raise RuntimeError("Kernel upload differs; execution refused.")
        with usb_guard.reenumeration("sunstoneos-a733", "FreeRTOS check and forced-FEL return", 2):
            transaction("write", "0x42000000", args.evidence / "blank.bin")
            transaction("reset64", "0x41000000")
            deadline = time.monotonic() + args.return_timeout
            # Poll only host device metadata. Do not send protocol requests until
            # the new instance exists and udev has granted access.
            while True:
                try:
                    candidate = access.session_node(config)
                    if (candidate is not None and access.identity(candidate) != node_identity
                            and os.access(candidate, os.R_OK | os.W_OK)):
                        break
                except FileNotFoundError:
                    pass
                if time.monotonic() >= deadline:
                    access.quarantine_instance(node,
                        "Independent board recovery and a new USB instance are required.",
                        node_identity)
                    raise RuntimeError("FEL did not return with access before the deadline; no reset retry.")
                time.sleep(0.1)
            node = candidate
            node_identity = access.identity(node)
            access.require_session(node)
            access.require_rom(node)
            record.update(returned_node=str(node), returned_identity=node_identity,
                          automatic_access=True)
            save()
        if "soc=00001903(A733)" not in transaction("version"):
            raise RuntimeError("Returned device is not A733.")
        transaction("read", "0x42000000", "16384", args.evidence / "result.bin")
        data = (args.evidence / "result.bin").read_bytes()
        values = struct.unpack_from("<16Q", data)
        names = ("magic", "stage", "status", "log_length", "esr", "elr", "far", "interrupt",
                 "start_counter", "end_counter", "mode", "gicd_ctlr", "gicd_typer",
                 "gicr_typer", "priority_mask", "reserved")
        record["result"] = dict(zip(names, values))
        if values[3] > 16384 - 257:
            raise RuntimeError("Invalid log length in result.")
        log = data[256:256 + values[3]].decode(errors="replace")
        (args.evidence / "kernel.log").write_text(log)
        print(log, end="")
        save()
        expected_status = 2 if args.mode == "timeout" else 0
        if values[:3] != (0x534b4552, 5, expected_status):
            raise RuntimeError(f"Unexpected kernel result: {record['result']}")
        if args.mode == "kernel" and args.expect not in log:
            raise RuntimeError("Kernel did not report successful completion.")
        if args.mode == "timeout" and (values[7] != 29 or "CHECK recovery:" not in log):
            raise RuntimeError("Recovery was not the expected secure timer interrupt.")
        record["complete"] = True
        save()
        print(f"PASS: {args.mode} check, FEL recovery, automatic USB access.\n{args.evidence}")
except (OSError, ValueError, RuntimeError) as error:
    record["failure"] = str(error)
    save()
    sys.exit(f"Stopped: {error}\n{args.evidence}")
