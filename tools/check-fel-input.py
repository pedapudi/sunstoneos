#!/usr/bin/env python3
"""Sample the A733 FEL input and restore the SRAM used by the read helpers.

This does not arm a watchdog or reset the board. Four low samples establish
the observed input level only; they do not qualify recovery across reset.
"""
import argparse
import fcntl
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / ".cache/sunstoneos"
FEL_HASH = "4a54cdc179baafbdecac47eb85714c3745d6d9952e05b482c7016afd4b22bf3e"
ROM_HASH = "a8e31821e8a1611d75867825914c58373c732742cb8f2235e8168c1d7e93b5aa"
REGIONS = (("copy", 0x52000, 0x400), ("scratch", 0x71000, 128))


class InspectionRefused(ValueError):
    """Inspection ended without a usable result, after restoring SRAM."""


def inspect_input(transaction, evidence, rom):
    """Return samples after successful restoration; stop on protocol failure."""
    transaction(*[arg for name, address, length in REGIONS for arg in
                  ("read", hex(address), length, evidence / f"{name}-before.bin")])
    # These windows cover the reset prefix and boot-input routine. The complete
    # reference was captured earlier; this check does not recapture that ROM.
    matches = []
    for label, address, length in (("reset", 0, 0x2f4), ("input", 0x10100, 0x100)):
        result = evidence / f"rom-{label}.bin"
        transaction("memmove", "0x52000", hex(address), length,
                    "read", "0x52000", length, result)
        matches.append(result.read_bytes() == rom[address:address + length])
    if all(matches):
        response = transaction(*[arg for _ in range(4) for arg in
                                 ("readl", "0x03000024")])
        # Parse only after restoration so a malformed textual response does not
        # unnecessarily leave helper bytes installed in an otherwise live ROM.
    transaction(*[arg for name, address, _ in REGIONS for arg in
                  ("write", hex(address), evidence / f"{name}-before.bin")])
    transaction(*[arg for name, address, length in REGIONS for arg in
                  ("read", hex(address), length, evidence / f"{name}-after.bin")])
    if any((evidence / f"{name}-before.bin").read_bytes() !=
           (evidence / f"{name}-after.bin").read_bytes() for name, _, _ in REGIONS):
        raise RuntimeError("SRAM restoration differs from its saved contents")
    if not all(matches):
        raise InspectionRefused("ROM differs from the inspected recovery code; input was not sampled")
    try:
        samples = [int(word, 16) for word in response.split()]
    except ValueError as error:
        raise InspectionRefused("Non-numeric boot-input response") from error
    if len(samples) != 4 or any(not 0 <= word <= 0xffffffff for word in samples):
        raise InspectionRefused("Unexpected boot-input response")
    return samples


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fel", type=Path, required=True)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--evidence", type=Path,
                        default=CACHE / "evidence" / time.strftime("fel-input-%Y%m%d-%H%M%S"))
    args = parser.parse_args()
    rom = args.rom.read_bytes()
    for name, data, expected in (("FEL tool", args.fel.read_bytes(), FEL_HASH),
                                 ("ROM", rom, ROM_HASH)):
        if hashlib.sha256(data).hexdigest() != expected:
            parser.error(f"{name} differs from the reviewed input")
    spec = importlib.util.spec_from_file_location("access", ROOT / "tools/usb-access.py")
    access = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(access)
    sys.path.insert(0, str(Path.home() / "git/mimus/tools"))
    import usb_guard
    usb_guard.check_health()
    args.evidence.mkdir(parents=True, exist_ok=False)
    record = dict(tool_sha256=FEL_HASH, rom_sha256=ROM_HASH,
                  runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  transactions=[], complete=False)
    node, identity = None, None
    transport_failed = False
    restored = False
    workspace_started = False

    def save():
        (args.evidence / "session.json").write_text(json.dumps(record, indent=2) + "\n")

    def transaction(*arguments):
        nonlocal transport_failed
        if transport_failed:
            raise RuntimeError("Requests are disabled after a transport failure")
        command = [str(args.fel), "-d", f"{int(node.parent.name)}:{int(node.name)}",
                   *map(str, arguments)]
        entry = {"command": command}
        record["transactions"].append(entry)
        save()
        try:
            if access.identity(node) != identity:
                raise RuntimeError("Device identity changed")
            result = subprocess.run(command, capture_output=True, text=True, timeout=15)
            entry.update(exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr)
            if result.returncode:
                raise RuntimeError(f"FEL request failed: {result.stderr.strip()}")
        except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
            transport_failed = True
            entry["failure"] = str(error)
            save()
            raise
        save()
        return result.stdout

    try:
        with open("/tmp/mimus-board.lock", "a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            node = access.discover()
            access.require_session(node)
            access.require_rom(node)
            identity = access.identity(node)
            record.update(node=str(node), identity=identity, port=access.target(node).name)
            save()
            if "soc=00001903(A733)" not in transaction("version"):
                raise ValueError("Unexpected SoC")
            workspace_started = True
            try:
                samples = inspect_input(transaction, args.evidence, rom)
            except InspectionRefused:
                # Inspection refuses incompatible ROM/text only after restoring.
                restored = True
                raise
            restored = True
            asserted = all((sample & 0x100) == 0 for sample in samples)
            record.update(samples=samples, asserted=asserted, workspace_restored=True,
                          complete=True)
            save()
            print("FEL input " + ("LOW in all four samples." if asserted else
                                   "HIGH in at least one sample; reset protection is absent."))
            print(f"SRAM restored. No reset or watchdog enable occurred.\n{args.evidence}")
            return 0 if asserted else 2
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        record.update(failure=str(error), workspace_restored=restored)
        save()
        if identity is not None and (transport_failed or (workspace_started and not restored)):
            access.quarantine_instance(node, "FEL input inspection stopped; inspect saved state before further access.", identity)
        print(f"Stopped: {error}\nEvidence: {args.evidence}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
