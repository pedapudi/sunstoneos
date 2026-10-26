#!/usr/bin/env python3
"""Capture the DRAM parameter block and controller and PHY registers after training.

Run once, in the same FEL lifetime and directly after tools/run-ddr.py has
trained DRAM. The runner first requires the SRAM mailbox of that DDR run to
equal the one saved in its evidence, then reads the boot-parameter buffer,
turns the instruction cache off, uploads the reader from
tools/prepare-ddr-capture.py and runs its stage tables in order: one FEL
execution per stage, each followed by a read of its results. It stops at the
first failed command or stage and never retries (docs/designs/dram-fast-boot.md).
"""
import argparse
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
MAILBOX, BOOTPARAM, READER, TABLE = 0x86000, 0x82df8, 0x85000, 0x43000000
MAGIC, DONE = 0x50414344, 0x454e4f44
EXPECTED_MIB = 12288


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fel", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--ddr-evidence", type=Path, required=True,
                        help="the run-ddr.py evidence directory of this FEL lifetime")
    parser.add_argument("--stages", type=int, default=0,
                        help="run only the first N stages, for a board whose last stage stopped the bus")
    parser.add_argument("--allow-untrained", action="store_true",
                        help="capture after the init-only DDR variant, which leaves DRAM untrained")
    parser.add_argument("--temperature-note", default="",
                        help="the coordinator's temperature observation, recorded unchanged")
    parser.add_argument("--evidence", type=Path,
                        default=CACHE / "evidence" / time.strftime("ddr-capture-%Y%m%d-%H%M%S"))
    args = parser.parse_args()
    manifest = json.loads((args.build / "manifest.json").read_text())
    if manifest.get("kind") != "a733-ddr-capture-build":
        sys.exit("Not a DRAM capture build.")
    for name, expected in manifest["files"].items():
        content = (args.build / name).read_bytes()
        if len(content) != expected["size"] or hashlib.sha256(content).hexdigest() != expected["sha256"]:
            sys.exit(f"Payload mismatch: {name}")
    if args.stages:
        if not 1 <= args.stages <= len(manifest["stages"]):
            sys.exit("--stages is out of range.")
        manifest["stages"] = manifest["stages"][:args.stages]
    ddr = json.loads((args.ddr_evidence / "session.json").read_text())
    result = ddr.get("result", {})
    trained = result.get("dram_mib") == EXPECTED_MIB and not ddr.get("untrained")
    if not (ddr.get("complete") and result.get("magic") == 0x53524444 and result.get("stage") == 5
            and result.get("board_result") == 0
            and (trained or (args.allow_untrained and ddr.get("untrained") is True))):
        sys.exit("The DDR evidence does not record a completed 12,288 MiB training"
                 " (or, with --allow-untrained, an init-only return).")
    ddr_mailbox = (args.ddr_evidence / "mailbox.bin").read_bytes()
    args.evidence.mkdir(parents=True, exist_ok=False)
    record = {"manifest": manifest, "build": str(args.build), "ddr_evidence": str(args.ddr_evidence),
              "tool_sha256": hashlib.sha256(args.fel.read_bytes()).hexdigest(),
              "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "port": os.environ.get("SUNSTONE_USB_PORT", ""), "temperature_note": args.temperature_note,
              "ddr_variant": ddr.get("manifest", {}).get("variant", "four-p-state"),
              "transactions": [], "stages": [], "complete": False,
              "launch_attempted": False, "device_access_attempted": False}

    def save():
        (args.evidence / "session.json").write_text(json.dumps(record, indent=2) + "\n")

    save()
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
            completed = subprocess.run(command, capture_output=True, text=True, timeout=30)
        except subprocess.TimeoutExpired as error:
            entry["failure"] = "Host timeout; no further device access."
            save()
            raise RuntimeError(entry["failure"]) from error
        entry.update(exit_code=completed.returncode, stdout=completed.stdout, stderr=completed.stderr)
        save()
        if completed.returncode:
            raise RuntimeError(f"FEL command failed; stopped: {completed.stdout}{completed.stderr}")
        return completed.stdout

    def upload(address, path, label):
        transaction("write", hex(address), path)
        readback = args.evidence / f"{label}-upload.bin"
        transaction("read", hex(address), str(path.stat().st_size), readback)
        if readback.read_bytes() != path.read_bytes():
            raise RuntimeError(f"Upload comparison failed: {label}")

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
                # Bind to the DDR run before any execution: its mailbox is still in SRAM.
                transaction("read", hex(MAILBOX), "128", args.evidence / "mailbox.bin",
                            "read", hex(BOOTPARAM), "2048", args.evidence / "bootparam-buffer.bin",
                            "read", hex(READER), "1024", args.evidence / "reader-area-before.bin")
                if (args.evidence / "mailbox.bin").read_bytes() != ddr_mailbox:
                    raise RuntimeError("SRAM mailbox differs from the DDR evidence; not the same lifetime.")
                buffer = (args.evidence / "bootparam-buffer.bin").read_bytes()
                record["parameter_block"] = ["0x%08x" % word for word in struct.unpack_from("<128I", buffer, 0x400)]
                save()
                transaction("write", "0x71000", args.build / "cache-off.bin", "exe", "0x71000")
                upload(READER, args.build / "ddr-capture.bin", "reader")
                for index, stage in enumerate(manifest["stages"], 1):
                    table = args.build / stage["file"]
                    upload(TABLE, table, f"stage{index}-table")
                    print(f"Stage {index}: {stage['name']}", flush=True)
                    transaction("exe", hex(READER))
                    header_path = args.evidence / f"stage{index}-header.bin"
                    output_path = args.evidence / f"stage{index}-{stage['name']}.bin"
                    transaction("read", hex(TABLE), "32", header_path,
                                "read", hex(stage["output"]), str(stage["output_bytes"]), output_path)
                    header = struct.unpack("<8I", header_path.read_bytes())
                    outcome = {"name": stage["name"], "progress": header[4], "muxsel": header[5],
                               "ucclk": header[6], "done": header[7] == DONE, "output": output_path.name}
                    record["stages"].append(outcome)
                    save()
                    if header[0] != MAGIC or not outcome["done"] or header[4] != len(stage["entries"]) - 1:
                        raise RuntimeError(f"Stage {index} did not complete: {outcome}")
                record["complete"] = True
                save()
                print(f"PASS: {len(manifest['stages'])} stages captured. Evidence: {args.evidence}")
            except BaseException as error:
                if record["device_access_attempted"]:
                    try:
                        base.stop_after_failure(record, save, lambda: access.quarantine_instance(
                            device, "DDR capture stopped; no retry; coordinator recovery required.", identity), error)
                    finally:
                        if not record["launch_attempted"]:
                            base.hold_ownership()
                raise
    except (OSError, RuntimeError, ValueError) as error:
        record["failure"] = str(error)
        save()
        sys.exit(f"Stopped: {error}\nEvidence: {args.evidence}")


if __name__ == "__main__":
    main()
