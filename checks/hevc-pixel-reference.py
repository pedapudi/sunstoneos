#!/usr/bin/env python3
"""Compare two copied VGA NV12 pictures with FFmpeg's software HEVC decoder.

This offline comparison reads no device. Equality establishes only agreement
with the CPU decoder; launch, DMA visibility, drain and recovery need separate
evidence. Clip and copy hashes join this report to the authenticated capture.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import harness

PICTURE_BYTES = 640 * 480 * 3 // 2
LUMA_BYTES = 640 * 480


def identity(path):
    data = Path(path).read_bytes()
    return dict(path=str(Path(path).resolve()), bytes=len(data),
                sha256=hashlib.sha256(data).hexdigest())


def run(command, output, label):
    result = subprocess.run(list(map(str, command)), capture_output=True,
                            env=harness.environment())
    (output / (label + '.stdout')).write_bytes(result.stdout)
    (output / (label + '.stderr')).write_bytes(result.stderr)
    (output / (label + '-command.json')).write_text(json.dumps(list(map(str, command)), indent=2) + '\n')
    if result.returncode:
        raise ValueError(f'{label} failed with exit {result.returncode}; inspect its retained stderr')
    return result.stdout


def cpu_reference(clip, output, ffmpeg, ffprobe):
    metadata = json.loads(run([ffprobe, '-v', 'error', '-select_streams', 'v',
        '-show_entries', 'stream=codec_name,profile,width,height,pix_fmt',
        '-of', 'json', clip], output, 'probe'))
    expected = dict(codec_name='hevc', profile='Main', width=640, height=480, pix_fmt='yuv420p')
    if metadata.get('streams') != [expected]:
        raise ValueError('reference requires one Main 8-bit 640x480 HEVC video stream')
    path = output / 'reference.nv12'
    run([ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'error', '-xerror',
        '-hwaccel', 'none', '-c:v', 'hevc', '-threads', '1', '-i', clip,
        '-map', '0:v:0', '-an', '-sn', '-dn', '-fps_mode', 'passthrough',
        '-pix_fmt', 'nv12', '-threads', '1', '-f', 'rawvideo', '-n', path], output, 'decode')
    reference = path.read_bytes()
    if len(reference) != 2 * PICTURE_BYTES:
        raise ValueError('CPU decoder must produce exactly two complete VGA NV12 pictures')
    return reference


def compare(reference, copies):
    if len(reference) != 2 * PICTURE_BYTES or len(copies) != 2:
        raise ValueError('comparison requires two complete reference pictures and two copies')
    pictures = []
    for ordinal, copied in enumerate(copies):
        if len(copied) != PICTURE_BYTES:
            raise ValueError(f'picture {ordinal} copy must contain {PICTURE_BYTES} NV12 bytes')
        expected = reference[ordinal * PICTURE_BYTES:(ordinal + 1) * PICTURE_BYTES]
        planes = []
        # NV12 stores U and V in alternating bytes after the luma plane.
        for name, first, end, step in (('Y', 0, LUMA_BYTES, 1),
                ('U', LUMA_BYTES, PICTURE_BYTES, 2), ('V', LUMA_BYTES + 1, PICTURE_BYTES, 2)):
            mismatches = 0
            maximum = 0
            first_difference = None
            for offset in range(first, end, step):
                difference = abs(copied[offset] - expected[offset])
                if difference:
                    mismatches += 1
                    maximum = max(maximum, difference)
                    if first_difference is None:
                        first_difference = dict(offset=offset, expected=expected[offset], actual=copied[offset])
            planes.append(dict(plane=name, mismatched_bytes=mismatches,
                maximum_absolute_error=maximum, first_difference=first_difference))
        pictures.append(dict(ordinal=ordinal, pixels_equal=all(p['mismatched_bytes'] == 0 for p in planes),
            reference_sha256=hashlib.sha256(expected).hexdigest(),
            copy_sha256=hashlib.sha256(copied).hexdigest(), planes=planes))
    return pictures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('clip', 'picture0', 'picture1', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--ffmpeg', type=Path, default=Path('/usr/bin/ffmpeg'))
    parser.add_argument('--ffprobe', type=Path, default=Path('/usr/bin/ffprobe'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    report = dict(pixels_equal=False, physical_qualified=False,
        scope='Offline CPU pixel comparison only; match input hashes to the authenticated capture separately.')
    try:
        frozen = {}
        report['inputs'] = {}
        for name, filename in (('clip', 'input.h265'), ('picture0', 'picture-0.nv12'), ('picture1', 'picture-1.nv12')):
            source = getattr(args, name)
            frozen[name] = args.output / filename
            frozen[name].write_bytes(source.read_bytes())
            report['inputs'][name] = {**identity(frozen[name]), 'source': str(source.resolve())}
        report['comparator'] = identity(Path(__file__))
        report['tools'] = {}
        for name in ('ffmpeg', 'ffprobe'):
            tool = getattr(args, name)
            report['tools'][name] = {**identity(tool), 'version':
                run([tool, '-version'], args.output, name + '-version').decode().splitlines()[0]}
        reference = cpu_reference(frozen['clip'], args.output, args.ffmpeg, args.ffprobe)
        report['reference'] = identity(args.output / 'reference.nv12')
        report['pictures'] = compare(reference, [frozen['picture0'].read_bytes(), frozen['picture1'].read_bytes()])
        report['pixels_equal'] = all(p['pixels_equal'] for p in report['pictures'])
    except (OSError, ValueError) as error:
        report['error'] = str(error)
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    if not report['pixels_equal']:
        raise SystemExit('FAIL HEVC pixel comparison; inspect result.json and retained logs')
    print('PASS both HEVC NV12 pictures equal the software CPU reference; physical qualification remains separate')


if __name__ == '__main__':
    main()
