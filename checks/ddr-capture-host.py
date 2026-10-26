#!/usr/bin/env python3
"""Run the DRAM capture runner against a modeled FEL device and lock.

tools/run-ddr-capture.py runs unchanged; the FEL executable, USB access, the
health guard and the ownership hold are replaced. The model checks the
command order: the DDR mailbox binding before any execution, the cache-off
helper, the reader upload with readback, then for each stage the table
upload, one execution and the result read. Cases: nominal, incomplete DDR
evidence (refused before device access), a mailbox from another lifetime
(refused before any execution) and a stage that does not complete (stops
with quarantine, later stages not run).
"""
import argparse
from contextlib import contextmanager
import importlib.abc
import importlib.util
import json
from pathlib import Path
import runpy
import struct
import subprocess
import sys
import types
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "tools/run-ddr-capture.py"
MAILBOX = struct.pack("<7I", 0x53524444, 5, 12288, 0, 0x600001d3, 0x00c50838, 600) + bytes(100)


class Held(Exception):
    pass


class Loader(importlib.abc.Loader):
    def __init__(self, fill):
        self.fill = fill

    def create_module(self, spec):
        return None

    def exec_module(self, module):
        self.fill(module)


def run_case(case, build, out, extra=()):
    out.mkdir()
    ddr = out / "ddr"
    ddr.mkdir()
    complete = case != "ddr-incomplete"
    untrained = case.startswith("untrained")
    (ddr / "session.json").write_text(json.dumps({"complete": complete, "untrained": untrained,
        "manifest": {"variant": "init-only"} if untrained else {}, "result": {
        "magic": 0x53524444, "stage": 5, "board_result": 0, "dram_mib": 0 if untrained else 12288}}))
    (ddr / "mailbox.bin").write_bytes(MAILBOX)
    memory = {}
    manifest = json.loads((build / "manifest.json").read_text())

    def put(address, data):
        for offset, byte in enumerate(data):
            memory[address + offset] = byte

    def get(address, size):
        return bytes(memory.get(address + offset, 0) for offset in range(size))

    put(0x86000, MAILBOX if case != "other-lifetime" else MAILBOX[:4] + b"\x06" + MAILBOX[5:])
    put(0x82df8 + 0x400, struct.pack("<32I", *range(0x1000, 0x1020)))
    log, events = [], []

    def execute(address):
        if address == 0x71000:
            return
        assert address == 0x85000 and get(0x85000, 4) == (build / "ddr-capture.bin").read_bytes()[:4]
        count, output = struct.unpack("<II", get(0x43000008, 8))
        stage = next(s for s in manifest["stages"] if s["output"] == output)
        put(output, bytes(stage["output_bytes"]))
        if case == "stage-refused" and stage["name"] == "phy-blocks":
            put(0x43000010, struct.pack("<I", 2))
            return
        put(0x43000010, struct.pack("<4I", count - 1, 1, 2, 0x454e4f44))

    def fake_run(command, capture_output, text, timeout):
        assert command[1] == "-d" and capture_output and text and timeout == 30
        words, stdout = command[3:], ""
        position = 0
        while position < len(words):
            verb = words[position]
            if verb == "version":
                stdout += "AWUSBFEX soc=00001903(A733) 00000001 ver=0001 44 08 scratchpad=000ee800\n"
                position += 1
            elif verb == "read":
                address, size, path = int(words[position + 1], 16), int(words[position + 2]), Path(words[position + 3])
                path.write_bytes(get(address, size))
                log.append(("read", address))
                position += 4
            elif verb == "write":
                address, path = int(words[position + 1], 16), Path(words[position + 2])
                put(address, path.read_bytes())
                log.append(("write", address))
                position += 3
            elif verb == "exe":
                address = int(words[position + 1], 16)
                log.append(("exe", address))
                execute(address)
                position += 2
            else:
                raise AssertionError(verb)
        return subprocess.CompletedProcess(command, 0, stdout, "")

    node = Path("/fixture-usb/003/072")

    def fill_access(module):
        @contextmanager
        def board_lock():
            events.append("lock")
            yield node
        module.board_lock = board_lock
        module.require_rom = lambda device: None
        module.identity = lambda device: [7, 1, 2, 0, 0]
        module.quarantine_instance = lambda *unused: events.append("quarantine")

    def fill_base(module):
        def hold():
            raise Held()

        def stop_after_failure(record, save, quarantine, error, hold=hold):
            try:
                record["failure"] = str(error)
                save()
                quarantine()
            finally:
                if record["launch_attempted"]:
                    hold()
        module.hold_ownership = hold
        module.stop_after_failure = stop_after_failure

    real_spec = importlib.util.spec_from_file_location

    def spec(name, path, *rest, **keywords):
        fills = {"usb-access.py": fill_access, "check-a733-smp.py": fill_base}
        if Path(path).name in fills:
            return importlib.util.spec_from_loader(name, Loader(fills[Path(path).name]))
        return real_spec(name, path, *rest, **keywords)

    guard = types.ModuleType("usb_guard")
    guard.check_health = lambda: None
    evidence = out / "capture"
    argv = [str(RUNNER), "--fel", str(build / "manifest.json"), "--build", str(build),
            "--ddr-evidence", str(ddr), "--evidence", str(evidence), "--temperature-note", "fixture", *extra]
    outcome = "returned"
    with patch.object(sys, "argv", argv), patch.dict(sys.modules, {"usb_guard": guard}), \
            patch.dict("os.environ", {"SUNSTONE_USB_PORT": "3-3"}), \
            patch("importlib.util.spec_from_file_location", spec), patch("subprocess.run", fake_run):
        try:
            runpy.run_path(str(RUNNER), run_name="__main__")
        except SystemExit as error:
            outcome = "exit" if error.code else "returned"
        except Held:
            outcome = "held"
    record = json.loads((evidence / "session.json").read_text()) if evidence.exists() else None
    return outcome, record, log, events


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    args.evidence.mkdir(parents=True, exist_ok=False)
    stages = json.loads((args.build / "manifest.json").read_text())["stages"]

    outcome, record, log, events = run_case("nominal", args.build, args.evidence / "nominal")
    assert outcome == "returned" and record["complete"] and not events.count("quarantine")
    exes = [entry for entry in log if entry[0] == "exe"]
    assert exes == [("exe", 0x71000)] + [("exe", 0x85000)] * len(stages)
    first_exe = log.index(("exe", 0x71000))
    assert ("read", 0x86000) in log[:first_exe] and ("read", 0x82df8) in log[:first_exe]
    assert log[first_exe + 1:first_exe + 3] == [("write", 0x85000), ("read", 0x85000)]
    assert [s["name"] for s in record["stages"]] == [s["name"] for s in stages]
    assert all(s["done"] for s in record["stages"])
    assert record["parameter_block"][:2] == ["0x00001000", "0x00001001"]
    assert record["port"] == "3-3" and record["temperature_note"] == "fixture"

    outcome, record, log, events = run_case("nominal", args.build, args.evidence / "first-stages", ["--stages", "2"])
    assert outcome == "returned" and record["complete"]
    assert [s["name"] for s in record["stages"]] == [s["name"] for s in stages[:2]]
    assert [entry for entry in log if entry[0] == "exe"] == [("exe", 0x71000)] + [("exe", 0x85000)] * 2

    outcome, record, log, events = run_case("untrained", args.build, args.evidence / "untrained",
                                            ["--allow-untrained"])
    assert outcome == "returned" and record["complete"] and record["ddr_variant"] == "init-only"
    outcome, record, log, events = run_case("untrained-refused", args.build, args.evidence / "untrained-refused")
    assert outcome == "exit" and record is None and not log

    outcome, record, log, events = run_case("ddr-incomplete", args.build, args.evidence / "ddr-incomplete")
    assert outcome == "exit" and record is None and not log

    outcome, record, log, events = run_case("other-lifetime", args.build, args.evidence / "other-lifetime")
    assert outcome == "held" and "quarantine" in events and not record["complete"]
    assert not [entry for entry in log if entry[0] in ("exe", "write")]

    outcome, record, log, events = run_case("stage-refused", args.build, args.evidence / "stage-refused")
    assert outcome == "held" and "quarantine" in events and not record["complete"]
    refused = [s["name"] for s in stages].index("phy-blocks") + 1
    assert [s["name"] for s in record["stages"]] == [s["name"] for s in stages[:refused]]
    assert not record["stages"][-1]["done"]
    print("PASS: nominal capture order; a stage limit; an init-only DDR return only with --allow-untrained; incomplete DDR evidence, another lifetime's mailbox "
          "and an incomplete stage stop as required")


if __name__ == "__main__":
    main()
