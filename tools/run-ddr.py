#!/usr/bin/env python3
"""Load the reviewed A733 DDR wrapper, return to FEL, and check two RAM pages.

A restore build (tools/prepare-ddr.py --variant restore) runs under WDT0. The
runner compares the board's SID with the record before any launch, arms WDT0
in its four-second mode immediately before the wrapper and disarms it in the
same FEL request once the wrapper returns. A restore that reports an error,
fails the RAM pages or does not return ends in one WDT0 reset to a new FEL
instance and one full training with --fallback-build; a SID mismatch trains
fully without a reset. Exit status 0: DRAM ready through the requested build.
Exit status 3: DRAM ready through the fallback full training.
"""
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
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--fel", type=Path, required=True)
parser.add_argument("--build", type=Path, default=CACHE / "build/a733-ddr")
parser.add_argument("--evidence", type=Path,
                    default=CACHE / "evidence" / time.strftime("ddr-%Y%m%d-%H%M%S"))
parser.add_argument("--allow-untrained", action="store_true",
                    help="init-only capture variant: accept a wrapper return without trained DRAM")
parser.add_argument("--record", type=Path, help="training record for the restore variant")
parser.add_argument("--fallback-build", type=Path,
                    help="plain full-training build that a failed restore falls back to")
args = parser.parse_args()


def verified(build):
    manifest = json.loads((build / "manifest.json").read_text())
    for name, expected in manifest["files"].items():
        content = (build / name).read_bytes()
        if len(content) != expected["size"] or hashlib.sha256(content).hexdigest() != expected["sha256"]:
            sys.exit(f"Payload mismatch: {name}")
    return manifest


manifest = json.loads((args.build / "manifest.json").read_text())
if args.allow_untrained != (manifest.get("variant") == "init-only"):
    sys.exit("--allow-untrained goes with, and only with, an init-only variant build.")
restore = manifest.get("variant") == "restore"
if (args.record is not None) != restore or (args.fallback_build is not None) != restore:
    sys.exit("--record and --fallback-build go with, and only with, a restore variant build.")
training_bytes = chip_id = fallback = None
if restore:
    training_bytes = args.record.read_bytes()
    if not 128 <= len(training_bytes) <= 0x6000:
        sys.exit("Training record exceeds its SRAM reservation or lacks a header.")
    if len(training_bytes) != 128 + struct.unpack_from("<I", training_bytes, 16)[0]:
        sys.exit("Training record length differs from its header.")
    spec = importlib.util.spec_from_file_location("training_record", ROOT / "tools/training-record.py")
    training = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(training)
    try:
        chip_id = training.parse(training_bytes)["chip_id"]
    except (ValueError, struct.error) as error:
        sys.exit(f"Invalid training record: {error}")
manifest = verified(args.build)
if restore:
    if not 0 < manifest["files"]["ddr-restore.bin"]["size"] <= 0x8000:
        sys.exit("Restore helper exceeds its SRAM reservation.")
    fallback = verified(args.fallback_build)
    if fallback.get("variant") is not None or fallback.get("boot0_sha256") != manifest.get("boot0_sha256"):
        sys.exit("--fallback-build must be the plain full-training wrapper for the same boot0.")
args.evidence.mkdir(parents=True, exist_ok=False)
record = {"manifest": manifest, "tool_sha256": hashlib.sha256(args.fel.read_bytes()).hexdigest(),
          "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
          "transactions": [], "complete": False, "launch_attempted": False,
          "device_access_attempted": False}
if restore:
    record.update(training_record_sha256=hashlib.sha256(training_bytes).hexdigest(),
                  fallback_manifest=fallback)
    (args.evidence / "training-record.bin").write_bytes(training_bytes)
    (args.evidence / "training-record-upload.bin").write_bytes(training_bytes + bytes(0x6000-len(training_bytes)))

# WDT0 at 0x02050000 in the order measured on both boards on 2026-10-31
# (evidence/wdt-fel-return-20261031): reset output, system reset, mode 0x41
# (enabled, four seconds), restart. The board left USB about 4.2 s after
# arming and a new FEL instance enumerated at about 5.9 s. The mode survives
# the reset, so an enabled mode on the new instance means WDT0 fired.
WDT_RESTART, WDT_CONFIG, WDT_MODE, WDT_OUTPUT, WDT_KEY = 0x0205000c, 0x02050010, 0x02050014, 0x02050018, 0x16aa0000
ARM = ("writel", hex(WDT_OUTPUT), hex(WDT_KEY | 2), "writel", hex(WDT_CONFIG), hex(WDT_KEY | 1),
       "writel", hex(WDT_MODE), hex(WDT_KEY | 0x41), "writel", hex(WDT_RESTART), "0x14af",
       "readl", hex(WDT_MODE), "readl", hex(WDT_CONFIG), "readl", hex(WDT_OUTPUT))
