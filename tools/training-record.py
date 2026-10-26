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
import hashlib
import json
import os
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
MAGIC = b"AS73DTRN"
VERSION, HEADER_SIZE, SECTOR = 1, 128, 512
PLAN_KIND = "a733-training-record-plan"
BOOT0_SHA256 = "829ea8e3b65d71a02f2d0e094bc073de3a10869f948f62db49dbf023d07fabe3"
RESTORE_SET_SHA256 = "d1a7c6fe2570726e75057bb71bacaf154f937eede05a4fc9237ec8647e087991"
CAPTURE_LAYOUT_SHA256 = "c7a012ca80a27eb6c666d4db0f9e5e10dbfefa23b1ba30ff08eac3206f594b2d"
PARAMETERS = 0x831f8
RECORD_LIMIT = 0x6000


def restore_set(path):
    data = Path(path).read_bytes()
    if hashlib.sha256(data).hexdigest() != RESTORE_SET_SHA256:
        raise ValueError("Restore set differs from the reviewed ten-lifetime analysis")
    return json.loads(data)


def profile(analysis):
    """Ordered PHY addresses and masks; parameters use a separate full-width run."""
    entries = analysis["kind_diff"]["registers"]
    assert len(entries) == 1036
    assert {k for k in entries if k.startswith("param[")} == {f"param[{i}]" for i in range(32, 52)}
    return [(int(k, 16), v["mask"]) for k, v in sorted(entries.items()) if k.startswith("0x")]


def from_capture(directory, set_path, boot0_path, generation):
    directory = Path(directory).resolve()
    analysis = restore_set(set_path)
    metadata = next((c for c in analysis["captures"] if Path(c["path"]).resolve() == directory), None)
    if metadata is None or metadata["training_kind"] != "pstate0":
        raise ValueError("Capture is not an admitted P-state-0 reference")
    session = json.loads((directory / "session.json").read_text())
    stages = session["manifest"]["stages"]
    layout = [(s["name"], s["output_bytes"],
               [(e["address"], e["count"], e["width"], e["offset"]) for e in s["entries"]]) for s in stages]
    if hashlib.sha256(json.dumps(layout, separators=(",", ":")).encode()).hexdigest() != CAPTURE_LAYOUT_SHA256:
        raise ValueError("Capture stage tables differ from the reviewed capture")
    if not session.get("complete") or len(session["stages"]) != len(stages):
        raise ValueError("Capture is incomplete")
    ddr = json.loads((Path(session["ddr_evidence"]) / "session.json").read_text())
    if (not ddr.get("complete") or ddr["manifest"].get("variant") != "pstate0"
            or ddr["manifest"]["boot0_sha256"] != BOOT0_SHA256
            or ddr["result"]["dram_mib"] != 12288 or ddr["result"]["board_result"] != 0):
        raise ValueError("Capture training kind or DDR identity differs")
    values = {}
    for stage, outcome in zip(stages, session["stages"]):
        if not outcome["done"] or outcome["name"] != stage["name"]:
            raise ValueError("Capture stage did not finish")
        data = (directory / outcome["output"]).read_bytes()
        if len(data) != stage["output_bytes"]:
            raise ValueError("Capture stage size differs")
        for entry in stage["entries"]:
            for n in range(entry["count"]):
                address = entry["address"] + n * entry["width"]
                value = struct.unpack_from("<H" if entry["width"] == 2 else "<I", data,
                                           entry["offset"] + n * entry["width"])[0]
                # The vendor-window stage overlaps the later full PHY capture.
                values[address] = value
    chip = [values[0x03006200 + 4 * n] for n in range(4)]
    if "chip-" + "-".join(f"{v:08x}" for v in chip) != metadata["board"]:
        raise ValueError("Capture SID differs from the analysis board")
    params = [int(word, 16) for word in session["parameter_block"]]
    if len(params) != 128 or params[30] & 4 or (params[27] >> 16) & 0x1fff != 0x125f:
        raise ValueError("Capture parameters are not P-state-0 trained values")
    # Every selected register is in varying, including the fixed trained values:
    # they differ from initialization-only captures. Bind bytes to that evidence.
    for key in analysis["kind_diff"]["registers"]:
        actual = params[int(key[6:-1])] if key.startswith("param[") else values[int(key, 16)]
        if analysis["varying"][key]["values"][str(directory)] != actual:
            raise ValueError("Capture value differs from the frozen analysis: " + key)
    boot0 = Path(boot0_path).read_bytes()
    if hashlib.sha256(boot0).hexdigest() != BOOT0_SHA256:
        raise ValueError("boot0 differs from the reviewed image")
    phy = profile(analysis)
    runs = [{"base": PARAMETERS + 128,
             "values": list(struct.unpack("<40H", struct.pack("<20I", *params[32:52])))}]
    for address, mask in phy:
        if runs[-1]["base"] + 2 * len(runs[-1]["values"]) == address:
            runs[-1]["values"].append(values[address])
        else:
            runs.append({"base": address, "values": [values[address]]})
    return {"generation": generation, "chip_id": chip,
            "boot0_checksum": struct.unpack_from("<I", boot0, 12)[0], "set_index": 5,
            "strap_crc": crc(boot0[0x574:0x5f4]), "temperature_mc": -2147483648,
            "temperature_limit_mc": 0, "step_value": 0x125f, "size_mib": 12288,
            "runs": runs, "masks": [{"address": a, "mask": m} for a, m in phy],
            "mode_registers": []}
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
    if (record[100:124] != bytes(24) or runs_offset != 0
            or not 0 <= masks_offset <= mrs_offset <= length
            or masks_offset + 8 * masks != mrs_offset or mrs_offset + 4 * mrs != length
            or runs > masks_offset // 12):
        raise ValueError("Invalid record section geometry")
    registers, offset = [], runs_offset
    for _ in range(runs):
        if offset + 8 > masks_offset:
            raise ValueError("Run header exceeds register section")
        base, count = struct.unpack_from("<II", payload, offset)
        offset += 8
        if not count or base % 2 or count > (masks_offset - offset) // 2 or base + 2 * count > 0x100000000:
            raise ValueError("Invalid register run")
        values = struct.unpack_from("<%dH" % count, payload, offset)
        offset += 2 * count + (-2 * count % 4)
        if offset > masks_offset:
            raise ValueError("Run padding exceeds register section")
        registers.append({"base": base, "values": list(values)})
    if offset != masks_offset or any(struct.unpack_from("<H", payload, masks_offset + 8*i + 6)[0]
                                     for i in range(masks)):
        raise ValueError("Invalid run length or mask reserved field")
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


def write_plan(slots, output, raw, sectors, writes, fd, metadata=None):
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
              "allocation_sha256": slots.sha(raw), "writes": listed, **(metadata or {})}
    slots.write_new(out / "plan.json", (json.dumps(result, indent=2) + "\n").encode())
    return result


