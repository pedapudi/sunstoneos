#!/usr/bin/env python3
"""Inspect the NBG 0x1001e container without executing its command stream.

The serialized layout comes from SDK nbinfo debug types and reader traces.
See docs/npu.md for pinned references, evidence, and interpretation limits.
"""

import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import struct
import sys


VERSION = 0x1001E
HEADER_BYTES = 592
MAX_FILE_BYTES = 256 * 1024 * 1024
# Wire order differs from the order of members in nbinfo's in-memory structure.
SECTIONS = (
    "input", "output", "layer", "operation", "coeff_header_table",
    "coeff_header", "nn_op", "tp_op", "sh_op", "patch", "layer_param",
    "sw_op", "hw_init", "ICDT", "ICD", "ppu_param", "op_sync", "dma_sbi",
    "ppu_instr", "dyna_io", "load_states", "dma_addi", "coeff_table",
    "coeff", "ppu_instr_table", "dyna_io_table", "load_states_table",
)
FORMATS = {
    0: ("FP32", 32), 1: ("FP16", 16), 2: ("UINT8", 8), 3: ("INT8", 8),
    4: ("UINT16", 16), 5: ("INT16", 16), 6: ("CHAR", 8), 7: ("BFP16", 16),
    8: ("INT32", 32), 9: ("UINT32", 32), 10: ("INT64", 64),
    11: ("UINT64", 64), 12: ("FP64", 64), 13: ("INT4", 4),
    14: ("UINT4", 4), 16: ("BOOL8", 8),
}
OPERATIONS = {
    0: "NONE", 1: "SH", 2: "NN", 3: "TP", 4: "SW", 5: "SC", 6: "SP",
    7: "NN_LUT", 8: "NT", 65534: "END", 65535: "INIT",
}
SOURCES = {
    0: "COMMAND", 1: "INPUT", 2: "OUTPUT", 3: "MEMORY_POOL", 4: "AXI_SRAM",
    5: "MISC_DYNA_GENERIC", 6: "MISC_DYNA_INPUT", 7: "MISC_DYNA_OUTPUT",
    8: "VIP_SRAM", 9: "KERNEL_BITSTREAM", 10: "LAYER_DUMP_OUTPUT",
    100: "MULTIVIP_CHIPID", 101: "MULTIVIP_SYNC_CMDS",
}


class FormatError(ValueError):
    """The file does not satisfy this inspector's supported format profile."""


def require(condition, message):
    if not condition:
        raise FormatError(message)


