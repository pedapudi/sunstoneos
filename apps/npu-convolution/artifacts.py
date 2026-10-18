#!/usr/bin/env python3
"""Embed the existing fixed convolution and reference data after authentication."""
import argparse
import hashlib
import json
from pathlib import Path

EXPECTED = (
    ('model', 1928, 'db42d815100417f4b8913896462ed031beafae35fb4ee1c28569ac950a9e8fca'),
    ('input', 192, '976fdde5d777fd2b60d5146fa9acde1f2a4340c758555c9393f48c09cb1abf12'),
    ('reference', 72, '899612ebf5d62b2bebba2926ef68af8b63cf0050b2dd67fa2d3a5d759a5b9c9b'),
)


def prepare(model, input_path, reference, output):
    lines = ['#include "a733_npu_application.h"']
    identities = {}
    for (name, size, expected), path in zip(EXPECTED, (model, input_path, reference)):
        data = path.read_bytes()
        if len(data) != size or hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f'Unaccepted {name}: {path}')
        identities[name] = {'path': str(path.resolve()), 'bytes': size, 'sha256': expected}
        lines.append(f'const unsigned char a733_npu_application_{name}[{size}]={{' +
                     ','.join(str(x) for x in data) + '};')
    lines.append('const unsigned char a733_npu_application_artifact_hashes[3][32]={' +
                 ','.join('{' + ','.join(str(x) for x in bytes.fromhex(h)) + '}' for _, _, h in EXPECTED) + '};')
    output.write_text('\n'.join(lines) + '\n')
    output.with_suffix('.json').write_text(json.dumps(identities, indent=2) + '\n')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('model', 'input', 'reference', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args()
    prepare(a.model, a.input, a.reference, a.output)
