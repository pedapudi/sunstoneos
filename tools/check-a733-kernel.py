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


def validate_platform_admission(raw, node, identity, port, tool_hash, pinned_tool_hash, warm_raw=()):
    """Validate copied records against the selected live instance under its lock.

    The survey producer pins the FEL executable. Older survey records omit its
    hash, so DDR's recorded hash and the current tool must both match that pin.
    Explicit successful returns connect DDR to a later ROM instance: a
    kernel-mode return, a forced-stall return through the monitor's secure
    timer (status 2, interrupt 29), a kernel-mode return that ended in one of
    its bundle's controlled outcomes, or a returning native CPU diagnostic that
    restored its saved DRAM. Each leaves DRAM powered and trained; none is a
    power cycle. This admits initialized RAM; it does not qualify peripheral
    ownership.
    """
    ddr, survey = (json.loads(value) for value in raw)
    previous_node = ddr.get("device")
    previous_identity = ddr.get("node_identity")
    if not (isinstance(previous_node, str)
            and Path(previous_node).parent.parent == Path("/dev/bus/usb")
            and Path(previous_node).parent.name.isdigit() and Path(previous_node).name.isdigit()
            and isinstance(previous_identity, list) and len(previous_identity) == 5
            and all(type(part) is int for part in previous_identity)):
        raise RuntimeError("DDR evidence does not identify its original USB instance.")
    for value in warm_raw:
        returned = json.loads(value)
        terminal = returned.get("result", {})
        next_node = returned.get("returned_node")
        next_identity = returned.get("returned_identity")
        monitor_return = (returned.get("transport_failed") is False
                and returned.get("automatic_access") is True
                and returned.get("tool_sha256") == tool_hash == pinned_tool_hash
                and terminal.get("magic") == 0x534b4552 and terminal.get("stage") == 5
                and ((returned.get("mode") == "kernel" and terminal.get("status") == 0)
                     or (returned.get("mode") == "timeout" and terminal.get("status") == 2
                         and terminal.get("interrupt") == 29)
                     or controlled_return(returned)))
        # tools/run-native-startup-probe.py records: no mode, restored DRAM.
        native_return = ("mode" not in returned and returned.get("physical_access") is True
                and returned.get("rom_usable") is True and returned.get("dram_restored") is True
                and isinstance(returned.get("execution_profile"), str))
        if not (returned.get("complete") is True
                and returned.get("launch_attempted") is True
                and (monitor_return or native_return)
                and returned.get("port") == port
                and returned.get("initial_node") == previous_node
                and returned.get("initial_identity") == previous_identity
                and isinstance(next_node, str) and next_node.startswith("/dev/bus/usb/")
                and next_node != previous_node
                and isinstance(next_identity, list) and len(next_identity) == 5
                and all(type(part) is int for part in next_identity)
                and next_identity != previous_identity):
            raise RuntimeError("Warm-return evidence does not establish contiguous successful ROM return.")
        previous_node, previous_identity = next_node, next_identity
    result = ddr.get("result", {})
    gates = survey.get("result", {}).get("launcher_gates", {})
    required_gates = {"svc", "little_endian", "mmu_caches_endian_off",
                      "primary_mpidr", "frequency", "counter_monotonic"}
    if not (ddr.get("complete") is True and result.get("magic") == 0x53524444
            and result.get("stage") == 5 and result.get("board_result") == 0
            and isinstance(result.get("dram_mib"), int) and result["dram_mib"] > 0
            and previous_identity == identity and previous_node == str(node)
            and ddr.get("tool_sha256") == tool_hash == pinned_tool_hash
            and survey.get("complete") is True and survey.get("physical_access") is True
            and survey.get("rom_usable") is True and survey.get("sram_restored") is True
            and survey.get("identity") == identity and survey.get("node") == str(node)
            and survey.get("port") == port
            and required_gates <= gates.keys()
            and all(value is True for value in gates.values())
            and survey.get("tool_sha256", pinned_tool_hash) == pinned_tool_hash):
        raise RuntimeError("DDR/survey evidence does not admit the selected live board instance.")


def returned_to_rom(record):
    """True when the launched image is known to have ended: the runner found
    the new ROM instance with access, verified it, and read its result page,
    all without a transport failure."""
    return (record.get("launch_attempted") is True and record.get("transport_failed") is False
            and record.get("automatic_access") is True and isinstance(record.get("returned_node"), str)
            and isinstance(record.get("result"), dict))


