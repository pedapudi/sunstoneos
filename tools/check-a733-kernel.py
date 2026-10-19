#!/usr/bin/env python3
"""Run a physical FreeRTOS or forced-stall check and collect the FEL result."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
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


def validate_platform_admission(raw, node, identity, port, tool_hash, pinned_tool_hash, warm_raw=()):
    """Validate copied records against the selected live instance under its lock.

    The survey producer pins the FEL executable. Older survey records omit its
    hash, so DDR's recorded hash and the current tool must both match that pin.
    Explicit successful kernel returns connect DDR to a later ROM instance.
    This admits initialized RAM; it does not qualify peripheral ownership.
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
        if not (returned.get("complete") is True
                and returned.get("launch_attempted") is True
                and returned.get("transport_failed") is False
                and returned.get("automatic_access") is True
                and returned.get("mode") == "kernel"
                and returned.get("port") == port
                and returned.get("tool_sha256") == tool_hash == pinned_tool_hash
                and returned.get("initial_node") == previous_node
                and returned.get("initial_identity") == previous_identity
                and isinstance(next_node, str) and next_node.startswith("/dev/bus/usb/")
                and next_node != previous_node
                and isinstance(next_identity, list) and len(next_identity) == 5
                and all(type(part) is int for part in next_identity)
                and next_identity != previous_identity
                and terminal.get("magic") == 0x534b4552
                and terminal.get("stage") == 5 and terminal.get("status") == 0):
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


def retain_failure(record, save, quarantine, error, helper):
    """Called inside board_lock: a failed transport or launch keeps ownership."""
    if record.get("launch_attempted") or record.get("transport_failed"):
        try:
            helper.stop_after_failure(record, save, quarantine, error)
        finally:
            # The shared helper holds launches. Pre-launch transport loss also
            # requires intervention even if journaling or quarantine fails.
            if not record.get("launch_attempted"):
                helper.hold_ownership()


def main(argv=None, observe=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fel", type=Path, required=True)
    parser.add_argument("--image", type=Path, default=CACHE / "build/qemu-virt/a733-kernel-check.bin")
    parser.add_argument("--evidence", type=Path,
                        default=CACHE / "evidence" / time.strftime("kernel-%Y%m%d-%H%M%S"))
    parser.add_argument("--mode", choices=("kernel", "timeout"), default="kernel")
    parser.add_argument("--expect", default="PASS kernel:",
                        help="Required success text for a bounded runtime check")
    parser.add_argument("--ram-input", type=Path, help="Input bundle loaded at 0x43000000; at most 32 MiB")
    parser.add_argument("--ram-input-present", action="store_true", help="Verify existing RAM input instead of uploading it")
    parser.add_argument("--return-timeout", type=float, default=10)
    parser.add_argument("--ddr-evidence", type=Path, help="Successful DDR session for this board, before any supplied warm returns")
    parser.add_argument("--platform-survey", type=Path, help="Returning A32 survey for the current instance; paired with --ddr-evidence")
    parser.add_argument("--warm-return", type=Path, action="append", default=[],
                        help="Successful kernel session connecting DDR to the current instance; repeat in execution order")
    args = parser.parse_args(argv)
    if bool(args.ddr_evidence) != bool(args.platform_survey):
        parser.error("--ddr-evidence and --platform-survey must be supplied together.")
    if args.warm_return and not args.ddr_evidence:
        parser.error("--warm-return requires --ddr-evidence and --platform-survey.")
    warm_returns = tuple(path.read_bytes() for path in args.warm_return)
    admission = None
    if args.ddr_evidence:
        admission = (args.ddr_evidence.read_bytes(), args.platform_survey.read_bytes())
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
                         "gicr_typer", "priority_mask", "reserved")
                record["result"] = dict(zip(names, values))
                if values[3] > 16384 - 257:
                    raise RuntimeError("Invalid log length in result.")
                log = data[256:256 + values[3]].decode(errors="replace")
                (args.evidence / "kernel.log").write_text(log)
                print(log, end="")
                save()
                expected_status = 2 if args.mode == "timeout" else 0
                if values[:3] != (0x534b4552, 5, expected_status):
                    raise RuntimeError(f"Unexpected kernel result: {record['result']}")
                if args.mode == "kernel" and args.expect not in log:
                    raise RuntimeError("Kernel did not report successful completion.")
                if args.mode == "timeout" and (values[7] != 29 or "CHECK recovery:" not in log):
                    raise RuntimeError("Recovery was not the expected secure timer interrupt.")
                record["complete"] = True
                save()
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
