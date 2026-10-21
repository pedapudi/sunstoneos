#!/usr/bin/env python3
"""Serve a read-only, loopback development dashboard from canonical documents."""
from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path, PurePosixPath
import re
import sqlite3
import stat
import threading
import unicodedata
from urllib.parse import parse_qs, quote, urlsplit

# Each attached board has its own queue; docs/board-test-queue.md lists them.
DEFAULT_QUEUES = {'3-1': Path.home() / '.cache/sunstoneos/board-tests.sqlite3',
                  '3-3': Path.home() / '.cache/sunstoneos/board-tests-3-3.sqlite3'}
PORT_PATTERN = r'[0-9]+-[0-9]+(?:\.[0-9]+)*'
SOURCES = ('docs/hardware.md', 'docs/handoff.md', 'docs/mimus.md', 'docs/validation.md',
           'docs/hardware-dependencies.md')
ASSETS = {'/': ('index.html', 'text/html; charset=utf-8'),
          '/index.html': ('index.html', 'text/html; charset=utf-8'),
          '/app.js': ('app.js', 'text/javascript; charset=utf-8'),
          '/app.css': ('app.css', 'text/css; charset=utf-8')}
MAX_DOCUMENT = 2 * 1024 * 1024
NEXT_SECTIONS = ('next actions', 'continuation and evidence reuse')
# These aliases join prose updates to inventory rows. They contain no status.
ALIASES = {
    'boot rom and boot firmware': ('boot rom',),
    'cpu cores': ('cpu',),
    'ddr and cpu memory management': ('ddr', 'memory mappings, interrupt dispatch, and task scheduling'),
    'interrupt controller and timers': ('memory mappings, interrupt dispatch, and task scheduling', 'timers and recovery'),
    'clocks, resets, regulators, power domains': ('cpu clock diagnostics', 'i2c and pmic'),
    'pin control and gpio': ('header pin ownership',),
    'dma and device address translation': ('dma', 'system iommu', 'iommu', 'a733 system iommu'),
    'i2c': ('i2c and pmic',),
    'sd, emmc, ufs, spi storage': ('storage', 'ufs initialization'),
    'usb controllers and phys': ('usb0 device channel', 'usb host and type-c'),
    'type-c and display multiplexing': ('usb host and type-c',),
    'wi-fi': ('radio',),
    'bluetooth controller and profiles': ('radio',),
    'audio controllers': ('i2s0 capture',),
    'csi receivers and phys': ('camera',),
    'ov5640': ('camera',),
    'image processing': ('camera',),
    'rtc and watchdog': ('independent watchdog', 'timers and recovery'),
    'thermal and frequency control': ('cpu clock diagnostics',),
}
# Explicit source scopes already established by the canonical hardware matrix.
# A **tested** marker is still required; a host DMA result cannot enter here.
PHYSICAL_SCOPES = {
    'boot rom': '**tested** for identity, SRAM transfer',
    'cpu': '**tested** for instruction execution on the boot A55',
    'ddr': '**tested** independently on both attached boards for initialization and distinct transfers',
    'memory mappings, interrupt dispatch, and task scheduling': '**tested** on the boot CPU for the physical kernel check',
    'timers and recovery': '**tested** for the nonsecure tick and an EL3 secure-timer timeout',
    'rtc and watchdog': '**tested** coherent RTC reads in inherited normal RC mode',
    'cpu clock diagnostics': '**tested** for read-only reporting of the boot A55 clock configuration',
    'storage': 'Controller access, pin/clock restoration, and ROM return are **tested**',
    'i2c and pmic': '**tested** single-byte reads on the inherited always-on bus',
    'usb0 device channel': '**tested** at high speed for status, packet-boundary echoes',
}


def normalized(text):
    return ' '.join(unicodedata.normalize('NFKD', text).casefold().split())


def slug(text):
    return re.sub(r'[^a-z0-9]+', '-', normalized(text)).strip('-')


