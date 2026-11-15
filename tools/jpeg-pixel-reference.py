#!/usr/bin/env python3
"""Make the pixel reference for the 48-by-32 nonconstant JPEG fixture, and compare a copied board frame with it.

The fixture (apps/jpeg/fixture.py nonconstant) codes only coefficients whose inverse transform is exact, so every
correct baseline decoder gives the same samples whatever its IDCT. The reference is accepted only when the analytic
oracle, libjpeg's raw-data path (no upsampling, no colour conversion) with each of its three IDCTs, and FFmpeg's
MJPEG decoder with each listed IDCT all give identical bytes. Output is packed active YU12, as the OS decoder copies
it (apps/jpeg/native-shared.h jpeg_copy_rows): 48x32 Y, then 24x16 Cb, then 24x16 Cr, 2304 bytes.

libjpeg headers are not installed on the host; pass the include directory of the libjpeg-turbo development package
that matches the installed library (apt-get download libjpeg-turbo8-dev; dpkg-deb -x). No device is read.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import zlib

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
FRAME_BYTES = 48 * 32 + 2 * 24 * 16
FFMPEG_IDCTS = ('auto', 'int', 'simple', 'simplemmx', 'faani')
LIBJPEG_IDCTS = {0: 'JDCT_ISLOW', 1: 'JDCT_IFAST', 2: 'JDCT_FLOAT'}


def identity(path):
    data = Path(path).read_bytes()
    return dict(path=str(Path(path).resolve()), bytes=len(data), sha256=hashlib.sha256(data).hexdigest())


def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True)
    if result.returncode:
        raise ValueError(f'{command[0]} failed with exit {result.returncode}: {result.stderr.strip()}')
    return result.stdout


def reference(args):
    out = args.output
    out.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('jpeg_fixture', ROOT / 'apps/jpeg/fixture.py')
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    jpeg, oracle = fixture.nonconstant(), fixture.nonconstant_reference()
    profile = [p for p in fixture.profiles() if p['name'] == 'nonconstant'][0]
    if (hashlib.sha256(jpeg).hexdigest(), hashlib.sha256(oracle).hexdigest()) != \
            (profile['fixture_sha256'], profile['reference_sha256']) or len(oracle) != FRAME_BYTES:
        raise ValueError('fixture or analytic oracle differs from apps/jpeg/fixtures.json')
    (out / 'fixture.jpg').write_bytes(jpeg)
    (out / 'analytic.yu12').write_bytes(oracle)
    decoder = out / 'jpeg-raw-decode'
    run([args.cc, '-O1', '-Wall', '-Werror', '-I', args.libjpeg_include,
         '-I', args.libjpeg_include / 'x86_64-linux-gnu', ROOT / 'tools/jpeg-raw-decode.c', '-o', decoder, args.libjpeg])
    outputs = {}
    for method, name in LIBJPEG_IDCTS.items():
        path = out / f'libjpeg-{name}.yu12'
        run([decoder, out / 'fixture.jpg', path, method])
        outputs[f'libjpeg {name}'] = path
    for idct in FFMPEG_IDCTS:
        path = out / f'ffmpeg-{idct}.yu12'
        run([args.ffmpeg, '-nostdin', '-v', 'error', '-xerror', '-idct', idct, '-c:v', 'mjpeg', '-i', out / 'fixture.jpg',
             '-f', 'rawvideo', '-pix_fmt', 'yuvj420p', '-n', path])
        outputs[f'ffmpeg {idct}'] = path
    differing = [label for label, path in outputs.items() if path.read_bytes() != oracle]
    report = dict(fixture=identity(out / 'fixture.jpg'), reference=identity(out / 'analytic.yu12'),
                  reference_crc32=f'{zlib.crc32(oracle):08x}', decoders={k: identity(v) for k, v in outputs.items()},
                  tools=dict(libjpeg=identity(args.libjpeg), ffmpeg=identity(args.ffmpeg),
                             ffmpeg_version=run([args.ffmpeg, '-hide_banner', '-version']).splitlines()[0],
                             libjpeg_include=str(args.libjpeg_include), raw_decoder_source=identity(ROOT / 'tools/jpeg-raw-decode.c')),
                  agree=not differing, differing=differing)
    (out / 'report.json').write_text(json.dumps(report, indent=1) + '\n')
    if differing:
        raise ValueError('decoders differ from the analytic oracle: ' + ', '.join(differing))
    print(f"PASS {len(outputs)} decoder runs equal the analytic oracle: sha256 {report['reference']['sha256']}, "
          f"crc32 {report['reference_crc32']}")


def compare(args):
    frame, oracle = args.frame.read_bytes(), json.loads(args.report.read_text())
    expected = Path(oracle['reference']['path']).read_bytes()
    if hashlib.sha256(expected).hexdigest() != oracle['reference']['sha256']:
        raise ValueError('reference file changed since its report')
    if len(frame) != FRAME_BYTES:
        raise ValueError(f'board frame has {len(frame)} bytes, not {FRAME_BYTES}')
    planes = (('Y', 0, 48, 32), ('Cb', 1536, 24, 16), ('Cr', 1920, 24, 16))
    result = dict(frame=identity(args.frame), crc32=f'{zlib.crc32(frame):08x}', reference_crc32=oracle['reference_crc32'],
                  equal=frame == expected, planes={})
    for name, base, width, height in planes:
        diffs = [(i % width, i // width, frame[base + i], expected[base + i]) for i in range(width * height)
                 if frame[base + i] != expected[base + i]]
        result['planes'][name] = dict(differing=len(diffs), max_abs=max((abs(a - b) for *_, a, b in diffs), default=0),
                                      first=diffs[:8])
    print(json.dumps(result, indent=1))
    return 0 if result['equal'] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    make = sub.add_parser('reference', help='make and cross-check the reference')
    make.add_argument('--output', type=Path, required=True, help='new directory')
    make.add_argument('--libjpeg-include', type=Path, required=True)
    make.add_argument('--libjpeg', type=Path, default=Path('/usr/lib/x86_64-linux-gnu/libjpeg.so.8'))
    make.add_argument('--ffmpeg', type=Path, default=Path('/usr/bin/ffmpeg'))
    make.add_argument('--cc', default='cc')
    check = sub.add_parser('compare', help='compare a copied 2304-byte board frame with the reference')
    check.add_argument('--report', type=Path, required=True, help='report.json from the reference command')
    check.add_argument('--frame', type=Path, required=True)
    args = parser.parse_args()
    return reference(args) if args.command == 'reference' else compare(args)


if __name__ == '__main__':
    sys.exit(main() or 0)
