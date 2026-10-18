#!/usr/bin/env python3
"""Execute the EL1 handoff probe once and collect its result after FEL recovery."""
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
parser.add_argument("--image", type=Path, default=CACHE / "build/a733-el1/probe.bin")
parser.add_argument("--evidence", type=Path,
                    default=CACHE / "evidence" / time.strftime("el1-%Y%m%d-%H%M%S"))
args = parser.parse_args()
image = args.image.read_bytes()
if not image or len(image) > 0x10000:
    sys.exit("EL1 probe exceeds the reserved image interval.")
args.evidence.mkdir(parents=True, exist_ok=False)
record = dict(image_sha256=hashlib.sha256(image).hexdigest(),
              tool_sha256=hashlib.sha256(args.fel.read_bytes()).hexdigest(),
              transactions=[], complete=False)
spec = importlib.util.spec_from_file_location("access", ROOT / "tools/usb-access.py")
access = importlib.util.module_from_spec(spec)
spec.loader.exec_module(access)
sys.path.insert(0, str(Path.home() / "git/mimus/tools"))
import usb_guard


def save():
    (args.evidence / "session.json").write_text(json.dumps(record, indent=2) + "\n")


def transaction(*arguments):
    if access.identity(node) != node_identity:
        raise RuntimeError("Device identity changed before a protocol request.")
    command = [str(args.fel), "-d", f"{int(node.parent.name)}:{int(node.name)}",
               *map(str, arguments)]
    item = dict(command=command)
    record["transactions"].append(item)
    save()
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
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
        if signature not in (0x41363450, 0x454c3150):
            raise RuntimeError("No prior AArch64 probe signature; establish initialized DDR first.")
        (args.evidence / "blank.bin").write_bytes(bytes(128))
        transaction("write", "0x41000000", args.image,
                    "write", "0x42000000", args.evidence / "blank.bin",
                    "read", "0x41000000", len(image), args.evidence / "uploaded.bin")
        if (args.evidence / "uploaded.bin").read_bytes() != image:
            raise RuntimeError("EL1 probe upload differs; execution refused.")
        with usb_guard.reenumeration("sunstoneos-a733", "EL1 handoff and forced-FEL return", 2):
            transaction("reset64", "0x41000000")
            deadline = time.monotonic() + 10
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
                    raise RuntimeError("FEL did not return with access within 10 seconds; no reset retry.")
                time.sleep(0.1)
            node = candidate
            node_identity = access.identity(node)
            access.require_session(node)
            record.update(returned_node=str(node), returned_identity=node_identity,
                          automatic_access=True)
            save()
        if "soc=00001903(A733)" not in transaction("version"):
            raise RuntimeError("Returned device is not A733.")
        transaction("read", "0x42000000", "128", args.evidence / "result.bin")
        values = struct.unpack("<16Q", (args.evidence / "result.bin").read_bytes())
        names = ("magic", "stage", "CurrentEL", "CNTFRQ_EL0", "CNTPCT_EL0", "SCTLR_EL1",
                 "SCR_EL3", "HCR_EL2", "CNTHCTL_EL2", "ESR_EL3", "ELR_EL3", "FAR_EL3",
                 "status", "return_counter", "ESR_EL1", "ELR_EL1")
        record["result"] = dict(zip(names, values))
        if (values[:4] != (0x454c3150, 4, 4, 24000000) or values[12] != 0
                or values[14] != 0 or values[15] != 0):
            raise RuntimeError(f"EL1 handoff failed: {record['result']}")
        record["complete"] = True
        save()
        print(f"PASS: EL1 entry, counter access, SMC return, FEL recovery, automatic USB access.\n{args.evidence}")
except (OSError, ValueError, RuntimeError) as error:
    record["failure"] = str(error)
    save()
    sys.exit(f"Stopped: {error}\n{args.evidence}")