DISARM = ("writel", hex(WDT_MODE), hex(WDT_KEY), "readl", hex(WDT_MODE))
RETURN_WINDOW = 20      # seconds from arming to an accessible new FEL instance
EXE_TIMEOUT = 12        # longer than the four-second interval plus the USB drop
MAGIC = 0x53524444      # "DDRS"


def save():
    (args.evidence / "session.json").write_text(json.dumps(record, indent=2) + "\n")


spec = importlib.util.spec_from_file_location("usb_access", ROOT / "tools/usb-access.py")
access = importlib.util.module_from_spec(spec)
spec.loader.exec_module(access)
spec = importlib.util.spec_from_file_location("ddr_base", ROOT / "tools/check-a733-smp.py")
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)
sys.path.insert(0, str(ROOT / "tools"))
import usb_guard


def transaction(*commands, timeout=30):
    if access.identity(device) != identity:
        raise RuntimeError("Device node changed; stopped.")
    command = [str(args.fel), "-d", f"{int(device.parent.name)}:{int(device.name)}", *map(str, commands)]
    entry = {"command": command}
    record["transactions"].append(entry)
    save()
    try:
        record["device_access_attempted"] = True
        record["launch_attempted"] |= "exe" in commands
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        entry["failure"] = "Host timeout; no further device access."
        save()
        raise RuntimeError(entry["failure"]) from error
    entry.update(exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr)
    save()
    if result.returncode:
        raise RuntimeError(f"FEL command failed; stopped: {result.stdout}{result.stderr}")
    return result.stdout


def words(stdout):
    return [int(value, 16) for value in stdout.split()]


def upload(build, manifest, evidence, with_restore):
    # Save all SRAM regions this attempt uses. The live FEL interval is
    # restored by the wrapper while interrupts are disabled, not by USB.
    ranges = [("fel", 0x47000, 0x3000), ("body", 0x4a000, 0x39c28),
              ("wrapper", 0x85000, 0x1100), ("stack", 0x9b000 if with_restore else 0x89000, 0x4000),
              ("shadow", 0x8e000, 0x3000), ("log", 0xe0000, 0x4000)]
    if with_restore:
        ranges.extend([("record", 0x87000, 0x6000), ("restore", 0x92000, 0x8000)])
    commands = []
    for name, address, size in ranges:
        commands.extend(["read", hex(address), str(size), evidence / f"{name}-before.bin"])
    transaction(*commands)
    transaction("write", "0x71000", build / "cache-off.bin", "exe", "0x71000")
    transaction("write", "0x8e000", build / "boot0-head.bin",
                "write", "0x4a000", build / "boot0-body.bin",
                "write", "0x85000", build / "ddr-entry.bin")
    if with_restore:
        transaction("write", "0x87000", evidence / "training-record-upload.bin",
                    "write", "0x92000", build / "ddr-restore.bin")
        transaction("read", "0x87000", "24576", evidence / "record-upload.bin",
                    "read", "0x92000", str(manifest["files"]["ddr-restore.bin"]["size"]),
                    evidence / "restore-upload.bin")
        for actual, expected in ((evidence / "record-upload.bin", evidence / "training-record-upload.bin"),
                                 (evidence / "restore-upload.bin", build / "ddr-restore.bin")):
            if actual.read_bytes() != expected.read_bytes():
                raise RuntimeError("Restore upload comparison failed: " + actual.name)
    # A bounded upload readback establishes the complete payload before
    # invoking power/clock setup. No firmware runs during this comparison.
    transaction("read", "0x8e000", "12288", evidence / "head-upload.bin",
                "read", "0x4a000", "233472", evidence / "body-upload.bin",
                "read", "0x85000", str(manifest["files"]["ddr-entry.bin"]["size"]),
                evidence / "entry-upload.bin")
    for short, source in (("head", "boot0-head.bin"), ("body", "boot0-body.bin"),
                          ("entry", "ddr-entry.bin")):
        if (evidence / f"{short}-upload.bin").read_bytes() != (build / source).read_bytes():
            raise RuntimeError(f"Upload comparison failed: {short}")


def collect(evidence, outcome):
    transaction("read", "0x86000", "128", evidence / "mailbox.bin",
                "read", "0xe0000", "16384", evidence / "log.bin",
                "read", "0x831f8", "128", evidence / "dram-parameters.bin")
    mailbox = (evidence / "mailbox.bin").read_bytes()
    values = struct.unpack("<7I", mailbox[:28])
    outcome["result"] = dict(zip(("magic", "stage", "dram_mib", "board_result",
                                  "entry_cpsr", "entry_sctlr", "log_length"), values))
    log = (evidence / "log.bin").read_bytes()[:min(values[6], 16384)].decode(errors="replace")
    (evidence / "firmware.log").write_text(log)
    print(log, end="", flush=True)
    save()
    return mailbox, values


