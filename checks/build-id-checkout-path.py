#!/usr/bin/env python3
"""One tree gives one build id wherever it is checked out.

The inputs of an image's build id are hashed in list order. A glob sorts its results by absolute path, so two checkouts of one tree at
different depths or names put the same files in a different place beside the files of other roots, and the same sources gave two ids
(the stage-1 decoder image: a block of kernel/scheduler headers moved within build-id-inputs.txt). The identities now sort their inputs
by portable path (cmake/aarch64-clang.cmake: sunstone_sort_inputs), and the text of compile options, definitions and link options that
the gpu-application and cached-devices identities hash has its checkout and build paths replaced by fixed names.

The check copies the tree to two paths of different depth and name, configures both with the same options (the OS image, the board
application, the GPU application and the other configure-time identities all write build-id.txt), and requires the same set of
build-id.txt files with the same ids. A copy with one source byte changed must give a different OS image id, so the comparison is not
vacuous. Usage: build-id-checkout-path.py --output DIR
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402  (enforces assertions before any decision)

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['cmake', 'CMakeLists.txt']
GATE_RUNS = [['--output', '{output}']]
OPTIONS = ['-DPython3_EXECUTABLE=' + sys.executable, '-DSUNSTONE_EXTERNAL_APPLICATIONS=', '-DMIMUS_DIR=', '-DSUNSTONE_A733_OS=ON',
           '-DSUNSTONE_PARTITIONED_SCHEDULER=ON']
MINIMUM_IDS = 8


def copy_tree(destination):
    destination.mkdir(parents=True)
    shutil.copytree(ROOT, destination / 'tree', symlinks=True, ignore=shutil.ignore_patterns('.git', 'build', '__pycache__'))
    return destination / 'tree'


def build_ids(source, build):
    configured = subprocess.run([str(harness.CACHE / 'bin/cfg.sh'), str(source), str(build), *OPTIONS], capture_output=True, text=True)
    assert configured.returncode == 0, ('configure failed', source, configured.stdout[-800:] + configured.stderr[-800:])
    ids = {str(path.parent.relative_to(build)): path.read_text().strip() for path in sorted(build.rglob('build-id.txt'))}
    assert len(ids) >= MINIMUM_IDS, ('too few build ids to compare', sorted(ids))
    return ids


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    shallow = copy_tree(out / 'a')
    deep = copy_tree(out / 'long-checkout-name' / 'deeper' / 'than-the-first')
    first, second = build_ids(shallow, out / 'build-a'), build_ids(deep, out / 'build-b')
    assert first.keys() == second.keys(), ('different images configured', sorted(first.keys() ^ second.keys()))
    differing = sorted(name for name in first if first[name] != second[name])
    assert not differing, ('the build id depends on the checkout path', differing)
    print('PASS %d build ids are the same from two checkout paths (%s)' % (len(first), ', '.join(sorted(first))))

    changed = copy_tree(out / 'changed')
    target = changed / 'runtime/memory.c'
    target.write_text(target.read_text() + '\n/* one source byte changed */\n')
    third = build_ids(changed, out / 'build-c')
    assert third['os-image'] != first['os-image'], 'a changed source did not change the OS image id'
    print('PASS a changed source changes the OS image id, so the comparison is not vacuous')


if __name__ == '__main__':
    main()
