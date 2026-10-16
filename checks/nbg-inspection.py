#!/usr/bin/env python3
"""Compare a pinned compiled network with recorded SDK metadata and corruptions."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("nbg_inspector", ROOT / "tools/inspect-nbg.py")
nbg = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(nbg)
FIXTURE_SHA256 = "fc0d50bdc863e4dfc74eef6dad9d34655c560e759a2f426979a01c505fa9b95f"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path)
    parser.add_argument("reference", type=Path, help="directory containing recorded nbinfo reports")
    args = parser.parse_args()
    data = args.model.read_bytes()
    assert hashlib.sha256(data).hexdigest() == FIXTURE_SHA256, "wrong model fixture"
    # Exercise the public command once, then use the parser directly for faults.
    proc = subprocess.run([sys.executable, str(ROOT / "tools/inspect-nbg.py"), str(args.model)],
                          text=True, capture_output=True, timeout=5, check=True)
    model = json.loads(proc.stdout)
    reference = (args.reference / "reference-summary.txt").read_text()

    def field(text, label):
        match = re.search(r"^" + re.escape(label) + r":\s+(\S.*)$", text, re.MULTILINE)
        assert match, f"missing reference field: {label}"
        return match[1].strip()

    assert model["name"] == field(reference, "Network Name")
    for key, label in (("version", "Version"), ("target", "Target"),
                       ("core_count", "Core Count"), ("layer_count", "Layer Count"),
                       ("operation_count", "Operation Count")):
        assert int(str(model[key]), 0) == int(field(reference, label), 0)
    for key, label in (("pool_bytes", "Memory Pool Size (bytes)"),
                       ("pool_alignment", "Memory Pool Alignment"),
                       ("vip_sram_bytes", "VIP SRAM Size"), ("axi_sram_bytes", "AXI SRAM Size")):
        assert model["memory"][key] == int(field(reference, label), 0)
    assert len(model["inputs"]) == len(model["outputs"]) == 1
    for tensor, filename, logical_bytes in ((model["inputs"][0], "reference-tensors.txt", 150528),
                                           (model["outputs"][0], "reference-output.txt", 2)):
        report = (args.reference / filename).read_text()
        assert len(tensor["dimensions"]) == int(field(report, "Dim Count"))
        for i, size in enumerate(tensor["dimensions"]):
            assert size == int(field(report, f"Size of Dim[{i}]"))
        assert field(report, "Data Format") == "VIP_BUFFER_FORMAT_" + tensor["format"]
        assert abs(tensor["quantization"]["scale"] - float(field(report, "TF Scale"))) < 0.0000005
        assert tensor["quantization"]["zero_point"] == int(field(report, "TF Zeropoint"))
        assert tensor["logical_bytes"] == logical_bytes
    # The SDK reports a 64-byte output allocation for two logical UINT8 values.
    assert model["outputs"][0]["logical_bytes"] < int(field(
        (args.reference / "reference-output.txt").read_text(), "Memory Size (bytes)"))
    assert model["operations"] == {"INIT": 1, "NN": 125}
    assert model["patch_count"] == 660
    assert model["patch_sources"] == {"COMMAND": 125, "INPUT": 1, "OUTPUT": 1,
        "MEMORY_POOL": 241, "VIP_SRAM": 215, "MISC_DYNA_GENERIC": 77}
    assert model["checksum_verified"] is False

    failures = []

    def refuse(label, damaged, message):
        try:
            nbg.inspect(damaged)
        except nbg.FormatError as error:
            assert message in str(error), f"{label}: wrong failure: {error}"
            failures.append(label)
        else:
            raise AssertionError(f"{label}: accepted corrupt model")

    def change(label, offset, value, message):
        damaged = bytearray(data)
        struct.pack_into("<I", damaged, offset, value)
        refuse(label, damaged, message)

    refuse("truncated fixed header", data[:591], "file size")
    refuse("truncated final payload", data[:-1], "outside file")
    refuse("wrong magic", b"BAD!" + data[4:], "magic")
    change("unknown layout", 4, 0x20000, "unsupported NBG version")
    change("section offset overflow", 376, 0xFFFFFFFC, "outside file")
    change("section size overflow", 380, 0xFFFFFFFF, "outside file")
    change("header alias", 376, 0, "overlaps header")
    change("section alias", 384, 592, "overlapping sections")
    change("tensor count mismatch", 84, 0xFFFFFFFF, "count mismatch")
    change("invalid rank", 592, 7, "dimension count")
    change("tensor size overflow", 596, 0xFFFFFFFF, "exceeds 32 bits")
    change("nonfinite quantization", 636, 0x7F800000, "quantization scale")
    change("nested payload escape", 61868, 0xFFFFFFFF, "entry outside coeff")
    change("operation patch escape", 7436 + 16, 0xFFFFFFFF, "patch range")
    change("operation state escape", 7436 + 12, 0xFFFFFFFF, "state index")
    change("operation command escape", 7436 + 32 + 4, 0xFFFFFFFF, "command index")
    # Third relocation references input tensor zero.
    change("patch tensor escape", 35468 + 2 * 40 + 12, 1, "tensor index")
    change("invalid allocation alignment", 352, 3, "power of two")
    assert hashlib.sha256(data).hexdigest() == FIXTURE_SHA256
    print(json.dumps({"result": "pass", "fixture_sha256": FIXTURE_SHA256,
                      "reference_metadata": "matched", "refused_cases": failures}, indent=2))


if __name__ == "__main__":
    main()
