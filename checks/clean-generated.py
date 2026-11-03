#!/usr/bin/env python3
"""Check the safety rules of tools/clean-generated.py in a scratch home directory.

The tool deletes generated files only under the build cache, the trash directory and session scratch directories, and only with --apply.
It runs here with HOME set to a scratch directory that holds a miniature cache, and the check requires:
  dry run     without --apply nothing is deleted, and the report says what would be;
  allowed     an old, unreferenced build directory is deleted with --apply, by `build`, `path` and `stale-builds`, and `trash` empties the trash;
  recent      a path changed within 6 hours is refused unless --recent is given;
  referenced  a build directory that an evidence record or a queue database names is refused, by name and by `stale-builds`, as are the
              pinned builds (sunxi-a733, a733-ddr, qemu-virt);
  protected   evidence, sources, host, checkpoints, inputs, venv and bin are refused, as are queue databases and tokens;
  roots       a path outside the allowed roots, an allowed root itself, and a symbolic link out of the build directory are refused.
Usage: clean-generated.py --output DIR
"""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import time

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / 'tools/clean-generated.py'
CHECK_INPUTS = ['tools/clean-generated.py']
OLD = time.time() - 30 * 86400


def make(path, old=True, size=1000):
    path.mkdir(parents=True, exist_ok=True)
    (path / 'file.bin').write_bytes(bytes(size))
    if old:
        for p in [path / 'file.bin', path]:
            os.utime(p, (OLD, OLD))
    return path


def run(home, *arguments):
    done = subprocess.run([sys.executable, '-B', str(TOOL), *arguments], env=dict(os.environ, HOME=str(home)),
                          capture_output=True, text=True)
    return done.returncode, done.stdout + done.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output
    home = out / 'home'
    store = home / '.cache/sunstoneos'
    build = store / 'build'
    passed = []
    # dry run and allowed deletions
    make(build / 'old-a')
    make(build / 'old-b')
    make(build / 'old-c')
    make(build / 'old-d')
    code, text = run(home, 'build', 'old-a')
    assert 'would delete' in text and 'dry run' in text and (build / 'old-a').exists(), text
    passed.append('dry run deletes nothing')
    code, text = run(home, 'build', 'old-a', '--apply')
    assert not (build / 'old-a').exists() and 'delete' in text, text
    code, text = run(home, 'path', str(build / 'old-b' / 'file.bin'), '--apply')
    assert not (build / 'old-b/file.bin').exists(), text
    code, text = run(home, 'stale-builds', '7', '--apply')
    assert not (build / 'old-c').exists() and not (build / 'old-d').exists(), text
    make(store / 'trash/x')
    make(store / 'trash/y')
    code, text = run(home, 'trash', '--apply')
    assert not list((store / 'trash').iterdir()), text
    passed.append('build, path, stale-builds and trash delete old unreferenced files with --apply')
    # recent
    make(build / 'fresh', old=False)
    code, text = run(home, 'build', 'fresh', '--apply')
    assert (build / 'fresh').exists() and 'changed within the last 6 hours' in text, text
    code, text = run(home, 'build', 'fresh', '--apply', '--recent')
    assert not (build / 'fresh').exists(), text
    passed.append('a recent path is refused unless --recent')
    # referenced by evidence, by a queue database, and pinned
    make(build / 'cited')
    make(build / 'queued')
    for pinned in ('sunxi-a733', 'a733-ddr', 'qemu-virt'):
        make(build / pinned)
    (store / 'evidence/run1').mkdir(parents=True)
    (store / 'evidence/run1/result.json').write_text(json.dumps({'image': str(build / 'cited/image.elf')}))
    connection = sqlite3.connect(store / 'queue.sqlite3')
    connection.execute('CREATE TABLE tests (submission TEXT, review TEXT)')
    connection.execute('INSERT INTO tests VALUES (?, ?)', (str(build / 'queued/session.bin'), None))
    connection.commit()
    connection.close()
    for name in ('cited', 'queued', 'sunxi-a733', 'a733-ddr', 'qemu-virt'):
        code, text = run(home, 'build', name, '--apply')
        assert (build / name).exists() and 'is referenced' in text, (name, text)
    code, text = run(home, 'stale-builds', '7', '--apply')
    assert all((build / n).exists() for n in ('cited', 'queued', 'sunxi-a733', 'a733-ddr', 'qemu-virt')), text
    passed.append('builds that evidence or a queue database names, and the pinned builds, are refused')
    # protected areas
    for area in ('evidence', 'sources', 'host', 'checkpoints', 'inputs', 'venv', 'bin'):
        target = make(store / area / 'keep')
        code, text = run(home, 'path', str(target), '--apply')
        assert target.exists() and ('protected' in text or 'not inside an allowed root' in text), (area, text)
    for name in ('queue.sqlite3', 'a.token'):
        (build / name).write_bytes(b'x')
        os.utime(build / name, (OLD, OLD))
        code, text = run(home, 'path', str(build / name), '--apply')
        assert (build / name).exists() and 'queue database or token' in text, (name, text)
    passed.append('evidence, sources, host, checkpoints, inputs, venv, bin, queue databases and tokens are refused')
    # roots
    elsewhere = make(home / 'documents')
    code, text = run(home, 'path', str(elsewhere), '--apply')
    assert elsewhere.exists() and 'not inside an allowed root' in text, text
    code, text = run(home, 'path', str(build), '--apply')
    assert build.exists() and 'not inside an allowed root' in text, text
    link = build / 'link'
    link.symlink_to(elsewhere)
    code, text = run(home, 'path', str(link), '--apply')
    assert elsewhere.exists() and 'not inside an allowed root' in text, text
    passed.append('a path outside the roots, a root itself and a link out of the build directory are refused')
    for line in passed:
        print('PASS ' + line)


if __name__ == '__main__':
    main()
