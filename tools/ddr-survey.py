#!/usr/bin/env python3
"""Decode the restore helper's write survey into one class per entry.

The diagnostic restore build (tools/prepare-ddr.py --survey) stores, at SRAM
0x96000, a 64-byte header and four halfwords per profile entry: the value
read before the write, the value read straight after it, the value read by
the readback pass after the last write, and the mask. With the record's
expected values, each entry falls in one class, decided by the readback-pass
value ("end") and, where it matters, the value straight after the write:

- held: the written bits read back at both points, and the value before
  the write differed from them under the mask.
- already-at-value: as held, but the value before already matched, so the
  write was not observable.
- stored-later: wrong straight after the write, right at the readback pass:
  a wide-memory halfword that is stored with its word's highest halfword.
- changed-later: right straight after the write, wrong at the readback pass
  (changed by the PHY or by a later write).
- unchanged: wrong at the readback pass and equal to the value before the
  write in all 16 bits; the write did not reach a storage register.
- reads-zero: reads 0 at the readback pass although it did not before.
- other-value: reads neither the value before nor the written value; the
  PHY produced it.

The current helper's mailbox word 27 counts entries wrong at the readback
pass at the hook ("mismatches") or, with error final_readback, at the
readback after init_DRAM, which the survey does not record per entry; its
"after_init" control words show the PHY clock state that pass found. Helpers
before the whole-word writes counted entries wrong straight after their
write ("immediate_mismatches").

Usage: tools/ddr-survey.py SURVEY.BIN --record RECORD.BIN [--output JSON]
"""
import argparse
import importlib.util
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
MAGIC = 0x59565253
HEADER_BYTES, ENTRY_BYTES = 64, 8
MISMATCH_CLASSES = ("changed-later", "unchanged", "reads-zero", "other-value")
# Header word 7: wide-memory halfwords rewritten with their stored values.
REFILLS_WORD = 7
# Header words 2-6, read by the helper at the start of the write pass.
CONTROL = ("micro_cont_mux_sel_at_hook", "ucclk_hclk_enables", "micro_reset", "dct_write_prot",
           "uct_shadow_regs")
# Header word 8 is 1 once the readback after init_DRAM starts; words 9 and 10
# hold the MicroContMuxSel and UcclkHclkEnables values it found.
AFTER_INIT = ("micro_cont_mux_sel", "ucclk_hclk_enables")


def family(address):
    """Byte-lane registers by lane offset; other PHY registers by 4 KiB block."""
    if 0x0a920000 <= address < 0x0a940000:
        return f"byte-lane offset {(address - 0x0a920000) % 0x200:#05x}"
    return f"block {address & ~0xfff:#010x}"


def classify(before, after, end, mask, expected):
    after_ok, end_ok = not (after ^ expected) & mask, not (end ^ expected) & mask
    if end_ok:
        if not after_ok:
            return "stored-later"
        return "held" if (before ^ expected) & mask else "already-at-value"
    if after_ok:
        return "changed-later"
    if end == before:
        return "unchanged"
    if end == 0:
        return "reads-zero"
    return "other-value"


def decode(data, addresses, expected):
    """Entries and a summary; raises ValueError for a missing or partial survey."""
    if len(data) < HEADER_BYTES + ENTRY_BYTES * len(addresses):
        raise ValueError("survey shorter than its profile")
    header = struct.unpack_from("<16I", data)
    if header[0] != MAGIC:
        raise ValueError(f"survey magic {header[0]:#010x}; the write pass did not start")
    if header[1] != len(addresses):
        raise ValueError(f"survey holds {header[1]} of {len(addresses)} entries")
    entries = []
    for index, (address, wanted) in enumerate(zip(addresses, expected)):
        before, after, end, mask = struct.unpack_from("<4H", data, HEADER_BYTES + ENTRY_BYTES * index)
        entries.append({"index": index, "address": f"{address:#010x}", "family": family(address),
                        "mask": f"{mask:#06x}", "expected": f"{wanted & mask:#06x}",
                        "before": f"{before:#06x}", "after": f"{after:#06x}", "end": f"{end:#06x}",
                        "class": classify(before, after, end, mask, wanted)})
    counts, families, immediate = {}, {}, 0
    for entry in entries:
        mask, wanted = int(entry["mask"], 16), int(entry["expected"], 16)
        immediate += bool((int(entry["after"], 16) ^ wanted) & mask)
        counts[entry["class"]] = counts.get(entry["class"], 0) + 1
        if entry["class"] in MISMATCH_CLASSES:
            row = families.setdefault(entry["family"], {})
            row[entry["class"]] = row.get(entry["class"], 0) + 1
    summary = {"control": {name: f"{value:#06x}" for name, value in zip(CONTROL, header[2:7])},
               "classes": counts,
               "mismatches": sum(counts.get(c, 0) for c in MISMATCH_CLASSES),
               "immediate_mismatches": immediate,
               "refilled_halfwords": header[REFILLS_WORD],
               "after_init": ({name: f"{value:#06x}" for name, value in zip(AFTER_INIT, header[9:11])}
                              if header[8] == 1 else None),
               "mismatch_families": dict(sorted(families.items()))}
    return entries, summary


def record_profile(record_bytes):
    """PHY addresses and expected values in helper order, after the parameter run."""
    spec = importlib.util.spec_from_file_location("training_record", ROOT / "tools/training-record.py")
    training = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(training)
    runs = training.parse(record_bytes)["runs"][1:]
    return ([run["base"] + 2 * n for run in runs for n in range(len(run["values"]))],
            [value for run in runs for value in run["values"]])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("survey", type=Path)
    parser.add_argument("--record", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    entries, summary = decode(args.survey.read_bytes(), *record_profile(args.record.read_bytes()))
    text = json.dumps({"summary": summary, "entries": entries}, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    print(json.dumps(summary, indent=2))
