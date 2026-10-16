#!/usr/bin/env python3
"""Prepare authenticated external inputs for the fixed convolution composition."""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def sha(data):
    return hashlib.sha256(data).hexdigest()


def array(name, data):
    values = [f"0x{x:02x}" for x in data]
    rows = [",".join(values[i:i + 16]) for i in range(0, len(values), 16)]
    return f"static const unsigned char {name}[{len(data)}]={{\n" + ",\n".join(rows) + "\n};\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output-reference", type=Path, required=True)
    parser.add_argument("--assembly-reference", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    args = parser.parse_args()
    inputs = [
        ("fixture_model", args.model, 1928, "db42d815100417f4b8913896462ed031beafae35fb4ee1c28569ac950a9e8fca"),
        ("fixture_input", args.input, 192, "976fdde5d777fd2b60d5146fa9acde1f2a4340c758555c9393f48c09cb1abf12"),
        ("fixture_output", args.output_reference, 72, "899612ebf5d62b2bebba2926ef68af8b63cf0050b2dd67fa2d3a5d759a5b9c9b"),
    ]
    text, identities, snapshots = "", {}, {}
    for name, path, size, expected in inputs:
        data = path.read_bytes()
        if len(data) != size or sha(data) != expected:
            raise ValueError(f"unaccepted {name}: {path}")
        text += array(name, data)
        identities[str(path.resolve())] = sha(data)
        snapshots[name + ".bin"] = data
    # Accepted INIT writes both external SRAM endpoints to zero in this order.
    init = struct.pack("<10I", 0x0801028a, 0x11, 0x08010e13, 2,
                       0x08010e4e, 0, 0x08010e4f, 0, 0x08010e50, 0)
    if init not in snapshots["fixture_model.bin"]:
        raise ValueError("accepted INIT sequence absent")
    # Independently spelled fixed bootstrap, including inserted INIT state.
    words = [0x08010061, 0xffffff7f, 0x08010e02, 0x30000701, 0x48000000, 0x30000701,
             0x0801028a, 0x11, 0x08010e13, 2, 0x08010e12, 0x18010000]
    for i in range(8):
        words += [0x08010e55, 0x03010000 + i]
    words += [0x08010e21, 0x20000, 0x08010e02, 0x701, 0x48000000, 0x701,
              0x0801022c, 0x1f, 0x08010e4e, 0x400000,
              0x08010e4f, 0, 0x08010e50, 0,
              0x08010e02, 0x30000701, 0x48000000, 0x30000701,
              0x08010e01, 0x40, 0x10000000, 0, 0x10000000, 0]
    assert len(words) * 4 == 208
    boot = struct.pack("<52I", *words) + struct.pack("<2I", 0x10000000, 0) * ((524288 - 208) // 8)
    snapshots["expected-bootstrap.bin"] = boot
    hashes = []
    accepted_buffers = {
        0: "49004b4cf4ec27e0f2d041eca75c7f82a7853e6e0ab3689a8c305df861e9c3f0",
        1: "5a1da72427f04fd2520905033e1eb6f6967e08114317ea9b59e912d70c59bc1f",
        3: "dc41d8da1fee1b98a6d57dc15dca91d404293b908d4d4e1ddcfbe9c9ae2728bb",
    }
    for index, expected in accepted_buffers.items():
        path = args.assembly_reference / f"buffer-{index}.bin"
        data = path.read_bytes()
        if len(data) != (524288 if index == 3 else 4096) or sha(data) != expected:
            raise ValueError(f"unexpected captured allocation length: {path}")
        identities[str(path.resolve())] = sha(data)
        snapshots[path.name] = data
        hashes.append(hashlib.sha256(data).digest())
    hashes.append(hashlib.sha256(boot).digest())
    # Known empty/abc answers and padding-boundary cases use independent hashlib.
    vectors = [b"", b"abc", b"a" * 56, b"a" * 64]
    for name, values in (("fixture_hashes", [hashlib.sha256(x).digest() for x in vectors]),
                         ("fixture_immutable", hashes)):
        text += f"static const unsigned char {name}[4][32]={{\n"
        text += ",\n".join("{" + ",".join(f"0x{x:02x}" for x in value) + "}" for value in values)
        text += "\n};\n"
    args.fixtures.mkdir(parents=True, exist_ok=False)
    for name, data in snapshots.items():
        (args.fixtures / name).write_bytes(data)
    header = args.fixtures / "npu-convolution-data.h"
    header.write_text(text)
    manifest = {"inputs": identities, "header_sha256": sha(header.read_bytes()),
                "bootstrap_sha256": sha(boot), "sha_vectors": [sha(x) for x in vectors],
                "method": "Fixture generation only. Captured assembly reused; no compiler, simulator or device execution."}
    (args.fixtures / "fixtures.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(header)


if __name__ == "__main__":
    main()
