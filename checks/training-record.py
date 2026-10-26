#!/usr/bin/env python3
"""Check the DRAM training record format and its card update plans on an image file.

A 64 MiB zero image gets slots A and B and a training-record unit from
tools/boot-slots.py allocate. A record built by tools/training-record.py
must parse back unchanged and refuse a changed header or payload. Its plan
must reach tools/storage-update.py plan as two sessions inside the unit,
payload before header; applying both to a copy of the image yields the
record, applying only the payload leaves no valid header, and an
invalidation plan clears it. The allocation refuses a unit that overlaps a
slot, and the planner refuses a record write outside the unit or without
one. No command opens a device.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/training-record.py"
SLOTS = ROOT / "tools/boot-slots.py"
SECTORS = 131072
UNIT = 57344


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


def run(*argv):
    return subprocess.run([sys.executable, *map(str, argv)], capture_output=True, text=True)


def refused(action, text):
    try:
        action()
    except (ValueError, KeyError) as error:
        assert text in str(error), error
        return
    raise AssertionError("not refused: " + text)


def apply_sessions(image, plan_dir, plan, only=None):
    with image.open("r+b") as handle:
        for session in plan["sessions"][:only]:
            handle.seek(session["first"] * 512)
            handle.write((plan_dir / session["candidate"]).read_bytes())


def read_record(image):
    data = image.read_bytes()
    header = data[UNIT * 512:UNIT * 512 + 128]
    return header + data[(UNIT + 1) * 512:(UNIT + 1) * 512 + 65536]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    out = args.output
    out.mkdir(parents=True, exist_ok=False)
    record_tool = module("training_record", TOOL)
    update = module("storage_update", ROOT / "tools/storage-update.py")

    spec = {"generation": 3, "chip_id": [0x11, 0x22, 0x33, 0x44], "boot0_checksum": 0x2fce7bb3,
            "set_index": 5, "strap_crc": 0x1234abcd, "temperature_mc": 31500, "temperature_limit_mc": 15000,
            "step_value": 0x125f, "size_mib": 12288,
            "runs": [{"base": 0x0a920000, "values": list(range(300))},
                     {"base": 0x0a940010, "values": [7, 8, 9]}],
            "masks": [{"address": 0x0a920004, "mask": 0x007f}],
            "mode_registers": [{"channel": c, "rank": 0, "register": r, "value": v}
                               for c in (0, 1) for r, v in ((16, 5), (14, 0x2e), (15, 0x30))]}
    (out / "spec.json").write_text(json.dumps(spec))
    built = run(TOOL, "build", "--input", out / "spec.json", "--output", out / "record.bin")
    assert built.returncode == 0, built.stderr
    record = (out / "record.bin").read_bytes()
    parsed = record_tool.parse(record)
    assert {key: parsed[key] for key in spec} == spec
    changed = bytearray(record)
    changed[200] ^= 1
    refused(lambda: record_tool.parse(bytes(changed)), "Payload CRC mismatch")
    changed = bytearray(record)
    changed[40] ^= 1
    refused(lambda: record_tool.parse(bytes(changed)), "Header CRC mismatch")

    image = out / "card.img"
    with image.open("wb") as handle:
        handle.truncate(SECTORS * 512)
    overlap = run(SLOTS, "allocate", "--backup", image, "--slot-a", "40960:8192", "--slot-b", "49152:8192",
                  "--training-record", "49152:8192", "--output", out / "overlap.json")
    assert overlap.returncode != 0 and "overlaps a slot" in overlap.stderr
    plain = run(SLOTS, "allocate", "--backup", image, "--slot-a", "40960:8192", "--slot-b", "49152:8192",
                "--output", out / "plain.json")
    allocated = run(SLOTS, "allocate", "--backup", image, "--slot-a", "40960:8192", "--slot-b", "49152:8192",
                    "--training-record", f"{UNIT}:8192", "--output", out / "allocation.json")
    assert plain.returncode == 0 and allocated.returncode == 0, allocated.stderr

    planned = run(TOOL, "plan", "--record", out / "record.bin", "--allocation", out / "allocation.json",
                  "--card-image", image, "--output", out / "record-plan")
    assert planned.returncode == 0, planned.stderr
    plan = json.loads((out / "record-plan/plan.json").read_text())
    assert [w["name"] for w in plan["writes"]] == ["record-payload", "record-header"]
    assert plan["writes"][0]["first_sector"] == UNIT + 1 and plan["writes"][1]["first_sector"] == UNIT

    card = {"kind": 1, "host": 0, "cid": [1, 2, 3, 4], "sectors": SECTORS,
            "target": {"port": "3-3", "sysfs": "/fixture"}}
    sessions = update.plan(out / "record-plan/plan.json", out / "allocation.json", image, card, out / "update")
    assert [s["name"] for s in sessions["sessions"]] == ["record-payload", "record-header"]
    assert all(s["region"] == "training-record" for s in sessions["sessions"])
    refused(lambda: update.plan(out / "record-plan/plan.json", out / "plain.json", image, card, out / "no-unit"),
            "Boot-slot plan was made with another allocation")

    forged_dir = out / "forged"
    shutil.copytree(out / "record-plan", forged_dir)
    forged = json.loads((forged_dir / "plan.json").read_text())
    forged["writes"][1]["first_sector"] = UNIT - 1
    (forged_dir / "plan.json").write_text(json.dumps(forged))
    refused(lambda: update.plan(forged_dir / "plan.json", out / "allocation.json", image, card, out / "forged-update"),
            "leaves the SunstoneOS training-record region")
    unallocated_dir = out / "unallocated"
    shutil.copytree(out / "record-plan", unallocated_dir)
    unallocated = json.loads((unallocated_dir / "plan.json").read_text())
    unallocated["allocation_sha256"] = update.tool_module("boot_slots", "boot-slots.py").sha(
        (out / "plain.json").read_bytes())
    (unallocated_dir / "plan.json").write_text(json.dumps(unallocated))
    refused(lambda: update.plan(unallocated_dir / "plan.json", out / "plain.json", image, card, out / "unallocated-update"),
            "without a training-record allocation")

    interrupted = out / "interrupted.img"
    shutil.copyfile(image, interrupted)
    apply_sessions(interrupted, out / "update", sessions, only=1)
    refused(lambda: record_tool.parse(read_record(interrupted)), "Header CRC mismatch")
    complete = out / "complete.img"
    shutil.copyfile(image, complete)
    apply_sessions(complete, out / "update", sessions)
    assert {key: record_tool.parse(read_record(complete))[key] for key in spec} == spec

    invalidated = run(TOOL, "invalidate", "--allocation", out / "allocation.json", "--card-image", complete,
                      "--output", out / "invalidate-plan")
    assert invalidated.returncode == 0, invalidated.stderr
    clear = update.plan(out / "invalidate-plan/plan.json", out / "allocation.json", complete, card, out / "clear")
    assert [s["name"] for s in clear["sessions"]] == ["record-invalidate"]
    apply_sessions(complete, out / "clear", clear)
    refused(lambda: record_tool.parse(read_record(complete)), "Header CRC mismatch")
    print("PASS: record round trip and CRC refusals; payload-then-header sessions inside the unit; "
          "interrupted and invalidated records refuse; overlapping, missing and outside writes refused")


if __name__ == "__main__":
    main()
