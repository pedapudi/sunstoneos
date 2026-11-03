#!/usr/bin/env python3
"""Check that the gate yields the disk and CPUs to the board runner, and that every run records the host's pressure.

Gate sweeps (the fat32-rw power-loss sweep wrote 271 GiB of copies of a 52 MiB image) saturated the disk and slowed the host stages
of board runs ten to twenty times. tools/host_priority.py sets the priorities; this check requires:
  gate      a process that calls lower() has nice +10 and the idle I/O class, and a child it starts inherits both;
  runner    a process that calls raise_runner() has the best-effort I/O class at level 0 and keeps its niceness;
  wiring    tools/integration-gate.py and tools/land.py call lower() first in main(), tools/board.py calls raise_runner() first in
            main(), so a restart of the daemon or a new gate keeps them;
  pressure  pressure() reads the kernel's figures into some/full records with avg10, avg60, avg300 and total, for a sample file text;
            a result written by board.finish_result carries host_pressure.start and host_pressure.end;
  copies    harness.copy_image makes a sparse copy of a mostly empty image: the copy equals the source and allocates under 1% of it.
Usage: host-priority.py --output DIR
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest.mock import patch

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
CHECK_INPUTS = ['tools/host_priority.py', 'tools/board.py', 'tools/integration-gate.py', 'tools/land.py', 'checks/harness.py']
CHILD = """
import os, subprocess, sys
sys.path.insert(0, os.environ['TOOLS'])
import host_priority as h
getattr(h, os.environ['FUNCTION'])()
print(os.nice(0), *h.io_priority())
print(subprocess.run([sys.executable, '-c', 'import os,sys; sys.path.insert(0, os.environ["TOOLS"]); import host_priority as h; '
                      'print(os.nice(0), *h.io_priority())'], capture_output=True, text=True).stdout.strip())
"""


def run_child(function):
    out = subprocess.run([sys.executable, '-c', CHILD], capture_output=True, text=True, check=True,
                         env={**os.environ, 'TOOLS': str(ROOT / 'tools'), 'FUNCTION': function})
    return [tuple(int(x) for x in line.split()) for line in out.stdout.split('\n') if line.strip()]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output
    out.mkdir(parents=True, exist_ok=True)
    import host_priority
    import board
    parent_nice = subprocess.run([sys.executable, '-c', 'import os; print(os.nice(0))'], capture_output=True, text=True).stdout.strip()
    own, inherited = run_child('lower')
    assert own[1] == 3 and own[0] == min(int(parent_nice) + 10, 19) and inherited == own, (own, inherited)   # nice stops at 19: the gate itself runs at 10
    own, inherited = run_child('raise_runner')
    assert own[1:] == (2, 0) and own[0] == int(parent_nice) and inherited == own, (own, inherited)
    for name, call in (('tools/integration-gate.py', 'host_priority.lower()'), ('tools/land.py', 'host_priority.lower()'),
                       ('tools/board.py', 'host_priority.raise_runner()')):
        text = (ROOT / name).read_text()
        body = text[text.index('\ndef main(argv=None):') + 1:].split('\n', 2)[1]
        assert body.strip().startswith(call), (name, 'the first statement of main() is not ' + call)
    sample = 'some avg10=1.50 avg60=2.25 avg300=3.00 total=1234\nfull avg10=0.50 avg60=1.00 avg300=1.50 total=99\n'
    with tempfile.TemporaryDirectory() as scratch:
        real_open = open

        def fake(path, *args, **kwargs):
            if str(path).startswith('/proc/pressure/'):
                return real_open(Path(scratch) / 'sample', *args, **kwargs)
            return real_open(path, *args, **kwargs)
        (Path(scratch) / 'sample').write_text(sample)
        with patch('builtins.open', fake):
            figures = host_priority.pressure()
    assert set(figures) == {'io', 'cpu', 'memory'} and figures['io']['some'] == {'avg10': 1.5, 'avg60': 2.25, 'avg300': 3.0, 'total': 1234}
    assert figures['io']['full']['total'] == 99
    result = {'port': '3-1', 'kind': 'probe', 'outcome': 'returned', 'durations': {}, 'host_pressure': {'start': figures}}
    evidence = out / 'evidence'
    evidence.mkdir()
    import time
    summary = board.finish_result(result, evidence, time.monotonic(), 'done')
    saved = json.loads((evidence / 'result.json').read_text())
    assert saved['host_pressure']['start'] == figures and set(saved['host_pressure']['end']) <= {'io', 'cpu', 'memory'}, saved['host_pressure']
    assert summary['host_pressure']['end'].keys() == host_priority.pressure().keys()
    image = out / 'image.bin'
    with image.open('wb') as stream:
        stream.truncate(8 << 20)
        stream.seek(4096)
        stream.write(b'data' * 1024)
    copy = out / 'copy.bin'
    harness.copy_image(image, copy)
    assert copy.read_bytes() == image.read_bytes()
    assert copy.stat().st_blocks * 512 < copy.stat().st_size // 100, copy.stat()
    print('PASS host priority: the gate runs at nice +10 and the idle I/O class and its children inherit them; the runner and daemon take '
          'best-effort level 0; the three entry points set them first; results carry host_pressure at the start and the end; '
          'image copies are sparse')


if __name__ == '__main__':
    main()