def parse_dependencies(text, hardware):
    """Validate topology separately from evidence; never infer subsystem readiness."""
    inventory = {item['id']: item for item in hardware}
    nodes, warnings = {}, []
    for line in text.splitlines():
        if not line.startswith('| ') or line.startswith(('| ID |', '| ---')):
            continue
        cells = [cell.strip() for cell in line.strip().strip('|').split('|')]
        if len(cells) != 6:
            warnings.append('Dependency row must have six cells.')
            continue
        key, name, requires, scope, conditions, record = cells
        if not re.fullmatch(r'[a-z0-9]+(?:-[a-z0-9]+)*', key) or key in nodes:
            warnings.append('Invalid or duplicate dependency ID: ' + key)
            continue
        prerequisites = (list(inventory) if requires == '@hardware' else
                         [] if requires == '-' else [part.strip() for part in requires.split(',')])
        path = 'docs/' + record
        if not doc_path(path):
            warnings.append('Invalid dependency record: ' + record)
        area = inventory.get(key)
        nodes[key] = {'id': key, 'name': name, 'requires': prerequisites,
                      'scope': scope, 'conditions': conditions, 'record': path,
                      'hardware_id': key if area else None,
                      'status': area['status'] if area else 'structural',
                      'status_label': area['status_label'] if area else 'Component evidence in record',
                      'qualification': area['qualification'] if area else 'not_assessed',
                      'evidence': area['evidence'] if area else []}
    missing = sorted(set(inventory) - nodes.keys())
    if missing:
        warnings.append('Hardware missing from dependency graph: ' + ', '.join(missing))
    if not nodes:
        warnings.append('Dependency graph is unavailable.')
    for node in nodes.values():
        for prerequisite in node['requires']:
            if prerequisite not in nodes:
                warnings.append('Unknown prerequisite: ' + prerequisite)
    # Kahn traversal detects cycles and supplies stable, dependency-ordered columns.
    remaining = set(nodes)
    resolved = {}
    while remaining:
        ready = [key for key in nodes if key in remaining and
                 all(parent in resolved for parent in nodes[key]['requires'])]
        if not ready:
            if not any('Unknown prerequisite:' in warning for warning in warnings):
                warnings.append('Dependency graph contains a cycle.')
            break
        for key in ready:
            resolved[key] = max((resolved[parent] + 1 for parent in nodes[key]['requires']), default=0)
            nodes[key]['level'] = resolved[key]
            remaining.remove(key)
    if warnings:
        return {'status': 'invalid', 'warnings': warnings, 'nodes': [], 'edges': []}
    edges = [{'from': parent, 'to': node['id']} for node in nodes.values()
             for parent in node['requires']]
    return {'status': 'ok', 'warnings': [], 'nodes': list(nodes.values()), 'edges': edges,
            'hardware_count': len(inventory), 'source': 'docs/hardware-dependencies.md'}


def iso(timestamp):
    return datetime.fromtimestamp(timestamp, timezone.utc).isoformat()


