#!/usr/bin/env python3
"""Check pinned radio firmware against the native engine and SDIO register model."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = {
    "fw_adid_8800d80_u02.bin": "a526cbd02fcdc495f049f3ad6b5933cb08cd984b16790c716a060d582fee1a56",
    "fw_patch_8800d80_u02.bin": "4ad4eede4d347c45ed61b23508e7843dc884f0e411bf957f95410001d0cfc4d0",
    "fw_patch_table_8800d80_u02.bin": "5ff184105fdc4c4679e026418f4da293efec8df02a3a3ca22cee723c17df4a60",
    "fmacfw_8800d80_h_u02.bin": "ae8d548df96c132c445f888b91eab5ef03c19991b8bdef4fc12ebfcbf9f953e7",
    "fmacfw_8800d80_u02.bin": "55b5a20865aca7cd2edaae1733a36da45d2b0b29060bc243d8e9ec212e139f6c",
}
REFERENCES = {
    "aic_bsp_driver.c": "43439d69cd1c3f537ee90f50d4bc52d2a54379f41092f625ded080daacf7de6e",
    "aic_bsp_driver.h": "9f72ebdd743ac0af134df50ebc36ea25876b9a361a89b82e17a4a8c2e4d918b3",
    "aic_bsp_8800d80.c": "fcf647395a7f007f22cd19f4e15dcc821e924da55b72528395a9972e5c6290e8",
    "aicsdio.c": "f08bcfa63f9b7226f4b26832ee9b6088c057fd5ee06389f3ad872f13f9246f0c",
}
SOURCES = ["checks/aic8800-firmware.c", "drivers/net/aic8800_firmware.c", "drivers/net/aic8800_runtime.c", "drivers/net/aic8800_wifi.c", "drivers/net/aic8800_security.c", "drivers/net/aic8800_data.c",
           "drivers/net/aic8800_sdio_transport.c", "drivers/net/aic8800_sdio_codec.c", "drivers/storage/storage_digest.c",
           "drivers/storage/sunxi_sdio.c", "drivers/storage/sdio_card.c"]
INCLUDES = ["include/aic8800_firmware.h", "include/aic8800_runtime.h", "include/aic8800_wifi.h", "include/aic8800_security.h", "include/aic8800_data.h", "include/aic8800_sdio_transport.h",
            "include/aic8800_sdio_codec.h", "include/sunxi_sdio.h", "include/sdio_card.h",
            "checks/aic8800-sdio-transport.c", "checks/sdio-controller.c"]


def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def transcript(blobs, header):
    """Independent source sequence, using exact admitted artifact bytes."""
    enum = header[header.index("enum dbg_msg_tag"):].split("};", 1)[0]
    names = re.findall(r"^\s*(DBG_\w+)\s*(?:=[^,]*)?,", enum, re.M)
    ids = {name: 0x400 + i for i, name in enumerate(names)}
    assert ids["DBG_MEM_BLOCK_WRITE_REQ"] == 0x040b
    assert ids["DBG_START_APP_REQ"] == 0x040d
    port_enum = header[header.index("enum aicbt_btport_type"):].split("};", 1)[0]
    ports = re.findall(r"^\s*(AICBT_BTPORT_\w+),", port_enum, re.M)
    uart_port = ports.index("AICBT_BTPORT_UART")
    assert uart_port == 2
    records = []

    def command(kind, parameters, response, delay=0):
        ident = ids[kind + "_REQ"]
        payload = b"\0" * 4 + struct.pack("<4H", ident, 1, 100, len(parameters)) + parameters
        outer = bytearray(struct.pack("<HB", len(payload), 0x11))
        crc = 0
        for byte in outer:
            crc ^= byte
            for _ in range(8):
                crc = ((crc << 1) ^ (7 if crc & 128 else 0)) & 255
        packet = bytes(outer) + bytes([crc]) + payload
        packet += b"\0" * (((len(packet) + 4 + 511) // 512) * 512 - len(packet))
        records.append(struct.pack("<5I", len(packet), ids[kind + "_CFM"], len(response), delay, 0)
                       + packet + response)

    def word(address, value, delay=0):
        command("DBG_MEM_WRITE", struct.pack("<2I", address, value),
                struct.pack("<2I", 0xdeadcafe, 0x11223344), delay)

    def upload(name, address):
        data = blobs[name]
        for offset in range(0, len(data), 1024):
            chunk = data[offset:offset + 1024]
            command("DBG_MEM_BLOCK_WRITE", struct.pack("<2I", address + offset, len(chunk))
                    + chunk.ljust(1024, b"\0"), struct.pack("<I", 0x80000003))

    table = blobs["fw_patch_table_8800d80_u02.bin"]
    assert table.startswith(b"AICBT_PT_TAG\0")
    upload("fw_adid_8800d80_u02.bin", struct.unpack_from("<I", table, 44)[0])
    upload("fw_patch_8800d80_u02.bin", struct.unpack_from("<I", table, 52)[0])
    at = 16
    while at < len(table):
        kind, count = struct.unpack_from("<2I", table, at + 16)
        for i in range(count):
            if kind == 6:
                break
            address, value = struct.unpack_from("<2I", table, at + 24 + 8 * i)
            if kind == 3:
                value = [1, 0xffffffff, 0, 5, uart_port, 1500000, 1, 1, 0x6f2f][i]
            word(address, value, 500 if kind == 4 and i + 1 == count else 0)
        at += 24 + count * 8
    assert at == len(table)
    upload("fmacfw_8800d80_h_u02.bin", 0x120000)
    wifi = blobs["fmacfw_8800d80_h_u02.bin"]
    pointers = []
    for offset in [0x198, 0x1a0, 0x1c, 0x1a4]:
        value = struct.unpack_from("<I", wifi, offset)[0]
        pointers.append(value)
        command("DBG_MEM_READ", struct.pack("<I", 0x120000 + offset), struct.pack("<2I", 0x120000 + offset, value))
    config, patch, version, pairs = pointers
    assert version > 0x06090100
    for address, value in [(patch, 0x48435450), (patch + 8, 0x50544348), (patch + 4, pairs),
                           (patch + 12, 1), (pairs, config + 0x170), (pairs + 4, 0x0100000a)]:
        word(address, value)
    for i in range(4):
        word(patch + 48 + 4 * i, 0)
    command("DBG_START_APP", struct.pack("<2I", 0x120000, 1), struct.pack("<I", 0xa501))
    assert len(records) == 3256
    return b"".join(records)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--artifacts", type=Path, required=True)
    p.add_argument("--reference-dir", type=Path, required=True)
    p.add_argument("--evidence", type=Path, required=True)
    p.add_argument("--cc", default="cc")
    p.add_argument("--case", choices=["all", "mode-alias", "table-bursts"], default="all")
    a = p.parse_args()
    out = a.evidence.resolve()
    if out == ROOT or ROOT in out.parents:
        p.error("evidence must remain outside Git")
    for name, expected in ARTIFACTS.items():
        if digest(a.artifacts / name) != expected:
            raise SystemExit(f"artifact hash mismatch: {name}")
    for name, expected in REFERENCES.items():
        if digest(a.reference_dir / name) != expected:
            raise SystemExit(f"source hash mismatch: {name}")
    out.mkdir(parents=True, exist_ok=True)
    expected = out / "source-transcript.bin"
    expected.write_bytes(transcript({n: (a.artifacts / n).read_bytes() for n in ARTIFACTS},
                                    (a.reference_dir / "aic_bsp_driver.h").read_text()))
    exe = out / "aic8800-firmware-check"
    command = [a.cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(ROOT / "include"),
               *(str(ROOT / s) for s in SOURCES), "-o", str(exe)]
    build = subprocess.run(command, text=True, capture_output=True)
    (out / "build.log").write_text(build.stdout + build.stderr)
    build.check_returncode()
    run_command = [str(exe), *(str(a.artifacts / n) for n in ARTIFACTS), str(expected)] + ([a.case] if a.case != "all" else [])
    run = subprocess.run(run_command, text=True, capture_output=True)
    (out / "run.log").write_text(run.stdout + run.stderr)
    result = {"result": "passed" if run.returncode == 0 else "failed", "command": command,
              "run_command": run_command, "case": a.case, "exit_code": run.returncode, "stdout": run.stdout,
              "stderr": run.stderr, "sources": {n: digest(ROOT / n) for n in SOURCES + INCLUDES},
              "runner_sha256": digest(Path(__file__).resolve()), "artifacts": ARTIFACTS,
              "references": REFERENCES, "transcript_sha256": digest(expected),
              "executable_sha256": digest(exe),
              "compiler": subprocess.check_output([a.cc, "--version"], text=True).splitlines()[0]}
    (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"result": result["result"], "stdout": run.stdout, "evidence": str(out)}))
    run.check_returncode()


if __name__ == "__main__":
    main()
