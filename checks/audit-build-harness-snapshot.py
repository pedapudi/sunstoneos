#!/usr/bin/env python3
"""Check that evidence snapshots hard-link only verified identical earlier files."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]


def snapshot(repository, output, artifact, tool, reuse=()):
    command = [sys.executable, str(ROOT / 'tools/evidence-snapshot.py'), '--repository', str(repository),
               '--output', str(output), '--source', 'input.c', '--artifact', str(artifact), '--tool', str(tool),
               '--note', 'host check of snapshot reuse']
    for earlier in reuse:
        command += ['--reuse', str(earlier)]
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL, env=harness.environment())
    return json.loads((output / 'manifest.json').read_text())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output
    out.mkdir(parents=True, exist_ok=False)
    repository = out / 'repository'
    repository.mkdir()
    subprocess.run(['git', 'init', '-q', str(repository)], check=True)
    (repository / 'input.c').write_text('int input;\n')
    subprocess.run(['git', '-C', str(repository), 'add', 'input.c'], check=True)
    subprocess.run(['git', '-C', str(repository), '-c', 'user.name=check', '-c', 'user.email=check@localhost',
                    'commit', '-q', '-m', 'input'], check=True)
    files = out / 'files'
    files.mkdir()
    artifact, tool = files / 'image.elf', files / 'tool'
    artifact.write_bytes(b'image' * 1000)
    tool.write_bytes(b'tool' * 1000)
    cases = {}

    first = snapshot(repository, out / 'first', artifact, tool)
    assert all('linked_from' not in row for row in first['artifacts'] + first['tools'])
    assert os.stat(out / 'first' / first['tools'][0]['snapshot']).st_nlink == 1
    cases['copies-without-reuse'] = True

    second = snapshot(repository, out / 'second', artifact, tool, [out / 'first'])
    for category in ('artifacts', 'tools'):
        old, new = first[category][0], second[category][0]
        assert new['sha256'] == old['sha256'] and new['bytes'] == old['bytes']
        assert new['linked_from'] == str(out / 'first' / old['snapshot'])
        assert os.path.samefile(out / 'second' / new['snapshot'], out / 'first' / old['snapshot'])
    source = second['sources']['input.c']
    assert source['linked_from'] == str(out / 'first' / first['sources']['input.c']['snapshot'])
    cases['links-identical-files'] = True

    # A changed artifact is copied, and its earlier snapshot file is untouched.
    artifact.write_bytes(b'changed' * 1000)
    third = snapshot(repository, out / 'third', artifact, tool, [out / 'first'])
    assert 'linked_from' not in third['artifacts'][0] and 'linked_from' in third['tools'][0]
    assert (out / 'first' / first['artifacts'][0]['snapshot']).read_bytes() == b'image' * 1000
    cases['copies-changed-files'] = True

    # An earlier snapshot file whose bytes no longer match its record is not linked.
    damaged = out / 'first' / first['tools'][0]['snapshot']
    os.chmod(damaged, 0o644)
    damaged.unlink()
    damaged.write_bytes(b'tool' * 999 + b'TOOL')
    fourth = snapshot(repository, out / 'fourth', artifact, tool, [out / 'first'])
    assert 'linked_from' not in fourth['tools'][0]
    assert (out / 'fourth' / fourth['tools'][0]['snapshot']).read_bytes() == b'tool' * 1000
    cases['refuses-mismatched-earlier-file'] = True

    harness.save(out / 'result.json', {'passed': True, 'physical_access': False, 'cases': cases})
    print(f'PASS {len(cases)} snapshot reuse cases')


if __name__ == '__main__':
    main()