def retain_failure(record, save, quarantine, error, helper):
    """Called inside board_lock: a failed transport or launch keeps ownership.

    A launch whose image returned to the ROM and whose result was read has a
    known outcome. Once its returned instance is quarantined, the runner exits
    with that recorded result: the quarantine record refuses further requests
    to the instance and the caller keeps the queue claim. Every other launch,
    or a return whose journal or quarantine failed, holds the port lock until
    coordinator recovery."""
    if record.get("launch_attempted") or record.get("transport_failed"):
        quarantined = []

        def protect():
            quarantine()
            quarantined.append(True)

        def hold():
            if not (quarantined and returned_to_rom(record)):
                helper.hold_ownership()

        try:
            helper.stop_after_failure(record, save, protect, error, hold)
        finally:
            # The shared helper holds launches. Pre-launch transport loss also
            # requires intervention even if journaling or quarantine fails.
            if not record.get("launch_attempted"):
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
    parser.add_argument("--ram-input-present", action="store_true", help="Verify existing RAM input instead of uploading it")
    parser.add_argument("--return-timeout", type=float, default=10)
    parser.add_argument("--ddr-evidence", type=Path, help="Successful DDR session for this board, before any supplied warm returns")
    parser.add_argument("--platform-survey", type=Path, help="Returning A32 survey for the current instance; paired with --ddr-evidence")
    parser.add_argument("--warm-return", type=Path, action="append", default=[],
                        help="Successful kernel, forced-stall or native returning session connecting DDR to the current instance; repeat in execution order")
    args = parser.parse_args(argv)
    if bool(args.ddr_evidence) != bool(args.platform_survey):
        parser.error("--ddr-evidence and --platform-survey must be supplied together.")
    if args.warm_return and not args.ddr_evidence:
        parser.error("--warm-return requires --ddr-evidence and --platform-survey.")
    warm_returns = tuple(path.read_bytes() for path in args.warm_return)
    admission = None
    if args.ddr_evidence:
        admission = (args.ddr_evidence.read_bytes(), args.platform_survey.read_bytes())
    controlled = None
    if args.controlled:
        if args.mode != "kernel":
            parser.error("--controlled applies to kernel mode only.")
        controlled_raw = args.controlled.read_bytes()
        try:
            controlled = controlled_outcomes(controlled_raw)
        except ValueError as error:
            sys.exit(str(error))
    if not 5 <= args.return_timeout <= 600:
        sys.exit("Return timeout must be between 5 and 600 seconds.")
    if args.ram_input_present and not args.ram_input:
        sys.exit("--ram-input-present requires --ram-input.")
    ram_input = args.ram_input.read_bytes() if args.ram_input else None
    if ram_input is not None and not 0 < len(ram_input) <= 32 * 1024 * 1024:
        sys.exit("RAM input exceeds its reserved interval.")
    image = args.image.read_bytes()
    if not image or len(image) > 0x100000:
        sys.exit("Kernel image exceeds the reserved image interval.")
    args.evidence.mkdir(parents=True, exist_ok=False)
    record = dict(image_sha256=hashlib.sha256(image).hexdigest(),
                  tool_sha256=hashlib.sha256(args.fel.read_bytes()).hexdigest(),
                  mode=args.mode, expected_text=args.expect, transactions=[], complete=False,
                  launch_attempted=False, transport_failed=False)
    if controlled is not None:
        record["controlled_sha256"] = hashlib.sha256(controlled_raw).hexdigest()
    access = load_module("access", ROOT / "tools/usb-access.py")
    sys.path.insert(0, str(Path.home() / "git/mimus/tools"))
    import usb_guard
    failure_helper = load_module("kernel_failure", ROOT / "tools/check-a733-smp.py")
    if admission:
        (args.evidence / "ddr-admission.json").write_bytes(admission[0])
        (args.evidence / "platform-survey.json").write_bytes(admission[1])
        record["admission"] = {"ddr_sha256": hashlib.sha256(admission[0]).hexdigest(),
                               "survey_sha256": hashlib.sha256(admission[1]).hexdigest()}
        if warm_returns:
            record["admission"]["warm_returns"] = []
            for index, raw in enumerate(warm_returns):
                name = f"warm-return-{index + 1}.json"
                (args.evidence / name).write_bytes(raw)
                record["admission"]["warm_returns"].append(
                    {"file": name, "sha256": hashlib.sha256(raw).hexdigest()})


    def save():
        (args.evidence / "session.json").write_text(json.dumps(record, indent=2) + "\n")


    def transaction(*arguments, timeout=30):
        if record["transport_failed"]:
            raise RuntimeError("Transport already failed; no further device requests.")
        try:
            if access.identity(node) != node_identity:
                raise RuntimeError("Device identity changed before a protocol request.")
        except (OSError, RuntimeError):
            record["transport_failed"] = True
            raise
        command = [str(args.fel), "-d", f"{int(node.parent.name)}:{int(node.name)}",
                   *map(str, arguments)]
        item = dict(command=command)
        record["transactions"].append(item)
        save()
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
        except BaseException as error:
            record["transport_failed"] = True
            item["failure"] = "FEL transport failed; no further device requests."
            save()
            raise RuntimeError(item["failure"]) from error
        item.update(exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr)
        if result.returncode:
            record["transport_failed"] = True
        save()
        if result.returncode:
            raise RuntimeError(f"FEL request failed; stopped: {result.stdout}{result.stderr}")
        return result.stdout


    try:
        usb_guard.check_health()
        with access.board_lock() as node:
            try:
                access.require_session(node)
                access.require_rom(node)
                node_identity = access.identity(node)
                port = access.target(node)
                config = dict(port=port.name, sysfs=str(port.resolve()))
                record.update(initial_node=str(node), initial_identity=node_identity, port=port.name)
                if admission:
                    validate_platform_admission(admission, node, node_identity, port.name, record["tool_sha256"], failure_helper.FEL_HASH, warm_returns)
                if "soc=00001903(A733)" not in transaction("version"):
                    raise RuntimeError("Target is not the verified A733.")
                # The forced-FEL reset entry must still match the inspected ROM words.
                words = transaction("readl", "0x20", "readl", "0x24", "readl", "0x28",
                                    "readl", "0x3c", "readl", "0x40")
                if [int(word, 16) for word in words.split()] != [
                        0xeb00008f, 0xe3a00911, 0xe3a0193d, 0xe3a0605c, 0xea00004d]:
                    raise RuntimeError("Forced-FEL ROM entry differs from the reviewed instructions.")
                transaction("read", "0x42000000", "128", args.evidence / "before.bin")
                signature = struct.unpack_from("<I", (args.evidence / "before.bin").read_bytes())[0]
                if not admission and signature not in (0x41363450, 0x454c3150, 0x534b4552):
                    raise RuntimeError("No prior AArch64 probe signature; establish initialized DDR first.")
                if ram_input is not None:
                    record["ram_input_sha256"] = hashlib.sha256(ram_input).hexdigest()
                    record["ram_input_reused"] = args.ram_input_present
                    arguments = [] if args.ram_input_present else ["write", "0x43000000", args.ram_input]
                    transaction(*arguments, "read", "0x43000000", len(ram_input),
                                args.evidence / "input-readback.bin", timeout=120)
                    if (args.evidence / "input-readback.bin").read_bytes() != ram_input:
                        raise RuntimeError("RAM input differs; execution refused.")
                blank = bytearray(16384)
                struct.pack_into("<Q", blank, 80, args.mode == "timeout")
                (args.evidence / "blank.bin").write_bytes(blank)
                transaction("write", "0x41000000", args.image,
                            "read", "0x41000000", len(image), args.evidence / "uploaded.bin")
                if (args.evidence / "uploaded.bin").read_bytes() != image:
                    raise RuntimeError("Kernel upload differs; execution refused.")
                with usb_guard.reenumeration("sunstoneos-a733", "FreeRTOS check and forced-FEL return", 2):
                    transaction("write", "0x42000000", args.evidence / "blank.bin")
                    record["launch_attempted"] = True
                    save()
                    if observe is not None:
                        record["observation_start_monotonic"] = time.monotonic()
                        save()
                    transaction("reset64", "0x41000000")
                    deadline = (record["observation_start_monotonic"] if observe is not None
                                else time.monotonic()) + args.return_timeout
                    if observe is not None:
                        # Runs under this same port lock and total return deadline.
                        # Any transport exception enters the existing retained-failure path.
                        observe(access, config, node_identity, deadline, record, save)
                    # Poll only host device metadata. Do not send protocol requests until
                    # the new instance exists and udev has granted access.
                    while True:
                        try:
                            candidate = access.session_node(config)
                            if (candidate is not None and access.identity(candidate) != node_identity
                                    and os.access(candidate, os.R_OK | os.W_OK)):
                                if observe is not None:
                                    # A native USB instance may still be detaching.
                                    # Inspect metadata before accepting the ROM instance.
                                    if (access.target(candidate) / "bcdDevice").read_text().strip() != "02b3":
                                        candidate = None
                                    if candidate is None:
                                        if time.monotonic() >= deadline:
                                            raise RuntimeError("ROM did not return before the total deadline.")
                                        time.sleep(0.01)
                                        continue
                                break
                        except FileNotFoundError:
                            pass
                        if time.monotonic() >= deadline:
                            raise RuntimeError("FEL did not return with access before the deadline; no reset retry.")
                        time.sleep(0.1)
                    node = candidate
                    node_identity = access.identity(node)
                    access.require_session(node)
                    access.require_rom(node)
                    record.update(returned_node=str(node), returned_identity=node_identity,
                                  automatic_access=True)
                    save()
                if "soc=00001903(A733)" not in transaction("version"):
                    raise RuntimeError("Returned device is not A733.")
                transaction("read", "0x42000000", "16384", args.evidence / "result.bin")
                data = (args.evidence / "result.bin").read_bytes()
                values = struct.unpack_from("<16Q", data)
                names = ("magic", "stage", "status", "log_length", "esr", "elr", "far", "interrupt",
                         "start_counter", "end_counter", "mode", "gicd_ctlr", "gicd_typer",
                         "gicr_typer", "priority_mask", "replaced_status")
                record["result"] = dict(zip(names, values))
                if values[3] > 16384 - 257:
                    raise RuntimeError("Invalid log length in result.")
                log = data[256:256 + values[3]].decode(errors="replace")
                (args.evidence / "kernel.log").write_text(log)
                print(log, end="")
                save()
                expected_status = 2 if args.mode == "timeout" else 0
                outcome = None
                if controlled is not None and values[2] != expected_status:
                    # Matched on the full record; any other nonzero status
                    # takes the retained-failure path below.
                    faults = fault_lines(log)
                    for item in controlled:
                        found = [text for text in item["log_contains"] if text in log]
                        progress = progress_record(data, item["progress"]["offset"]) if "progress" in item else None
                        if controlled_match(item, record["result"], faults, found, progress):
                            outcome = item
                            record["controlled"] = dict(outcome=item, faults=faults, log_found=found)
                            if progress is not None:
                                record["controlled"]["progress"] = progress
                            save()
                            break
                if outcome is None and values[:3] == (0x534b4552, 5, 6):
                    raise RuntimeError(f"Monitor interrupt cleanup failed (status 6) after status "
                                       f"{values[15]}: {record['result']}")
                if outcome is None and values[:3] != (0x534b4552, 5, expected_status):
                    raise RuntimeError(f"Unexpected kernel result: {record['result']}")
                if outcome is None and args.mode == "kernel" and args.expect not in log:
                    raise RuntimeError("Kernel did not report successful completion.")
                if args.mode == "timeout" and (values[7] != 29 or "CHECK recovery:" not in log):
                    raise RuntimeError("Recovery was not the expected secure timer interrupt.")
                # A validator returns serializable evidence or raises ValueError.
                # Rejection precedes success and uses the existing failure policy.
                if validate_result is not None:
                    try:
                        record["application_result"] = validate_result(record["result"], log)
                    except ValueError as error:
                        record["application_result"] = {"accepted": False, "failure": str(error)}
                        save()
                        raise RuntimeError(f"Application result rejected: {error}") from error
                    save()
                record["complete"] = True
                save()
                if outcome is not None:
                    print(f"CONTROLLED: {outcome['name']}, status {outcome['status']}, FEL recovery, "
                          f"automatic USB access.\n{args.evidence}")
                else:
                    print(f"PASS: {args.mode} check, FEL recovery, automatic USB access.\n{args.evidence}")
            except BaseException as error:
                retain_failure(record, save, lambda: access.quarantine_instance(
                    node, "Kernel runner stopped; coordinator recovery required.", node_identity),
                    error, failure_helper)
                raise

    except (OSError, ValueError, RuntimeError) as error:
        record["failure"] = str(error)
        save()
        sys.exit(f"Stopped: {error}\n{args.evidence}")


if __name__ == "__main__":
    main()
