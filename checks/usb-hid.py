#!/usr/bin/env python3
"""Check HID decode and public USB recordings offline; never opens USB devices."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'checks'))
import harness  # Refuse disabled assertions before any check can report success.
CAPTURES = {
    'microsoft-keyboard': 'd7afd68431bdfa003350d7f3559ecdde77e39647a93a4bd6d416d4ded9a9151f',
    'logitech-unifying': '689923eaf8dbb4af6b97cf2ce717c8cb798a8757689eb51be372ff459faa6c74',
}


def packets(data):
    """Read little-endian pcapng EPBs, with Darwin USB link type 266."""
    at = 0
    interfaces = []
    index = 0
    while at < len(data):
        kind, size = struct.unpack_from('<II', data, at)
        assert size >= 12 and size % 4 == 0 and at + size <= len(data)
        assert struct.unpack_from('<I', data, at + size - 4)[0] == size
        body = data[at + 8:at + size - 4]
        at += size
        if kind == 0x0a0d0d0a:
            assert body[:4] == b'\x4d\x3c\x2b\x1a'
            interfaces = []
        elif kind == 1:
            interfaces.append(struct.unpack_from('<H', body)[0])
        elif kind == 6:
            index += 1
            interface, _, _, captured, original = struct.unpack_from('<IIIII', body)
            assert interfaces[interface] == 266 and captured == original
            raw = body[20:20 + captured]
            assert len(raw) == captured and len(raw) >= 40 and raw[2] == 40
            yield index, raw


def extract(data):
    requests, descriptors, reports = {}, [], []
    for packet, raw in packets(data):
        transfer = int.from_bytes(raw[16:24], 'little')
        if raw[3] == 0 and raw[31] == 0 and raw[40:44] == b'\x81\x06\x00\x22':
            requests[transfer] = (packet, raw[44], int.from_bytes(raw[46:48], 'little'))
        elif raw[3] == 1 and transfer in requests:
            request, interface, length = requests.pop(transfer)
            assert int.from_bytes(raw[8:12], 'little') == 0
            assert int.from_bytes(raw[4:8], 'little') == len(raw) - 40
            assert len(raw[40:]) == length
            descriptors.append((packet, interface, raw[40:]))
        elif raw[3] == 1 and raw[31] == 3:
            assert int.from_bytes(raw[8:12], 'little') == 0
            assert int.from_bytes(raw[4:8], 'little') == len(raw) - 40
            reports.append((packet, raw[30], raw[40:]))
    return descriptors, reports


def signed(value, width):
    return value - (1 << width) if value & (1 << (width - 1)) else value


def reference(reports, pointer):
    """Independent fixed layouts from recorded descriptors, no parser constants."""
    result = [[1, 2 if pointer else 1, 0, 0, 0, 0]]
    held = set()
    for raw in reports:
        if pointer:
            assert len(raw) == 8 and raw[0] == 2
            value = int.from_bytes(raw[1:3], 'little')
            current = {i + 1 for i in range(16) if value >> i & 1}
            packed = int.from_bytes(raw[3:6], 'little')
            x, y = signed(packed & 4095, 12), signed(packed >> 12, 12)
            wheel = signed(raw[6], 8)
            assert raw[7] == 0  # Captured horizontal pan is zero; decoder omits pan.
            kind = 3
        else:
            assert len(raw) == 8
            current = {224 + i for i in range(8) if raw[0] >> i & 1}
            current.update(v for v in raw[2:] if v > 3)
            assert not any(1 <= v <= 3 for v in raw[2:])
            kind = 2
        for usage in sorted(held ^ current):
            result.append([kind, usage, int(usage in current), 0, 0, 0])
        if pointer and (x or y or wheel):
            result.append([4, 0, 0, x, y, wheel])
        held = current
    result.extend([kind, usage, 0, 0, 0, 0] for usage in sorted(held))
    result.append([5, 2 if pointer else 1, 0, 0, 0, 0])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--resume', action='store_true', help='Reuse successful commands recorded in this output directory')
    parser.add_argument('--captures', type=Path, required=True)
    parser.add_argument('--tinyusb', type=Path, default=Path.home()/'.cache/sunstoneos/sources/tinyusb-d668b0abb7219b9ce324fa88d46604bb9e869973')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=args.resume)
    inputs = {str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in [ROOT/'include/sunstone_usb_hid.h', ROOT/'drivers/usb/host/hid.c',
            *sorted((ROOT/'checks').glob('usb-hid*')), ROOT/'drivers/usb/host/tusb_config.h',
            args.tinyusb/'src/class/hid/hid_host.c'] if path.is_file()}
    inputs_path = out/'inputs.json'
    if args.resume and inputs_path.exists():
        assert json.loads(inputs_path.read_text()) == inputs, 'Changed inputs require a fresh output directory'
    inputs_path.write_text(json.dumps(inputs, indent=2)+'\n')
    clang = shutil.which('clang-21')
    assert clang
    flags = [clang, '-std=c11', '-g', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT/'include')]
    source = ROOT/'drivers/usb/host/hid.c'
    steps_path = out/'steps.json'
    steps = json.loads(steps_path.read_text()) if args.resume and steps_path.exists() else []

    def run(command, label):
        command = [str(v) for v in command]
        if args.resume and any(s['command'] == command and s['exit'] == 0 for s in steps):
            return (out/(label+'.log')).read_text()
        done = subprocess.run(command, capture_output=True, text=True)
        (out/(label+'.log')).write_text(done.stdout+done.stderr)
        steps.append({'label': label, 'command': command, 'exit': done.returncode})
        steps_path.write_text(json.dumps(steps, indent=2)+'\n')
        assert done.returncode == 0, (label, done.stderr)
        return done.stdout

    sanitize = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    run(flags+sanitize+[source, ROOT/'checks/usb-hid.c', '-o', out/'unit'], 'unit-build')
    run([out/'unit'], 'unit')
    run(flags+sanitize+['-Wno-unused-function', '-ffunction-sections', '-fdata-sections',
        '-I'+str(ROOT/'checks'), '-I'+str(ROOT/'drivers/usb/host'), '-I'+str(args.tinyusb/'src'),
        source, ROOT/'checks/usb-hid-tinyusb.c', args.tinyusb/'src/class/hid/hid_host.c',
        '-Wl,--gc-sections', '-o', out/'tinyusb'], 'tinyusb-build')
    run([out/'tinyusb'], 'tinyusb')
    run(flags+sanitize+[source, ROOT/'checks/usb-hid-replay.c', '-o', out/'replay'], 'replay-build')
    results = []
    for name, sha in CAPTURES.items():
        compressed = (args.captures/(name+'.pcapng.gz')).read_bytes()
        assert hashlib.sha256(compressed).hexdigest() == sha
        descriptors, traffic = extract(gzip.decompress(compressed))
        selected = [(0, 0x81, False)] if name == 'microsoft-keyboard' else [(0, 0x81, False), (1, 0x82, True)]
        for interface, endpoint, pointer in selected:
            packet, _, descriptor = next(v for v in descriptors if v[1] == interface)
            reports = [r for _, ep, r in traffic if ep == endpoint]
            fixture = struct.pack('<HH', len(descriptor), len(reports))+descriptor
            fixture += b''.join(struct.pack('<H', len(r))+r for r in reports)
            label = name+('-mouse' if pointer else '-keys')
            path = out/(label+'.bin')
            path.write_bytes(fixture)
            observed = json.loads(run([out/'replay', path], label))
            expected = reference(reports, pointer)
            assert observed == expected, label
            if pointer or name == 'microsoft-keyboard':
                assert len(observed) > 2  # Recorded transitions; receiver keyboard capture is idle.
            results.append({'fixture': label, 'descriptor_packet': packet, 'descriptor_bytes': len(descriptor),
                'descriptor_sha256': hashlib.sha256(descriptor).hexdigest(), 'reports': len(reports),
                'events': len(observed), 'capture_sha256': sha})
    result = {'result': 'PASS', 'hardware_access': False, 'recordings': results, 'steps': steps,
        'scope': 'Actual HID parser and pinned TinyUSB HID class with modeled host completion; public captured reports compared with independent fixed-layout decoders. No board, DMA, electrical, interrupt timing or physical hotplug qualification.'}
    (out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({'result': 'PASS', 'recordings': results}, indent=2))


if __name__ == '__main__':
    main()
