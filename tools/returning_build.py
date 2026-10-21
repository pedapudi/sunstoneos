"""Header-closure checks for builders that link accepted objects with new units.

The returning and finite builders compile new units against the current tree
and link them with objects accepted from earlier evidence. Those objects keep
the structure layouts and constants of the headers they were compiled with. A
header that has changed since then gives the image two different layouts for
one type, so the builders refuse to link until the reused objects are rebuilt.
"""
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def depfile_headers(depfile):
    """Return the files a make-style dependency file lists after its translation unit."""
    text = Path(depfile).read_text().replace('\\\n', ' ')
    return shlex.split(text.split(':', 1)[1])[1:]


def evidence_root(path):
    """Return the nearest ancestor holding the evidence snapshot manifest."""
    for parent in Path(path).parents:
        if (parent / 'snapshot/manifest.json').is_file():
            return parent
    raise SystemExit(f'No evidence snapshot records the inputs of {path}')


def snapshot_records(evidence):
    """Return the snapshot's original repository and its recorded file digests."""
    manifest = json.loads((evidence / 'snapshot/manifest.json').read_text())
    records = {}
    for item in list(manifest['sources'].values()) + manifest['artifacts']:
        records[os.path.normpath(item['original'])] = (item['sha256'], evidence / 'snapshot' / item['snapshot'])
    return manifest['repository'], records


def object_closure(obj, original=None):
    """Header closure of one reused object from its dependency file and evidence snapshot.

    Assembly objects built without a dependency file are accepted only when the
    snapshot copy of their source includes no other file.
    """
    candidates = [Path(str(x) + '.d') for x in (obj, original) if x]
    depfile = next((x for x in candidates if x.is_file()), None)
    repository, records = snapshot_records(evidence_root(depfile or original or obj))
    closure = {'object': str(obj), 'roots': [repository], 'headers': {}}
    if depfile:
        for header in depfile_headers(depfile):
            closure['headers'][header] = records.get(os.path.normpath(header), (None,))[0]
        return closure
    stem = str(original or obj)[:-len('.obj')]
    sources = [(name, value) for name, value in records.items()
               if stem.endswith(name) or stem.endswith('/' + os.path.relpath(name, repository))]
    if not stem.endswith(('.S', '.s')) or len(sources) != 1:
        raise SystemExit(f'No dependency record for reused object {obj}')
    text = sources[0][1][1].read_text()
    if '#include' in text or '.include' in text:
        raise SystemExit(f'No dependency record for reused assembly object {obj}')
    return closure


def image_object_closures(image_objects, anchor):
    """Header closures recorded per object in an accepted image-objects.json, keyed by object digest.

    Each original worktree root is found from a recorded path ending in anchor,
    a repository-relative source path.
    """
    entries = json.loads(Path(image_objects).read_text())
    paths = [entry['source'] for entry in entries]
    paths += [name for entry in entries for name in entry.get('dependency_hashes') or {}]
    roots = sorted({name[:-len(anchor) - 1] for name in paths if name.endswith('/' + anchor)})
    closures = {}
    for entry in entries:
        if entry.get('dependency_hashes') is not None:
            headers = dict(entry['dependency_hashes'])
        else:
            headers = {x['original']: x['frozen']['sha256'] for x in entry['frozen_dependencies']}
        source = _current(entry['source'], roots)
        headers = {name: value for name, value in headers.items() if _current(name, roots) != source}
        closures[entry['sha256']] = {'object': entry['object'], 'roots': roots, 'headers': headers}
    return closures


def _current(path, roots, repository=ROOT):
    path = os.path.normpath(path)
    for root in roots:
        if path.startswith(root.rstrip('/') + '/'):
            return str(repository / os.path.relpath(path, root))
    return path


def require_unchanged_headers(closures, compiler, repository=ROOT):
    """Refuse the build unless each reused object's headers match the current tree.

    Headers under the compiler's resource directory are part of the pinned
    compiler when no digest was recorded for them. Return the checked headers
    and their digests for the build's input record.
    """
    resource = subprocess.check_output([compiler, '-print-resource-dir'], text=True).strip()
    checked = {}
    changed = {}
    for closure in closures:
        for original, recorded in closure['headers'].items():
            path = _current(original, closure['roots'], repository)
            current = digest(path) if Path(path).is_file() else None
            if recorded is None and current and path.startswith(resource.rstrip('/') + '/'):
                recorded = current
            if recorded is None or current != recorded:
                reason = 'unrecorded' if recorded is None else 'missing' if current is None else 'changed'
                changed.setdefault((path, reason), set()).add(Path(closure['object']).name)
            else:
                checked[path] = current
    if changed:
        lines = [f'{path} ({reason}; compiled into {", ".join(sorted(objects))})'
                 for (path, reason), objects in sorted(changed.items())]
        raise SystemExit('Refusing to link reused objects whose headers differ from the current tree:\n  '
                         + '\n  '.join(lines) + '\nRebuild those objects from current source instead.')
    return checked
