#!/usr/bin/env python3
"""Check a restore set against a board write survey.

Decodes the survey (tools/ddr-survey.py) that the diagnostic restore build
stored on a board, and requires of the restore set that every entry wrong at
the readback pass is either left out of the set, or a wide-memory halfword
whose word was left incomplete in the surveyed set, which the whole-word
writes of the current helper complete. Every entry the set keeps must also
be one the survey covers. No board is opened.

Usage: checks/ddr-restore-survey-set.py --survey SURVEY.BIN --record RECORD.BIN --set SET.JSON
"""
import argparse
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("--survey", type=Path, required=True, help="survey.bin from a board run")
parser.add_argument("--record", type=Path, required=True, help="training record the surveyed run used")
parser.add_argument("--set", type=Path, required=True, help="restore set to check, e.g. restore-set-v5.json")
args = parser.parse_args()
spec = importlib.util.spec_from_file_location("ddr_survey", ROOT / "tools/ddr-survey.py")
survey = importlib.util.module_from_spec(spec)
spec.loader.exec_module(survey)
addresses, expected = survey.record_profile(args.record.read_bytes())
entries, summary = survey.decode(args.survey.read_bytes(), addresses, expected)
kept = {int(key, 16) for key in json.loads(args.set.read_text())["kind_diff"]["registers"] if key.startswith("0x")}
surveyed = set(addresses)
assert kept <= surveyed, sorted(hex(a) for a in kept - surveyed)[:8]


def width(address):
    if 0x0a982000 <= address < 0x0a984000:
        return 8
    if 0x0a9b0000 <= address < 0x0a9c0000:
        return 4
    return 2


left_out, completed = [], []
for entry in entries:
    if entry["class"] not in survey.MISMATCH_CLASSES:
        continue
    address = int(entry["address"], 16)
    if address not in kept:
        left_out.append(address)
        continue
    w = width(address)
    last = (address & ~(w - 1)) + w - 2
    # Held in the survey's helper, which did not write the word's highest halfword.
    assert w > 2 and last not in surveyed and entry["class"] == "unchanged", entry
    completed.append(address)
result = {"survey_mismatches": summary["mismatches"], "left_out": len(left_out),
          "completed_by_whole_word_writes": [f"{a:#010x}" for a in completed],
          "kept": len(kept), "removed": len(surveyed - kept)}
print("PASS survey against set:", json.dumps(result))
