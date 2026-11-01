#!/usr/bin/env python3
"""Check exact FEL build destinations and pinned sources without executing FEL."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from harness import CACHE, CLANG

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from host_evidence import unique_directory

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
out = unique_directory(args.output)
requested = out / 'builds'
requested.mkdir()
(requested / 'earlier.txt').write_text('Preserve the previous build.\n')
source = json.loads((ROOT / 'sources.lock.json').read_text())['sunxi-tools']
include = CACHE / 'host/usr/include'
command = [sys.executable, '-B', str(ROOT / 'tools/build-sunxi-fel.py'),
           '--output', str(requested), '--cc', str(CLANG),
           'LIBUSB_CFLAGS=-I' + str(include / 'libusb-1.0'),
           'LIBUSB_LIBS=-l:libusb-1.0.so.0',
           'LIBFDT_CFLAGS=-I' + str(include),
           'LIBFDT_LIBS=-l:libfdt.so.1']
result = subprocess.run(command, capture_output=True, text=True)
(out / 'existing-output-refusal.log').write_text(result.stdout + result.stderr)
assert result.returncode != 0 and 'FileExistsError' in result.stderr, result.stdout + result.stderr
assert list(requested.iterdir()) == [requested / 'earlier.txt']
assert (requested / 'earlier.txt').read_text() == 'Preserve the previous build.\n'
directory = out / 'fresh-build'
command[command.index('--output') + 1] = str(directory)
result = subprocess.run(command, capture_output=True, text=True)
(out / 'build.log').write_text(result.stdout + result.stderr)
assert result.returncode == 0, result.stdout + result.stderr
record = json.loads((directory / 'build.json').read_text())
tool = Path(record['sunxi_fel']['path'])
assert tool.parent == directory / source['directory']
assert record['sunxi_fel']['sha256'] == hashlib.sha256(tool.read_bytes()).hexdigest()
assert record['archive']['sha256'] == source['sha256'] and record['executed'] is False
assert len(record['patches']) == 4 and 'recorded_sha256' not in record
before = {p.relative_to(directory): p.read_bytes() if p.is_file() else None for p in directory.rglob('*')}
result = subprocess.run(command, capture_output=True, text=True)
(out / 'completed-build-refusal.log').write_text(result.stdout + result.stderr)
assert result.returncode != 0 and 'FileExistsError' in result.stderr, result.stdout + result.stderr
assert {p.relative_to(directory): p.read_bytes() if p.is_file() else None for p in directory.rglob('*')} == before
bad_cache = out / 'bad-source'
bad_cache.mkdir()
(bad_cache / source['archive']).write_bytes(b'An unpinned source archive')
refused_output = out / 'must-not-exist'
result = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/build-sunxi-fel.py'),
                         '--cache', str(bad_cache), '--output', str(refused_output)],
                        capture_output=True, text=True)
(out / 'source-refusal.log').write_text(result.stdout + result.stderr)
assert result.returncode != 0 and 'differs from the sunxi-tools pin' in result.stderr
assert not refused_output.exists()
(out / 'result.json').write_text(json.dumps(dict(passed=True, builds=[str(directory)], existing_output_refused_without_changes=True,
    source_pin_refusal=True, executed_tool=False), indent=2) + '\n')
print('PASS: FEL builds refuse existing output, record actual hashes and reject unpinned source.')