def admit_restore(record, analysis, chip=None):
    """Host equivalent of the existing fixed SRAM profile, before selection."""
    spec = parse(record)
    if len(record) > RECORD_LIMIT or len(record) != HEADER_SIZE + struct.unpack_from('<I', record, 16)[0]:
        raise ValueError('Record exceeds the admitted SRAM geometry')
    fields = {'boot0_checksum': 0x2fce7bb3, 'set_index': 5, 'strap_crc': 0xf874a6a5,
              'temperature_mc': -2147483648, 'temperature_limit_mc': 0,
              'step_value': 0x125f, 'size_mib': 12288}
    if any(spec[k] != v for k, v in fields.items()):
        raise ValueError('Record differs from the admitted restore identity')
    phy = profile(analysis)
    addresses = [r['base'] + 2*i for r in spec['runs'] for i in range(len(r['values']))]
    expected = [PARAMETERS+128+2*i for i in range(40)] + [a for a, m in phy]
    if (addresses != expected or len(spec['runs'][0]['values']) != 40
            or spec['masks'] != [{'address': a, 'mask': m} for a, m in phy] or spec['mode_registers']):
        raise ValueError('Record differs from the admitted address/mask profile')
    board = 'chip-' + '-'.join(f'{v:08x}' for v in spec['chip_id'])
    if board not in analysis['kind_diff']['per_board'] or (chip is not None and spec['chip_id'] != chip):
        raise ValueError('Record belongs to another board')
    if not spec['generation']:
        raise ValueError('Record generation must be nonzero')
    return spec