def inspect(data):
    """Return metadata only; success does not authorize hardware submission."""
    require(HEADER_BYTES <= len(data) <= MAX_FILE_BYTES, "file size outside supported range")
    require(data[:4] == b"VPMN", "invalid NBG magic")

    def u32(offset):
        return struct.unpack_from("<I", data, offset)[0]

    def name(offset, size):
        raw = data[offset:offset + size]
        # A full fixed-width name need not contain a terminating zero.
        return raw.split(b"\0", 1)[0].decode("utf-8", errors="replace")

    require(u32(4) == VERSION, f"unsupported NBG version {u32(4):#x}")
    flags = u32(92)
    nn_size_code, tp_size_code = (flags >> 2) & 3, (flags >> 5) & 3
    require(nn_size_code <= 1 and tp_size_code <= 1, "unknown command size encoding")
    nn_bytes, tp_bytes = (128, 192)[nn_size_code], (128, 192)[tp_size_code]
    sections = {}
    occupied = []
    for i, key in enumerate(SECTIONS):
        offset, size = struct.unpack_from("<II", data, 376 + i * 8)
        require(offset <= len(data) and size <= len(data) - offset,
                f"{key}: section outside file")
        if size:
            require(offset >= HEADER_BYTES, f"{key}: section overlaps header")
            occupied.append((offset, offset + size, key))
        sections[key] = {"offset": offset, "bytes": size}
    occupied.sort()
    for left, right in zip(occupied, occupied[1:]):
        require(left[1] <= right[0], f"{left[2]} and {right[2]}: overlapping sections")

    def rows(key, stride):
        section = sections[key]
        require(section["bytes"] % stride == 0, f"{key}: partial record")
        return range(section["offset"], section["offset"] + section["bytes"], stride)

    def count(key, stride):
        return len(rows(key, stride))

    for key, header_at, stride in (("input", 84, 116), ("output", 88, 116),
                                   ("layer", 76, 76), ("operation", 80, 32)):
        require(count(key, stride) == u32(header_at), f"{key}: header count mismatch")

    def tensors(key):
        result = []
        for offset in rows(key, 116):
            dims = u32(offset)
            require(1 <= dims <= 6, f"{key}: invalid dimension count")
            shape = list(struct.unpack_from("<6I", data, offset + 4))[:dims]
            require(all(shape), f"{key}: zero dimension")
            fmt, kind, quant = struct.unpack_from("<3I", data, offset + 28)
            require(fmt in FORMATS, f"{key}: unknown element format {fmt}")
            require(kind <= 3, f"{key}: unknown buffer type {kind}")
            require(quant <= 2, f"{key}: unknown quantization {quant}")
            format_name, bits = FORMATS[fmt]
            logical_bytes = (math.prod(shape) * bits + 7) // 8
            require(logical_bytes <= 0xFFFFFFFF, f"{key}: logical tensor size exceeds 32 bits")
            fixed_pos = struct.unpack_from("<i", data, offset + 40)[0]
            scale = struct.unpack_from("<f", data, offset + 44)[0]
            zero_point = struct.unpack_from("<i", data, offset + 48)[0]
            quantization = {"kind": ("none", "dynamic_fixed_point", "asymmetric")[quant]}
            if quant == 1:
                quantization["fixed_position"] = fixed_pos
            elif quant == 2:
                require(math.isfinite(scale) and scale > 0, f"{key}: invalid quantization scale")
                quantization.update(scale=scale, zero_point=zero_point)
            result.append({"name": name(offset + 52, 64), "dimensions": shape,
                           "format": format_name, "buffer_type": kind,
                           "logical_bytes": logical_bytes, "quantization": quantization})
        return result

    # These entries contain offsets relative to their corresponding payload.
    for table, payload in (("coeff_header_table", "coeff_header"), ("coeff_table", "coeff"),
                           ("ppu_instr_table", "ppu_instr"), ("dyna_io_table", "dyna_io"),
                           ("load_states_table", "load_states"), ("ICDT", "ICD")):
        for at in rows(table, 8):
            offset, size = struct.unpack_from("<II", data, at)
            total = sections[payload]["bytes"]
            require(offset <= total and size <= total - offset,
                    f"{table}: entry outside {payload}")

    patch_count = count("patch", 40)
    state_count = count("load_states_table", 8)
    nn_count, tp_count = count("nn_op", nn_bytes), count("tp_op", tp_bytes)
    for key, stride in (("sh_op", 8), ("layer_param", 76), ("sw_op", 68),
                         ("hw_init", 12), ("ppu_param", 48)):
        count(key, stride)
    operation_counts = Counter()
    for at in rows("operation", 32):
        kind, index, layer, state, patch, n_patch = struct.unpack_from("<6I", data, at)
        operation_counts[OPERATIONS.get(kind, f"UNKNOWN_{kind}")] += 1
        require(patch <= patch_count and n_patch <= patch_count - patch,
                "operation: patch range outside table")
        require(state < state_count, "operation: load state index outside table")
        if kind in (2, 3):
            require(index < (nn_count if kind == 2 else tp_count), "operation: command index outside table")
        if kind in range(1, 9):
            require(layer < u32(76), "operation: layer index outside table")
    for at in rows("hw_init", 12):
        state, patch, n_patch = struct.unpack_from("<3I", data, at)
        require(state < state_count, "hw_init: load state index outside table")
        require(patch <= patch_count and n_patch <= patch_count - patch,
                "hw_init: patch range outside table")

    patch_sources = Counter()
    patch_types = Counter()
    for at in rows("patch", 40):
        kind, _, source, index = struct.unpack_from("<4I", data, at)
        patch_types[str(kind)] += 1
        patch_sources[SOURCES.get(source, f"UNKNOWN_{source}")] += 1
        if source in (1, 2):
            require(index < u32(84 if source == 1 else 88), "patch: tensor index outside table")

    alignment = u32(352)
    require(alignment != 0 and alignment & (alignment - 1) == 0,
            "memory pool alignment is not a power of two")
    return {
        "scope": "structural inspection; commands, relocations, and checksum are not validated",
        "sha256": hashlib.sha256(data).hexdigest(), "file_bytes": len(data),
        "version": hex(VERSION), "target": hex(u32(8)), "name": name(12, 64),
        "feature_bits": hex(flags), "core_count": data[100],
        "address_width_code": (flags >> 9) & 3,
        "nn_command_bytes": nn_bytes, "tp_command_bytes": tp_bytes,
        "stored_checksum": hex(u32(108)), "checksum_verified": False,
        "memory": {"pool_bytes": u32(348), "pool_alignment": alignment,
                   "pool_base": u32(356), "axi_sram_base": u32(360),
                   "axi_sram_bytes": u32(364), "vip_sram_base": u32(368),
                   "vip_sram_bytes": u32(372)},
        "layer_count": u32(76), "operation_count": u32(80),
        "operations": dict(sorted(operation_counts.items())),
        "patch_count": patch_count, "patch_types": dict(sorted(patch_types.items())),
        "patch_sources": dict(sorted(patch_sources.items())),
        "inputs": tensors("input"), "outputs": tensors("output"), "sections": sections,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path)
    args = parser.parse_args()
    try:
        with args.model.open("rb") as stream:
            require(stream.seek(0, 2) <= MAX_FILE_BYTES, "model exceeds 256 MiB inspection limit")
            stream.seek(0)
            data = stream.read(MAX_FILE_BYTES + 1)
        print(json.dumps(inspect(data), indent=2, allow_nan=False))
    except (OSError, FormatError) as error:
        print(f"NBG inspection failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
