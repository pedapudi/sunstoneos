#!/usr/bin/env python3
"""Run a physical FreeRTOS or forced-stall check and collect the FEL result."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / ".cache/sunstoneos"
def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# One log line of platform_exception() (include/a733_fault.h, a733_fault_line).
FAULT_LINE = re.compile(r"FAIL exception core=([0-7]) ESR=0x([0-9a-f]{16}) ELR=0x([0-9a-f]{16}) "
                        r"FAR=0x([0-9a-f]{16}) SPSR=0x([0-9a-f]{16}) task=([ -~]{0,16})")
OUTCOME_KEYS = {"name", "status", "interrupt", "fail_lines", "fault", "log_contains"}
FAULT_KEYS = {"esr_mask", "esr_value", "far", "elr_low", "elr_high", "task"}
# An outcome may also name a progress record that the image keeps between the
# result header and the log: little-endian magic, stage, count and CNTPCT
# stamp of the last progress, which the forced return preserves.
PROGRESS_KEYS = {"offset", "magic", "stage", "count_below", "age_above"}


def progress_record(data, offset):
    """The four progress words at offset of the result page."""
    magic, stage, count, stamp = struct.unpack_from("<4Q", data, offset)
    return dict(magic=magic, stage=stage, count=count, stamp=stamp)


def fault_lines(log):
    """Every FAIL exception line of a log, parsed; None for a line that does not parse."""
    found = []
    for line in log.splitlines():
        if line.startswith("FAIL exception"):
            match = FAULT_LINE.fullmatch(line)
            found.append(None if match is None else dict(
                core=int(match[1]), esr=int(match[2], 16), elr=int(match[3], 16), far=int(match[4], 16),
                spsr=int(match[5], 16), task=match[6]))
    return found


def controlled_outcomes(raw):
    """The listed controlled outcomes of a bundle; each names a nonzero status and its full expected record."""
    value = json.loads(raw)
    outcomes = value.get("outcomes") if isinstance(value, dict) else None
    if value.get("schema") != "kernel-controlled-outcomes-v1" or not isinstance(outcomes, list) or not outcomes:
        raise ValueError("Controlled outcomes have no outcome list.")
    for outcome in outcomes:
        fault = outcome.get("fault") if isinstance(outcome, dict) else None
        progress = outcome.get("progress") if isinstance(outcome, dict) else None
        if not (isinstance(outcome, dict) and set(outcome) in (OUTCOME_KEYS, OUTCOME_KEYS | {"progress"})
                and ("progress" not in outcome
                     or (isinstance(progress, dict) and set(progress) == PROGRESS_KEYS
                         and all(type(progress[k]) is int and progress[k] >= 0 for k in PROGRESS_KEYS)
                         and 128 <= progress["offset"] and progress["offset"] + 32 <= 256
                         and progress["offset"] % 8 == 0 and progress["count_below"] > 0
                         and progress["age_above"] > 0))
                and isinstance(outcome["name"], str) and outcome["name"]
                and type(outcome["status"]) is int and outcome["status"] != 0
                and (outcome["interrupt"] is None or type(outcome["interrupt"]) is int)
                and type(outcome["fail_lines"]) is int and outcome["fail_lines"] >= 0
                and isinstance(outcome["log_contains"], list) and outcome["log_contains"]
                and all(isinstance(text, str) and text for text in outcome["log_contains"])
                and (fault is None or (isinstance(fault, dict) and set(fault) == FAULT_KEYS
                                       and outcome["fail_lines"] == 1
                                       and all(type(fault[k]) is int for k in FAULT_KEYS - {"task"})
                                       and fault["elr_low"] < fault["elr_high"]
                                       and isinstance(fault["task"], str)))):
            raise ValueError(f"Malformed controlled outcome: {outcome!r}")
    if len({o["name"] for o in outcomes}) != len(outcomes):
        raise ValueError("Controlled outcome names repeat.")
    return outcomes


def controlled_match(outcome, terminal, faults, found, progress=None):
    """True when the result record, the parsed FAIL lines, the log texts found and, for an outcome that names
    one, the progress record equal one outcome. A progress outcome requires the record's magic and stage, a
    count below its bound and a last stamp older than age_above counter ticks at the monitor's end_counter."""
    fault = outcome["fault"]
    spec = outcome.get("progress")
    end = terminal.get("end_counter")
    return (terminal.get("magic") == 0x534b4552 and terminal.get("stage") == 5
            and terminal.get("status") == outcome["status"]
            and (outcome["interrupt"] is None or terminal.get("interrupt") == outcome["interrupt"])
            and isinstance(faults, list) and len(faults) == outcome["fail_lines"]
            and all(isinstance(f, dict) for f in faults)
            and sorted(found) == sorted(outcome["log_contains"])
            and (fault is None or (faults[0]["esr"] & fault["esr_mask"] == fault["esr_value"]
                                   and faults[0]["far"] == fault["far"]
                                   and fault["elr_low"] <= faults[0]["elr"] < fault["elr_high"]
                                   and faults[0]["task"] == fault["task"]))
            and (spec is None or (isinstance(progress, dict) and type(end) is int
                                  and all(type(progress.get(k)) is int for k in ("magic", "stage", "count", "stamp"))
                                  and progress["magic"] == spec["magic"] and progress["stage"] == spec["stage"]
                                  and progress["count"] < spec["count_below"]
                                  and progress["stamp"] <= end and end - progress["stamp"] > spec["age_above"])))


