#!/usr/bin/env python3
"""Load the USB transcription service, run recorded inputs, and return to FEL."""
import argparse
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time
import zlib
import wave
import usb.core
import usb.util

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / ".cache/sunstoneos"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--fel", type=Path, required=True)
parser.add_argument("--image", type=Path, default=CACHE / "build/qemu-virt/a733-mimus-usb.bin")
parser.add_argument("--evidence", type=Path, default=CACHE / "evidence" / time.strftime("mimus-usb-%Y%m%d-%H%M%S"))
parser.add_argument("--inputs", type=Path, required=True, help="Directory from prepare-mimus.py")
parser.add_argument("--wav", type=Path, action="append", help="Repeat for multiple recordings")
parser.add_argument("--reference", type=Path, action="append", required=True, help="Matching host records, in WAV order")
parser.add_argument("--finish", choices=("recover", "lease"), default="recover",
                    help="Return by command, or verify recovery after host requests stop")
args = parser.parse_args()
manifest = json.loads((args.inputs / "inputs.json").read_text())
models = [(args.inputs / name).read_bytes() for name in ("base.mimus", "overlay.mimus")]
for name, data in zip(("base.mimus", "overlay.mimus"), models):
    if hashlib.sha256(data).hexdigest() != manifest["model"][name]["sha256"]:
        sys.exit(f"Prepared model identity changed: {name}")
    if not 0 < len(data) <= 8 * 1024 * 1024:
        sys.exit("Model exceeds the native asset slot.")
wavs = args.wav or [Path(manifest["audio"])]
if len(wavs) != len(args.reference):
    sys.exit("Each WAV requires one matching host reference.")
cases = []
for wav, reference in zip(wavs, args.reference):
    audio = wav.read_bytes()
    if not 0 < len(audio) <= 2 * 1024 * 1024:
        sys.exit("WAV exceeds the native asset slot.")
    with wave.open(str(wav), "rb") as stream:
        if (stream.getnchannels(), stream.getsampwidth(), stream.getframerate(), stream.getcomptype()) != (1, 2, 16000, "NONE"):
            sys.exit("Expected uncompressed 16 kHz mono PCM16 WAV.")
    inputs = [*models, audio]
    bundle = bytearray(128)
    struct.pack_into('<8sQQQIII', bundle, 0, b'SUNMIM01', *map(len, inputs), *(zlib.crc32(data) for data in inputs))
    for data in inputs:
        bundle.extend(data)
        bundle.extend(b'\0' * (-len(bundle) % 64))
    bundle_hash = hashlib.sha256(bundle).hexdigest()
    host = json.loads(reference.read_text())
    if host.get('exit') != 0 or host['bundle_sha256'] != bundle_hash:
        sys.exit("Host reference failed or its inputs differ.")
    cases.append(dict(wav=str(wav.resolve()), audio_sha256=hashlib.sha256(audio).hexdigest(),
                      bundle_sha256=bundle_hash, reference=str(reference.resolve()), data=audio))
image = args.image.read_bytes()
if not image or len(image) > 0x100000:
    sys.exit("Runtime image exceeds its load interval.")
args.evidence.mkdir(parents=True, exist_ok=False)
record = dict(image_sha256=hashlib.sha256(image).hexdigest(),
              fel_sha256=hashlib.sha256(args.fel.read_bytes()).hexdigest(),
              transactions=[], cases=[], finish=args.finish, complete=False)
sequence = 0
device = None
spec = importlib.util.spec_from_file_location("access", ROOT / "tools/usb-access.py")
access = importlib.util.module_from_spec(spec)
spec.loader.exec_module(access)
sys.path.insert(0, str(Path.home() / "git/mimus/tools"))
import usb_guard


def save():
    (args.evidence / "session.json").write_text(json.dumps(record, indent=2) + "\n")


def fel(*arguments):
    if access.identity(node) != identity:
        raise RuntimeError("FEL node identity changed.")
    item = dict(protocol="fel", arguments=list(map(str, arguments)))
    record["transactions"].append(item)
    save()
    r = subprocess.run([str(args.fel), "-d", f"{int(node.parent.name)}:{int(node.name)}",
                        *map(str, arguments)], capture_output=True, text=True, timeout=30)
    item.update(exit=r.returncode, stdout=r.stdout, stderr=r.stderr)
    save()
    if r.returncode:
        raise RuntimeError(f"FEL request failed: {r.stdout}{r.stderr}")
    return r.stdout


def product_name():
    path = port / "product"
    return path.read_text().strip() if path.exists() else ""


