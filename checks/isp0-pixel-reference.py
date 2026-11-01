#!/usr/bin/env python3
"""Generate an independent CPU NV16 reference and compare a recorded ISP frame.

FFmpeg supplies Bayer interpolation and BT.601 conversion. ISP600's mandatory
interpolation, rounding and range have not been established by the available
vendor source. A generated reference is a comparison input, not a claim of
hardware equivalence. The caller selects range and any accepted error budget
before capture; the checker never derives a tolerance from the observed output.
"""
import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PLANE = 640 * 480


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def reference(raw, output, value_range, ffmpeg=Path('/usr/bin/ffmpeg')):
    if len(raw) != 2 * PLANE:
        raise ValueError('BGGR12 input must contain 614400 bytes')
    values = [v[0] for v in struct.iter_unpack('<H', raw)]
    if max(values) > 4095:
        raise ValueError('BGGR12 containers must have zero upper four bits')
    manifest = json.loads((ROOT / 'drivers/camera/isp0-reference-sources.json').read_text())
    if digest(ffmpeg) != manifest['tool']['sha256']:
        raise ValueError('FFmpeg binary differs from the pinned CPU reference')
    output.mkdir(parents=True, exist_ok=False)
    source = output / 'input.bggr16'
    # FFmpeg interprets sixteen-bit Bayer at full scale. Extend the twelve-bit
    # samples to sixteen bits without changing their Bayer positions.
    source.write_bytes(struct.pack('<' + 'H' * PLANE, *(v << 4 for v in values)))
    planar = output / 'reference.yuv422p'
    command = [str(ffmpeg), '-hide_banner', '-nostdin', '-loglevel', 'error', '-n',
               '-f', 'rawvideo', '-pixel_format', 'bayer_bggr16le', '-video_size', '640x480',
               '-framerate', '1', '-i', str(source), '-frames:v', '1', '-threads', '1',
               '-vf', f'format=rgb24,scale=in_range=full:out_range={value_range}:out_color_matrix=bt601:flags=bitexact+bilinear+accurate_rnd:sws_dither=none',
               '-pix_fmt', 'yuv422p', '-f', 'rawvideo', str(planar)]
    result = subprocess.run(command, capture_output=True, text=True)
    (output / 'ffmpeg.log').write_text(result.stdout + result.stderr)
    result.check_returncode()
    data = planar.read_bytes()
    if len(data) != 2 * PLANE:
        raise ValueError('CPU reference produced an unexpected extent')
    nv16 = bytearray(data[:PLANE]) + bytearray(PLANE)
    nv16[PLANE::2] = data[PLANE:PLANE + PLANE // 2]
    nv16[PLANE + 1::2] = data[PLANE + PLANE // 2:]
    target = output / 'reference.nv16'
    target.write_bytes(nv16)
    libraries = subprocess.check_output(['ldd', str(ffmpeg)], text=True)
    library_hashes = {word: digest(word) for word in libraries.split()
                      if word.startswith('/') and Path(word).is_file()}
    report = dict(input_sha256=hashlib.sha256(raw).hexdigest(), reference_sha256=digest(target),
                  ffmpeg_sha256=digest(ffmpeg), libraries=library_hashes, command=command,
                  geometry=dict(width=640, height=480, format='NV16', plane_bytes=PLANE),
                  matrix='BT.601', intermediate='RGB24', value_range=value_range, physical_access=False,
                  isp600_equivalence=False, numerical_qualified=False)
    (output / 'reference.json').write_text(json.dumps(report, indent=2) + '\n')
    return bytes(nv16), report


def compare(expected, actual, max_error):
    if len(actual) != len(expected) or len(expected) != 2 * PLANE:
        raise ValueError('NV16 comparison requires two complete 614400-byte frames')
    if not 0 <= max_error <= 255:
        raise ValueError('Maximum per-component error must be between 0 and 255')
    planes = {}
    for name, start, stop, stride in (('Y', 0, PLANE, 1), ('U', PLANE, 2 * PLANE, 2),
                                       ('V', PLANE + 1, 2 * PLANE, 2)):
        errors = [abs(expected[i] - actual[i]) for i in range(start, stop, stride)]
        planes[name] = dict(max_error=max(errors), mean_error=sum(errors) / len(errors),
                            mismatched=sum(e > max_error for e in errors), samples=len(errors))
    return dict(passed=all(p['mismatched'] == 0 for p in planes.values()), planes=planes,
                max_error_budget=max_error, actual_sha256=hashlib.sha256(actual).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--range', dest='value_range', choices=('full', 'limited'), required=True)
    parser.add_argument('--actual', type=Path)
    parser.add_argument('--max-error', type=int, default=0)
    args = parser.parse_args()
    expected, report = reference(args.input.read_bytes(), args.output, args.value_range)
    if args.actual:
        report['comparison'] = compare(expected, args.actual.read_bytes(), args.max_error)
        (args.output / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')
        if not report['comparison']['passed']:
            raise SystemExit('FAIL ISP0 output differs from the declared CPU reference')
    print('PASS independent CPU reference generated' + (' and compared' if args.actual else ''))


if __name__ == '__main__':
    main()