def controlled_return(returned):
    """A kernel-mode session that ended in a listed controlled outcome, as recorded by this runner."""
    controlled = returned.get("controlled")
    if not isinstance(controlled, dict):
        return False
    try:
        outcomes = controlled_outcomes(json.dumps(dict(schema="kernel-controlled-outcomes-v1",
                                                       outcomes=[controlled.get("outcome")])))
    except (ValueError, AttributeError):
        return False
    return (returned.get("mode") == "kernel"
            and controlled_match(outcomes[0], returned.get("result", {}), controlled.get("faults"),
                                 controlled.get("log_found", []), controlled.get("progress")))




def returned_to_rom(record):
    """True when the launched image is known to have ended: the runner found
    the new ROM instance with access, verified it, and read its result page,
    all without a transport failure."""
    return (record.get("launch_attempted") is True and record.get("transport_failed") is False
            and record.get("automatic_access") is True and isinstance(record.get("returned_node"), str)
            and isinstance(record.get("result"), dict))


def sd_power_removal_reason(raw, allow_sd_1v8=False):
    """Recognize the native SD returning input before output or board access."""
    if raw is None or len(raw)<8 or struct.unpack_from('<Q',raw)[0]!=0x314e525445524453:
        return None
    if len(raw)!=2192 or struct.unpack_from('<2Q',raw,8) not in ((1,2192),(2,2192)):
        raise ValueError('Invalid SD returning input header.')
    profile=struct.unpack_from('<I',raw,56)[0]
    if profile not in (2,3):
        raise ValueError('Invalid SD returning input profile.')
    if profile==3:
        if not allow_sd_1v8:
            raise ValueError('SD profile 3 requires --allow-sd-1v8 and whole-board power removal after execution.')
        return 'SD profile 3 may leave the card at 1.8 V'
    return None


def retain_failure(record, save, quarantine, error, helper):
    """Called inside board_lock: a failed transport or launch keeps ownership.

    A launch whose image returned to the ROM and whose result was read has a
    known outcome. Once its returned instance is quarantined, the runner exits
    with that recorded result: the quarantine record refuses further requests
    to the instance. Every other launch,
    or a return whose journal or quarantine failed, holds the port lock until
    coordinator recovery."""
    if record.get("launch_attempted") or record.get("transport_failed") or record.get("dram_probe_dirty"):
        quarantined = []

        def protect():
            quarantine()
            quarantined.append(True)

        def hold():
            if record.get("power_removal_required") or record.get("dram_probe_dirty") or not (quarantined and returned_to_rom(record)):
                helper.hold_ownership()

        try:
            helper.stop_after_failure(record, save, protect, error, hold)
        finally:
            # The shared helper holds launches. Pre-launch transport loss also
            # requires intervention even if journaling or quarantine fails.
            if not record.get("launch_attempted") and not record.get("dram_probe_dirty"):
                helper.hold_ownership()


def main(argv=None, observe=None, validate_result=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fel", type=Path, required=True)
    parser.add_argument("--image", type=Path, default=CACHE / "build/qemu-virt/a733-kernel-check.bin")
    parser.add_argument("--evidence", type=Path,
                        default=CACHE / "evidence" / time.strftime("kernel-%Y%m%d-%H%M%S"))
    parser.add_argument("--mode", choices=("kernel", "timeout"), default="kernel")
    parser.add_argument("--expect", default="PASS kernel:",
                        help="Required success text for a bounded runtime check")
    parser.add_argument("--controlled", type=Path,
                        help="Bundle's controlled outcomes: nonzero results that pass without quarantine")
    parser.add_argument("--ram-input", type=Path, help="Input bundle loaded at 0x43000000; at most 32 MiB")
    parser.add_argument("--allow-sd-1v8", action="store_true",
                        help="Allow SD profile 3; retain ownership until whole-board power removal after execution")
    parser.add_argument("--ram-input-present", action="store_true", help="Verify existing RAM input instead of uploading it")
    parser.add_argument("--return-timeout", type=float, default=10)
    parser.add_argument("--ddr-evidence", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--platform-survey", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--warm-return", type=Path, action="append", default=[],
                        help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    sys.path.insert(0, str(ROOT/'tools'))
    import board
    raise SystemExit(board.legacy_run(args, image=args.image))


if __name__ == "__main__":
    main()
