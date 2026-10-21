#!/usr/bin/env python3
"""Compare the minimum radio firmware bus time with the supervised bootstrap bound.

The firmware engine sends every patch-table pair as its own padded 512-byte
CMD53 block and every 1 KiB image chunk as a padded 1,536-byte transfer. This
check counts those transfers from the pinned artifacts, adds the smallest
SD-protocol framing for each command and its confirmation, and converts the
clock cycles to time at the admitted one-bit SDIO clock. The result is a lower
bound: it excludes controller and task latency, card busy time, card setup,
boot work before the radio platform starts and the time spent polling between
transfers. The fixed platform power-up waits (supply, module settle and reset)
are subtracted from the board profile's bootstrap offset. The check fails when
the bus lower bound does not fit in the time that remains. A pass shows only
that this bound fits; it does not show that the bootstrap meets its deadline.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
BLOBS = {
    "fw_adid_8800d80_u02.bin": "a526cbd02fcdc495f049f3ad6b5933cb08cd984b16790c716a060d582fee1a56",
    "fw_patch_8800d80_u02.bin": "4ad4eede4d347c45ed61b23508e7843dc884f0e411bf957f95410001d0cfc4d0",
    "fw_patch_table_8800d80_u02.bin": "5ff184105fdc4c4679e026418f4da293efec8df02a3a3ca22cee723c17df4a60",
    "fmacfw_8800d80_u02.bin": "55b5a20865aca7cd2edaae1733a36da45d2b0b29060bc243d8e9ec212e139f6c",
    "fmacfw_8800d80_h_u02.bin": "ae8d548df96c132c445f888b91eab5ef03c19991b8bdef4fc12ebfcbf9f953e7",
}
# Minimum SD clock cycles (SD Physical Layer Simplified Specification timing).
COMMAND = 48 + 2 + 48          # command, minimum NCR, R5 response
CMD52 = COMMAND + 8            # plus NCC before the next command
WRITE_BLOCK = 2 + 1 + 512 * 8 + 16 + 1 + 2 + 5   # NWR, start, data, CRC16, end, NCRC, CRC status
CONFIRMATION_BYTES = 20        # Smallest D80 confirmation record the engine reads


def source(path, pattern):
    text = (ROOT / path).read_text()
    match = re.search(pattern, text)
    if not match:
        raise SystemExit(f"{path}: expected source pattern not found: {pattern}")
    return match


def read_cycles(byte_count):
    return COMMAND + 2 + 1 + 8 * byte_count + 16 + 1   # NAC, start, data, CRC16, end


def write_cycles(byte_count):
    assert byte_count % 512 == 0
    return COMMAND + (byte_count // 512) * WRITE_BLOCK


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware", type=Path, required=True, help="Pinned aic8800d80 firmware directory")
    parser.add_argument("--output", type=Path, help="Write the computed budget as JSON")
    args = parser.parse_args()
    data = {}
    for name, expected in BLOBS.items():
        content = (args.firmware / name).read_bytes()
        if hashlib.sha256(content).hexdigest() != expected:
            raise SystemExit(f"{name}: unexpected artifact")
        data[name] = content

    # Keep the model tied to the implemented framing and configuration.
    source("drivers/net/aic8800_firmware.c", r"request_bytes=\(\(16u\+parameters\+4u\+511u\)/512u\)\*512u")
    source("drivers/net/aic8800_firmware.c", r"packet\(f,0x040b,1032\)")
    source("drivers/net/aic8800_firmware.c", r"if\(bytes>1024\)bytes=1024")
    source("drivers/net/aic8800_firmware.c", r"word_write\(f,address,value\)")
    module_hz = int(source("drivers/net/a733_radio_platform.c",
                           r"host_time\}\s*,\s*p\s*,\s*(\d+)\}").group(1))
    bootstrap_us = int(source("apps/board/radio_profile.c",
                              r"#define BOARD_RADIO_BOOTSTRAP_US (\d+)u").group(1))
    bus_hz = module_hz // 2   # sunxi_sdio selects the SDR divide-by-two path, one data line.
    # Minimum waits the radio platform holds before the first card command.
    power_up_us = sum(int(source("drivers/net/a733_radio_platform.c",
                                 r"p->last_now-p->wait_since>=(\d+)\)p->stage=" + stage).group(1))
                      for stage in ("ENABLE_MODULE", "BUS_GATE", "RESET_RELEASE"))

    spec = importlib.util.spec_from_file_location("radio_patch", ROOT / "tools/inspect-radio-patch.py")
    patch = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(patch)
    table = patch.inspect(data["fw_patch_table_8800d80_u02.bin"])
    # Type 6 carries the version string; the engine skips it and writes every other pair.
    pairs = sum(r["pair_count"] for r in table["records"] if r["type"] != 6)

    def request_bytes(parameters):
        return ((16 + parameters + 4 + 511) // 512) * 512

    chunk_bytes = request_bytes(1032)
    word_bytes = request_bytes(8)
    results = {}
    for image in ("fmacfw_8800d80_u02.bin", "fmacfw_8800d80_h_u02.bin"):
        chunks = sum(-(-len(data[n]) // 1024) for n in
                     ("fw_adid_8800d80_u02.bin", "fw_patch_8800d80_u02.bin", image))
        # Identity reads and setup (3), pointer reads (4), Wi-Fi patch words (10), start (1).
        small = pairs + 3 + 4 + 10 + 1
        commands = chunks + small
        tx_bytes = chunks * chunk_bytes + small * word_bytes
        # Each command: credit poll, write, status poll, length poll, confirmation read.
        cycles = (chunks * write_cycles(chunk_bytes) + small * write_cycles(word_bytes) +
                  commands * (3 * CMD52 + read_cycles(CONFIRMATION_BYTES)))
        seconds = cycles / bus_hz
        results[image] = {"chunks": chunks, "word_commands": small, "tx_bytes": tx_bytes,
                          "payload_only_seconds": round(tx_bytes * 8 / bus_hz, 3),
                          "lower_bound_cycles": cycles, "lower_bound_seconds": round(seconds, 3)}
    worst = max(r["lower_bound_seconds"] for r in results.values())
    budget = (bootstrap_us - power_up_us) / 1e6
    report = {"module_hz": module_hz, "bus_hz": bus_hz, "data_lines": 1, "patch_table_pairs": pairs,
              "bootstrap_offset_seconds": bootstrap_us / 1e6, "power_up_wait_seconds": power_up_us / 1e6,
              "bootstrap_budget_seconds": budget, "images": results,
              "lower_bound_seconds": worst, "within_budget": worst < budget,
              "excludes": ["card setup", "boot work before radio platform start",
                           "controller and task latency", "card busy time", "polling between transfers"]}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    if worst >= budget:
        print(f"FAIL radio bootstrap: at least {worst:.1f} s of SDIO bus time at {bus_hz} Hz "
              f"exceeds the {budget:.3f} s left of the bootstrap bound after power-up waits")
        raise SystemExit(1)
    print(f"PASS radio bootstrap bus bound: {worst:.1f} s fits in the {budget:.3f} s left after "
          f"power-up waits; card setup and latency are not counted")


if __name__ == "__main__":
    main()
