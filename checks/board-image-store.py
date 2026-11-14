#!/usr/bin/env python3
"""Check that tools/board_image_store.py keeps each queue image once and that every path keeps its bytes.

It builds a miniature queue in the output directory: entries that hold the same image at the four places an entry holds one
(image.elf, run/image.elf, run/bundle/image.elf, template/board/image.elf), an entry with another image, a queued and a running
entry, and an entry whose copy differs from the stored file of its name. The check requires:
  dry run    fold without --apply changes nothing and reports the copies and bytes it would fold;
  fold       after fold --apply every copy of a done entry is a hard link to QUEUE/images/SHA256.elf, the stored files are
             read-only, each path holds exactly the bytes it held before, and the bytes freed are the copies beyond the first;
  active     a queued or running entry, or one whose entry.json does not say done, is left as it is;
  mismatch   a stored file whose bytes differ from its name refuses the copies it would replace, and those copies keep their bytes;
  repeat     a second fold changes nothing and frees nothing; verify reports the stored files and the mismatching one;
  read-only  a write through any link fails, so no entry can change another entry's image.
Usage: board-image-store.py --output DIR
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import board_image_store as store

CHECK_INPUTS = ['tools/board_image_store.py']
PLACES = ('image.elf', 'run/image.elf', 'run/bundle/image.elf', 'template/board/image.elf')


def snapshot(queue):
    """{path: SHA-256} of every image copy under the queue's entries."""
    return {str(p.relative_to(queue)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted((queue / 'entries').rglob('image.*')) if p.is_file()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output
    out.mkdir(parents=True)
    queue = out / 'queue'
    first, second = b'\x7fELF' + bytes(70000), b'\x7fELF' + b'\x01' * 50000
    entries = {'done-a': first, 'done-b': first, 'done-c': second, 'queued': first, 'running': first}
    for name, data in entries.items():
        (queue / 'entries' / name).mkdir(parents=True)
        if name.startswith('done'):
            (queue / 'entries' / name / 'entry.json').write_text(json.dumps({'id': name, 'state': 'done'}))
        for place in PLACES:
            path = queue / 'entries' / name / place
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
    (queue / 'state.json').write_text(json.dumps({'jobs': [{'id': 'queued', 'state': 'queued'}, {'id': 'running', 'state': 'running'},
                                                           {'id': 'done-a', 'state': 'done'}], 'history': [], 'pauses': {}}))
    before = snapshot(queue)
    passed = []

    assert store.main(['fold', '--queue', str(queue)]) == 0
    summary = store.fold_queue(queue, apply=False)
    assert snapshot(queue) == before and not (queue / 'images').exists(), 'a dry run changed the queue'
    blocks = (queue / 'entries/done-a/image.elf').lstat().st_blocks * 512
    assert summary['copies'] == 12 and summary['skipped_active'] == 2, summary
    assert summary['freed_bytes'] == 7 * blocks + 3 * (queue / 'entries/done-c/image.elf').lstat().st_blocks * 512, summary
    passed.append('a dry run changes nothing and counts the copies beyond the first of each image')

    summary = store.fold_queue(queue, apply=True)
    assert not summary['mismatches'] and summary['copies'] == 12, summary
    assert snapshot(queue) == before, 'a path changed its bytes'
    stored = {p.name: p for p in (queue / 'images').iterdir()}
    names = {hashlib.sha256(first).hexdigest() + '.elf', hashlib.sha256(second).hexdigest() + '.elf'}
    assert set(stored) == names, stored
    for name in ('done-a', 'done-b', 'done-c'):
        data = entries[name]
        target = stored[hashlib.sha256(data).hexdigest() + '.elf']
        for place in PLACES:
            assert os.path.samefile(queue / 'entries' / name / place, target), (name, place)
    assert stored[hashlib.sha256(first).hexdigest() + '.elf'].stat().st_nlink == 9          # the stored name and eight copies
    assert all(p.stat().st_mode & 0o777 == 0o444 for p in stored.values())
    assert not list((queue / 'entries').rglob('.*store-link*')), 'a temporary link stayed'
    passed.append('fold holds every copy of a done entry as a hard link to its read-only stored file, and every path keeps its bytes')
    for name in ('queued', 'running'):
        for place in PLACES:
            assert (queue / 'entries' / name / place).stat().st_nlink == 1, (name, place)
    passed.append('queued and running entries are left as they are')

    again = store.fold_queue(queue, apply=True)
    assert again['freed_bytes'] == 0 and not again['mismatches'] and snapshot(queue) == before, again
    passed.append('a second fold changes nothing')

    try:
        (queue / 'entries/done-b/run/image.elf').write_bytes(b'changed')
        raise AssertionError('a write through a link succeeded')
    except PermissionError:
        pass
    assert snapshot(queue) == before
    passed.append('a write through a link fails and changes no entry')

    # A stored file that does not match its name: the copies that would be replaced by it are refused and keep their bytes.
    third = b'\x7fELF' + b'\x02' * 30000
    (queue / 'entries/done-d').mkdir()
    (queue / 'entries/done-d/entry.json').write_text(json.dumps({'id': 'done-d', 'state': 'done'}))
    for place in PLACES:
        path = queue / 'entries/done-d' / place
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(third)
    corrupt = queue / 'images' / (hashlib.sha256(third).hexdigest() + '.elf')
    corrupt.write_bytes(b'\x7fELF' + b'\x03' * 30000)
    before = snapshot(queue)
    assert store.main(['fold', '--queue', str(queue), '--apply']) == 1
    summary = store.fold_queue(queue, apply=True)
    assert len(summary['mismatches']) == 4 and all('done-d' in line for line in summary['mismatches']), summary
    assert snapshot(queue) == before
    assert all((queue / 'entries/done-d' / place).stat().st_nlink == 1 for place in PLACES)
    files, size, unreferenced, bad = store.verify_store(queue)
    assert files == 3 and unreferenced == 1 and bad == [corrupt.name], (files, unreferenced, bad)
    passed.append('a stored file that does not match its name refuses the copies, which keep their bytes; verify names it')
    for line in passed:
        print('PASS ' + line)


if __name__ == '__main__':
    main()
