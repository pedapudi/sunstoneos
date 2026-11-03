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
    slim(home, build, passed)
    # a build directory that a check names as a default input is protected, derived from the checks' sources
    declared = {'npu-graph-variants': 'checks/os-npu-graphs.py', 'npu-board': 'tools/gate_images.py'}
    for name, source in declared.items():
        assert "build/" + name in (ROOT / source).read_text(), (name, source)
        make(build / name)
        code, text = run(home, 'build', name, '--apply')
        assert (build / name).exists() and 'is referenced' in text, (name, text)
    code, text = run(home, 'stale-builds', '7', '--apply')
    assert all((build / name).exists() for name in declared), text
    passed.append('a build directory that a check or tools/gate_images.py names as a default input is never deleted')
    # stale-builds carries on past a read-only fixture directory instead of stopping with PermissionError
    fixture = make(build / 'old-readonly/case-24/bundle/build')
    fixture.chmod(0o500)
    for item in (fixture.parent, fixture.parent.parent, build / 'old-readonly'):
        os.utime(item, (OLD, OLD))
    fixture.parent.chmod(0o500)
    make(build / 'old-after')
    code, text = run(home, 'stale-builds', '7', '--apply')
    assert code == 0 and not (build / 'old-readonly').exists() and not (build / 'old-after').exists(), text
    passed.append('stale-builds removes a tree with read-only directories and carries on')
    for line in passed:
        print('PASS ' + line)


def slim(home, build, passed):
    """slim-gates and slim_after_pass keep the records of a gate and remove its bulky outputs, whatever their mode."""
    def gate(name, verdict):
        directory = build / name
        for kept in ('summary.txt', 'gate-record.json', 'timing.txt', 'disk-peak.json', 'image-x.log', 'evidence/images/a/result.json'):
            (directory / kept).parent.mkdir(parents=True, exist_ok=True)
            (directory / kept).write_text(verdict if kept == 'summary.txt' else '{}\n')
        (directory / 'evidence/images/a/build').mkdir()
        (directory / 'evidence/images/a/build/object.o').write_bytes(bytes(100))
        fixture = directory / 'evidence/images/b/bundle/build'
        fixture.mkdir(parents=True)
        (fixture / 'x').write_bytes(bytes(10))
        fixture.chmod(0o500)                       # a read-only fixture directory, as a check leaves it
        (fixture.parent).chmod(0o500)
        (directory / 'evidence/images/c').mkdir()
        (directory / 'evidence/images/c/huge.bin').write_bytes(bytes(9 << 20))
        for root, dirs, files in os.walk(directory):
            for item in [root, *[os.path.join(root, f) for f in files]]:
                os.utime(item, (OLD, OLD))
        return directory
    passing, failing = gate('gate-land-demo-1103-1103-100000', 'PASS selected\n'), gate('gate-land-demo-1103-1103-090000', 'FAIL kernel\n')
    code, text = run(home, 'slim-gates', '--apply')
    assert code == 0 and 'Freed' in text and '1 passing' in text, text
    assert not (passing / 'evidence/images/a/build').exists() and not (passing / 'evidence/images/b/bundle/build').exists()
    assert not (passing / 'evidence/images/c/huge.bin').exists(), 'a file over 8 MiB stayed'
    for kept in ('summary.txt', 'gate-record.json', 'timing.txt', 'disk-peak.json', 'image-x.log', 'evidence/images/a/result.json'):
        assert (passing / kept).exists(), kept
    assert (failing / 'evidence/images/a/build/object.o').exists(), 'a failed gate lost its outputs'
    passed.append('slim-gates removes the bulky outputs of a passing gate, read-only directories included, and keeps its records and a failed gate')
    # A later passing gate of the same landing label slims the earlier failed one; another label's gate is untouched.
    other = gate('gate-land-demo-two-1103-1103-080000', 'FAIL kernel\n')
    code = subprocess.run([sys.executable, '-B', '-c', 'import importlib.util as u, pathlib, sys\n'
        's = u.spec_from_file_location("cg", sys.argv[1]); m = u.module_from_spec(s); s.loader.exec_module(m)\n'
        'print(m.slim_after_pass(pathlib.Path(sys.argv[2]), sys.argv[3]))', str(TOOL), str(passing), 'land-demo-1103-1103-100000'],
        env=dict(os.environ, HOME=str(home)), capture_output=True, text=True)
    assert code.returncode == 0, code.stderr
    assert not (failing / 'evidence/images/a/build').exists() and (failing / 'summary.txt').exists()
    assert (other / 'evidence/images/a/build/object.o').exists()
    passed.append('a passing gate also slims the earlier failed gates of its landing label and no other')
    for item in build.glob('gate-*'):
        for root, dirs, files in os.walk(item):
            for name in dirs:
                os.chmod(os.path.join(root, name), 0o700)


if __name__ == '__main__':
    main()
