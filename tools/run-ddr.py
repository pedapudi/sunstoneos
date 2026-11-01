#!/usr/bin/env python3
"""Load the reviewed A733 DDR wrapper, return to FEL, and check two RAM pages.

Without --build the runner restores the board's recorded training
(docs/board-runbooks/dram-restore-records.md). It reads the board's SID chip
ID and uploads the record --records/chip-<four SID words>.bin with the
restore build --restore-build; --fallback-build (default: the plain
full-training build) trains fully when the restore cannot run or fails.
--full-training runs only the plain build. --build selects one build
explicitly: a plain, capture-variant or restore build, the last with an
explicit --record and --fallback-build.

A restore build (tools/prepare-ddr.py --variant restore) runs under WDT0. The
runner compares the board's SID with the record before any launch, arms WDT0
in its four-second mode immediately before the wrapper and disarms it in the
same FEL request once the wrapper returns. A restore that reports an error,
fails the RAM pages or does not return ends in one WDT0 reset to a new FEL
instance and one full training with --fallback-build; a SID mismatch trains
fully without a reset. Without --build, a missing, unreadable or invalid
record, a record holding another chip ID, a missing restore build or missing
reconnect access also trains fully, without a launch of the restore.
Exit status 0: DRAM ready through the requested build (the restore, without
--build). Exit status 3: DRAM ready through a full training that ran in place
of the restore; the runner prints why. After a fallback through WDT0, the
trained instance's DDR evidence for later runners is EVIDENCE/fallback; the
runner prints the directory to pass on in every case.
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
PLAIN_BUILD, RESTORE_BUILD = CACHE / "build/a733-ddr", CACHE / "build/a733-ddr-restore"
RECORDS = CACHE / "ddr-records"
parser.add_argument("--build", type=Path,
                    help="run this build only (plain, capture variant, or restore with --record)")
parser.add_argument("--full-training", action="store_true",
                    help=f"train fully with the plain build (--build, default {PLAIN_BUILD}) and skip the restore")
parser.add_argument("--records", type=Path, default=RECORDS,
                    help="directory of per-board records named chip-<four SID words>.bin")
parser.add_argument("--restore-build", type=Path, default=RESTORE_BUILD,
                    help="restore build used without --build")
parser.add_argument("--evidence", type=Path,
                    default=CACHE / "evidence" / time.strftime("ddr-%Y%m%d-%H%M%S"))
parser.add_argument("--allow-untrained", action="store_true",
                    help="init-only capture variant: accept a wrapper return without trained DRAM")
parser.add_argument("--record", type=Path, help="training record for the restore variant")
parser.add_argument("--fallback-build", type=Path,
                    help="plain full-training build that a failed restore falls back to")
owner = globals().get("BOARD_RUNNER")
args = parser.parse_args(owner.ddr_arguments if owner is not None else None)
# Without --build: restore by the board's SID, falling back to full training.
auto = args.build is None and not args.full_training
if args.full_training and (args.record or args.fallback_build or args.allow_untrained):
    sys.exit("--full-training runs the plain build alone; it takes no --record, "
             "--fallback-build or --allow-untrained.")
if auto and (args.record or args.allow_untrained):
    sys.exit("--record and --allow-untrained need --build; without it the record comes from "
             "--records by the board's chip ID.")
if not auto and (args.records != RECORDS or args.restore_build != RESTORE_BUILD):
    sys.exit("--records and --restore-build apply only without --build and --full-training.")
if args.build is None:
    args.build = args.restore_build if auto else PLAIN_BUILD


def read_manifest(build):
    try:
        return json.loads((build / "manifest.json").read_text())
    except (OSError, ValueError) as error:
        sys.exit(f"No usable DDR build at {build} ({error}); build it with tools/prepare-ddr.py.")


def verified(build):
    manifest = read_manifest(build)
    for name, expected in manifest["files"].items():
        content = (build / name).read_bytes()
        if len(content) != expected["size"] or hashlib.sha256(content).hexdigest() != expected["sha256"]:
            sys.exit(f"Payload mismatch: {name}")
    return manifest


spec = importlib.util.spec_from_file_location("training_record", ROOT / "tools/training-record.py")
training = importlib.util.module_from_spec(spec)
spec.loader.exec_module(training)


def load_record(data):
    """Chip ID and the PHY entries in helper order; raises ValueError."""
    if not 128 <= len(data) <= 0x6000:
        raise ValueError("Training record exceeds its SRAM reservation or lacks a header.")
    if len(data) != 128 + struct.unpack_from("<I", data, 16)[0]:
        raise ValueError("Training record length differs from its header.")
    try:
        parsed = training.parse(data)
    except struct.error as error:
        raise ValueError(str(error)) from error
    # PHY entries in helper order, after the parameter run.
    return (parsed["chip_id"],
            [run["base"] + 2 * n for run in parsed["runs"][1:] for n in range(len(run["values"]))],
            [value for run in parsed["runs"][1:] for value in run["values"]])


def record_name(chip):
    return "chip-" + "-".join(f"{word:08x}" for word in chip) + ".bin"


# Why a run without --build trains fully in place of the restore, once known.
skip_reason = None
if auto and not (args.build / "manifest.json").exists():
    skip_reason = f"no restore build at {args.build}"
    args.build = args.fallback_build or PLAIN_BUILD
if auto:
    args.fallback_build = args.fallback_build or PLAIN_BUILD
manifest = read_manifest(args.build)
if args.full_training and manifest.get("variant") is not None:
    sys.exit("--full-training needs the plain full-training build.")
restore = manifest.get("variant") == "restore"
if auto and not restore and skip_reason is None:
    sys.exit(f"--restore-build {args.build} is not a restore build.")
if args.allow_untrained != (manifest.get("variant") == "init-only"):
    sys.exit("--allow-untrained goes with, and only with, an init-only variant build.")
if not auto and ((args.record is not None) != restore or (args.fallback_build is not None) != restore):
    sys.exit("--record and --fallback-build go with, and only with, a restore variant build.")
training_bytes = chip_id = fallback = profile_addresses = None
if auto and skip_reason:
    fallback = verified(args.fallback_build)
    if fallback.get("variant") is not None or fallback.get("clock_point_word") != 15:
        sys.exit(f"--fallback-build {args.fallback_build} must be the plain full-training wrapper with the CPU "
                 "clock return; build it with tools/prepare-ddr.py.")
if restore and not auto:
    try:
        training_bytes = args.record.read_bytes()
        chip_id, profile_addresses, profile_values = load_record(training_bytes)
    except ValueError as error:
        sys.exit(f"Invalid training record: {error}")
manifest = verified(args.build)
# Diagnostic restore build: the helper's write survey (tools/ddr-survey.py).
survey = manifest.get("restore", {}).get("survey")
if survey:
    spec = importlib.util.spec_from_file_location("ddr_survey", ROOT / "tools/ddr-survey.py")
    survey_decoder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(survey_decoder)
    if survey["address"] != 0x96000 or survey["entries"] != manifest["restore"]["phy_registers"] \
            or survey["bytes"] != survey_decoder.HEADER_BYTES + survey_decoder.ENTRY_BYTES * survey["entries"]:
        sys.exit("Survey layout differs from tools/ddr-survey.py.")
# Builds before the CPU clock return leave CPU-L and the DSU at boot0's points.
if manifest.get("clock_point_word") != 15:
    sys.exit("--build predates the CPU clock return (manifest has no clock_point_word 15); rebuild it.")
if restore:
    if not 0 < manifest["files"]["ddr-restore.bin"]["size"] <= 0x8000:
        sys.exit("Restore helper exceeds its SRAM reservation.")
    # Helpers up to f0595d83 read the PHY data memory after
    # init_DRAM with its clock stopped, and report that pass as "readback".
    if manifest["restore"]["errors"].get("final_readback") != 15:
        sys.exit("--build predates the clocked final readback (no final_readback error); rebuild it.")
    fallback = verified(args.fallback_build)
    if fallback.get("variant") is not None or fallback.get("boot0_sha256") != manifest.get("boot0_sha256"):
        sys.exit("--fallback-build must be the plain full-training wrapper for the same boot0.")
    if fallback.get("clock_point_word") != 15:
        sys.exit(f"--fallback-build {args.fallback_build} predates the CPU clock return; rebuild it with tools/prepare-ddr.py.")
args.evidence.mkdir(parents=True, exist_ok=False)
record = {"manifest": manifest, "tool_sha256": hashlib.sha256(args.fel.read_bytes()).hexdigest(),
          "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
          "transactions": [], "complete": False, "launch_attempted": False,
          "device_access_attempted": False,
          "mode": "restore-by-sid" if auto else "full-training" if args.full_training else "build"}
if auto:
    record["records"] = str(args.records)


def keep_record(data):
    record.update(training_record_sha256=hashlib.sha256(data).hexdigest(), fallback_manifest=fallback)
    (args.evidence / "training-record.bin").write_bytes(data)
    (args.evidence / "training-record-upload.bin").write_bytes(data + bytes(0x6000-len(data)))


if restore and not auto:
    keep_record(training_bytes)

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
if owner is not None:
    access, base, usb_guard = owner.ddr_access, owner.ddr_base, owner.ddr_guard


def transaction(*commands, timeout=30):
    if owner is not None:
        return owner.ddr_transaction(device, identity, record, save, commands, timeout)
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
        checked = [(evidence / "record-upload.bin", evidence / "training-record-upload.bin"),
                   (evidence / "restore-upload.bin", build / "ddr-restore.bin")]
        clear = []
        if survey:
            # Zero the survey first, so a helper that stops before its write
            # pass leaves no earlier run's values behind.
            (evidence / "survey-clear.bin").write_bytes(bytes(survey["bytes"]))
            clear = ["write", hex(survey["address"]), evidence / "survey-clear.bin"]
            checked.append((evidence / "survey-clear-upload.bin", evidence / "survey-clear.bin"))
        transaction("write", "0x87000", evidence / "training-record-upload.bin",
                    "write", "0x92000", build / "ddr-restore.bin", *clear)
        transaction("read", "0x87000", "24576", evidence / "record-upload.bin",
                    "read", "0x92000", str(manifest["files"]["ddr-restore.bin"]["size"]),
                    evidence / "restore-upload.bin",
                    *(["read", hex(survey["address"]), str(survey["bytes"]), evidence / "survey-clear-upload.bin"]
                      if survey else []))
        for actual, expected in checked:
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


# CPU-L, CPU-B and DSU PLL, pattern, LFM and mux words. The wrapper returns
# CPU-L and the DSU to the boot ROM's words after boot0's board setup
# (boot/a733/ddr-entry.S, mailbox word 15); CPU-B keeps boot0's. The required
# words are those the OS entry guard requires (apps/os/cpu-return-entry.c).
CPU_CLOCKS = tuple((f"{name}_{word}", base + offset)
                   for name, base in (("cpu_l", 0x08871000), ("cpu_b", 0x08872000), ("dsu", 0x08873000))
                   for word, offset in (("pll", 0), ("pattern0", 4), ("pattern1", 8), ("lfm", 0x18), ("mux", 0x1c)))
ROM_CLOCKS = {"cpu_l_pll": 0xf8802600, "cpu_l_mux": 0x03010000, "cpu_b_pll": 0xf8802700,
              "cpu_b_mux": 0x03000000, "dsu_pll": 0xf8802600, "dsu_mux": 0x03010105}
# Pattern (spread-spectrum) and LFM enable bits that must be clear.
CLOCK_OFF_BITS = {"pattern0": 3 << 29, "pattern1": 1 << 31, "lfm": 1 << 31}


def collect(evidence, outcome):
    stdout = transaction("read", "0x86000", "128", evidence / "mailbox.bin",
                         "read", "0xe0000", "16384", evidence / "log.bin",
                         "read", "0x831f8", "128", evidence / "dram-parameters.bin",
                         *[part for _, address in CPU_CLOCKS for part in ("readl", hex(address))])
    mailbox = (evidence / "mailbox.bin").read_bytes()
    clocks = words(stdout)
    if len(clocks) != len(CPU_CLOCKS):
        raise RuntimeError(f"CPU clock readback returned {len(clocks)} words.")
    outcome["cpu_clocks"] = {name: f"{value:#010x}" for (name, _), value in zip(CPU_CLOCKS, clocks)}
    outcome["clock_point_result"] = struct.unpack_from("<I", mailbox, 60)[0]
    values = struct.unpack("<7I", mailbox[:28])
    outcome["result"] = dict(zip(("magic", "stage", "dram_mib", "board_result",
                                  "entry_cpsr", "entry_sctlr", "log_length"), values))
    log = (evidence / "log.bin").read_bytes()[:min(values[6], 16384)].decode(errors="replace")
    (evidence / "firmware.log").write_text(log)
    print(log, end="", flush=True)
    save()
    return mailbox, values


def clock_problems(outcome):
    """Words that differ from the entry point: the boot ROM's CPU-L and DSU
    words, boot0's CPU-B words, pattern and LFM off (apps/os/cpu-return-entry.c)."""
    clocks = outcome["cpu_clocks"]
    wrong = {name: clocks[name] for name, value in ROM_CLOCKS.items() if int(clocks[name], 16) != value}
    wrong.update({name: value for name, value in clocks.items()
                  if int(value, 16) & CLOCK_OFF_BITS.get(name.split("_", 2)[-1], 0)})
    return wrong


