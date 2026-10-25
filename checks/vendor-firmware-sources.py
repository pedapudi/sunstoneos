#!/usr/bin/env python3
"""Validate the vendor firmware manifest, its README table and every link they document.

Checks docs/vendor-firmware/sources.json against its schema: unique names, pinned
SHA-256 and size, acquisition-specific fields, a license only as a non-empty
string, repository consumers that exist, and agreement with
sources.lock.json for items pinned there. Checks that docs/vendor-firmware/README.md
lists every item with its hash, that its local links resolve, and that each
absent linked document is declared pending and marked in preparation. A pending
document that now exists is reported so that its entry can be removed. Then resolves every http(s) link in
both files with a HEAD request, falling back to a one-byte ranged GET; it
downloads no item. --offline skips the network step. The xxd decoder of
tools/fetch-vendor-firmware.py is exercised on a synthetic dump.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import sys
import urllib.error
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402,F401  (assertion policy)

ROOT = Path(__file__).resolve().parents[1]
DIRECTORY = ROOT / 'docs/vendor-firmware'
MANIFEST = DIRECTORY / 'sources.json'
README = DIRECTORY / 'README.md'
LOCK = ROOT / 'sources.lock.json'
SCHEMA = 'sunstoneos-vendor-firmware-sources-v1'
REQUIRED = {'name': str, 'group': str, 'role': str, 'acquisition': str, 'revision': str, 'sha256': str,
            'bytes': int, 'cache_path': str, 'used_by': list}
OPTIONAL = {'url', 'license', 'analysis', 'lock', 'derive', 'tool', 'procedure', 'evidence'}
ACQUISITION = {'download', 'derived', 'board', 'manual'}
URL = re.compile(r'https?://[^\s)<>"\'`]+')
LINK = re.compile(r'\]\(([^)\s]+)\)')


def load_tool():
    spec = importlib.util.spec_from_file_location('fetch_vendor_firmware', ROOT / 'tools/fetch-vendor-firmware.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check_item(item, names, pending):
    name = item.get('name')
    for key, kind in REQUIRED.items():
        assert isinstance(item.get(key), kind), f'{name}: {key} must be {kind.__name__}'
    assert not set(item) - set(REQUIRED) - OPTIONAL, f'{name}: unknown fields {set(item) - set(REQUIRED) - OPTIONAL}'
    assert re.fullmatch(r'[a-z0-9][a-z0-9-]*', name), f'{name}: names are lowercase words joined by hyphens'
    assert re.fullmatch(r'[0-9a-f]{64}', item['sha256']), f'{name}: sha256 must be 64 lowercase hex digits'
    assert item['bytes'] > 0, f'{name}: bytes must be positive'
    assert item['acquisition'] in ACQUISITION, f"{name}: unknown acquisition {item['acquisition']}"
    cache = Path(item['cache_path'])
    assert not cache.is_absolute() and '..' not in cache.parts, f'{name}: cache_path must stay below the cache root'
    if item['acquisition'] in ('download', 'derived', 'manual'):
        assert URL.fullmatch(item.get('url') or ''), f'{name}: {item["acquisition"]} items need an http(s) url'
    if item['acquisition'] == 'derived':
        derive = item.get('derive', {})
        assert derive.get('from') in names, f'{name}: derive.from must name another item'
        assert derive.get('transform') in TOOL.TRANSFORMS, f'{name}: unknown transform'
    if item['acquisition'] in ('board', 'manual'):
        steps = item.get('procedure')
        assert isinstance(steps, list) and steps and all(isinstance(s, str) and s for s in steps), \
            f'{name}: board and manual items need a procedure'
    if 'license' in item:
        assert isinstance(item['license'], str) and item['license'], f'{name}: license must be a non-empty string'
    for path in item['used_by']:
        assert (ROOT / path).exists(), f'{name}: used_by path {path} does not exist'
    analysis = item.get('analysis')
    if analysis is not None:
        assert (DIRECTORY / analysis).is_file() or (ROOT / 'docs' / analysis).is_file() or analysis in pending, \
            f'{name}: analysis document {analysis} is neither present nor declared pending'


def check_lock(items):
    """Items pinned in sources.lock.json must carry the same identity in both files."""
    lock = json.loads(LOCK.read_text())
    count = 0
    for item in items:
        if 'lock' not in item:
            continue
        entry = lock.get(item['lock'])
        assert entry is not None, f"{item['name']}: sources.lock.json has no entry {item['lock']}"
        assert entry['sha256'] == item['sha256'], f"{item['name']}: hash differs from sources.lock.json"
        assert entry['url'] == item['url'], f"{item['name']}: URL differs from sources.lock.json"
        assert 'sources/' + entry['archive'] == item['cache_path'], \
            f"{item['name']}: cache path differs from sources.lock.json"
        count += 1
    # Derived artifacts recorded in the lock agree with the derived items here.
    by_path = {i['cache_path']: i for i in items}
    for entry in lock.values():
        for path, artifact in entry.get('derived_artifacts', {}).items():
            item = by_path.get('sources/' + path)
            if item:
                assert item['sha256'] == artifact['sha256'] and item['bytes'] == artifact['bytes'], \
                    f"{item['name']}: derived identity differs from sources.lock.json"
                count += 1
    return count


def check_readme(items, pending):
    text = README.read_text()
    for item in items:
        assert f"`{item['name']}`" in text, f"README does not list {item['name']}"
        assert item['sha256'] in text, f"README does not give the SHA-256 of {item['name']}"
    local = 0
    for target in LINK.findall(text):
        if URL.match(target) or target.startswith('#'):
            continue
        path = target.split('#', 1)[0]
        resolved = (DIRECTORY / path).resolve()
        if not resolved.exists():
            assert path in pending, f'README link {target} does not resolve and is not pending'
        local += 1
    for name in pending:
        if (DIRECTORY / name).exists():
            print(f'note: {name} now exists; remove it from pending_documents')
            continue
        assert any(f']({name}' in line and 'in preparation' in line for line in text.splitlines()), \
            f'README must link {name} and mark it in preparation on the same line'
    return local


def resolve(url):
    """HTTP status of a HEAD request, or of a one-byte ranged GET when HEAD is refused."""
    headers = {'User-Agent': 'sunstoneos-vendor-firmware-check'}
    for method, extra in (('HEAD', {}), ('GET', {'Range': 'bytes=0-0'})):
        request = urllib.request.Request(url, method=method, headers={**headers, **extra})
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                if method == 'GET':
                    response.read(1)
                return response.status
        except urllib.error.HTTPError as error:
            status = error.code
            if method == 'GET' or status not in (403, 405, 501):
                return status
        except (urllib.error.URLError, TimeoutError) as error:
            if method == 'GET':
                return f'unreachable: {error}'
    return status


def strip(url):
    return url.rstrip('.,;:')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--offline', action='store_true', help='skip resolving the documented links')
    args = parser.parse_args()

    # The decoder must reproduce bytes from a dump with a short final line.
    sample = bytes(range(256)) + b'tail'
    lines = []
    for offset in range(0, len(sample), 16):
        chunk = sample[offset:offset + 16]
        hexes = ' '.join(f'{b:02x}' for b in chunk)
        lines.append(f'{offset:08x}: {hexes:<47}  ' + ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk))
    assert TOOL.decode_xxd('\n'.join(lines) + '\n') == sample, 'xxd decoder does not reproduce its input'
    harness.refused(lambda: TOOL.decode_xxd(lines[1]), 'does not follow')

    manifest = json.loads(MANIFEST.read_text())
    assert manifest.get('schema') == SCHEMA, f'schema must be {SCHEMA}'
    pending = manifest.get('pending_documents', [])
    items = manifest['items']
    names = [item['name'] for item in items]
    assert len(names) == len(set(names)), 'item names must be unique'
    for item in items:
        check_item(item, set(names), pending)
    locked = check_lock(items)
    local = check_readme(items, pending)
    print(f'{len(items)} items valid; {locked} identities agree with sources.lock.json; '
          f'{local} local README links resolve or are pending')
    if args.offline:
        return
    urls = sorted({strip(u) for u in URL.findall(MANIFEST.read_text() + README.read_text())})
    failures = []
    for url in urls:
        status = resolve(url)
        if not (isinstance(status, int) and 200 <= status < 400):
            failures.append(f'{status} {url}')
    assert not failures, 'links that do not resolve:\n' + '\n'.join(failures)
    print(f'{len(urls)} documented links resolve')


TOOL = load_tool()
if __name__ == '__main__':
    main()
