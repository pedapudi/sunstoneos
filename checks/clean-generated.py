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
  slim        a gate keeps the records of each passing step (subdirectories packed in records.tar.gz, records over 1 MiB gzip-compressed,
              the hashes of linked executables in retention.json) and removes its build products; a failing or skipped step keeps
              everything; a passing gate slims the earlier gates of its landing label whole; a second slim removes nothing.
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
GATE_RUNS = [['--output', '{output}']]
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
    declared = {'npu-graph-variants': 'checks/os-npu-graphs.py'}
    assert 'build/npu-graph-variants' in (ROOT / 'tools/gate_images.py').read_text()   # the gate's fixture names it too
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


def elf(kind):
    """The 64-byte header of a little-endian AArch64 ELF file of type kind (1 an object file, 2 an executable), padded."""
    return b'\x7fELF\x02\x01\x01' + bytes(9) + kind.to_bytes(2, 'little') + (183).to_bytes(2, 'little') + bytes(44) + bytes(4000)


def slim(home, build, passed):
    """A gate keeps the records of its passing steps and removes their build products; a failing step keeps everything."""
    import gzip
    import hashlib
    import tarfile

    def gate(name, summary):
        directory = build / name
        files = {'summary.txt': summary.encode(), 'commands.txt': b'x\n', 'gate-record.json': b'{}\n', 'timing.txt': b'1 x\n',
                 'disk-peak.json': b'{}\n', 'image-a.log': b'log\n', 'image-b.log': b'log\n',
                 'evidence/sessions/a.bin': bytes(64)}                     # written by a session step: no step names it, so it stays
        for step in ('images/a', 'images/b', 'indexed/c'):
            files.update({
                'evidence/%s/result.json' % step: b'{"passed": true}\n',
                'evidence/%s/manifest.json' % step: (b'{"pad": "' + b'x' * (2 << 20) + b'"}\n'),   # a record over 1 MiB
                'evidence/%s/case/run.log' % step: b'case passed\n',
                'evidence/%s/case/input.bin' % step: bytes(100),
                'evidence/%s/exe/model' % step: elf(2),                    # a linked executable
                'evidence/%s/caller.o' % step: elf(1),
                'evidence/%s/generated.c' % step: b'int x;\n',
                'evidence/%s/tree/CMakeCache.txt' % step: b'cache\n',
                'evidence/%s/tree/linked.elf' % step: elf(2),
                'evidence/%s/fixture/.git/HEAD' % step: b'ref\n',
                'evidence/%s/build/object.o' % step: elf(1),
                'evidence/%s/huge.json' % step: b'{' + b' ' * (9 << 20) + b'}',
            })
        for relative, content in files.items():
            (directory / relative).parent.mkdir(parents=True, exist_ok=True)
            (directory / relative).write_bytes(content)
        readonly = directory / 'evidence/images/a/bundle/build'
        readonly.mkdir(parents=True)
        (readonly / 'x').write_bytes(bytes(10))
        readonly.chmod(0o500)                       # a read-only fixture directory, as a check leaves it
        readonly.parent.chmod(0o500)
        for root, dirs, names in os.walk(directory):
            for item in [root, *[os.path.join(root, f) for f in names]]:
                os.utime(item, (OLD, OLD))
        return directory

    steps = 'PASS img-a-build\nPASS image-a\nFAIL image-b\nWARN indexed-c (documentation prose)\n'
    current = gate('gate-land-demo-1103-1103-100000', steps + 'FAIL selected integration checks: image-b\n')
    code, text = run(home, 'slim-gates', '--apply')
    assert code == 0 and 'Freed' in text, text
    for step in ('images/a', 'indexed/c'):
        kept = current / 'evidence' / step
        assert (kept / 'result.json').read_bytes() == b'{"passed": true}\n', step
        for gone in ('exe', 'caller.o', 'generated.c', 'tree', 'fixture', 'build', 'case', 'huge.json', 'manifest.json'):
            assert not (kept / gone).exists(), '%s/%s stayed' % (step, gone)
        assert gzip.decompress((kept / 'manifest.json.gz').read_bytes()) == b'{"pad": "' + b'x' * (2 << 20) + b'"}\n'
        with tarfile.open(kept / 'records.tar.gz') as archive:
            assert sorted(archive.getnames()) == ['case/run.log'], archive.getnames()
            assert archive.extractfile('case/run.log').read() == b'case passed\n'
        record = json.loads((kept / 'retention.json').read_text())
        assert sorted(record['linked_elf']) == ['exe/model', 'tree/linked.elf'], record
        assert record['linked_elf']['exe/model']['sha256'] == hashlib.sha256(elf(2)).hexdigest(), record
    assert not (current / 'evidence/images/a/bundle').exists(), 'the read-only fixture stayed'
    failed = current / 'evidence/images/b'
    for kept in ('exe/model', 'caller.o', 'generated.c', 'tree/linked.elf', 'case/input.bin', 'huge.json', 'manifest.json'):
        assert (failed / kept).exists(), 'the failing step lost ' + kept
    assert not (failed / 'retention.json').exists()
    for kept in ('summary.txt', 'commands.txt', 'gate-record.json', 'timing.txt', 'disk-peak.json', 'image-a.log', 'evidence/sessions/a.bin'):
        assert (current / kept).exists(), kept
    passed.append('slim-gates keeps the records of the passing steps of a failed gate, packs their subdirectories, records the '
                  'hashes of their linked executables and removes their build products; the failing step keeps everything')
    code, text = run(home, 'slim-gates', '--apply')
    assert code == 0 and 'Freed 0.0 GiB' in text and (current / 'evidence/images/a/records.tar.gz').is_file(), text
    passed.append('a second slim removes nothing and keeps the archives it wrote')

    def after_gate(output, label, verdicts, passed_gate):
        done = subprocess.run([sys.executable, '-B', '-c', 'import importlib.util as u, json, pathlib, sys\n'
            's = u.spec_from_file_location("cg", sys.argv[1]); m = u.module_from_spec(s); s.loader.exec_module(m)\n'
            'print(m.slim_after_gate(pathlib.Path(sys.argv[2]), sys.argv[3], json.loads(sys.argv[4]), sys.argv[5] == "1"))',
            str(TOOL), str(output), label, json.dumps(verdicts), '1' if passed_gate else '0'],
            env=dict(os.environ, HOME=str(home)), capture_output=True, text=True)
        assert done.returncode == 0, done.stderr
    # The gate passes its own verdicts; a failed gate leaves the earlier gates of its label alone, a passing one slims them whole.
    other = gate('gate-land-demo-two-1103-1103-080000', steps)
    newer = gate('gate-land-demo-1103-1103-110000', '')
    after_gate(newer, 'land-demo-1103-1103-110000', {'image-a': 'PASS', 'image-b': 'FAIL', 'indexed-c': 'SKIP'}, False)
    assert not (newer / 'evidence/images/a/exe').exists() and (newer / 'evidence/images/b/exe/model').exists()
    assert (newer / 'evidence/indexed/c/exe/model').exists(), 'a skipped step was slimmed'
    assert (failed / 'exe/model').exists(), 'a failed gate slimmed an earlier gate of its label'
    after_gate(newer, 'land-demo-1103-1103-110000', {'image-a': 'PASS', 'image-b': 'PASS', 'indexed-c': 'PASS'}, True)
    assert not (failed / 'exe').exists() and (failed / 'result.json').exists() and (failed / 'retention.json').exists()
    assert not (current / 'evidence/sessions/a.bin').exists() and (current / 'summary.txt').exists()
    assert (other / 'evidence/images/b/exe/model').exists() and (other / 'evidence/images/a/exe/model').exists()
    passed.append('a passing gate slims the earlier gates of its landing label whole and no other; a failing gate slims only its own passing steps')
    for item in build.glob('gate-*'):
        for root, dirs, files in os.walk(item):
            for name in dirs:
                os.chmod(os.path.join(root, name), 0o700)


if __name__ == '__main__':
    main()
