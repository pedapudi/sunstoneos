#!/usr/bin/env python3
"""Create a host reference for paced segmentation and local recognition."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zlib

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--inputs', type=Path, required=True)
parser.add_argument('--wav', type=Path, required=True)
parser.add_argument('--runner', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
manifest = json.loads((args.inputs / 'inputs.json').read_text())
paths = [args.inputs / 'base.mimus', args.inputs / 'overlay.mimus', args.wav]
inputs = [path.read_bytes() for path in paths]
for path, data in zip(paths[:2], inputs[:2]):
    if hashlib.sha256(data).hexdigest() != manifest['model'][path.name]['sha256']:
        raise SystemExit('Model identity differs from the prepared input manifest.')
bundle = bytearray(128)
struct.pack_into('<8sQQQIII', bundle, 0, b'SUNMIM01', *map(len, inputs),
                 *(zlib.crc32(data) for data in inputs))
for data in inputs:
    bundle.extend(data)
    bundle.extend(b'\0' * (-len(bundle) % 64))
command = list(map(str, [args.runner, *paths]))
result = subprocess.run(command, capture_output=True, text=True, timeout=180)
record = dict(paced=True, command=command, exit=result.returncode,
              stdout=result.stdout, stderr=result.stderr,
              runner_sha256=hashlib.sha256(args.runner.read_bytes()).hexdigest(),
              bundle_sha256=hashlib.sha256(bundle).hexdigest(),
              scope='Unpaced host reference; timing and hardware behavior are not qualified.')
args.output.write_text(json.dumps(record, indent=2) + '\n')
if result.returncode:
    raise SystemExit(f'Reference failed; see {args.output}')
print(args.output)
