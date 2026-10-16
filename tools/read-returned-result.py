#!/usr/bin/env python3
"""Read diagnostics after a failed native session has independently returned to ROM."""
import argparse
import fcntl
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("evidence", type=Path)
parser.add_argument("--fel", type=Path, required=True)
args = parser.parse_args()
prior = json.loads((args.evidence / "session.json").read_text())
spec = importlib.util.spec_from_file_location("access", Path(__file__).with_name("usb-access.py"))
access = importlib.util.module_from_spec(spec)
spec.loader.exec_module(access)
sys.path.insert(0, str(Path.home() / "git/mimus/tools"))
import usb_guard
usb_guard.check_health()
with open("/tmp/mimus-board.lock", "a") as lock:
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    node = access.discover()
    port = access.target(node)
    access.require_session(node)
    if (port.name != prior["port"] or (port / "bcdDevice").read_text().strip() != "02b3"
            or access.identity(node) == prior["initial_identity"]):
        sys.exit("No independently returned ROM instance; do not issue device requests.")
    command = [str(args.fel), "-d", f"{int(node.parent.name)}:{int(node.name)}", "version"]
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    if result.returncode or "soc=00001903(A733)" not in result.stdout:
        sys.exit(f"Identity request failed; stopped: {result.stdout}{result.stderr}")
    command = command[:-1] + ["read", "0x42000000", "20480", str(args.evidence / "returned-result.bin")]
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    record = dict(command=command, exit=result.returncode, stdout=result.stdout, stderr=result.stderr,
                  node=str(node), identity=access.identity(node))
    (args.evidence / "recovery-read.json").write_text(json.dumps(record, indent=2) + "\n")
    if result.returncode:
        sys.exit("Result read failed; stopped.")
    data = (args.evidence / "returned-result.bin").read_bytes()
    header = struct.unpack_from("<16Q", data)
    print("Header:", header)
    print("USB state:", struct.unpack_from("<5I", data, 128))
    length = min(header[3], 16384 - 256)
    log = data[256:256 + length].decode(errors="replace")
    (args.evidence / "returned.log").write_text(log)
    print(log, end="")
    count = min(struct.unpack_from("<I", data, 144)[0], 256)
    traces = [struct.unpack_from("<4I", data, 16384 + 16 * i) for i in range(count)]
    (args.evidence / "usb-trace.json").write_text(json.dumps(traces, indent=2) + "\n")
    for timestamp, event, first, second in traces:
        print(timestamp, event, hex(first), hex(second))