def read_copy(slots, fd, unit):
    """Read only the bounded bytes used for this copy's selection decision."""
    header = slots.read_at(fd, unit['first'], 1)
    guards = [{'first': unit['first'], 'blocks': 1, 'sha256': slots.sha(header)}]
    try:
        if crc(header[:124]) != struct.unpack_from('<I', header, 124)[0]:
            raise ValueError('Header CRC mismatch')
        if header[:8] != MAGIC or struct.unpack_from('<II', header, 8) != (VERSION, HEADER_SIZE):
            raise ValueError('Unsupported record header')
        length = struct.unpack_from('<I', header, 16)[0]
        if not length or length > RECORD_LIMIT-HEADER_SIZE:
            raise ValueError('Record exceeds the admitted SRAM geometry')
        blocks = (length+SECTOR-1)//SECTOR
        if blocks+1 > unit['blocks']:
            raise ValueError('Record exceeds its allocation')
        payload = slots.read_at(fd, unit['first']+1, blocks)
        guards.append({'first': unit['first']+1, 'blocks': blocks, 'sha256': slots.sha(payload)})
        record = header[:HEADER_SIZE] + payload[:length]
        parsed = parse(record)
        return {'status': 'valid', 'generation': parsed['generation'], '_record': record, '_spec': parsed, 'guards': guards}
    except (ValueError, struct.error) as error:
        return {'status': 'invalid', 'reason': str(error), 'guards': guards}


def select_copies(slots, fd, units, analysis, reference):
    if set(units) != {'A', 'B'}:
        raise ValueError('Selection requires a paired training-record allocation')
    target = admit_restore(reference, analysis)
    copies = {name: read_copy(slots, fd, unit) for name, unit in units.items()}
    for state in copies.values():
        if state['status'] == 'valid':
            try:
                admit_restore(state['_record'], analysis, target['chip_id'])
                state['status'] = 'eligible'
            except ValueError as error:
                state.update(status='ineligible', reason=str(error))
    order = sorted((name for name in copies if copies[name]['status'] == 'eligible'),
                   key=lambda name: (-copies[name]['generation'], name))
    return {'selected': order[0] if order else None, 'action': 'restore' if order else 'full-training',
            'copies': copies}


def public_selection(selection):
    return {**selection, 'copies': {name: {k:v for k,v in state.items() if not k.startswith('_')}
                                   for name,state in selection['copies'].items()}}


def paired_image(slots, allocation_path, card_image):
    raw = Path(allocation_path).read_bytes()
    allocation = json.loads(raw)
    fd, sectors = slots.open_image(card_image)
    try:
        slots.check_allocation(allocation, sectors, slots.read_at(fd, 0, 1))
        units = slots.training_units(allocation)
        if set(units) != {'A', 'B'}:
            raise ValueError('Operation requires a paired training-record allocation')
        return raw, units, fd, sectors
    except BaseException:
        os.close(fd)
        raise


def plan_standby(slots, allocation_path, card_image, reference, analysis, output):
    raw, units, fd, sectors = paired_image(slots, allocation_path, card_image)
    try:
        desired = admit_restore(reference, analysis)
        selected = select_copies(slots, fd, units, analysis, reference)
        for state in selected['copies'].values():
            if '_record' in state:
                try:
                    other = admit_restore(state['_record'], analysis)
                except ValueError:
                    continue
                if other['chip_id'] != desired['chip_id']:
                    raise ValueError('A valid allocated copy belongs to another board')
        active = selected['selected']
        if active:
            current = selected['copies'][active]['_spec']
            if {k:v for k,v in current.items() if k != 'generation'} == {k:v for k,v in desired.items() if k != 'generation'}:
                raise ValueError('Selected copy already holds this record; resume the existing plan')
        generation = max((state['generation'] for state in selected['copies'].values()
                          if state['status'] == 'eligible'), default=0) + 1
        if generation > 0xffffffffffffffff:
            raise ValueError('Record generation is exhausted')
        target = 'B' if active == 'A' else 'A'
        other = 'B' if target == 'A' else 'A'
        desired['generation'] = generation
        record = build(desired)
        unit = units[target]
        writes = [(f'record-{target}-payload', unit['first']+1, record[HEADER_SIZE:]),
                  (f'record-{target}-header', unit['first'], record[:HEADER_SIZE])]
        metadata = {'record_copy': target, 'generation': generation, 'before': public_selection(selected),
                    'guards': selected['copies'][other]['guards'], 'restore_set_sha256': RESTORE_SET_SHA256,
                    'reference_sha256': slots.sha(reference)}
    except BaseException:
        os.close(fd)
        raise
    return write_plan(slots, output, raw, sectors, writes, fd, metadata)


