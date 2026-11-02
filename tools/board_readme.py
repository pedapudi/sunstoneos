#!/usr/bin/env python3
"""Write short board-image instructions from a build manifest; no launch admission."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex

ROOT = Path(__file__).resolve().parents[1]
FIRST_FAILURE = ('Read result.json first_failure.site and first_failure.detail, then breadcrumbs.json. '
                 'After watchdog-reset, use retained RTC breadcrumbs; DRAM results are invalid.')
EXPECTED = 'Normal ROM return, zero native status and output matching the declared expectation.'


def render(manifest, image=None, *, kind=None, expected=None, first_failure=None):
    """Return instructions bound to the exact image named by the build record."""
    manifest = Path(manifest).resolve()
    record = json.loads(manifest.read_text())
    launch = record.get('board_image', {})
    explicit_image = image is not None
    selected = image or launch.get('path') or record.get('elf') or 'build/probe.elf'
    image = Path(selected)
    if not image.is_absolute() and not explicit_image:
        image = manifest.parent/image
    image = image.resolve()
    digest = hashlib.sha256(image.read_bytes()).hexdigest()
    declared = launch.get('sha256') or record.get('image_sha256') or record.get('elf_sha256')
    if declared != digest:
        raise ValueError('Build manifest does not match the image SHA-256.')
    kind = kind or launch.get('kind') or ('cpu-return' if record.get('lifetime') == 'return' else 'probe')
    fallback = EXPECTED
    if record.get('lifetime') in ('supervised', 'resident'):
        fallback = 'refused-before-launch: this OS lifetime needs a finite watchdog and return contract.'
    expected = expected or launch.get('expected_result') or record.get('expected_result') or fallback
    decode = first_failure or launch.get('first_failure') or FIRST_FAILURE
    def manifest_path(value):
        path = Path(value)
        return str((path if path.is_absolute() else manifest.parent/path).resolve())
    command = [str(Path.home()/'.cache/sunstoneos/venv/bin/python3'), '-B', str(ROOT/'tools/board.py'),
               'submit', str(image), '--kind', kind, '--port', launch.get('port', 'PORT'),
               '--worker', 'image-builder', '--hypothesis', launch.get('hypothesis', expected),
               '--expect', launch.get('expect', 'EXPECTED_OUTPUT'),
               '--reference', launch.get('reference', 'docs/SUBSYSTEM.md#ANCHOR')]
    if launch.get('template'):
        command += ['--template', manifest_path(launch['template'])]
    for path in launch.get('inputs', []):
        command += ['--input', manifest_path(path)]
    return (f'# Board image\n\nPath: `{image}`\n\nSHA-256: `{digest}`\n\n'
            f'Command (replace PORT and EXPECTED_OUTPUT when present):\n\n```sh\n{shlex.join(command)}\n```\n\n'
            f'Expected result: {expected}\n\nFirst failure: {decode}\n')


def write(manifest, image=None, *, output=None, **options):
    manifest = Path(manifest)
    output = Path(output) if output else manifest.parent/'README.md'
    text = render(manifest, image, **options)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--image', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--kind', choices=('cpu-return', 'gpu-return', 'npu', 'probe', 'usb-class'))
    parser.add_argument('--expected')
    parser.add_argument('--first-failure')
    args = parser.parse_args()
    print(write(args.manifest, args.image, output=args.output, kind=args.kind,
                expected=args.expected, first_failure=args.first_failure))


if __name__ == '__main__':
    main()