def trained(values):
    return values[0] == MAGIC and values[1] == 5 and values[3] == 0 and values[2] >= 64


def ram_pages(evidence):
    pages = [(0x41000000, 0xa55a5aa5), (0x42000000, 0x73307330)]
    commands = []
    for address, seed in pages:
        payload = struct.pack("<1024I", *((seed ^ address ^ (i * 0x10201)) & 0xffffffff for i in range(1024)))
        path = evidence / f"ram-{address:x}.bin"
        path.write_bytes(payload)
        commands.extend(["write", hex(address), path])
    for address, _ in pages:
        commands.extend(["read", hex(address), "4096", evidence / f"ram-{address:x}-read.bin"])
    transaction(*commands)
    for address, _ in pages:
        if (evidence / f"ram-{address:x}.bin").read_bytes() != (evidence / f"ram-{address:x}-read.bin").read_bytes():
            return address
    return None


def full_training(build, manifest, evidence, outcome):
    upload(build, manifest, evidence, False)
    print("Executing the DDR wrapper once.", flush=True)
    start = time.monotonic()
    transaction("exe", "0x85000")
    outcome["exe_seconds"] = round(time.monotonic() - start, 3)
    _, values = collect(evidence, outcome)
    if not trained(values):
        raise RuntimeError(f"DDR initialization did not succeed: {outcome['result']}")
    failed = ram_pages(evidence)
    if failed is not None:
        raise RuntimeError(f"RAM page comparison failed: {failed:#x}")


def arm_watchdog(watchdog):
    readback = [value & 0xffff for value in words(transaction(*ARM))]
    watchdog.update(armed=True, armed_monotonic=time.monotonic(), arm_readback=readback)
    save()
    return readback == [0x41, 1, 2]


def restore_launch(watchdog):
    """Run the restore wrapper with WDT0 armed; True if it returned and WDT0 is off."""
    if words(transaction("readl", hex(WDT_MODE)))[0] & 1:
        raise RuntimeError("WDT0 is already enabled; restore not launched.")
    if not arm_watchdog(watchdog):
        transaction(*DISARM)
        raise RuntimeError("WDT0 readback after arming differs; disarmed, restore not launched.")
    print("Executing the DDR restore wrapper once under WDT0.", flush=True)
    start = time.monotonic()
    try:
        mode = words(transaction("exe", "0x85000", *DISARM, timeout=EXE_TIMEOUT))[-1]
    except RuntimeError as error:
        # The wrapper did not return, or FEL failed while WDT0 counts: the
        # reset follows within the interval. No further request goes to this
        # instance.
        watchdog["exe_failure"] = str(error)
        save()
        return False
    record["exe_seconds"] = round(time.monotonic() - start, 3)
    watchdog["mode_after_disarm"] = mode
    save()
    return not mode & 1


def await_reset(watchdog):
    """Adopt the new FEL instance on this port after WDT0 fired, and disarm it."""
    global device, identity
    deadline = watchdog["armed_monotonic"] + RETURN_WINDOW
    while True:
        candidate, current = base.present_instance(access, config)
        if candidate is not None and current != identity and os.access(candidate, os.R_OK | os.W_OK):
            break
        if time.monotonic() >= deadline:
            raise RuntimeError(f"No new FEL instance within {RETURN_WINDOW} s of arming WDT0; no retry.")
        time.sleep(0.05)
    watchdog["returned_after_seconds"] = round(time.monotonic() - watchdog["armed_monotonic"], 3)
    device, identity = candidate, current
    access.require_session(device)
    access.require_rom(device)
    if access.target(device).name != port_name:
        raise RuntimeError("The new FEL instance is on another USB port.")
    record.update(returned_device=str(device), returned_identity=identity)
    save()
    before, after = words(transaction("readl", hex(WDT_MODE), *DISARM))
    watchdog.update(mode_after_reset=before, mode_after_reset_disarm=after)
    save()
    if not before & 1:
        raise RuntimeError("WDT0 was not enabled on the new instance; the reset is unexplained; no fallback.")
    if after & 1:
        raise RuntimeError("WDT0 did not disarm on the new instance.")
    if "soc=00001903(A733)" not in transaction("version"):
        raise RuntimeError("Unexpected SoC after the WDT0 reset.")


