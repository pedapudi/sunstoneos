#!/usr/bin/env python3
"""An image carries the revision of the source it was built from, and the board record takes it from the image.

The runner used to record the host tree's HEAD at run time as the image's revision. A worker can commit after building, or build from a
dirty tree, so that revision is not the image's. Every image now embeds a 48-byte object a733_source_revision (cmake/source-revision-write.cmake,
run at every build by cmake/sunstone-source-revision.cmake): "SRV1", the 40 hex digits of HEAD, a known flag and a dirty flag. The runner reads
it from the ELF (tools/board_images.py, embedded_source_revision) and records it as git_revision; the HEAD of the host tree is a separate
field, host_tree_git_revision, and git_revision_host_tree_mismatch is true when they differ (tools/board.py, source_revisions).
Cases, on a scratch git repository and the object compiled and linked as the images do:
  commit-after-build  an image built at commit A, then a new commit B in the tree: the record says A, the host tree says B and the mismatch
                      is flagged; the image rebuilt at B says B and has no mismatch;
  dirty-build         a build with a changed tracked file carries the dirty flag; an untracked file does not make it dirty;
  unknown             a build outside any git checkout carries no revision (known false) and the record falls back to the host tree;
  old-image           an ELF without the object, and a file that is not an ELF, give no embedded revision and the same fallback;
  quiet-rebuild       a build with no new commit leaves the generated file untouched, so nothing recompiles or relinks;
  every-image         a configure of this tree lists every image that has a build id in source-revision-targets.txt;
  selection           a change to cmake/source-revision-write.cmake, which a build runs with cmake -P, selects the checks that build images,
                      as a change to an included CMake file does (checks/check-index.py, CMAKE_SCRIPT).
Usage: image-source-revision.py --output DIR
"""
import argparse
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402  (enforces assertions before any decision)
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import board  # noqa: E402
import board_images  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['cmake/source-revision-write.cmake', 'cmake/sunstone-source-revision.cmake', 'tools/board_images.py', 'tools/board.py']
GATE_RUNS = [['--output', '{output}']]


def git(tree, *arguments):
    done = subprocess.run(['git', '-C', str(tree), *arguments], capture_output=True, text=True)
    assert done.returncode == 0, (arguments, done.stderr)
    return done.stdout.strip()


