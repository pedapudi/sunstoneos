#!/usr/bin/env python3
"""Load native USB diagnostics once, exercise the channel, and return to FEL."""
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
import usb.core
import usb.util

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / ".cache/sunstoneos"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--fel", type=Path, required=True)
parser.add_argument("--image", type=Path, default=CACHE / "build/qemu-virt/a733-usb-runtime.bin")
parser.add_argument("--evidence", type=Path, default=CACHE / "evidence" / time.strftime("usb-%Y%m%d-%H%M%S"))
args = parser.parse_args()
image = args.image.read_bytes()
if not image or len(image) > 0x100000:
    sys.exit("Runtime image exceeds its load interval.")
args.evidence.mkdir(parents=True, exist_ok=False)
record = dict(image_sha256=hashlib.sha256(image).hexdigest(),
              fel_sha256=hashlib.sha256(args.fel.read_bytes()).hexdigest(),
              transactions=[], complete=False)
spec = importlib.util.spec_from_file_location("access", ROOT / "tools/usb-access.py")
access = importlib.util.module_from_spec(spec)
spec.loader.exec_module(access)
sys.path.insert(0, str(Path.home() / "git/mimus/tools"))
import usb_guard


def save():
    (args.evidence / "session.json").write_text(json.dumps(record, indent=2) + "\n")


def fel(*arguments):
    if access.identity(node) != identity:
        raise RuntimeError("FEL node identity changed.")
    item = dict(protocol="fel", arguments=list(map(str, arguments)))
    record["transactions"].append(item)
    save()
    r = subprocess.run([str(args.fel), "-d", f"{int(node.parent.name)}:{int(node.name)}",
                        *map(str, arguments)], capture_output=True, text=True, timeout=30)
    item.update(exit=r.returncode, stdout=r.stdout, stderr=r.stderr)
    save()
    if r.returncode:
        raise RuntimeError(f"FEL request failed: {r.stdout}{r.stderr}")
    return r.stdout


def product_name():
    path = port / "product"
    return path.read_text().strip() if path.exists() else ""


def wait_service(previous_identity, native, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            candidate = access.session_node(port_config)
            product = product_name()
            if (candidate is not None and access.identity(candidate) != previous_identity
                    and os.access(candidate, os.R_OK | os.W_OK)
                    and (product == "sunstoneos RAM diagnostics") == native):
                return candidate
        except FileNotFoundError:
            pass
        time.sleep(0.05)
    raise RuntimeError("Expected USB service did not appear with access; no reconnect retry.")


def message(command, payload=b""):
    sequence = len(record["transactions"]) + 1
    packet = struct.pack("<4I", 0x53534f53, sequence, command, len(payload)) + payload
    item = dict(protocol="native", command=command, sequence=sequence, payload_size=len(payload))
    record["transactions"].append(item)
    save()
    if device.write(0x02, packet, timeout=1500) != len(packet):
        raise RuntimeError("Native USB write was incomplete.")
    item["write_complete"] = True
    save()
    response = bytes(device.read(0x81, 4096, timeout=1500))
    if len(response) < 16:
        raise RuntimeError("Native USB response lacks its header.")
    header = struct.unpack_from("<4I", response)
    if header != (0x53534f53, sequence, command | 0x80000000, len(response) - 16):
        raise RuntimeError(f"Unexpected native USB response: {header}")
    item["response_size"] = len(response) - 16
    save()
    return response[16:]


try:
    usb_guard.check_health()
    with open("/tmp/mimus-board.lock", "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        node = access.discover()
        access.require_session(node)
        access.require_rom(node)
        identity = access.identity(node)
        port = access.target(node)
        if product_name() == "sunstoneos RAM diagnostics":
            raise RuntimeError("Runtime is already active; do not send it FEL requests.")
        port_config = dict(port=port.name, sysfs=str(port.resolve()))
        record.update(initial_node=str(node), initial_identity=identity, port=port.name)
        if "soc=00001903(A733)" not in fel("version"):
            raise RuntimeError("Unexpected target.")
        fel("read", "0x42000000", "128", args.evidence / "before.bin")
        if struct.unpack_from("<I", (args.evidence / "before.bin").read_bytes())[0] != 0x534b4552:
            raise RuntimeError("Establish the physical kernel check before native USB loading.")
        blank = bytearray(20480)
        struct.pack_into("<Q", blank, 80, 2)
        (args.evidence / "blank.bin").write_bytes(blank)
        fel("write", "0x41000000", args.image,
            "read", "0x41000000", len(image), args.evidence / "uploaded.bin")
        if (args.evidence / "uploaded.bin").read_bytes() != image:
            raise RuntimeError("Runtime upload differs; execution refused.")
        with usb_guard.reenumeration("sunstoneos-a733", "native USB diagnostics and bounded recovery", 4):
            # Preserve the prior result while waiting for the shared USB cooldown.
            fel("write", "0x42000000", args.evidence / "blank.bin")
            fel("reset64", "0x41000000")
            node = wait_service(identity, True, 10)
            identity = access.identity(node)
            access.require_session(node)
            record.update(runtime_node=str(node), runtime_identity=identity)
            save()
            device = usb.core.find(idVendor=0x1f3a, idProduct=0xefe8,
                                   bus=int(node.parent.name), address=int(node.name))
            if device is None or device.bcdDevice != 0x0100:
                raise RuntimeError("Device descriptor does not identify the native runtime.")
            device.set_configuration(1)
            usb.util.claim_interface(device, 0)
            status = struct.unpack("<5Q", message(1))
            record["status"] = dict(zip(("ticks", "CurrentEL", "commands", "speed", "counter"), status))
            if status[1] != 4:
                raise RuntimeError("Runtime is not executing at EL1.")
            for size in (0, 496, 497, 2048):
                payload = bytes((i * 73 + size) & 255 for i in range(size))
                if message(2, payload) != payload:
                    raise RuntimeError(f"Echo differs for payload length {size}.")
            # Survive beyond the initial two-second secure recovery deadline.
            time.sleep(2.1)
            later = struct.unpack("<5Q", message(1))
            if later[0] <= status[0] + 2000:
                raise RuntimeError("Runtime tick count did not advance through the wait.")
            log = message(3).decode(errors="replace")
            (args.evidence / "runtime.log").write_text(log)
            print(log, end="", flush=True)
            message(4)
            usb.util.release_interface(device, 0)
            usb.util.dispose_resources(device)
            node = wait_service(identity, False, 10)
            identity = access.identity(node)
            access.require_session(node)
            access.require_rom(node)
            record["returned_node"] = str(node)
            if "soc=00001903(A733)" not in fel("version"):
                raise RuntimeError("Returned device is not A733 FEL.")
            fel("read", "0x42000000", "20480", args.evidence / "result.bin")
        data = (args.evidence / "result.bin").read_bytes()
        result = struct.unpack_from("<16Q", data)
        record["result_header"] = result
        if result[:3] != (0x534b4552, 5, 0):
            raise RuntimeError(f"Runtime did not return successfully: {result}")
        record["complete"] = True
        save()
        print(f"PASS: native USB status, packet boundaries, echo, live runtime, logs, and FEL return.\n{args.evidence}")
except (OSError, ValueError, RuntimeError, usb.core.USBError, subprocess.TimeoutExpired) as error:
    record["failure"] = str(error)
    save()
    sys.exit(f"Stopped on first failure: {error}\n{args.evidence}")
