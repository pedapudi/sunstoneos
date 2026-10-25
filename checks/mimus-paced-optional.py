#!/usr/bin/env python3
"""Check paced optional punctuation with real engine types and formatter, modeled recognition.

This checks the adapter and parity protocol only. Recognition results, capture
timing and scheduling are modeled; existing arithmetic evidence remains separate.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from harness import CACHE, CLANG, SANITIZE, environment

ROOT = Path(__file__).resolve().parents[1]
PIN = json.loads((ROOT / 'apps/mimus/source.json').read_text())
CASES = ('no-head-empty', 'no-head-scores', 'head', 'head-unscored',
         'head-misaligned', 'empty', 'short-pause', 'long-pause')
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--bundle', type=Path, default=CACHE / 'inputs/mimus-bench/inputs.bin')
parser.add_argument('--mimus', type=Path, default=Path(PIN['local_worktree']))
parser.add_argument('--revision', default=PIN['revision'], help='explicit engine revision; defaults to the application pin')
parser.add_argument('--library', type=Path, help='reuse the library built by an earlier invocation')
parser.add_argument('--case', choices=CASES, action='append')
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
commands = []


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(name, command, refusal=None):
    command = list(map(str, command))
    commands.append(command)
    (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    result = subprocess.run(command, cwd=ROOT, env=environment(), capture_output=True, text=True)
    (out / (name + '.log')).write_text(result.stdout + result.stderr)
    if refusal is None:
        assert result.returncode == 0, f'{name} failed; see {out / (name + ".log")}'
    else:
        assert result.returncode != 0 and refusal in result.stdout + result.stderr, name
    return result.stdout


head = run('engine-head', ['git', '-C', args.mimus, 'rev-parse', 'HEAD']).strip()
assert head == args.revision, 'Engine revision differs from the selected revision.'
assert not run('engine-clean', ['git', '-C', args.mimus, 'status', '--porcelain', '--', 'firmware', 'host'])
engine_root = args.mimus / 'firmware/components'
sources = [ROOT / 'apps/mimus/paced.cc', ROOT / 'checks/mimus-paced-optional.cc',
           engine_root / 'pipeline/src/formatter.cc']
library = args.library
if library is None:
    engine = out / 'engine'
    engine.mkdir()
    (engine / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.24)\nproject(paced_fixture LANGUAGES C CXX)\n'
        'set(CMAKE_CXX_STANDARD 23)\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\n'
        f'add_subdirectory("{engine_root / "asr"}" asr)\n')
    run('engine-configure', ['cmake', '-S', engine, '-B', engine / 'build',
                            '-DCMAKE_BUILD_TYPE=RelWithDebInfo', f'-DCMAKE_C_COMPILER={CLANG}',
                            f'-DCMAKE_CXX_COMPILER={CLANG.with_name("clang++-21")}'])
    run('engine-build', ['cmake', '--build', engine / 'build', '--target', 'mimus_asr', '-j', '4'])
    library = engine / 'build/asr/libmimus_asr.a'
else:
    previous = json.loads((library.parents[3] / 'engine-provenance.json').read_text())
    assert previous['mimus_revision'] == head and previous['library_sha256'] == digest(library)
(out / 'engine-provenance.json').write_text(json.dumps(dict(
    mimus_revision=head, library_sha256=digest(library), library=str(library)), indent=2) + '\n')

flags = ['-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *SANITIZE,
         '-fno-exceptions', '-fno-rtti', '-DSUNSTONE_SMP_NEWLIB=1', '-DSUNSTONE_MIMUS_NATIVE=1', '-Iinclude',
         '-Ichecks/bluetooth-mimus-fixture', f'-I{engine_root / "asr/include"}',
         f'-I{engine_root / "pipeline/include"}']
cxx = CLANG.with_name('clang++-21')
run('compile-paced', [cxx, *flags, '-c', sources[0], '-o', out / 'paced.o'])
undefined = run('symbols', ['nm', '-u', out / 'paced.o'])
symbols = [line.split()[-1] for line in undefined.splitlines() if 'Recognizer9Recognize' in line]
assert len(symbols) == 1, 'Resolve exactly the actual recognition call before wrapping it.'
symbol = symbols[0]
run('link', [cxx, *flags, f'-DWRAPPED_RECOGNIZE="__wrap_{symbol}"', sources[1], sources[2],
             out / 'paced.o', library, f'-Wl,--wrap={symbol}', '-o', out / 'fixture'])
provenance = dict(mimus_revision=head, library_sha256=digest(library), library=str(library),
                  bundle_sha256=digest(args.bundle), image_sha256=digest(out / 'fixture'),
                  sources={str(path): digest(path) for path in sources}, physical_access=False,
                  recognition='linker-wrapped deterministic results; arithmetic not executed')
(out / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
cases = args.case or CASES
for name in cases:
    run(name, [out / 'fixture', args.bundle, name, out / (name + '.txt')])
    if name == 'head-misaligned':
        continue
    text = (out / (name + '.txt')).read_text()
    reference = dict(exit=0, paced=True, stdout=text, bundle_sha256=provenance['bundle_sha256'])
    candidate = dict(exit=0, paced=True, stderr=text, bundle_sha256=provenance['bundle_sha256'],
                     image_sha256=provenance['image_sha256'])
    ref, cand = out / (name + '-reference.json'), out / (name + '-candidate.json')
    ref.write_text(json.dumps(reference) + '\n')
    cand.write_text(json.dumps(candidate) + '\n')
    command = [sys.executable, ROOT / 'checks/mimus-parity.py', '--reference', ref, '--candidate', cand,
               '--output', out / (name + '-parity.json')]
    run(name + '-parity', command)
    # A fabricated punctuation line, a missing score record and changed output
    # must each be rejected by the same checker used for native captures.
    changes = {'formatted': text.replace('FORMATTED ', 'FORMATTED changed ', 1)}
    if name.startswith('no-head'):
        changes['extra-marks'] = text.replace('TEXT ', 'MARKS 00\nTEXT ', 1)
        changes['extra-caps'] = text.replace('TEXT ', 'CAPS 00\nTEXT ', 1)
    if name == 'head':
        changes['missing-marks'] = text.replace('MARKS 13\n', '', 1)
    for mutation, changed in changes.items():
        cand.write_text(json.dumps({**candidate, 'stderr': changed}) + '\n')
        run(name + '-' + mutation, command, 'Paced boundaries, recognition, formatting, or completion differs.')
    cand.write_text(json.dumps(candidate) + '\n')
(out / 'result.json').write_text(json.dumps(dict(passed=True, cases=list(cases), **provenance), indent=2) + '\n')
print('PASS: paced optional punctuation, formatter output, alignment refusal and parity refusals.')
