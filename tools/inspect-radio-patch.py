#!/usr/bin/env python3
"""Inspect AIC Bluetooth patch tables without executing their address/value writes."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import sys

MAX_BYTES = 16 * 1024 * 1024
MAX_RECORDS = 8192
TAG = b"AICBT_PT_TAG\0"
TYPE_NAMES = {0: "info", 1: "trap", 2: "before", 3: "mode", 4: "power_on",
              5: "after", 6: "version"}


def inspect(data):
    """Validate record bounds; return metadata, not an executable upload plan.

    Format reference: board kernel 2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f,
    aic8800_bsp/aic_bsp_driver.c and .h. Unspecified tag/name padding is retained.
    Unknown types are reported because the reference writes their pairs too;
    this inspection does not approve those writes or any target address.
    """
    if len(data) > MAX_BYTES:
        raise ValueError("patch table exceeds inspection size limit")
    if len(data) < 16 or data[:len(TAG)] != TAG:
        raise ValueError("invalid or truncated patch-table tag")
    records = []
    offset = 16
    while offset < len(data):
        if len(records) >= MAX_RECORDS:
            raise ValueError("patch table exceeds inspection record limit")
        if len(data) - offset < 24:
            raise ValueError(f"truncated record header at {offset}")
        kind, count = struct.unpack_from("<II", data, offset + 16)
        start = offset + 24
        if count > (len(data) - start) // 8:
            raise ValueError(f"truncated pair array at {offset}")
        end = start + count * 8
        record = {
            "offset": offset,
            "name_hex": data[offset:offset + 16].hex(),
            "type": kind,
            "type_name": TYPE_NAMES.get(kind, "unknown"),
            "pair_count": count,
            "payload_sha256": hashlib.sha256(data[start:end]).hexdigest(),
        }
        if kind == 3 and count < 9:
            raise ValueError(f"mode record at {offset} lacks nine writable values")
        if kind == 6:
            # A bounded display does not require a C string terminator.
            record["version"] = data[start:end].split(b"\0", 1)[0].decode(
                "ascii", errors="backslashreplace")
        if kind == 0 and not records:
            # Only the first record supplies bootstrap addresses in this ABI.
            names = ("adid", "patch", "reset", "adid_flag")
            record["info"] = {
                name: list(struct.unpack_from("<II", data, start + i * 8))
                for i, name in enumerate(names[:min(count, 4)])
            }
            if count > 4:
                count_address, extensions = struct.unpack_from("<II", data, start + 32)
                if extensions > count - 5:
                    raise ValueError(f"extension count exceeds info record at {offset}")
                record["extension_count_address"] = count_address
                record["extensions"] = [
                    list(struct.unpack_from("<II", data, start + (5 + i) * 8))
                    for i in range(extensions)
                ]
        records.append(record)
        offset = end
    return {"format": "aic-bluetooth-patch-table", "validation": "structure_only",
            "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "records": records}


def read_table(path):
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as stream:
        # Reject device nodes/FIFOs and cap reads even if the file grows.
        if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
            raise ValueError("input must be a regular file")
        data = stream.read(MAX_BYTES + 1)
    return inspect(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("table", type=Path)
    parser.add_argument("--sha256", help="require this complete-file SHA-256")
    args = parser.parse_args()
    try:
        result = read_table(args.table)
        if args.sha256 is not None and result["sha256"] != args.sha256.lower():
            raise ValueError("patch-table SHA-256 mismatch")
    except (OSError, ValueError) as error:
        print(f"inspection failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
