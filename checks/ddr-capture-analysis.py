#!/usr/bin/env python3
"""Check the DRAM capture classifier on synthetic captures with planted registers.

Two boards, two lifetimes each and two captures per lifetime carry one
register of each class: constant, board, lifetime and status, in a PHY block
and in the controller, plus a parameter word that training changes. The
check runs tools/analyse-ddr-captures.py on them and on two reduced sets:
one capture per lifetime (no status class is possible) and one lifetime per
board (the lifetime class merges into board). It also requires the tool to
refuse an incomplete capture and captures with different stage tables.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/analyse-ddr-captures.py"
PHY = {"constant": 0x0a920000, "board": 0x0a920002, "lifetime": 0x0a920004, "status": 0x0a920006}
CONTROLLER = {"constant": 0x0a110000, "board": 0x0a110004, "lifetime": 0x0a110008, "status": 0x0a11000c}
LOG = ("[100]vaild para:4  select dram para5\n[101]DRAM BOOT DRIVE INFO: V0.6034\n[103]DRAM Type =9\n"
       "[254]Training result is = 7\n[430]Training result is = 7\n[776]Training result is = 7\n"
       "[5190]Training result is = 7\n[5194]Actual DRAM SIZE =12288 M\n")


def stages(extra=False):
    phy = {"address": 0x0a920000, "count": 4 + extra, "width": 2, "offset": 0}
    controller = {"address": 0x0a110000, "count": 4, "width": 4, "offset": 0}
    sid = {"address": 0x03006200, "count": 4, "width": 4, "offset": 0}
    return [{"name": "identity", "entries": [sid], "output_bytes": 16},
            {"name": "phy", "entries": [phy], "output_bytes": (2 * (4 + extra) + 3) & ~3},
            {"name": "controller", "entries": [controller], "output_bytes": 16}]


def value(kind, board, lifetime, capture):
    return {"constant": 0x11, "board": 0x20 + board, "lifetime": 0x30 + 2 * board + lifetime,
            "status": 0x40 + 4 * board + 2 * lifetime + capture}[kind]


def make(root, board, lifetime, capture, complete=True, extra=False, truncated=False):
    ddr = root / f"b{board}-l{lifetime}-ddr"
    ddr.mkdir(exist_ok=True)
    (ddr / "firmware.log").write_text(LOG)
    # Lifetime 1 of each board is a P-state-0-only training, lifetime 0 an
    # initialization-only run, so the design's kind diff has two kinds.
    (ddr / "session.json").write_text(json.dumps({"manifest": {"variant": "pstate0" if lifetime else "init-only"}}))
    out = root / f"b{board}-l{lifetime}-c{capture}"
    out.mkdir()
    layout = stages(extra)[:2] if truncated else stages(extra)
    (out / "stage1-identity.bin").write_bytes(struct.pack("<4I", 0xabc0 + board, 1, 2, 3))
    kinds = ("constant", "board", "lifetime", "status")
    phy = [value(k, board, lifetime, capture) for k in kinds] + [0] * extra
    (out / "stage2-phy.bin").write_bytes(struct.pack("<%dH" % len(phy), *phy).ljust(layout[1]["output_bytes"], b"\0"))
    if not truncated:
        (out / "stage3-controller.bin").write_bytes(struct.pack("<4I", *(value(k, board, lifetime, capture) for k in kinds)))
    words = ["0x%08x" % w for w in [0x7f8, 9] + [0] * 126]
    words[7] = "0x%08x" % (0x30001001 if lifetime or board else 0x1001)
    session = {"complete": complete, "port": f"3-{1 + 2 * board}", "temperature_note": "fixture",
               "ddr_evidence": str(ddr), "parameter_block": words, "manifest": {"stages": layout},
               "stages": [{"output": f"stage{i}-{s['name']}.bin"} for i, s in enumerate(layout, 1)]}
    (out / "session.json").write_text(json.dumps(session))
    return out


def analyse(paths, output, *extra):
    completed = subprocess.run([sys.executable, str(TOOL), *map(str, paths), "--output", str(output), *extra],
                               capture_output=True, text=True)
    report = json.loads(output.read_text()) if completed.returncode == 0 else None
    return completed, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    data = args.output / "captures"
    data.mkdir()
    full = [make(data, b, l, c) for b in (0, 1) for l in (0, 1) for c in (0, 1)]
    completed, report = analyse(full, args.output / "full.json")
    assert completed.returncode == 0, completed.stderr
    for kind, address in PHY.items():
        got = report["varying"].get("0x%08x" % address, {"class": "constant"})["class"]
        assert got == kind, (kind, got)
    for kind, address in CONTROLLER.items():
        got = report["varying"].get("0x%08x" % address, {"class": "constant"})["class"]
        assert got == kind, (kind, got)
    assert report["varying"]["param[7]"]["class"] == "lifetime"
    assert sorted(report["restore_candidates"]) == ["0x%08x" % PHY["board"], "0x%08x" % PHY["lifetime"]]
    assert all(report["resolution"][k] for k in ("status_separable", "lifetime_separable", "board_separable"))
    assert report["training_by_kind"]["pstate0"]["runs_median_ms"] == [151, 176, 346, 4414]
    assert report["training_kinds"] == ["init-only", "pstate0"]

    completed, report = analyse(full, args.output / "kind-diff.json", "--trained-kind", "pstate0",
                                "--baseline-kind", "init-only")
    assert completed.returncode == 0, completed.stderr
    diff = report["kind_diff"]["registers"]
    assert sorted(diff) == ["0x%08x" % CONTROLLER["lifetime"], "0x%08x" % PHY["lifetime"]], sorted(diff)
    assert all(entry["mask"] == 3 for entry in diff.values())
    completed, report = analyse(full, args.output / "kind-diff-excluded.json", "--trained-kind", "pstate0",
                                "--baseline-kind", "init-only", "--exclude-block", "controller")
    assert completed.returncode == 0 and sorted(report["kind_diff"]["registers"]) == ["0x%08x" % PHY["lifetime"]]
    assert report["boards"] == 2 and report["lifetimes"] == 4

    one_capture = [p for p in full if p.name.endswith("-c0")]
    completed, report = analyse(one_capture, args.output / "one-capture.json")
    assert completed.returncode == 0 and not report["resolution"]["status_separable"]
    assert report["varying"]["0x%08x" % PHY["status"]]["class"] == "lifetime"

    one_lifetime = [p for p in full if "-l0-" in p.name]
    completed, report = analyse(one_lifetime, args.output / "one-lifetime.json")
    assert completed.returncode == 0 and not report["resolution"]["lifetime_separable"]
    assert report["varying"]["0x%08x" % PHY["lifetime"]]["class"] == "board"

    incomplete = make(data, 2, 0, 0, complete=False)
    completed, _ = analyse(full + [incomplete], args.output / "incomplete.json")
    assert completed.returncode != 0 and "not a completed capture" in completed.stderr
    other = make(data, 3, 0, 0, extra=True)
    completed, _ = analyse(full + [other], args.output / "other-tables.json")
    assert completed.returncode != 0 and "different stage tables" in completed.stderr
    short = make(data, 4, 0, 0, truncated=True)
    completed, report = analyse(full + [short], args.output / "prefix.json")
    assert completed.returncode == 0, completed.stderr
    assert report["stages_compared"] == ["identity", "phy"]
    assert not any(key.startswith("0x0a11") for key in report["varying"])
    print("PASS: constant, board, lifetime and status registers classified; the trained-minus-baseline "
          "kind diff finds the planted registers with their masks; a stage-limited capture is "
          "compared over the shared stages; reduced sets merge "
          "classes as stated; incomplete captures and different tables refused")


if __name__ == "__main__":
    main()
