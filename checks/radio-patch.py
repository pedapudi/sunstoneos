#!/usr/bin/env python3
"""Check a published radio table and malformed structures on the host."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("radio_patch", ROOT / "tools/inspect-radio-patch.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
EXPECTED = "5ff184105fdc4c4679e026418f4da293efec8df02a3a3ca22cee723c17df4a60"


def record(kind, count, payload):
    return b"sample".ljust(16, b"\0") + struct.pack("<II", kind, count) + payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("table", type=Path)
    args = parser.parse_args()
    result = module.read_table(args.table)
    assert result["sha256"] == EXPECTED, "unexpected published artifact"
    assert result["bytes"] == 23288
    assert [r["type"] for r in result["records"]] == list(range(7))
    assert [r["pair_count"] for r in result["records"]] == [4, 2793, 48, 9, 3, 23, 8]
    info = result["records"][0]["info"]
    assert info["adid"] == [0x1e602c, 0x201940]
    assert info["patch"] == [0x1e6030, 0x1e0000]
    assert result["records"][-1]["version"] == "- Jul 11 2024 10:38:54 - git 73d2ce5"
    tag = module.TAG.ljust(16, b"\xa5")
    malformed = {
        "tag": b"wrong" + bytes(11),
        "header": tag + bytes(23),
        "payload": tag + record(1, 1, bytes(7)),
        "count_overflow": tag + record(1, 0xffffffff, b""),
        "mode_values": tag + record(3, 8, bytes(64)),
        "extension_values": tag + record(0, 5, bytes(32) + struct.pack("<II", 0, 1)),
    }
    for name, data in malformed.items():
        try:
            module.inspect(data)
        except ValueError:
            continue
        raise AssertionError(f"accepted malformed case: {name}")
    extended = module.inspect(tag + record(0, 6, bytes(32) + struct.pack("<IIII", 4, 1, 7, 0x200000)))
    assert extended["records"][0]["extensions"] == [[7, 0x200000]]
    unknown = module.inspect(tag + record(99, 0, b""))
    assert unknown["records"][0]["type_name"] == "unknown"
    print(json.dumps({"result": "passed", "artifact_sha256": EXPECTED,
                      "published_records": len(result["records"]),
                      "malformed_cases": list(malformed),
                      "extended_info_and_unspecified_padding": "passed",
                      "unknown_type_preserved": "passed",
                      "sources": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                  for p in (Path(__file__), ROOT / "tools/inspect-radio-patch.py")}}, indent=2))


if __name__ == "__main__":
    main()
