#!/usr/bin/env python3
"""Classify DRAM controller and PHY registers across training captures.

Each argument is a completed tools/run-ddr-capture.py evidence directory.
Captures that name the same DDR evidence directory share one FEL lifetime;
captures whose identity stage reads the same chip ID come from one board.
Every register, and every word of the parameter block, gets one class:

- status: two captures of one lifetime differ, so the value changes without
  retraining;
- lifetime: stable within each lifetime, different between lifetimes of one
  board, so training sets it;
- board: stable on each board, different between boards;
- constant: equal in every capture.

Captures run with a stage limit are compared over the stages they share.
A class needs the data to separate it: with one capture per lifetime no
register can be called status, and with one lifetime per board the lifetime
and board classes merge. The summary records both counts. PHY registers of
the lifetime and board classes are the candidates for a training restore
(docs/designs/dram-training-restore.md). The output is a new JSON file.
"""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re
import statistics
import struct
import sys

PHY_BLOCKS = [  # (first byte address, last byte address, block name)
    (0x0a900000, 0x0a917fff, "csr-0x0000-0xbfff"), (0x0a920000, 0x0a927fff, "dbyte"), (0x0a940000, 0x0a94ffff, "master"),
    (0x0a960000, 0x0a963fff, "csr-0x30000"), (0x0a980000, 0x0a98ffff, "acsm"),
    (0x0a9b0000, 0x0a9bffff, "message-block"), (0x0aa20000, 0x0aa2ffff, "initeng"),
    (0x0aa40000, 0x0aa47fff, "csr-0xa0000"), (0x0aa80000, 0x0aa8ffff, "drtub"),
    (0x0aaa0000, 0x0aaaffff, "apbonly")]


def block_of(address):
    if address < 0:
        return "parameter-block"
    for first, last, name in PHY_BLOCKS:
        if first <= address <= last:
            return "phy-" + name
    if 0x03006000 <= address < 0x03007000:
        return "sid"
    return "controller"


def fail(message):
    sys.exit("Refused: " + message)


def load(directory):
    record = json.loads((directory / "session.json").read_text())
    if not record.get("complete"):
        fail(f"{directory} is not a completed capture")
    stages = record["manifest"]["stages"]
    values = {}
    for index, (stage, outcome) in enumerate(zip(stages, record["stages"]), 1):
        data = (directory / outcome["output"]).read_bytes()
        if len(data) != stage["output_bytes"]:
            fail(f"{directory}: stage {index} output has the wrong length")
        for entry in stage["entries"]:
            fmt = "<%d%s" % (entry["count"], "H" if entry["width"] == 2 else "I")
            words = struct.unpack_from(fmt, data, entry["offset"])
            for n, value in enumerate(words):
                values[entry["address"] + entry["width"] * n] = value
    for n, word in enumerate(record["parameter_block"]):
        values[-1 - n] = int(word, 16)                 # Parameter word n.
    chip = tuple(values.get(0x03006200 + 4 * n) for n in range(4))
    ddr = Path(record["ddr_evidence"]).resolve()
    log = (ddr / "firmware.log").read_text(errors="replace") if (ddr / "firmware.log").exists() else ""
    return {"path": str(directory), "values": values, "port": record.get("port", ""),
            "temperature_note": record.get("temperature_note", ""), "lifetime": str(ddr),
            "board": "chip-" + "-".join("%08x" % (w or 0) for w in chip),
            "layout": [(s["name"], [(e["address"], e["count"], e["width"]) for e in s["entries"]])
                       for s in stages],
            "training": training_times(log)}