def wait_service(previous_identity, native, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            candidate = access.session_node(port_config)
            product = product_name()
            if (candidate is not None and access.identity(candidate) != previous_identity
                    and os.access(candidate, os.R_OK | os.W_OK)
                    and (product == "sunstoneos RAM diagnostics") == native):
                return candidate
        except FileNotFoundError:
            pass
        time.sleep(0.05)
    raise RuntimeError("Expected USB service did not appear with access; no reconnect retry.")


def message(command, payload=b"", timeout=3000):
    global sequence
    if len(payload) > 2048:
        raise RuntimeError("Native request exceeds the protocol limit.")
    if access.identity(node) != identity:
        raise RuntimeError("Native USB instance changed; no further protocol requests.")
    sequence += 1
    packet = struct.pack("<4I", 0x53534f53, sequence, command, len(payload)) + payload
    item = dict(command=command, sequence=sequence, payload_size=len(payload), prefix=payload[:12].hex())
    with (args.evidence / 'native.jsonl').open('a') as journal:
        journal.write(json.dumps(dict(item, event='begin')) + '\n')
    try:
        if device.write(0x02, packet, timeout=timeout) != len(packet):
            raise RuntimeError("Native USB write was incomplete.")
        response = bytes(device.read(0x81, 4096, timeout=timeout))
        if len(response) < 16:
            raise RuntimeError("Native USB response lacks its header.")
        header = struct.unpack_from("<4I", response)
        if header != (0x53534f53, sequence, command | 0x80000000, len(response) - 16):
            raise RuntimeError(f"Unexpected native USB response: {header}")
        with (args.evidence / 'native.jsonl').open('a') as journal:
            journal.write(json.dumps(dict(item, event='complete', response_size=len(response)-16)) + '\n')
        return response[16:]
    except (OSError, RuntimeError, usb.core.USBError) as error:
        with (args.evidence / 'native.jsonl').open('a') as journal:
            journal.write(json.dumps(dict(item, event='failed', error=str(error))) + '\n')
        raise


def application(command, payload=b"", expected=0, timeout=3000):
    body = message(command, payload, timeout)
    if len(body) < 4 or struct.unpack_from('<I', body)[0] != expected:
        raise RuntimeError(f"Application response differs for {command:#x}: {body.hex()}")
    return body[4:]


def upload(slot, data):
    application(0x11, struct.pack('<3I', slot, len(data), zlib.crc32(data)))
    for offset in range(0, len(data), 2040):
        application(0x12, struct.pack('<2I', slot, offset) + data[offset:offset+2040])
    application(0x13, struct.pack('<I', slot), timeout=20000)
    print(f"Uploaded asset {slot}: {len(data)} bytes, CRC verified.", flush=True)


def transcribe(index, case):
    upload(2, case['data'])
    started = time.monotonic()
    application(0x14)
    # Recognition owns its asset buffers; concurrent writes and starts must fail.
    application(0x11, struct.pack('<3I', 2, 16, 0), expected=2)
    application(0x14, expected=2)
    deadline = time.monotonic() + 190
    while True:
        state, generation, length, valid = struct.unpack('<4I', application(0x10))
        if generation != index + 1 or valid != 7:
            raise RuntimeError("Application generation or committed assets differ.")
        if state in (2, 3):
            break
        if state != 1 or time.monotonic() > deadline:
            raise RuntimeError("Recognition did not finish before its deadline.")
        time.sleep(0.25)
    if not 0 < length <= 16384 - 257:
        raise RuntimeError("Invalid application result length.")
    log = bytearray()
    while len(log) < length:
        part = application(0x15, struct.pack('<I', len(log)))
        if not part or len(log) + len(part) > length:
            raise RuntimeError("Incomplete or oversized result chunk.")
        log.extend(part)
    directory = args.evidence / f'case-{index:03d}'
    directory.mkdir()
    (directory / 'kernel.log').write_bytes(log)
    candidate = {k: v for k, v in case.items() if k != 'data'}
    candidate.update(complete=state == 2, image_sha256=record['image_sha256'],
                     generation=generation, elapsed_host_seconds=time.monotonic()-started)
    (directory / 'session.json').write_text(json.dumps(candidate, indent=2) + '\n')
    result = subprocess.run([sys.executable, str(ROOT/'checks/mimus-parity.py'),
                             '--reference', case['reference'], '--candidate', str(directory/'session.json'),
                             '--output', str(directory/'parity.json')], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"Reference comparison failed: {result.stdout}{result.stderr}")
    record['cases'].append(candidate)
    save()
    print(f"PASS recording {index+1}: host parity, {candidate['elapsed_host_seconds']:.3f} s including model load and result retrieval.", flush=True)


try:
    usb_guard.check_health()
    with open("/tmp/mimus-board.lock", "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        node = access.discover()
        access.require_session(node)
        access.require_rom(node)
        identity = access.identity(node)
        port = access.target(node)
        if product_name() == "sunstoneos RAM diagnostics":
            raise RuntimeError("Runtime is already active; do not send it FEL requests.")
        port_config = dict(port=port.name, sysfs=str(port.resolve()))
        record.update(initial_node=str(node), initial_identity=identity, port=port.name)
        if "soc=00001903(A733)" not in fel("version"):
            raise RuntimeError("Unexpected target.")
        fel("read", "0x42000000", "128", args.evidence / "before.bin")
        if struct.unpack_from("<I", (args.evidence / "before.bin").read_bytes())[0] != 0x534b4552:
            raise RuntimeError("Establish the physical kernel check before native USB loading.")
        blank = bytearray(20480)
        struct.pack_into("<Q", blank, 80, 4)
        (args.evidence / "blank.bin").write_bytes(blank)
        fel("write", "0x41000000", args.image,
            "read", "0x41000000", len(image), args.evidence / "uploaded.bin")
        if (args.evidence / "uploaded.bin").read_bytes() != image:
            raise RuntimeError("Runtime upload differs; execution refused.")
        with usb_guard.reenumeration("sunstoneos-a733", "native USB transcription and host lease recovery", 4):
            # Preserve the prior result while waiting for the shared USB cooldown.
            fel("write", "0x42000000", args.evidence / "blank.bin")
            fel("reset64", "0x41000000")
            node = wait_service(identity, True, 10)
            identity = access.identity(node)
            access.require_session(node)
            record.update(runtime_node=str(node), runtime_identity=identity)
            save()
            device = usb.core.find(idVendor=0x1f3a, idProduct=0xefe8,
                                   bus=int(node.parent.name), address=int(node.name))
            if device is None or device.bcdDevice != 0x0100:
                raise RuntimeError("Device descriptor does not identify the native runtime.")
            device.set_configuration(1)
            usb.util.claim_interface(device, 0)
            status = struct.unpack("<5Q", message(1))
            record["status"] = dict(zip(("ticks", "CurrentEL", "commands", "speed", "counter"), status))
            if status[1] != 4:
                raise RuntimeError("Runtime is not executing at EL1.")
            application(0x14, expected=5)
            application(0x12, struct.pack('<2I', 2, 1) + b'x', expected=3)
            # A complete upload with a wrong CRC must not become usable.
            application(0x11, struct.pack('<3I', 2, 1, 0))
            application(0x12, struct.pack('<2I', 2, 0) + b'x')
            application(0x13, struct.pack('<I', 2), expected=4)
            for slot, data in enumerate(models):
                upload(slot, data)
            for index, case in enumerate(cases):
                transcribe(index, case)
            if args.finish == "recover":
                message(4)
            else:
                print("Waiting for the 30-second host lease to return the board to ROM.", flush=True)
            closing = time.monotonic()
            usb.util.release_interface(device, 0)
            usb.util.dispose_resources(device)
            device = None
            node = wait_service(identity, False, 40 if args.finish == "lease" else 10)
            record["return_wait_seconds"] = time.monotonic() - closing
            identity = access.identity(node)
            access.require_session(node)
            access.require_rom(node)
            record["returned_node"] = str(node)
            if "soc=00001903(A733)" not in fel("version"):
                raise RuntimeError("Returned device is not A733 FEL.")
            fel("read", "0x42000000", "20480", args.evidence / "result.bin")
        data = (args.evidence / "result.bin").read_bytes()
        result = struct.unpack_from("<16Q", data)
        record["result_header"] = result
        expected_status = 1 if args.finish == "lease" else 0
        if result[:3] != (0x534b4552, 5, expected_status):
            raise RuntimeError(f"Runtime did not return successfully: {result}")
        returned_log = data[256:256 + result[3]].decode(errors="replace")
        (args.evidence / "returned.log").write_text(returned_log)
        if args.finish == "lease" and ("FAIL USB host lease expired:" not in returned_log
                or not 25 <= record["return_wait_seconds"] <= 40):
            raise RuntimeError("Return was not the expected host lease expiry.")
        record["complete"] = True
        save()
        print(f"PASS: native asset loading, repeated transcription, host parity, and FEL return.\n{args.evidence}")
except (OSError, ValueError, RuntimeError, usb.core.USBError, subprocess.TimeoutExpired) as error:
    record["failure"] = str(error)
    save()
    sys.exit(f"Stopped on first failure: {error}\n{args.evidence}")

finally:
    if device is not None:
        usb.util.dispose_resources(device)
