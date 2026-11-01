#!/usr/bin/env python3
"""Check HEVC CPU pixel comparison with real decoding and altered copied bytes.

The positive copies come from FFmpeg, so these cases qualify the comparator
only. They contain no native decoder output or physical device evidence.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

import harness


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--clip', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    path = Path(__file__).with_name('hevc-pixel-reference.py')
    spec = importlib.util.spec_from_file_location('hevc_pixel_reference', path)
    comparator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(comparator)
    reference_dir = args.output / 'reference'
    reference_dir.mkdir()
    reference = comparator.cpu_reference(args.clip, reference_dir, '/usr/bin/ffmpeg', '/usr/bin/ffprobe')
    size = comparator.PICTURE_BYTES
    copies = [reference[:size], reference[size:]]
    assert copies[0] != copies[1], 'fixture must distinguish the picture order'
    assert all(p['pixels_equal'] for p in comparator.compare(reference, copies))
    cases = ['CPU reference equality']
    files = [args.output / f'picture-{i}.nv12' for i in range(2)]
    for file, data in zip(files, copies):
        file.write_bytes(data)
    command = [sys.executable, '-B', str(path), '--clip', str(args.clip),
        '--picture0', str(files[0]), '--picture1', str(files[1])]
    def cli(label, passed):
        output = args.output / label
        result = subprocess.run([*command, '--output', str(output)], capture_output=True,
                                env=harness.environment())
        (args.output / (label + '.log')).write_bytes(result.stdout + result.stderr)
        assert (result.returncode == 0) == passed
        report = json.loads((output / 'result.json').read_text())
        assert report['pixels_equal'] == passed and report['physical_qualified'] is False
        for name in ('clip', 'picture0', 'picture1'):
            item = report['inputs'][name]
            assert comparator.identity(item['path'])['sha256'] == item['sha256']
        return report
    assert cli('cli-equal', True)['reference']['sha256'] == comparator.identity(reference_dir / 'reference.nv12')['sha256']
    cases.append('CLI freezes inputs and reports equality without physical qualification')
    for ordinal in range(2):
        for plane, offset in (('Y', 0), ('U', comparator.LUMA_BYTES), ('V', comparator.LUMA_BYTES + 1)):
            changed = [bytearray(data) for data in copies]
            changed[ordinal][offset] ^= 0xff
            result = comparator.compare(reference, changed)
            assert result[1 - ordinal]['pixels_equal'] and not result[ordinal]['pixels_equal']
            for metric in result[ordinal]['planes']:
                assert metric['mismatched_bytes'] == (1 if metric['plane'] == plane else 0)
                if metric['plane'] == plane:
                    assert metric['first_difference']['offset'] == offset
                    assert metric['maximum_absolute_error'] == abs(copies[ordinal][offset] - changed[ordinal][offset])
            cases.append(f'picture {ordinal} {plane} mutation')
    swapped = comparator.compare(reference, list(reversed(copies)))
    assert all(not p['pixels_equal'] for p in swapped)
    cases.append('swapped pictures')
    for data in (copies[0][:-1], copies[0] + b'\0'):
        harness.refused(lambda: comparator.compare(reference, [data, copies[1]]), 'NV12 bytes')
    for data in (reference[:-1], reference + copies[0]):
        harness.refused(lambda: comparator.compare(data, copies), 'two complete reference pictures')
    harness.refused(lambda: comparator.compare(reference, copies[:1]), 'two copies')
    cases.append('short or extra copy/reference bytes and missing picture')
    changed = bytearray(copies[0])
    changed[0] ^= 1
    files[0].write_bytes(changed)
    assert cli('cli-mismatch', False)['pictures'][0]['planes'][0]['mismatched_bytes'] == 1
    cases.append('CLI mismatch fails and preserves difference report')
    extra = args.output / 'four-pictures.hevc'
    extra.write_bytes(args.clip.read_bytes() * 2)
    extra_dir = args.output / 'extra-pictures'
    extra_dir.mkdir()
    harness.refused(lambda: comparator.cpu_reference(extra, extra_dir, '/usr/bin/ffmpeg', '/usr/bin/ffprobe'),
                    'exactly two complete VGA NV12 pictures')
    cases.append('CPU decoder extra pictures refused')
    small = args.output / 'small.hevc'
    comparator.run(['/usr/bin/ffmpeg', '-nostdin', '-v', 'error', '-f', 'lavfi', '-i',
        'testsrc2=size=64x64:rate=1', '-frames:v', '2', '-c:v', 'libx265', '-threads', '1',
        '-x265-params', 'pools=none:frame-threads=1:log-level=error', '-n', small], args.output, 'small-encode')
    small_dir = args.output / 'small-reference'
    small_dir.mkdir()
    harness.refused(lambda: comparator.cpu_reference(small, small_dir, '/usr/bin/ffmpeg', '/usr/bin/ffprobe'),
                    'one Main 8-bit 640x480 HEVC video stream')
    cases.append('wrong geometry refused')
    invalid = args.output / 'invalid.hevc'
    invalid.write_bytes(b'invalid HEVC stream')
    invalid_dir = args.output / 'invalid-reference'
    invalid_dir.mkdir()
    harness.refused(lambda: comparator.cpu_reference(invalid, invalid_dir, '/usr/bin/ffmpeg', '/usr/bin/ffprobe'),
                    'probe failed with exit')
    cases.append('failed probe refused')
    (args.output / 'result.json').write_text(json.dumps(dict(passed=True, physical_access=False,
        scope='Comparator validation with CPU-generated copies; no native decoding evidence.',
        cases=cases), indent=2) + '\n')
    print(f'PASS {len(cases)} HEVC pixel-comparison cases; CPU-generated copies only')


if __name__ == '__main__':
    main()
