#!/usr/bin/env python3
"""Compare the decoder's JPEG programming and reset with the pinned vendor library instructions, for the board fixture.

The vendor's AArch64 libraries (libawmjpeg.so, libVE.so, libvideoengine.so from the pinned CedarC package, under
~/.cache/sunstoneos/sources/video-codecs/runtime) run in Unicorn with service stubs; no codec hardware is emulated.
For the 48-by-32 nonconstant fixture that the board decodes (apps/jpeg/fixture.py), the check compares:

- the quantization writes, Huffman SRAM words, frame size, restart and format with the native parser and table builder;
- the complete register image before the trigger, the trigger byte and the completion handling with the native
  executor's write trace (checks/a733-video-jpeg-run.c);
- the reset and default writes of each admitted compound identity with the native platform binding's trace
  (checks/a733-video-platform.c) against libVE's VeReset and the kernel's reset sequence;
- the codec selection and register routing of the admitted identities.

The comparison scripts are in checks/video-reference/. The register trace is the programming reference; the pixel
reference is tools/jpeg-pixel-reference.py.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys

from harness import CLANG, SANITIZE, environment

sys.dont_write_bytecode = True
GATE_RUNS = [['--output', '{output}']]
CHECK_INPUTS = ['checks/video-reference/jpeg.py', 'checks/video-reference/reset.py', 'checks/video-reference/routing.py',
                'checks/a733-video-jpeg-run.c', 'checks/a733-video-platform.c', 'checks/register_latency.h',
                'drivers/video/a733_video_jpeg.c', 'drivers/video/a733_video_jpeg_run.c',
                'drivers/video/a733_video_platform.c', 'drivers/video/a733_video_unit.c',
                'drivers/gpu/a733_gpu_resources.c', 'drivers/clock/a733_clock_leases.c',
                'apps/jpeg/fixture.py', 'apps/jpeg/fixtures.json']
ROOT = Path(__file__).resolve().parents[1]
LIBRARIES = Path.home() / '.cache/sunstoneos/sources/video-codecs/runtime/usr/lib/aarch64-linux-gnu'
PINS = {'libawmjpeg.so': 'c6c0fff04dcddc633544831430727b445a3f1fe4873c881a16aeaed958513e65',
        'libVE.so': '3af49a5eb24f6832518a858274fb206d60e490985bff02f5d5be36d053f65be4',
        'libvideoengine.so': 'abfea1fa75f6a3ca5413d16eb69fcb87692f3e3b6552d83a222a998e909cef82'}
PLATFORM = ['drivers/video/a733_video_platform.c', 'drivers/video/a733_video_unit.c', 'drivers/video/a733_video_jpeg.c',
            'drivers/video/a733_video_jpeg_run.c', 'drivers/gpu/a733_gpu_resources.c', 'drivers/clock/a733_clock_leases.c']

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
log = []


def run(label, command):
    command = list(map(str, command))
    result = subprocess.run(command, cwd=out, env=environment(), capture_output=True, text=True)
    (out / (label + '.log')).write_text(result.stdout + result.stderr)
    log.append(dict(label=label, command=command, returncode=result.returncode))
    assert result.returncode == 0, (label, result.stdout[-2000:], result.stderr[-2000:])
    return result.stdout


for name, digest in PINS.items():
    assert hashlib.sha256((LIBRARIES / name).read_bytes()).hexdigest() == digest, name
spec = importlib.util.spec_from_file_location('jpeg_fixture', ROOT / 'apps/jpeg/fixture.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
jpeg = out / 'nonconstant.jpg'
jpeg.write_bytes(fixture.nonconstant())
profile = [p for p in fixture.profiles() if p['name'] == 'nonconstant'][0]
assert hashlib.sha256(jpeg.read_bytes()).hexdigest() == profile['fixture_sha256']

flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-I', ROOT / 'include', '-I', ROOT / 'checks']
run('native-library', [CLANG, *flags, '-shared', '-fPIC', ROOT / 'drivers/video/a733_video_jpeg.c', '-o', out / 'native-jpeg.so'])
run('executor-build', [CLANG, *flags, *SANITIZE, ROOT / 'checks/a733-video-jpeg-run.c', ROOT / 'drivers/video/a733_video_jpeg.c',
                       ROOT / 'drivers/video/a733_video_jpeg_run.c', '-o', out / 'executor-model'])
run('executor-trace', [out / 'executor-model', jpeg, out / 'executor-trace.bin'])
run('platform-build', [CLANG, *flags, *SANITIZE, *[ROOT / s for s in PLATFORM], ROOT / 'checks/a733-video-platform.c',
                       '-o', out / 'platform-model'])
(out / 'platform-traces').mkdir()
run('platform-trace', [out / 'platform-model', jpeg, out / 'platform-traces'])

python = sys.executable
reference = ROOT / 'checks/video-reference'
results = {
    'programming': run('jpeg-programming', [python, '-B', reference / 'jpeg.py', out / 'native-jpeg.so',
                                            LIBRARIES / 'libawmjpeg.so', jpeg, out / 'programming',
                                            '--executor-trace', out / 'executor-trace.bin']).strip(),
    'tables': run('jpeg-tables', [python, '-B', reference / 'jpeg.py', out / 'native-jpeg.so', LIBRARIES / 'libawmjpeg.so',
                                  jpeg, out / 'tables']).strip(),
    'reset': run('reset', [python, '-B', reference / 'reset.py', LIBRARIES, out / 'platform-traces', out / 'reset']).strip(),
    'routing': run('routing', [python, '-B', reference / 'routing.py', LIBRARIES, out / 'routing']).strip(),
}
assert all(line.startswith('PASS') for line in results.values()), results
programming = json.loads((out / 'programming/executor-native.json').read_text())
record = dict(fixture=dict(sha256=profile['fixture_sha256'], bytes=profile['fixture_bytes']),
              libraries={n: d for n, d in PINS.items()}, results=results,
              native_writes=len(programming),
              trigger=[w for w in programming if w[0] == 0x118],
              inputs={name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in CHECK_INPUTS},
              compiler=dict(path=str(CLANG), sha256=hashlib.sha256(Path(CLANG).read_bytes()).hexdigest()), commands=log)
(out / 'result.json').write_text(json.dumps(record, indent=1) + '\n')
shutil.copy2(jpeg, out / 'programming' / 'fixture.jpg')
print(f'PASS vendor programming matches for the board fixture: {len(programming)} native writes with the complete '
      'pre-trigger register image and trigger, tables, reset for three identities and register routing')
