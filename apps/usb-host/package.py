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
                    'ss_usb_serial_rx_complete', 'os_usb_memory_admitted'}
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
        jpeg = ' --service "5=$records/jpeg-session.bin"' if profile == 'camera-jpeg' else ''
        text = f'''# {info['application']} native image

SHA-256: `{info['sha256']}`

Source revision: `{revision}`. The linked image identity is `{info['build_id']}`.
The image passed the native link checks. It has not run on a board.

Attach to TYPEC24, the Type-C/DisplayPort host connector. Keep TYPEC16 connected
to the development host for board power and USB0/FEL. The FUSB302 and active-high
PL8 control the SY6280 VBUS switch from the board's 5 V supply.

{info['accessory']}

Expected result: {info['expected']}

## Prepare the coordinator's immutable session

The coordinator supplies independently reviewed records. The board prefix must
select host workload zero. The USB memory record grants exclusive use of
`0x47000000..0x48000000`; it is not the older image-RAM-only DDR record.
It also binds the fixed payload alias. Type-C/VBUS, PHY, clocks, source capacity,
storage preservation and supervised recovery need their existing board records.
The JPEG variant additionally needs the codec's isolation and power records.
No record is created by setting an evidence field to a nonzero value.

From the SunstoneOS checkout, set `records` to those reviewed inputs, `images`
to this package and `evidence` to a new cache directory. This command is offline:

```sh
py="$HOME/.cache/sunstoneos/venv/bin/python3"
"$py" -B apps/os/prepare.py --image "$images/{profile}/image.elf" \\
  --prefix "$records/board-prefix.bin" --service "1=$records/thermal.bin" \\
  --service "6=$records/usb-memory.bin"{jpeg} \\
  --devices {info['devices']:#x} --bind-sid --output "$evidence/{profile}-session"
```

Success reports `physical_access: false`, `admission_required: true` and the
sealed image/session file hashes. This package contains no electrical admission
and does not authorize a board launch.

## Board execution remains blocked

These are supervised images. They retain USB resources and provide no normal
ROM-return guarantee. The shared `tools/dev-run.py cpu-return` route requires
a returning image and must refuse them. A coordinator-owned supervised USB-class
launch/capture route is still required in that shared runner. Do not bypass it
with direct FEL commands. No executable board-launch command is claimed here.

Before qualification, attach the named accessory to TYPEC24, establish its
5 V/500 mA source authority and the new RAM reservation, and establish supervised
manual recovery for the selected board. A complete TYPEC24 disconnect revokes
the source session. Device hot-plug is implemented while the admitted Type-C
adapter or powered hub remains connected; a fresh Type-C attachment needs a new
admitted session. After a retained run, remove all board and externally supplied
accessory power before the coordinator restores ROM access. Preserve microSD.
'''
        (folder / 'README.md').write_text(text)
        info['source'] = str(info['source'])
        info['check'] = str(info['check'])
    (args.output / 'images.json').write_text(json.dumps({
        'git_revision': revision, 'source_status': sources, 'hardware_access': False,
        'images': records}, indent=2) + '\n')
    print(json.dumps({name: row['sha256'] for name, row in records.items()}, indent=2))


if __name__ == '__main__':
    main()