def require_rom_clocks(outcome):
    wrong = clock_problems(outcome)
    if wrong or outcome["clock_point_result"]:
        raise RuntimeError(f"CPU clocks not at the entry point after the DDR start: {wrong}; "
                           f"wrapper clock result {outcome['clock_point_result']:#x}")


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
    require_rom_clocks(outcome)
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


def read_survey(evidence):
    """Read and classify the write survey; survey.json holds every entry."""
    transaction("read", hex(survey["address"]), str(survey["bytes"]), evidence / "survey.bin")
    try:
        entries, summary = survey_decoder.decode((evidence / "survey.bin").read_bytes(),
                                                 profile_addresses, profile_values)
    except ValueError as error:
        return {"error": str(error)}
    (evidence / "survey.json").write_text(json.dumps({"summary": summary, "entries": entries}, indent=2) + "\n")
    print("Write survey:", json.dumps(summary["classes"]), flush=True)
    return summary


def select_record():
    """Without --build: read the SID and load the board's record. Returns the
    reason the restore cannot run, or None with the record loaded."""
    global training_bytes, chip_id, profile_addresses, profile_values
    board = words(transaction(*[part for n in range(4) for part in ("readl", hex(0x03006200 + 4*n))]))
    record["board_chip_id"] = board
    path = args.records / record_name(board)
    record["record_path"] = str(path)
    save()
    if not path.is_file():
        return f"no training record for this board ({path} does not exist)"
    try:
        data = path.read_bytes()
        chip, addresses, values = load_record(data)
    except OSError as error:
        return f"training record {path} is unreadable: {error}"
    except ValueError as error:
        return f"training record {path} is invalid: {error}"
    if chip != board:
        return f"training record {path} holds another chip ID"
    training_bytes, chip_id, profile_addresses, profile_values = data, chip, addresses, values
    keep_record(data)
    save()
    return None


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
            errors = manifest["restore"]["errors"]
            start, end = struct.unpack_from("<2I", mailbox, 100)
            record["restore_result"] = dict(zip(("error", "phy_writes", "hook_calls", "sample_read_bytes",
                                                 "failure_address", "expected", "actual"), restored))
            if not restored[0]:
                # Words 25 and 26: 24 MHz counter at the start and end of restore_check.
                record["restore_result"]["check_ms"] = round(((end - start) & 0xffffffff) / 24000, 3)
            elif restored[0] in (errors["readback"], errors["final_readback"]):
                # Word 27 counts readback mismatches; words 28-31 hold the
                # second to fifth as (profile index << 16) | value read.
                count, *more = struct.unpack_from("<5I", mailbox, 108)
                record["restore_result"]["failed_pass"] = (
                    "hook" if restored[0] == errors["readback"] else "after init_DRAM")
                record["restore_result"]["mismatches"] = count
                record["restore_result"]["later_mismatches"] = [
                    {"address": f"{profile_addresses[word >> 16]:#010x}"
                     if (word >> 16) < len(profile_addresses) else f"index {word >> 16}",
                     "actual": f"{word & 0xffff:#06x}"}
                    for word in more[:max(0, min(count, 5) - 1)]]
            if survey:
                # The survey records the hook's passes. Word 27 counts the
                # failing pass's mismatches; a failure after init_DRAM means
                # the hook's readback passed.
                result = record["restore_result"]["survey"] = read_survey(args.evidence)
                if "mismatches" in result:
                    hook_mismatches = (record["restore_result"]["mismatches"]
                                       if restored[0] == errors["readback"] else 0)
                    result["agrees_with_mailbox"] = result["mismatches"] == hook_mismatches
            save()
            settings = manifest["restore"]
            expected = (0, settings["phy_registers"], 1, settings["sample_read_bytes"])
            if restored[:4] != expected or not trained(values) or values[2] != 12288:
                reason = f"restore reported {record['restore_result']}"
            elif clock_problems(record) or record["clock_point_result"]:
                # The WDT0 reset re-enters the ROM, which sets CPU-L and the DSU again.
                reason = (f"CPU clocks not at the entry point: {clock_problems(record)}, "
                          f"wrapper clock result {record['clock_point_result']:#x}")
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
        if restore and auto:
            # The fallback re-enumerates through WDT0, which needs reconnect
            # access; without it, train fully in place of the restore.
            try:
                access.require_session(device)
            except (ValueError, OSError) as error:
                skip_reason = f"reconnect access is not installed for this port ({error})"
        elif restore:
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
            if auto and skip_reason is None:
                skip_reason = select_record()
            if auto and skip_reason:
                # Nothing was launched: one full training on this instance.
                print(f"NO RESTORE: {skip_reason}. Training fully once.", flush=True)
                # The evidence names the build that trained; the restore build is kept apart.
                record.update(restore_skipped=skip_reason, restore_manifest=record["manifest"] if restore else None,
                              manifest=fallback)
                save()
                full_training(args.fallback_build, fallback, args.evidence, record)
                record.update(complete=True, outcome="full-training-without-restore")
                save()
                exit_status = 3
                print(f"FULL TRAINING PASS: {record['result']['dram_mib']} MiB; both 4 KiB pages match; "
                      f"DRAM is trained, not restored. Evidence: {args.evidence}")
                print(f"DDR evidence for the next image: {args.evidence}")
            elif restore:
                reason = restore_attempt()
                if reason is None:
                    record.update(complete=True, outcome="restored")
                    save()
                    print(f"PASS: DRAM restored; quick check {record['restore_result']['check_ms']} ms; "
                          f"both 4 KiB pages match. Evidence: {args.evidence}")
                    print(f"DDR evidence for the next image: {args.evidence}")
                else:
                    restore_error = record.get("restore_result", {}).get("error")
                    print(f"RESTORE FAILED: {reason}. Training fully once.", flush=True)
                    record["fallback"] = {"reason": reason}
                    save()
                    evidence = args.evidence / "fallback"
                    evidence.mkdir()
                    full_training(args.fallback_build, fallback, evidence, record["fallback"])
                    record.update(complete=True, outcome="fallback-full-training")
                    save()
                    next_evidence = args.evidence
                    if "returned_device" in record:
                        # The trained instance is the one after the WDT0 reset. Later
                        # runners read a DDR session's top-level instance and result,
                        # so the fallback directory gets a complete session of its own.
                        session = {key: record[key] for key in ("tool_sha256", "runner_sha256", "mode")}
                        session.update(record["fallback"], manifest=fallback, device=record["returned_device"],
                                       node_identity=record["returned_identity"], complete=True,
                                       launch_attempted=True, device_access_attempted=True,
                                       outcome="fallback-full-training", restore_session=str(args.evidence))
                        (evidence / "session.json").write_text(json.dumps(session, indent=2) + "\n")
                        next_evidence = evidence
                    exit_status = 3
                    print(f"FALLBACK PASS: full training reports {record['fallback']['result']['dram_mib']} MiB; "
                          f"both 4 KiB pages match; DRAM is trained, not restored. Evidence: {args.evidence}")
                    if restore_error in (manifest["restore"]["errors"]["readback"],
                                         manifest["restore"]["errors"]["final_readback"]):
                        print("A PHY register did not hold its restored value; report it, do not "
                              "recapture (docs/board-runbooks/dram-restore-records.md).")
                    elif auto:
                        print("If restores of this board keep failing, recapture its record: "
                              "docs/board-runbooks/dram-restore-records.md.")
                    print(f"DDR evidence for the next image: {next_evidence}")
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
                    require_rom_clocks(record)
                    record.update(untrained=True, complete=True)
                    save()
                    print(f"PASS: init-only wrapper returned, DRAM untrained ({values[2]} MiB reported). Evidence: {args.evidence}")
                else:
                    if not trained(values):
                        raise RuntimeError(f"DDR initialization did not succeed: {record['result']}")
                    require_rom_clocks(record)
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
