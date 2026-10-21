#!/usr/bin/env python3
"""Build and run the host warm-engine check, then compare both warm results with the host reference.

The mimus worktree named in apps/mimus/source.json must be at the pinned
revision with no local changes under firmware/ or host/. Unless --library is
given, the portable host engine library is built from that worktree into the
output directory, so the check cannot run against an older engine. The
revision and the library's SHA-256 are written to provenance.json.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'
PIN = json.loads((ROOT / 'apps/mimus/source.json').read_text())
MIMUS = Path(PIN['local_worktree'])
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--bundle', type=Path, default=CACHE / 'inputs/mimus-bench/inputs.bin')
parser.add_argument('--reference', type=Path, default=CACHE / 'evidence/mimus-host.json')
parser.add_argument('--library', type=Path,
                    help='prebuilt libmimus_asr.a; its provenance is recorded but not verified')
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
commands = []


def run(name, command):
    command = [str(part) for part in command]
    commands.append(command)
    (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                            env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1'})
    (out / f'{name}.log').write_text(result.stdout + result.stderr)
    if result.returncode:
        sys.exit(f'{name} failed; see {out / name}.log')
    return result


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


head = subprocess.run(['git', '-C', MIMUS, 'rev-parse', 'HEAD'], capture_output=True, text=True, check=True).stdout.strip()
dirty = subprocess.run(['git', '-C', MIMUS, 'status', '--porcelain', '--', 'firmware', 'host'],
                       capture_output=True, text=True, check=True).stdout
if head != PIN['revision'] or dirty:
    sys.exit(f'{MIMUS} is at {head}{" with local changes" if dirty else ""}; '
             f'apps/mimus/source.json pins {PIN["revision"]}')
library = args.library
if library is None:
    engine = out / 'engine'
    engine.mkdir(exist_ok=True)
    # Only the portable engine library, with the host project's C++ settings.
    (engine / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.24)\nproject(mimus_warm_engine LANGUAGES C CXX)\n'
        'set(CMAKE_CXX_STANDARD 23)\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\nset(CMAKE_CXX_EXTENSIONS OFF)\n'
        'add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror;-fno-exceptions;-fno-rtti>")\n'
        f'add_subdirectory({MIMUS / "firmware/components/asr"} asr)\n')
    run('engine-configure', ['cmake', '-S', engine, '-B', engine / 'build', '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
                             '-DCMAKE_C_COMPILER=/usr/bin/clang-21', '-DCMAKE_CXX_COMPILER=/usr/bin/clang++-21'])
    run('engine-build', ['nice', '-n', '10', 'cmake', '--build', engine / 'build', '--target', 'mimus_asr', '-j', '6'])
    library = engine / 'build/asr/libmimus_asr.a'
(out / 'provenance.json').write_text(json.dumps({
    'mimus_worktree': str(MIMUS), 'mimus_revision': head, 'library': str(library),
    'library_built_here': args.library is None, 'library_sha256': digest(library)}, indent=2) + '\n')


flags = ['-g', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer', '-Ichecks/bluetooth-mimus-fixture', '-Iinclude',
         f'-I{MIMUS}/firmware/components/asr/include', f'-I{MIMUS}/firmware/components/pipeline/include',
         '-DSUNSTONE_MIMUS_NATIVE=1', '-std=c++23', '-ffp-contract=off', '-fno-exceptions', '-fno-rtti']
objects = []
for index, source in enumerate(['apps/mimus/recorded.cc', 'checks/mimus-warm.cc',
                                'checks/audit-security-radio-mimus-minors-warm.cc']):
    objects.append(out / f'{index}.o')
    run(f'compile-{index}', ['/usr/bin/clang++-21', *flags, '-c', source, '-o', objects[-1]])
for name, driver in (('mimus-warm', objects[1]), ('mimus-warm-options', objects[2])):
    run(f'link-{name}', ['/usr/bin/clang++-21', '-fsanitize=address,undefined', objects[0], driver, library,
                         MIMUS / 'firmware/components/pipeline/src/formatter.cc', *flags[-4:],
                         f'-I{MIMUS}/firmware/components/pipeline/include', '-o', out / name])
print(run('warm', [out / 'mimus-warm', args.bundle, out / 'warm']).stdout, end='')
print(run('warm-options', [out / 'mimus-warm-options', args.bundle]).stdout, end='')
for name in ('first', 'second'):
    candidate = out / f'{name}.json'
    candidate.write_text(json.dumps({'exit': 0, 'bundle_sha256': digest(args.bundle),
                                     'image_sha256': 'host-warm-check',
                                     'stderr': (out / f'warm-{name}.log').read_text()}) + '\n')
    print(run(f'parity-{name}', [sys.executable, 'checks/mimus-parity.py', '--reference', args.reference,
                                 '--candidate', candidate, '--output', out / f'parity-{name}.json']).stdout, end='')
