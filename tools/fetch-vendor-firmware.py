#!/usr/bin/env python3
"""Fetch and verify the pinned vendor binaries listed in docs/vendor-firmware/sources.json.

Each item names its acquisition:
  download  fetched from its pinned URL into the cache path, kept only when its
            SHA-256 and size match the manifest
  derived   decoded from another item, for example a ROM binary from its
            published hexadecimal dump, and verified the same way
  board     read from a board; the tool prints the recorded procedure
  manual    needs a browser session or a container; the tool prints the procedure

A verified file is skipped, so repeated runs download nothing new. An existing
file whose hash differs is reported and left in place; move it aside before
fetching again. Cache paths are relative to $SUNSTONE_CACHE, by default
~/.cache/sunstoneos.

  fetch-vendor-firmware.py                   fetch every downloadable and derived item
  fetch-vendor-firmware.py --check           verify the cache only; no network access
  fetch-vendor-firmware.py --only monitor    restrict either mode to named items
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / 'docs/vendor-firmware/sources.json'
CHUNK = 1 << 20


def digest(path):
    """SHA-256 and size of a file, read in chunks so multi-gigabyte archives fit in memory."""
    h, size = hashlib.sha256(), 0
    with open(path, 'rb') as f:
        while block := f.read(CHUNK):
            h.update(block)
            size += len(block)
    return h.hexdigest(), size


def decode_xxd(text):
    """Bytes of an xxd dump: the hexadecimal columns between the offset and the ASCII column."""
    out, expected = bytearray(), 0
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        offset, _, rest = line.partition(':')
        if int(offset, 16) != expected:
            raise ValueError(f'line {number}: offset {offset} does not follow the previous line')
        chunk = bytes.fromhex(''.join(rest.lstrip(' ').split('  ', 1)[0].split()))
        out += chunk
        expected += len(chunk)
    return bytes(out)


TRANSFORMS = {'xxd-hex-columns': decode_xxd}


def state(item, cache):
    """'ok', 'missing', or a description of the difference."""
    path = cache / item['cache_path']
    if not path.is_file():
        return 'missing'
    sha, size = digest(path)
    if sha != item['sha256'] or size != item['bytes']:
        return f'differs: {size} bytes, SHA-256 {sha}'
    return 'ok'


def install(item, cache, write):
    """Write the item through a temporary sibling, then rename it into place after verification."""
    path = cache / item['cache_path']
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f'.{path.name}.{os.getpid()}.part')
    try:
        h, size = write(temporary)
        if h != item['sha256'] or size != item['bytes']:
            raise SystemExit(f"{item['name']}: refused, got {size} bytes with SHA-256 {h}; "
                             f"the manifest pins {item['bytes']} bytes with SHA-256 {item['sha256']}")
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def download(item, cache):
    def write(temporary):
        h, size = hashlib.sha256(), 0
        request = urllib.request.Request(item['url'], headers={'User-Agent': 'sunstoneos-fetch-vendor-firmware'})
        with urllib.request.urlopen(request, timeout=60) as response, open(temporary, 'wb') as f:
            while block := response.read(CHUNK):
                f.write(block)
                h.update(block)
                size += len(block)
        return h.hexdigest(), size
    install(item, cache, write)


def derive(item, cache, items):
    source = items[item['derive']['from']]
    if state(source, cache) == 'missing' and source['acquisition'] == 'download':
        download(source, cache)
    if state(source, cache) != 'ok':
        raise SystemExit(f"{item['name']}: its source {source['name']} is not verified in the cache")
    data = TRANSFORMS[item['derive']['transform']]((cache / source['cache_path']).read_text(encoding='ascii'))

    def write(temporary):
        temporary.write_bytes(data)
        return hashlib.sha256(data).hexdigest(), len(data)
    install(item, cache, write)


def procedure(item, cache):
    print(f"{item['name']}: {item['acquisition']} item; place it at {cache / item['cache_path']}")
    print(f"  expected {item['bytes']} bytes, SHA-256 {item['sha256']}")
    if 'tool' in item:
        print(f"  tool: {item['tool']['name']} (SHA-256 {item['tool']['sha256']})")
    for number, step in enumerate(item['procedure'], 1):
        print(f'  {number}. {step}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--check', action='store_true', help='verify cached files without downloading')
    parser.add_argument('--only', action='append', metavar='NAME', help='restrict to this item; repeatable')
    parser.add_argument('--cache', type=Path,
                        default=Path(os.environ.get('SUNSTONE_CACHE', Path.home() / '.cache/sunstoneos')))
    parser.add_argument('--manifest', type=Path, default=MANIFEST)
    args = parser.parse_args()
    items = {item['name']: item for item in json.loads(args.manifest.read_text())['items']}
    unknown = sorted(set(args.only or ()) - set(items))
    if unknown:
        parser.error(f"unknown item: {', '.join(unknown)}")
    selected = [items[name] for name in (args.only or items)]

    if args.check:
        failed = 0
        for item in selected:
            result = state(item, args.cache)
            note = ''
            if result == 'missing' and item['acquisition'] in ('board', 'manual'):
                note = f" ({item['acquisition']} item; see its procedure)"
            elif result != 'ok':
                failed += 1
            print(f"{item['name']:36} {result}{note}")
        print(f'{len(selected)} items, {failed} missing or different')
        return 1 if failed else 0

    # Derived items need their sources, so downloads run first.
    order = sorted(selected, key=lambda i: ('download', 'derived', 'board', 'manual').index(i['acquisition']))
    refused = 0
    for item in order:
        result = state(item, args.cache)
        if result == 'ok':
            print(f"{item['name']}: verified")
            continue
        if result != 'missing':
            print(f"{item['name']}: {args.cache / item['cache_path']} {result}; move it aside to fetch again",
                  file=sys.stderr)
            refused += 1
        elif item['acquisition'] == 'download':
            download(item, args.cache)
            print(f"{item['name']}: downloaded and verified")
        elif item['acquisition'] == 'derived':
            derive(item, args.cache, items)
            print(f"{item['name']}: derived and verified")
        else:
            procedure(item, args.cache)
    return 1 if refused else 0


if __name__ == '__main__':
    sys.exit(main())