def safe_read(root, relative, limit=MAX_DOCUMENT):
    """Open only regular files below root; refuse symlinks in every component."""
    parts = PurePosixPath(relative).parts
    if not parts or PurePosixPath(relative).is_absolute() or any(p in ('.', '..') for p in parts):
        raise ValueError('Invalid relative path')
    directory = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        for component in parts[:-1]:
            next_directory = os.open(component, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                                     dir_fd=directory)
            os.close(directory)
            directory = next_directory
        descriptor = os.open(parts[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK,
                             dir_fd=directory)
        try:
            info = os.fstat(descriptor)
            if not stat.S_ISREG(info.st_mode) or info.st_size > limit:
                raise ValueError('Not a bounded regular file')
            with os.fdopen(descriptor, 'rb', closefd=False) as stream:
                return stream.read(limit + 1)
        finally:
            os.close(descriptor)
    finally:
        os.close(directory)


def doc_path(path):
    return bool(re.fullmatch(r'docs/[A-Za-z0-9][A-Za-z0-9_.-]*\.md', path))


def links(text, source='docs/hardware.md'):
    found = []
    for label, target in re.findall(r'\[([^\]]+)\]\(([^)]+)\)', text):
        target = target.split('#', 1)[0]
        if not target or ':' in target or target.startswith('/'):
            continue
        path = str(PurePosixPath(source).parent / target)
        if doc_path(path) and path not in {item['path'] for item in found}:
            found.append({'label': label, 'path': path,
                          'url': '/api/document?path=' + quote(path, safe='')})
    return found


def sections(markdown):
    result = []
    title = ''
    content = []
    for line in markdown.splitlines():
        if re.match(r'^#{1,3} ', line):
            if content:
                result.append({'title': title, 'markdown': '\n'.join(content).strip()})
            title = line.lstrip('# ').strip()
            content = []
        else:
            content.append(line)
    if content:
        result.append({'title': title, 'markdown': '\n'.join(content).strip()})
    return result


def area(name):
    n = normalized(name)
    if any(s in n for s in ('camera', 'csi', 'ov5640', 'image processing')):
        return 'Camera'
    if any(s in n for s in ('display', 'hdmi', 'dsi')):
        return 'Display'
    if any(s in n for s in ('gpu', 'npu', 'codec')):
        return 'Accelerators'
    if any(s in n for s in ('wi-fi', 'bluetooth', 'audio')):
        return 'Radio & audio'
    if any(s in n for s in ('storage', 'usb', 'uart', 'i2c', 'spi', 'pcie', 'pwm', 'type-c')):
        return 'I/O & storage'
    return 'Platform'


def parse_hardware(markdown):
    warnings = []
    rows, updates = [], []
    lines = markdown.splitlines()
    in_inventory = False
    for line in lines:
        if re.match(r'^\|\s*Hardware\s*\|\s*Required functions', line):
            in_inventory = True
            continue
        if in_inventory:
            if not line.startswith('|'):
                break
            cells = [cell.strip() for cell in line.strip().strip('|').split('|')]
            if cells and re.fullmatch(r'[-: ]+', cells[0]):
                continue
            if len(cells) != 3 or not cells[0]:
                warnings.append('An inventory row has an unsupported format.')
                continue
            rows.append(cells)
    for match in re.finditer(r'^- ([^\n:]+):\s*(.*?)(?=^- |^\| Hardware|\Z)', markdown,
                             re.M | re.S):
        title, prose = match.group(1), match.group(2).strip()
        updates.append({'title': title, 'markdown': prose, 'links': links(prose)})
    if not rows:
        warnings.append('Hardware inventory table was not recognized; no completion totals are inferred.')
    known_unqualified = 'Every row below has **no qualification evidence**' in markdown
    if not known_unqualified:
        warnings.append('The matrix-wide qualification declaration changed; qualification is not inferred.')
    matched = set()
    hardware = []
    for name, functions, dependencies in rows:
        key = normalized(name)
        aliases = set(ALIASES.get(key, ())) | {key}
        detail = [item for item in updates if normalized(item['title']) in aliases]
        matched.update(normalized(item['title']) for item in detail)
        prose = '\n\n'.join(item['markdown'] for item in detail)
        evidence = set()
        for item in detail:
            text = item['markdown']
            physical_anchor = PHYSICAL_SCOPES.get(normalized(item['title']))
            if physical_anchor and normalized(physical_anchor) in normalized(text):
                evidence.add('board')
            plain = re.sub(r'\[([^\]]+)\]\([^)]+\)', r'\1', text)
            for sentence in re.split(r'(?<=[.!?])\s+', plain):
                positive = re.search(r'\b(?:pass(?:es|ed)?|match(?:es|ed)?|covers?|evidence)\b|\*\*tested\*\*', sentence, re.I)
                negative = re.search(r'\b(?:pending|unrun|untested|not run|not executed)\b|\b(?:no|without|lacks?)\s+(?:\w+\s+){0,2}(?:host|emulator|source|reference)', sentence, re.I)
                if not positive or negative:
                    continue
                if re.search(r'\bhost(?:[-/ ](?:execution|sanitizer|protocol|reference|model|ownership|native|tested|composition|checks|bounds|translation|concurrency|integration|startup|lifecycle))\b', sentence, re.I):
                    evidence.add('host')
                if re.search(r'\b(?:emulator|emulation|QEMU|instruction[- ]model)\b', sentence, re.I):
                    evidence.add('emulator')
                if re.search(r'\b(?:source|reference|pinned|SDK)\b', sentence, re.I):
                    evidence.add('source')
        implemented = bool(re.search(r'\*\*(?:implemented|tested)\*\*', prose))
        status = ('board_tested' if 'board' in evidence else
                  'host_tested' if 'host' in evidence or 'emulator' in evidence else
                  'implemented' if implemented else 'planned')
        labels = {'board_tested': 'Physical result (partial)', 'host_tested': 'Host / model evidence',
                  'implemented': 'Partial implementation', 'planned': 'Planned'}
        all_links = []
        for item in detail:
            for link in item['links']:
                if link['path'] not in {x['path'] for x in all_links}:
                    all_links.append(link)
        hardware.append({'id': slug(name), 'name': name, 'area': area(name),
                         'functions': functions, 'dependencies': dependencies,
                         'implementation': 'implemented' if implemented else 'planned',
                         'implementation_scope': 'partial' if implemented else 'none_recorded',
                         'evidence': sorted(evidence),
                         'qualification': 'unqualified' if known_unqualified else 'not_recorded',
                         'status': status, 'status_label': labels[status],
                         'summary': ' '.join(prose.split()) if prose else functions,
                         'details': detail, 'links': all_links})
    unmatched = [item for item in updates if normalized(item['title']) not in matched]
    if unmatched:
        warnings.append('Some prose updates have no inventory alias; see unmatched_updates.')
    # A reworded physical bullet must not silently demote its row.
    for title, anchor in PHYSICAL_SCOPES.items():
        if not any(normalized(item['title']) == title and normalized(anchor) in normalized(item['markdown'])
                   for item in updates):
            warnings.append('Physical-result anchor no longer matches its update: ' + title)
    status = 'ok' if rows and known_unqualified and not any('unsupported format' in w for w in warnings) else 'unsupported'
    return hardware, {'status': status, 'warnings': warnings, 'unmatched_updates': unmatched}


def parse_handoff(markdown):
    parts = sections(markdown)
    # The current handoff keeps its ordered list under the continuation heading.
    next_section = next((s['markdown'] for s in parts if s['title'].casefold() in NEXT_SECTIONS), '')
    # An item ends at the next item or at an unindented paragraph after a blank line.
    actions = [{'order': int(m.group(1)), 'text': m.group(2).strip()}
               for m in re.finditer(r'^(\d+)\.\s+(.*?)(?=^\d+\.\s|\n[ \t]*\n(?=\S)|\Z)', next_section, re.M | re.S)]
    blockers = []
    for part in parts:
        for paragraph in re.split(r'\n\s*\n', part['markdown']):
            if re.search(r'\b(?:blocked|blocker|unavailable|missing|no microphone|unconfirmed|pending on)\b', paragraph, re.I):
                blockers.append({'text': paragraph, 'source': 'docs/handoff.md'})
    return {'overview': parts[0]['markdown'] if parts else '', 'next_actions': actions,
            'blockers': blockers, 'sections': parts, 'url': '/api/document?path=docs%2Fhandoff.md'}


def file_signature(path):
    try:
        info = path.lstat()
        return (info.st_mtime_ns, info.st_size, info.st_ino, info.st_mode)
    except OSError:
        return None


def git_paths(repository):
    """Resolve trusted repository metadata only; never parse Git config or run Git."""
    pointer = repository / '.git'
    try:
        if pointer.is_symlink():
            return None, None
        if pointer.is_dir():
            local = pointer
        else:
            data = safe_read(repository, '.git', 4096).decode().strip()
            if not data.startswith('gitdir: '):
                return None, None
            local = (repository / data[8:]).resolve()
        common_pointer = local / 'commondir'
        common = (local / safe_read(local, 'commondir', 4096).decode().strip()).resolve() if common_pointer.exists() else local
        return local, common
    except (OSError, ValueError, UnicodeError):
        return None, None


def git_metadata(local, common):
    empty = {'available': False, 'branch': '', 'head': '', 'commits': [],
             'note': 'Git metadata unavailable. No Git commands are run.'}
    if local is None:
        return empty
    try:
        head = safe_read(local, 'HEAD', 4096).decode().strip()
        branch = ''
        if head.startswith('ref: '):
            ref = head[5:]
            if not re.fullmatch(r'refs/heads/[A-Za-z0-9._/-]+', ref) or '..' in ref:
                return empty
            branch = ref.removeprefix('refs/heads/')
            try:
                head = safe_read(common, ref, 4096).decode().strip()
            except OSError:
                packed = safe_read(common, 'packed-refs').decode()
                head = next((line.split()[0] for line in packed.splitlines()
                             if line.endswith(' ' + ref)), '')
        if not re.fullmatch(r'[0-9a-f]{40,64}', head):
            return empty
        commits = []
        try:
            # Bounded tail avoids scanning a growing reflog on every source change.
            path = local / 'logs/HEAD'
            descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
            with os.fdopen(descriptor, 'rb') as stream:
                info = os.fstat(stream.fileno())
                if not stat.S_ISREG(info.st_mode):
                    raise ValueError('Invalid reflog')
                stream.seek(max(0, info.st_size - 65536))
                log = stream.read(65536).decode('utf-8', 'replace')
            for line in reversed(log.splitlines()):
                match = re.match(r'^[0-9a-f]{40,64} ([0-9a-f]{40,64}) .+ (\d+) [+-]\d{4}\t(.+)$', line)
                if match:
                    sha, stamp, subject = match.groups()
                    commits.append({'hash': sha, 'short_hash': sha[:8], 'subject': subject,
                                    'time': iso(int(stamp)), 'kind': 'reflog'})
                if len(commits) == 12:
                    break
        except (OSError, ValueError):
            pass
        return {'available': True, 'branch': branch or 'Detached HEAD', 'head': head,
                'commits': commits, 'note': 'Recent local reflog activity; entries can include checkout, reset, or commit operations.'}
    except (OSError, ValueError, UnicodeError):
        return empty


def queue_summary(path, port=None):
    empty = {'available': False, 'halted': False, 'reason': '', 'counts': {}, 'items': [],
             'note': 'Existing board-test queue unavailable; no database is created.'}
    # SQLite read-only mode cannot create the database. Reject symlink endpoints.
    if path.is_symlink() or not path.is_file():
        return empty
    try:
        before = tuple(file_signature(Path(str(path) + suffix)) for suffix in ('', '-wal', '-journal'))
        for suffix in ('-wal', '-journal'):
            sidecar = Path(str(path) + suffix)
            if sidecar.exists() and sidecar.stat().st_size:
                return empty | {'note': 'Queue is changing or uses a WAL journal; a side-effect-free snapshot is unavailable.'}
        # Immutable mode forbids SQLite lock, journal, and shared-memory writes.
        # Refuse WAL/hot journals rather than showing stale main-file contents.
        with sqlite3.connect(path.resolve().as_uri() + '?mode=ro&immutable=1', uri=True, timeout=0.1) as db:
            db.execute('PRAGMA query_only=ON')
            db.execute('PRAGMA trusted_schema=OFF')
            halt = db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()
            recorded = db.execute("SELECT value FROM settings WHERE key='port'").fetchone()
            counts = dict(db.execute('SELECT state,COUNT(*) FROM tests GROUP BY state'))
            items = []
            for test_id, submission, state, owner, created in db.execute(
                    'SELECT id,submission,state,owner,created FROM tests ORDER BY created DESC,id LIMIT 100'):
                value = json.loads(submission)
                title = value.get('component', 'Board test')
                items.append({'id': str(test_id), 'status': str(state), 'title': str(title)[:300],
                              'owner': str(owner or ''), 'updated_at': created})
            after = tuple(file_signature(Path(str(path) + suffix)) for suffix in ('', '-wal', '-journal'))
            if after != before or any(item and item[1] for item in after[1:]):
                return empty | {'note': 'Queue changed while reading; the next refresh will retry.'}
            note = 'Read-only queue state. A claim does not authorize device access; item times are submission times.'
            # Supervised runners refuse claims from a queue bound to no port or another port.
            if port is not None and not (recorded and recorded[0] == port):
                note += (f' The queue is bound to USB port {str(recorded[0])[:32]}, not {port}.' if recorded and recorded[0]
                         else ' The queue records no USB port; supervised runners refuse its claims.')
            return {'available': True, 'halted': bool(halt and halt[0]),
                    'reason': str(halt[0]) if halt and halt[0] else '', 'counts': counts,
                    'items': items, 'note': note}
    except (sqlite3.Error, OSError, ValueError, TypeError, AttributeError):
        return empty


def combined_queue(summaries):
    """Fold the per-board summaries into the single queue view the page shows."""
    available = [(port, value) for port, value in summaries if value['available']]
    notes = ' '.join(f'Port {port}: {value["note"]}' for port, value in summaries)
    if not available:
        return {'available': False, 'halted': False, 'reason': '', 'counts': {}, 'items': [], 'note': notes}
    counts = Counter()
    for _, value in available:
        counts.update(value['counts'])
    items = sorted(({**item, 'title': f'{port}: {item["title"]}'} for port, value in available
                    for item in value['items']), key=lambda item: item['updated_at'], reverse=True)
    halted = [f'Port {port} halted by {value["reason"]}.' for port, value in available if value['halted']]
    return {'available': True, 'halted': bool(halted), 'reason': ' '.join(halted), 'counts': dict(counts),
            'items': items[:100], 'note': notes}


class Progress:
    def __init__(self, repository, queues=None):
        self.repository = Path(repository).expanduser().resolve()
        self.queues = {port: Path(path).expanduser().absolute()
                       for port, path in (DEFAULT_QUEUES if queues is None else queues).items()}
        self.local_git, self.common_git = git_paths(self.repository)
        self._lock = threading.Lock()
        self._signature = None
        self._body = b''
        self._etag = ''
        self.rebuilds = 0

    def document(self, path):
        if not doc_path(path):
            raise ValueError('Document is outside the allowlist')
        return safe_read(self.repository, path)

    def signature(self):
        self.local_git, self.common_git = git_paths(self.repository)
        paths = [self.repository / name for name in SOURCES]
        for queue in self.queues.values():
            paths.extend([queue, Path(str(queue) + '-wal'), Path(str(queue) + '-journal')])
        paths.extend([self.repository / '.git'])
        if self.local_git is not None:
            paths.extend([self.local_git / 'HEAD', self.local_git / 'logs/HEAD', self.local_git / 'commondir',
                          self.common_git / 'packed-refs', self.common_git / 'refs/heads'])
            try:
                head = safe_read(self.local_git, 'HEAD', 4096).decode().strip()
                if head.startswith('ref: refs/heads/') and '..' not in head:
                    paths.append(self.common_git / head[5:])
            except (OSError, ValueError, UnicodeError):
                pass
        # Include docs directory metadata so replacing it with a symlink invalidates cached content.
        paths.append(self.repository / 'docs')
        return tuple((str(path), file_signature(path)) for path in paths)

    def snapshot(self):
        with self._lock:
            signature = self.signature()
            if signature == self._signature:
                return self._body, self._etag
            texts, sources, warnings = {}, [], []
            for name in SOURCES:
                try:
                    texts[name] = self.document(name).decode('utf-8')
                    available = True
                except (OSError, ValueError, UnicodeError):
                    texts[name] = ''
                    available = False
                    warnings.append(name + ' is unavailable.')
                source_stat = file_signature(self.repository / name)
                sources.append({'path': name, 'label': name.removeprefix('docs/'), 'available': available,
                                'updated_at': iso(source_stat[0] / 1e9) if source_stat else None,
                                'url': '/api/document?path=' + quote(name, safe='')})
            hardware, parser = parse_hardware(texts['docs/hardware.md'])
            queues = [(port, queue_summary(path, port)) for port, path in self.queues.items()]
            parser['warnings'].extend(warnings)
            if not texts['docs/hardware.md']:
                parser['status'] = 'unavailable'
            status_counts = Counter(item['implementation'] for item in hardware)
            mimus_parts = sections(texts['docs/mimus.md'])
            mimus_summary = mimus_parts[0]['markdown'] if mimus_parts else ''
            validation = sections(texts['docs/validation.md'])
            performance = next((part['markdown'].split('\n\n')[0] for part in validation
                                if part['title'] == 'Schedule performance work after functional hardware support'), '')
            changed = max((entry[1][0] / 1e9 for entry in signature if entry[1]), default=0)
            revision = hashlib.sha256(repr(signature).encode()).hexdigest()[:20]
            snapshot = {
                'schema_version': 1, 'revision': revision, 'updated_at': iso(changed),
                'generated_at': datetime.now(timezone.utc).isoformat(), 'sources': sources, 'parser': parser,
                'summary': {'total': len(hardware), 'implemented': status_counts['implemented'],
                            'planned': status_counts['planned'],
                            'board_tested': sum('board' in item['evidence'] for item in hardware),
                            'host_tested': sum('host' in item['evidence'] for item in hardware),
                            'emulator_tested': sum('emulator' in item['evidence'] for item in hardware),
                            'qualified': 0 if parser['status'] == 'ok' else None, 'headline': 'Full hardware support remains in progress',
                            'detail': ('Implementation and physical results cover the stated partial scopes. No subsystem is declared qualified.'
                                       if parser['status'] == 'ok' else
                                       'Canonical status format is unavailable or changed. Qualification totals are unknown.'),
                            'performance_note': performance},
                'performance_note': performance, 'hardware': hardware,
                'dependencies': parse_dependencies(texts['docs/hardware-dependencies.md'], hardware),
                'handoff': parse_handoff(texts['docs/handoff.md']),
                'mimus': {'summary': mimus_summary, 'markdown': texts['docs/mimus.md'],
                          'url': '/api/document?path=docs%2Fmimus.md', 'links': links(texts['docs/mimus.md'], 'docs/mimus.md')},
                'git': git_metadata(self.local_git, self.common_git), 'queue': combined_queue(queues),
                'queues': [{'port': port, **value} for port, value in queues]}
            self._body = json.dumps(snapshot, ensure_ascii=False, separators=(',', ':')).encode()
            self._etag = '"' + hashlib.sha256(self._body).hexdigest() + '"'
            self._signature = signature
            self.rebuilds += 1
            return self._body, self._etag


def handler(progress):
    class Handler(BaseHTTPRequestHandler):
        server_version = 'SunstoneProgress/1'

        def log_message(self, *_):
            pass

        def response(self, status, body=b'', content_type='text/plain; charset=utf-8', etag=None):
            self.send_response(status)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-cache')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.send_header('Referrer-Policy', 'no-referrer')
            self.send_header('Content-Security-Policy', "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'")
            if etag:
                self.send_header('ETag', etag)
            self.end_headers()
            if self.command != 'HEAD' and body:
                self.wfile.write(body)

        def do_HEAD(self):
            self.do_GET()

        def do_GET(self):
            # Loopback Host validation also prevents arbitrary-site DNS rebinding.
            host = self.headers.get('Host', '')
            allowed = {f'127.0.0.1:{self.server.server_port}', f'localhost:{self.server.server_port}',
                       f'[::1]:{self.server.server_port}'}
            origin = self.headers.get('Origin')
            if host not in allowed or (origin is not None and origin != 'http://' + host):
                self.response(403, b'Loopback Host required.\n')
                return
            try:
                request = urlsplit(self.path)
                if request.path == '/api/progress' and not request.query:
                    body, etag = progress.snapshot()
                    if self.headers.get('If-None-Match') == etag:
                        self.response(304, etag=etag)
                    else:
                        self.response(200, body, 'application/json; charset=utf-8', etag)
                elif request.path in ('/api/document', '/api/source'):
                    query = parse_qs(request.query, strict_parsing=True)
                    if set(query) != {'path'} or len(query['path']) != 1:
                        raise ValueError('Expected one document path')
                    path = query['path'][0]
                    data = progress.document(path)
                    if request.path == '/api/source':
                        text = data.decode('utf-8')
                        title = next((line[2:] for line in text.splitlines() if line.startswith('# ')), path)
                        data = json.dumps({'path': path, 'title': title, 'content': text}).encode()
                        self.response(200, data, 'application/json; charset=utf-8')
                    else:
                        self.response(200, data)
                elif request.path in ASSETS and not request.query:
                    filename, content_type = ASSETS[request.path]
                    data = safe_read(progress.repository, 'tools/progress-ui/' + filename)
                    self.response(200, data, content_type)
                else:
                    self.response(404, b'Not found.\n')
            except (OSError, ValueError, UnicodeError):
                self.response(404, b'Unavailable document or asset.\n')
    return Handler


def parse_queues(values):
    """Map each [USB-PORT=]PATH value to its port; a path alone is the 3-1 queue."""
    queues = {}
    for value in values:
        port, separator, path = value.partition('=')
        if not (separator and re.fullmatch(PORT_PATTERN, port)):
            port, path = '3-1', value
        if port in queues or not path:
            raise ValueError('--queue needs a distinct USB port and a path, for example 3-3=/path/to/queue.sqlite3')
        queues[port] = Path(path)
    return queues


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--queue', action='append', metavar='[USB-PORT=]PATH',
                        help='queue to display; repeat for each board (default: the 3-1 and 3-3 queues; '
                             'a path without a port is the 3-1 queue)')
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error('--port must be in 1..65535')
    queues = None
    if args.queue:
        try:
            queues = parse_queues(args.queue)
        except ValueError as error:
            parser.error(str(error))
    progress = Progress(args.repository, queues)
    server = ThreadingHTTPServer(('127.0.0.1', args.port), handler(progress))
    print(f'Development dashboard: http://127.0.0.1:{args.port}', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
