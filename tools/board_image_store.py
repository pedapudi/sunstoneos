#!/usr/bin/env python3
"""Keep each board-queue image once, named by its SHA-256, and let the queue entries hold it as hard links.

A queue entry holds the same image in up to four places: the submitted copy (entries/ID/image.elf), the runner's frozen copy
(run/image.elf), the prepared bundle's copy (run/bundle/image.elf) and a template copy (template/board/image.elf). On
2026-11-13 the queue held 2,711 such copies (17.7 GB) of 678 distinct images (4.2 GB).

The store is QUEUE/images/SHA256.elf (or .bin), on the same file system as the entries. After an entry's run has finished,
fold_entry() replaces each copy with a hard link to the stored file. Hard links, not a hash that readers resolve, because every
reader (the runner, the decoders, the evidence and timing tools, anyone holding an evidence path) keeps reading the same path
and the same bytes: nothing the runner uploads or admits changes, and no reader needs a resolver. A copy is replaced only after
its SHA-256 and size match the stored file, whose own SHA-256 is checked against its name first; the replacement is an atomic
rename, so a reader sees either the old copy or the link. A stored file is read-only (mode 0444), so a write through any of
its links fails instead of changing another entry. A stored file whose link count is 1 is referenced by no entry.

Folding happens only after the run, never at submission, because a queued entry's image is an input that the runner still
reads and a check may still change.

  board_image_store.py fold [--queue DIR] [--apply]     fold every entry that is done (dry run without --apply)
  board_image_store.py verify [--queue DIR]             check every stored file against its name and count its links
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import sys

NAMES = ('image.elf', 'image.bin')
STORE = 'images'


class StoreMismatch(RuntimeError):
    """A stored file does not hash to its name, or a copy differs from the stored file it would be replaced by."""


def digest(path):
    value = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            value.update(block)
    return value.hexdigest()


def copies(directory):
    """The image copies under an entry directory: regular files named image.elf or image.bin, not symbolic links."""
    return sorted(p for name in NAMES for p in Path(directory).rglob(name) if p.is_file() and not p.is_symlink())


def fold_file(path, store, verified, apply=True):
    """Hold path as a hard link to its stored file. Returns the bytes on disk that the copy freed (or would free).

    verified caches the SHA-256 of stored files already checked in this pass."""
    path, store = Path(path), Path(store)
    before = path.lstat()
    value = digest(path)
    target = store / (value + path.suffix)
    if not apply and target in verified and not target.exists():
        return before.st_blocks * 512 if verified[target] == value and before.st_nlink == 1 else 0   # a dry run's planned store file
    if not target.exists():
        verified[target] = value
        if apply:
            store.mkdir(parents=True, exist_ok=True)
            try:
                os.link(path, target)              # this copy becomes the stored file: no bytes are copied
            except FileExistsError:                # another fold stored it first: check that file like any other
                verified.pop(target, None)
                return fold_file(path, store, verified, apply)
            os.chmod(target, 0o444)
        return 0
    if os.path.samefile(target, path):
        return 0
    if target not in verified:
        verified[target] = digest(target)
    if verified[target] != value or target.stat().st_size != before.st_size:
        raise StoreMismatch('%s does not match the stored %s; it is left as it is' % (path, target))
    if apply:
        if digest(path) != value or path.lstat().st_ino != before.st_ino:
            raise StoreMismatch('%s changed while it was folded; it is left as it is' % path)
        number = 0
        while True:
            temporary = path.with_name('.%s.store-link-%d-%d' % (path.name, os.getpid(), number))
            try:
                os.link(target, temporary)         # a name left by an interrupted fold is only another link to the stored file
                break
            except FileExistsError:
                number += 1
        os.replace(temporary, path)                # atomic: the copy's blocks go once nothing else names them
    return before.st_blocks * 512 if before.st_nlink == 1 else 0


def fold_entry(directory, store, verified=None, apply=True):
    """Fold every image copy of one entry. Returns (copies folded, bytes freed)."""
    verified = {} if verified is None else verified
    folded = freed = 0
    for path in copies(directory):
        saved = fold_file(path, store, verified, apply)
        folded += 1
        freed += saved
    return folded, freed


def active_entries(queue):
    """The ids of the entries that are queued or running: their images are still inputs."""
    try:
        state = json.loads((Path(queue) / 'state.json').read_text())
    except (OSError, ValueError):
        raise SystemExit('cannot read %s; nothing folded' % (Path(queue) / 'state.json'))
    return {job['id'] for job in state.get('jobs', []) if job.get('state') != 'done'}


def finished(directory):
    """Whether an entry's own record says that it is done; an entry submitted while a pass runs is not yet in the state it read."""
    try:
        return json.loads((Path(directory) / 'entry.json').read_text()).get('state') == 'done'
    except (OSError, ValueError):
        return False


def fold_queue(queue, apply=False):
    """Fold every entry that is not active. Returns a summary dict."""
    queue = Path(queue)
    store, active, verified = queue / STORE, active_entries(queue), {}
    summary = {'entries': 0, 'skipped_active': 0, 'copies': 0, 'freed_bytes': 0, 'mismatches': []}
    for directory in sorted(p for p in (queue / 'entries').iterdir() if p.is_dir()):
        if directory.name in active or not finished(directory):
            summary['skipped_active'] += 1
            continue
        summary['entries'] += 1
        for path in copies(directory):
            try:
                summary['freed_bytes'] += fold_file(path, store, verified, apply)
                summary['copies'] += 1
            except StoreMismatch as error:
                summary['mismatches'].append(str(error))
    return summary


def verify_store(queue):
    """Check each stored file against its name. Returns (files, bytes, unreferenced files, mismatching files)."""
    files = size = unreferenced = 0
    bad = []
    for path in sorted((Path(queue) / STORE).glob('*')):
        if path.name.startswith('.'):
            continue
        files += 1
        info = path.stat()
        size += info.st_size
        unreferenced += info.st_nlink == 1
        if digest(path) != path.name.split('.')[0] or stat.S_IMODE(info.st_mode) & 0o222:
            bad.append(path.name)
    return files, size, unreferenced, bad


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('mode', choices=('fold', 'verify'))
    parser.add_argument('--queue', type=Path, default=Path.home() / '.cache/sunstoneos/board-queue')
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args(argv)
    if args.mode == 'verify':
        files, size, unreferenced, bad = verify_store(args.queue)
        print('%d stored images, %.2f GB, %d referenced by no entry, %d that do not match their name or are writable%s'
              % (files, size / 1e9, unreferenced, len(bad), ''.join('\n  ' + name for name in bad)))
        return 1 if bad else 0
    summary = fold_queue(args.queue, args.apply)
    print('%s %d copies in %d entries (%d entries not done skipped); %s %.2f GB; %d mismatches%s'
          % ('folded' if args.apply else 'would fold', summary['copies'], summary['entries'], summary['skipped_active'],
             'freed' if args.apply else 'would free', summary['freed_bytes'] / 1e9, len(summary['mismatches']),
             ''.join('\n  ' + line for line in summary['mismatches'])))
    return 1 if summary['mismatches'] else 0


if __name__ == '__main__':
    sys.exit(main())
