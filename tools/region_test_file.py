#!/usr/bin/env python3
"""Generate or verify the deterministic test file for large application regions.

The file is a counter-mode stream of SHA-256: block i (32 bytes) is
SHA-256(PREFIX || i as 8 bytes little-endian), for i from 0, truncated at SIZE bytes.
The default is 512 MiB (536,870,912 bytes). The stream is reproducible on any machine
with Python 3 and no other dependency, and any byte range can be recomputed from its offset.

    python3 tools/region_test_file.py write sunstone-region-test.bin
    python3 tools/region_test_file.py digest            # SHA-256 of the stream, without writing it
    python3 tools/region_test_file.py verify sunstone-region-test.bin

The loader reads the file at /sunstone-test/sunstone-region-test.bin on the card's FAT32 volume.
"""
import argparse
import hashlib
from pathlib import Path
import sys

PREFIX = b'sunstone-region-test-v1'
SIZE = 512 * 1024 * 1024
EXPECTED_SHA256 = 'a0ab21dca50740b257c495faa888058cb757ec5165d27afb0c13cc5bef48eabd'


def blocks(size):
    base = hashlib.sha256(PREFIX)
    for i in range((size + 31) // 32):
        h = base.copy()
        h.update(i.to_bytes(8, 'little'))
        yield h.digest()


def stream(size, chunk_blocks=32768):
    pending, total = [], 0
    for block in blocks(size):
        pending.append(block)
        if len(pending) == chunk_blocks:
            data = b''.join(pending)
            pending = []
            yield data[:size - total]
            total += len(data)
    if pending and total < size:
        yield b''.join(pending)[:size - total]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=('write', 'digest', 'verify'))
    parser.add_argument('path', nargs='?', type=Path)
    parser.add_argument('--size', type=int, default=SIZE)
    args = parser.parse_args()
    if args.command in ('write', 'verify') and not args.path:
        parser.error('a path is required')
    digest = hashlib.sha256()
    if args.command == 'write':
        with args.path.open('xb') as out:
            for data in stream(args.size):
                out.write(data)
                digest.update(data)
    elif args.command == 'digest':
        for data in stream(args.size):
            digest.update(data)
    else:
        if args.path.stat().st_size != args.size:
            sys.exit(f'{args.path} has {args.path.stat().st_size} bytes, expected {args.size}')
        with args.path.open('rb') as f:
            while data := f.read(1 << 20):
                digest.update(data)
    print(digest.hexdigest())
    if args.size == SIZE and EXPECTED_SHA256 and digest.hexdigest() != EXPECTED_SHA256:
        sys.exit(f'digest differs from the expected {EXPECTED_SHA256}')


if __name__ == '__main__':
    main()
