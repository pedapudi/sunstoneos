#!/usr/bin/env python3
"""Freeze checked USB class images and their per-image coordinator instructions."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[2]
PROFILES = {
    'input': ('usb-input', 2, 'A USB boot keyboard through a USB-C to USB-A adapter. Grant 0x20 instead for a mouse, or 0x22 for both through a hub or combined receiver.',
              'Press and release A, or move the granted mouse. The application returns one 48-byte input event. No host desktop input is injected.'),
    'serial': ('usb-serial', 64, 'A CDC-ACM, CP210x, CH340/CH341, FT232/FT2232 or PL2303 adapter. Connect its TTL TX/RX to a pattern source and echo receiver with compatible voltage; connect common ground.',
               'The application configures 1000000 baud, 8N1 without flow control, echoes one received buffer and returns two equal nonzero byte counts. Sustained no-drop operation needs a separate physical traffic run.'),
    'camera': ('usb-camera', 128, 'A Logitech C270-class UVC webcam. The default selection prefers VGA YUYV.',
               'The application returns sequence, frame bytes, FNV-1a checksum and packed width/height. VGA YUYV contains 614400 bytes.'),
    'camera-jpeg': ('usb-camera', 128, 'An MJPEG-capable webcam and an independently admitted hardware JPEG session. This image explicitly requests MJPEG.',
                    'The application decodes one MJPEG frame with the A733 JPEG engine and returns its planar YU12 byte count and FNV-1a checksum. VGA output contains 460800 active bytes. Codec memory and power remain retained.'),
}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    revision = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
    sources = subprocess.check_output(['git', '-C', str(ROOT), 'status', '--porcelain'], text=True)
    records = {}
    for profile, (app, devices, accessory, expected) in PROFILES.items():
        build = args.build_root.resolve() / profile
        name = 'sunstone-a733-os-' + app
        image = build / (name + '.elf')
        check = build / 'os-image' / name / 'link-check.json'
        facts = json.loads(check.read_text())
        digest = hashlib.sha256(image.read_bytes()).hexdigest()
        require(facts.get('passed') and facts['image_sha256'] == digest, 'Stale or failed link check: ' + profile)
        require(facts['manifest']['name'] == app, 'Wrong application: ' + profile)
        allowed = facts['manifest']['devices_required'] | facts['manifest']['devices_optional']
        require(not devices & ~allowed, 'Missing class grant: ' + profile)
        with image.open('rb') as stream:
            elf = ELFFile(stream)
            table = elf.get_section_by_name('.symtab')
            symbols = {s.name for s in table.iter_symbols() if s['st_shndx'] != 'SHN_UNDEF'}
        required = {'os_usb_service_dispatch', 'a733_xhci_dma_acquire',
                    'sunstone_usb_hid_report', 'sunstone_uvc_payload_fragment',
                    'ss_usb_serial_rx_complete', 'a733_memory_uncached_prepare'}
        if profile == 'camera-jpeg':
            required |= {'ss_usb_jpeg_native_begin', 'a733_video_jpeg_poll', 'jpeg'}
        require(required <= symbols, 'Image omits selected class code: ' + ', '.join(sorted(required - symbols)))
        records[profile] = {'source': image, 'check': check, 'sha256': digest,
                            'application': app, 'devices': devices, 'accessory': accessory,
                            'expected': expected, 'build_id': facts['build_id']}
    args.output.mkdir(parents=True, exist_ok=False)
    for profile, info in records.items():
        folder = args.output / profile
        folder.mkdir()
        shutil.copy2(info['source'], folder / 'image.elf')
        shutil.copy2(info['check'], folder / 'link-check.json')
        import sys
        sys.path.insert(0, str(ROOT/'tools'))
        from board_readme import write
        write(folder/'link-check.json', image=folder/'image.elf', expected=info['expected'])
        info['source'] = str(info['source'])
        info['check'] = str(info['check'])
    (args.output / 'images.json').write_text(json.dumps({
        'git_revision': revision, 'source_status': sources, 'hardware_access': False,
        'images': records}, indent=2) + '\n')
    print(json.dumps({name: row['sha256'] for name, row in records.items()}, indent=2))


if __name__ == '__main__':
    main()