def restore_attempt():
    """Return None when DRAM is restored, otherwise the reason for the fallback."""
    board = words(transaction(*[part for n in range(4) for part in ("readl", hex(0x03006200 + 4*n))]))
    record["board_chip_id"] = board
    if board != list(chip_id):
        return "chip ID differs from the record; restore not launched"
    upload(args.build, manifest, args.evidence, True)
    watchdog = record["watchdog"] = {"mode": 0x41, "interval_seconds": 4, "armed": False}
    with usb_guard.reenumeration("sunstoneos-a733-" + port_name, "DDR restore under WDT0", 1) as churn:
        if restore_launch(watchdog):
            mailbox, values = collect(args.evidence, record)
            restored = struct.unpack_from("<7I", mailbox, 28)
            start, end = struct.unpack_from("<2I", mailbox, 100)
            record["restore_result"] = dict(zip(("error", "phy_writes", "hook_calls", "sample_read_bytes",
                                                 "failure_address", "expected", "actual"), restored))
            if not restored[0]:
                # Words 25 and 26: 24 MHz counter at the start and end of restore_check.
                record["restore_result"]["check_ms"] = round(((end - start) & 0xffffffff) / 24000, 3)
            save()
            settings = manifest["restore"]
            expected = (0, settings["phy_registers"], 1, settings["sample_read_bytes"])
            if restored[:4] != expected or not trained(values) or values[2] != 12288:
                reason = f"restore reported {record['restore_result']}"
            else:
                failed = ram_pages(args.evidence)
                if failed is None:
                    churn["count"] = 0
                    return None
                reason = f"RAM page comparison failed after restore: {failed:#x}"
            # A post-edit failure leaves PHY state unknown. Reset to a cold
            # controller rather than training it in place.
            watchdog["deliberate_reset"] = reason
            if not arm_watchdog(watchdog):
                raise RuntimeError("WDT0 readback after the fallback arming differs; stopped.")
        elif "exe_failure" in watchdog:
            reason = "restore did not return; WDT0 reset expected"
        else:
            reason = "WDT0 stayed enabled after the restore returned; WDT0 reset expected"
        await_reset(watchdog)
    return reason


exit_status = 0
try:
    usb_guard.check_health()
    with access.board_lock() as device:
        access.require_rom(device)
        if restore:
            # The fallback re-enumerates through WDT0; require reconnect access first.
            access.require_session(device)
        identity = access.identity(device)
        port = access.target(device)
        port_name = port.name
        config = {"port": port_name, "sysfs": str(port.resolve())}
        record.update(device=str(device), node_identity=identity)
        try:
            version = transaction("version")
            if "soc=00001903(A733)" not in version:
                raise RuntimeError(f"Unexpected SoC: {version}")
            if restore:
                reason = restore_attempt()
                if reason is None:
                    record.update(complete=True, outcome="restored")
                    save()
                    print(f"PASS: DRAM restored; quick check {record['restore_result']['check_ms']} ms; "
                          f"both 4 KiB pages match. Evidence: {args.evidence}")
                else:
                    print(f"RESTORE FAILED: {reason}. Training fully once.", flush=True)
                    record["fallback"] = {"reason": reason}
                    save()
                    evidence = args.evidence / "fallback"
                    evidence.mkdir()
                    full_training(args.fallback_build, fallback, evidence, record["fallback"])
                    record.update(complete=True, outcome="fallback-full-training")
                    save()
                    exit_status = 3
                    print(f"FALLBACK PASS: full training reports {record['fallback']['result']['dram_mib']} MiB; "
                          f"both 4 KiB pages match. Evidence: {args.evidence}")
            else:
                upload(args.build, manifest, args.evidence, False)
                print("Executing the DDR wrapper once.", flush=True)
                transaction("exe", "0x85000")
                _, values = collect(args.evidence, record)
                if args.allow_untrained:
                    # The init-only variant leaves DRAM untrained; the capture that
                    # follows reads only the PHY and controller.
                    if values[0] != MAGIC or values[1] != 5 or values[3] != 0:
                        raise RuntimeError(f"Wrapper did not return normally: {record['result']}")
                    record.update(untrained=True, complete=True)
                    save()
                    print(f"PASS: init-only wrapper returned, DRAM untrained ({values[2]} MiB reported). Evidence: {args.evidence}")
                else:
                    if not trained(values):
                        raise RuntimeError(f"DDR initialization did not succeed: {record['result']}")
                    failed = ram_pages(args.evidence)
                    if failed is not None:
                        raise RuntimeError(f"RAM page comparison failed: {failed:#x}")
                    record["complete"] = True
                    save()
                    print(f"PASS: DDR reports {values[2]} MiB; both 4 KiB pages match. Evidence: {args.evidence}")
        except BaseException as error:
            if record["device_access_attempted"]:
                try:
                    base.stop_after_failure(record, save, lambda: access.quarantine_instance(
                        device, "DDR attempt stopped; no retry; coordinator recovery required.", identity), error)
                finally:
                    if not record["launch_attempted"]:
                        base.hold_ownership()
            raise

except (OSError, RuntimeError, ValueError) as error:
    record["failure"] = str(error)
    save()
    sys.exit(f"Stopped: {error}\nEvidence: {args.evidence}")
sys.exit(exit_status)