def build(tree, out, name):
    """What a build does: write the revision object, compile it, link it into an ELF that keeps the symbol."""
    source = out / (name + '.c')
    done = subprocess.run(['cmake', '-DSOURCE_DIR=' + str(tree), '-DOUTPUT=' + str(source),
                           '-P', str(ROOT / 'cmake/source-revision-write.cmake')], capture_output=True, text=True)
    assert done.returncode == 0, done.stderr
    obj, elf = out / (name + '.o'), out / (name + '.elf')
    subprocess.run([str(harness.CLANG), '--target=aarch64-none-elf', '-ffreestanding', '-c', str(source), '-o', str(obj)], check=True)
    subprocess.run([str(harness.LLD), '-e', '0', '--undefined=a733_source_revision', str(obj), '-o', str(elf)], check=True)
    return source, elf


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)

    tree = out / 'tree'
    tree.mkdir()
    for step in (['init', '-q'], ['config', 'user.email', 'x@example.invalid'], ['config', 'user.name', 'x']):
        git(tree, *step)
    (tree / 'tracked.txt').write_text('one\n')
    git(tree, 'add', 'tracked.txt')
    git(tree, 'commit', '-q', '-m', 'first')
    first = git(tree, 'rev-parse', 'HEAD')

    source, image = build(tree, out, 'at-first')
    carried = board_images.embedded_source_revision(image)
    assert carried == {'revision': first, 'known': True, 'dirty': False}, carried
    before = source.stat().st_mtime_ns
    build(tree, out, 'at-first')
    assert source.stat().st_mtime_ns == before, 'a build with no new commit rewrote the revision file'
    print('PASS an image carries the HEAD it was built at, clean; a build with no new commit leaves the generated file untouched')

    (tree / 'tracked.txt').write_text('two\n')
    git(tree, 'commit', '-qam', 'second')
    second = git(tree, 'rev-parse', 'HEAD')
    assert second != first
    record = board.source_revisions(tree, image)
    assert record['git_revision'] == first and record['git_revision_source'] == 'image' and record['git_revision_dirty'] is False, record
    assert record['host_tree_git_revision'] == second and record['git_revision_host_tree_mismatch'] is True, record
    _, rebuilt = build(tree, out, 'at-second')
    again = board.source_revisions(tree, rebuilt)
    assert again['git_revision'] == second and again['git_revision_host_tree_mismatch'] is False, again
    print('PASS an image built at commit A, then a new commit B: the record says A, the host tree B, and the mismatch is flagged; rebuilt at B it is not')

    (tree / 'tracked.txt').write_text('three\n')
    _, dirty = build(tree, out, 'dirty')
    assert board_images.embedded_source_revision(dirty) == {'revision': second, 'known': True, 'dirty': True}
    assert board.source_revisions(tree, dirty)['git_revision_dirty'] is True
    git(tree, 'checkout', 'tracked.txt')
    (tree / 'untracked.txt').write_text('x\n')
    _, clean = build(tree, out, 'untracked')
    assert board_images.embedded_source_revision(clean)['dirty'] is False
    print('PASS a changed tracked file sets the dirty flag in the image and the record; an untracked file does not')

    bare = out / 'no-git'
    bare.mkdir()
    _, unknown = build(bare, out, 'unknown')
    assert board_images.embedded_source_revision(unknown) == {'revision': '0' * 40, 'known': False, 'dirty': False}
    fallback = board.source_revisions(tree, unknown)
    assert fallback['git_revision'] == second and fallback['git_revision_source'] == 'host-tree' and \
        fallback['git_revision_host_tree_mismatch'] is None, fallback
    old = out / 'old.o'
    (out / 'old.c').write_text('const unsigned char other[4] = "abc";\n')
    subprocess.run([str(harness.CLANG), '--target=aarch64-none-elf', '-ffreestanding', '-c', str(out / 'old.c'), '-o', str(old)], check=True)
    notelf = out / 'raw.bin'
    notelf.write_bytes(b'\0' * 64)
    for path in (old, notelf):
        assert board_images.embedded_source_revision(path) is None, path
        record = board.source_revisions(tree, path)
        assert record['git_revision'] == second and record['git_revision_source'] == 'host-tree', record
    print('PASS a build outside git, an ELF without the object and a raw file fall back to the host tree')

    configured = out / 'configured'
    done = subprocess.run([str(harness.CACHE / 'bin/cfg.sh'), str(ROOT), str(configured), '-DPython3_EXECUTABLE=' + sys.executable,
                           '-DSUNSTONE_EXTERNAL_APPLICATIONS=', '-DMIMUS_DIR=', '-DSUNSTONE_A733_OS=ON', '-DSUNSTONE_PARTITIONED_SCHEDULER=ON'],
                          capture_output=True, text=True)
    assert done.returncode == 0, done.stdout[-600:] + done.stderr[-600:]
    listed = (configured / 'source-revision-targets.txt').read_text().split()
    images = {path.parent.name for path in configured.rglob('build-id.txt')}
    assert len(listed) >= len(images) >= 8, (len(listed), sorted(images))
    print('PASS a configure lists %d images for the revision object (%d build ids)' % (len(listed), len(images)))

    selected = subprocess.run([sys.executable, str(ROOT / 'checks/check-index.py'), '--changed', 'cmake/source-revision-write.cmake'],
                              capture_output=True, text=True, cwd=ROOT)
    assert selected.returncode == 0, selected.stderr
    names = {line.split(':')[0] for line in selected.stdout.splitlines()}
    assert 'checks/os-image-identity.py' in names and len(names) > 20, sorted(names)
    print('PASS a change to the revision script selects %d checks, the checks that build images among them' % len(names))


if __name__ == '__main__':
    main()