def validate_standby_plan(slots, plan, units, fd, directory):
    """Bind every inspected non-target byte and require header-last geometry."""
    target = plan.get('record_copy')
    if set(units) != {'A', 'B'} or target not in units or plan.get('restore_set_sha256') != RESTORE_SET_SHA256:
        raise ValueError('Not a paired restore-profile plan')
    other = 'B' if target == 'A' else 'A'
    if plan.get('guards') != read_copy(slots, fd, units[other])['guards']:
        raise ValueError('Non-target training record changed since selection')
    writes = plan['writes']
    if [w['name'] for w in writes] != [f'record-{target}-payload', f'record-{target}-header']:
        raise ValueError('Paired record plan must write payload then header')
    payload, header = [Path(directory, w['file']).read_bytes() for w in writes]
    if (writes[0]['first_sector'] != units[target]['first']+1
            or writes[1]['first_sector'] != units[target]['first'] or len(header) != SECTOR):
        raise ValueError('Paired record plan has invalid header/payload placement')
    length = struct.unpack_from('<I', header, 16)[0]
    if len(payload) != (length+SECTOR-1)//SECTOR*SECTOR or length > RECORD_LIMIT-HEADER_SIZE:
        raise ValueError('Paired record payload exceeds its profile')
    spec = parse(header[:HEADER_SIZE] + payload[:length])
    if spec['generation'] != plan.get('generation') or not spec['generation']:
        raise ValueError('Paired record generation differs from its plan')


def validate_helper_plan(slots, plan, units, directory):
    """Admit only identical helper copies in the reserved subextents."""
    size = plan.get('helper_bytes')
    if set(units) != {'A', 'B'} or type(size) is not int or not 0 < size <= 32768:
        raise ValueError('Helper plan requires paired units and a bounded image')
    writes = plan['writes']
    if [w['name'] for w in writes] != ['helper-A', 'helper-B']:
        raise ValueError('Helper plan must contain exactly copies A and B')
    copies = []
    for name, write in zip('AB', writes):
        data = Path(directory, write['file']).read_bytes()
        if (write['first_sector'] != units[name]['first']+64
                or write['sectors'] != (size+511)//512 or len(data) != write['sectors']*512
                or slots.sha(data[:size]) != plan.get('helper_sha256') or any(data[size:])):
            raise ValueError('Helper plan violates its identity or reserved extent')
        copies.append(data)
    if copies[0] != copies[1]:
        raise ValueError('Helper copies differ')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("build")
    p.add_argument("--input", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p = sub.add_parser("parse")
    p.add_argument("--record", type=Path, required=True)
    p = sub.add_parser("from-capture")
    for option in ("capture", "restore-set", "boot0", "output"):
        p.add_argument("--" + option, type=Path, required=True)
    p.add_argument("--generation", type=int, required=True)
    for name in ('select', 'plan-standby'):
        p = sub.add_parser(name)
        for option in ('allocation', 'card-image', 'restore-set', 'record'):
            p.add_argument('--'+option, type=Path, required=True)
        if name == 'plan-standby':
            p.add_argument('--output', type=Path, required=True)
    for name in ("plan", "invalidate"):
        p = sub.add_parser(name)
        if name == "plan":
            p.add_argument("--record", type=Path, required=True)
        for option in ("allocation", "card-image", "output"):
            p.add_argument("--" + option, type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command in ("build", "from-capture"):
            spec = (from_capture(args.capture, args.restore_set, args.boot0, args.generation)
                    if args.command == "from-capture" else json.loads(args.input.read_text()))
            record = build(spec)
            if args.command == "from-capture" and len(record) > RECORD_LIMIT:
                raise ValueError("Record exceeds restore SRAM reservation")
            parse(record)
            with args.output.open("xb") as handle:
                handle.write(record)
            result = {"bytes": len(record), "crc": crc(record)}
        elif args.command == "parse":
            result = parse(args.record.read_bytes())
        elif args.command in ('select', 'plan-standby'):
            slots = boot_slots()
            analysis = restore_set(args.restore_set)
            reference = args.record.read_bytes()
            if args.command == 'plan-standby':
                result = plan_standby(slots, args.allocation, args.card_image, reference, analysis, args.output)
            else:
                raw, units, fd, sectors = paired_image(slots, args.allocation, args.card_image)
                try:
                    result = public_selection(select_copies(slots, fd, units, analysis, reference))
                finally:
                    os.close(fd)
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
