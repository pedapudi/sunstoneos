#!/usr/bin/env python3
"""Build, check and plan the DRAM training record (docs/designs/dram-training-restore.md).

  build --input SPEC.json --output RECORD.bin
  parse --record RECORD.bin
  plan --record RECORD.bin --allocation ALLOC.json --card-image IMAGE --output DIR
  invalidate --allocation ALLOC.json --card-image IMAGE --output DIR

The record is a 128-byte header and a payload. On the card it fills the
allocation's training-record unit: the header in its first sector, the
payload from the second. A plan lists the payload write before the header
write, so an interrupted update leaves the old header, or no valid header,
and boot0 trains fully. Plans go to tools/storage-update.py plan, which
turns each write into a backup-first session. No command opens a device.
"""
import argparse
import binascii
import importlib.util
import json
import os
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
MAGIC = b"AS73DTRN"
VERSION, HEADER_SIZE, SECTOR = 1, 128, 512
PLAN_KIND = "a733-training-record-plan"
# magic, version, header size, payload length, payload CRC, generation,
# chip ID x4, boot0 checksum, set index, strap CRC, temperature, limit,
# step value, size, section offsets and counts, reserved; the header CRC
# follows at offset 124.
HEADER = struct.Struct("<8sIIIIQ4IIIIiIII6I24x")
assert HEADER.size == 124


def crc(data):
    return binascii.crc32(data) & 0xffffffff


def boot_slots():
    spec = importlib.util.spec_from_file_location("boot_slots", ROOT / "tools/boot-slots.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build(spec):
    runs = bytearray()
    for run in spec["runs"]:
        values = run["values"]
        if not values or run["base"] % 2 or any(not 0 <= v <= 0xffff for v in values):
            raise ValueError("Each run needs an even base and 16-bit values")
        runs += struct.pack("<II", run["base"], len(values)) + struct.pack("<%dH" % len(values), *values)
        runs += bytes(-len(runs) % 4)
    masks = b"".join(struct.pack("<IHH", m["address"], m["mask"], 0) for m in spec.get("masks", []))
    mrs = b"".join(struct.pack("<4B", m["channel"], m["rank"], m["register"], m["value"])
                   for m in spec.get("mode_registers", []))
    payload = runs + masks + mrs
    sections = (0, len(spec["runs"]), len(runs), len(spec.get("masks", [])),
                len(runs) + len(masks), len(spec.get("mode_registers", [])))
    if len(spec["chip_id"]) != 4:
        raise ValueError("The chip ID has four words")
    header = HEADER.pack(MAGIC, VERSION, HEADER_SIZE, len(payload), crc(payload), spec["generation"],
                         *spec["chip_id"], spec["boot0_checksum"], spec["set_index"], spec["strap_crc"],
                         spec["temperature_mc"], spec["temperature_limit_mc"], spec["step_value"],
                         spec["size_mib"], *sections)
    return header + struct.pack("<I", crc(header)) + payload


def parse(record):
    """Return the record's fields, or raise ValueError as the hook falls back."""
    if len(record) < HEADER_SIZE:
        raise ValueError("Record shorter than its header")
    header = record[:124]
    if struct.unpack_from("<I", record, 124)[0] != crc(header):
        raise ValueError("Header CRC mismatch")
    fields = HEADER.unpack(header)
    if fields[0] != MAGIC or fields[1] != VERSION or fields[2] != HEADER_SIZE:
        raise ValueError("Not a version 1 training record")
    length, payload_crc = fields[3], fields[4]
    payload = record[HEADER_SIZE:HEADER_SIZE + length]
    if len(payload) != length or crc(payload) != payload_crc:
        raise ValueError("Payload CRC mismatch")
    runs_offset, runs, masks_offset, masks, mrs_offset, mrs = fields[17:23]
    registers, offset = [], runs_offset
    for _ in range(runs):
        base, count = struct.unpack_from("<II", payload, offset)
        offset += 8
        values = struct.unpack_from("<%dH" % count, payload, offset)
        offset += 2 * count + (-2 * count % 4)
        registers.append({"base": base, "values": list(values)})
    return {"generation": fields[5], "chip_id": list(fields[6:10]), "boot0_checksum": fields[10],
            "set_index": fields[11], "strap_crc": fields[12], "temperature_mc": fields[13],
            "temperature_limit_mc": fields[14], "step_value": fields[15], "size_mib": fields[16],
            "runs": registers,
            "masks": [dict(zip(("address", "mask"), struct.unpack_from("<IH", payload, masks_offset + 8 * i)))
                      for i in range(masks)],
            "mode_registers": [dict(zip(("channel", "rank", "register", "value"),
                                        struct.unpack_from("<4B", payload, mrs_offset + 4 * i)))
                               for i in range(mrs)]}


def record_unit(slots, allocation_path, card_image):
    raw = Path(allocation_path).read_bytes()
    allocation = json.loads(raw)
    fd, sectors = slots.open_image(card_image)
    try:
        slots.check_allocation(allocation, sectors, slots.read_at(fd, 0, 1))
        unit = allocation.get("training_record")
        if unit is None:
            raise ValueError("The allocation assigns no training-record unit")
        return raw, unit, fd, sectors
    except BaseException:
        os.close(fd)
        raise


def write_plan(slots, output, raw, sectors, writes, fd):
    try:
        out = slots.new_directory(output)
        listed = []
        for name, first, data in writes:
            data += bytes(-len(data) % SECTOR)
            blocks = len(data) // SECTOR
            current = slots.read_at(fd, first, blocks)
            slots.write_new(out / f"{name}.bin", data)
            listed.append({"name": name, "first_sector": first, "sectors": blocks, "file": f"{name}.bin",
                           "sha256": slots.sha(data), "previous_sha256": slots.sha(current)})
    finally:
        os.close(fd)
    result = {"schema": 1, "kind": PLAN_KIND, "card_sectors": sectors,
              "allocation_sha256": slots.sha(raw), "writes": listed}
    slots.write_new(out / "plan.json", (json.dumps(result, indent=2) + "\n").encode())
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("build")
    p.add_argument("--input", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p = sub.add_parser("parse")
    p.add_argument("--record", type=Path, required=True)
    for name in ("plan", "invalidate"):
        p = sub.add_parser(name)
        if name == "plan":
            p.add_argument("--record", type=Path, required=True)
        for option in ("allocation", "card-image", "output"):
            p.add_argument("--" + option, type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "build":
            record = build(json.loads(args.input.read_text()))
            parse(record)
            with args.output.open("xb") as handle:
                handle.write(record)
            result = {"bytes": len(record), "crc": crc(record)}
        elif args.command == "parse":
            result = parse(args.record.read_bytes())
        else:
            slots = boot_slots()
            raw, unit, fd, sectors = record_unit(slots, args.allocation, args.card_image)
            if args.command == "plan":
                record = args.record.read_bytes()
                parse(record)
                payload = record[HEADER_SIZE:]
                if 1 + -(-len(payload) // SECTOR) > unit["blocks"]:
                    raise ValueError("The record does not fit its unit")
                writes = [("record-payload", unit["first"] + 1, payload)] if payload else []
                writes.append(("record-header", unit["first"], record[:HEADER_SIZE]))
            else:
                writes = [("record-invalidate", unit["first"], bytes(SECTOR))]
            result = write_plan(slots, args.output, raw, sectors, writes, fd)
    except (ValueError, OSError, KeyError, struct.error, json.JSONDecodeError) as error:
        parser.exit(1, f"training-record: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