def training_times(log):
    """Per-P-state run times in ms from boot0's log, in the order run."""
    events = [(int(t), text) for t, text in re.findall(r"\[(\d+)\]([^\n\[]*)", log)]
    results = [t for t, text in events if "Training result" in text]
    start = next((t for t, text in events if "DRAM Type" in text), None)
    size = next((t for t, text in events if "Actual DRAM SIZE" in text), None)
    info = next((t for t, text in events if "DRIVE INFO" in text), None)
    select = next((text.strip() for t, text in events if "select dram para" in text), "")
    if start is None or not results:
        return {}
    runs = [b - a for a, b in zip([start] + results, results)]
    return {"runs_ms": runs, "total_ms": (size - info) if size and info else None, "select": select}


def classify(captures):
    lifetimes, boards = defaultdict(list), defaultdict(list)
    for capture in captures:
        lifetimes[capture["lifetime"]].append(capture)
        boards[capture["board"]].append(capture)
    keys = sorted(set().union(*(c["values"] for c in captures)))
    result = {}
    for key in keys:
        if any(key not in c["values"] for c in captures):
            fail(f"register {key:#x} is missing from a capture")
        if all(c["values"][key] == captures[0]["values"][key] for c in captures):
            result[key] = "constant"
        elif any(len({c["values"][key] for c in group}) > 1 for group in lifetimes.values()):
            result[key] = "status"
        elif any(len({c["values"][key] for c in group}) > 1 for group in boards.values()):
            result[key] = "lifetime"
        else:
            result[key] = "board"
    return result, lifetimes, boards


def name(key):
    return "param[%d]" % (-1 - key) if key < 0 else "0x%08x" % key


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    captures = [load(path) for path in args.captures]
    # Captures limited with --stages share a prefix of the stage tables;
    # only those stages are compared.
    common = min(len(c["layout"]) for c in captures)
    if len({json.dumps(c["layout"][:common]) for c in captures}) != 1:
        fail("the captures use different stage tables")
    keep = {address + width * n for _, entries in captures[0]["layout"][:common]
            for address, count, width in entries for n in range(count)}
    for c in captures:
        c["values"] = {k: v for k, v in c["values"].items() if k < 0 or k in keep}
    classes, lifetimes, boards = classify(captures)
    counts = Counter(classes.values())
    by_block = defaultdict(Counter)
    for key, kind in classes.items():
        by_block[block_of(key)][kind] += 1
    per_board_lifetimes = [len({c["lifetime"] for c in group}) for group in boards.values()]
    per_lifetime_captures = [len(group) for group in lifetimes.values()]
    varying = {name(key): {"class": kind, "block": block_of(key),
                           "values": {c["path"]: c["values"][key] for c in captures}}
               for key, kind in classes.items() if kind != "constant"}
    candidates = [name(key) for key, kind in classes.items()
                  if kind in ("lifetime", "board") and block_of(key).startswith("phy-")]
    runs = [c["training"]["runs_ms"] for c in captures if c["training"].get("runs_ms")]
    report = {
        "kind": "a733-ddr-capture-analysis",
        "captures": [{key: c[key] for key in ("path", "port", "board", "lifetime", "temperature_note", "training")}
                     for c in captures],
        "stages_compared": [name for name, _ in captures[0]["layout"][:common]],
        "boards": len(boards), "lifetimes": len(lifetimes),
        "resolution": {
            "status_separable": min(per_lifetime_captures) >= 2,
            "lifetime_separable": min(per_board_lifetimes) >= 2,
            "board_separable": len(boards) >= 2,
            "captures_per_lifetime_min": min(per_lifetime_captures),
            "lifetimes_per_board_min": min(per_board_lifetimes)},
        "counts": dict(counts),
        "counts_by_block": {block: dict(count) for block, count in sorted(by_block.items())},
        "training_run_median_ms": [statistics.median(r[i] for r in runs) for i in range(len(runs[0]))] if runs else [],
        "restore_candidates": candidates,
        "varying": varying,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x") as handle:
        handle.write(json.dumps(report, indent=1) + "\n")
    print(json.dumps({key: report[key] for key in ("boards", "lifetimes", "resolution", "counts")}, indent=1))
    print(f"{len(candidates)} restore candidates; report: {args.output}")


if __name__ == "__main__":
    main()
