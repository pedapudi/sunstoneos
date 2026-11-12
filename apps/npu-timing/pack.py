#!/usr/bin/env python3
"""Write the data region of the NPU timing application (timing.h): a header that names, for each graph of the list, its single
jobs, the jobs in flight of its pipelined pass and where its inputs and reference outputs lie, then those files.

  pack.py --graphs DIR --jobs NAME=SINGLE[:DEPTH] ... --output vectors.region

DIR holds artifacts.json (name -> {model, inputs, references}) and list.json ({"npu_models": [names]}), in the order the OS
admits them. A graph not named in --jobs runs 100 single jobs and 100 with 2 in flight.
"""
import argparse
import json
import struct
from pathlib import Path

PAGE = 4096
p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('--graphs', type=Path, required=True)
p.add_argument('--jobs', action='append', default=[])
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
artifacts = json.loads((a.graphs / 'artifacts.json').read_text())
names = json.loads((a.graphs / 'list.json').read_text())['npu_models']
assert 0 < len(names) <= 16, len(names)
plan = {}
for item in a.jobs:
    name, spec = item.split('=')
    single, _, depth = spec.partition(':')
    plan[name] = (int(single), int(depth or 0))
assert set(plan) <= set(names), set(plan) - set(names)
body, rows = bytearray(), []
header_bytes = 16 + 16 * 40
for name in names:
    files = artifacts[name]
    blobs = []
    for key in ('inputs', 'references'):
        data = b''.join(Path(f).read_bytes() for f in files[key])
        body.extend(bytes(-(header_bytes + len(body)) % PAGE))
        blobs.append((header_bytes + len(body), len(data)))
        body.extend(data)
    single, depth = plan.get(name, (100, 2))
    assert 0 < single <= 1000 and 0 <= depth <= 64
    rows.append(struct.pack('<IIQQQQ', single, depth, *blobs[0], *blobs[1]))
rows += [bytes(40)] * (16 - len(rows))
region = struct.pack('<4I', 0x4d49544e, 1, len(names), 0) + b''.join(rows) + body
region += bytes(-len(region) % PAGE)
a.output.write_bytes(region)
print('%d graphs, %d bytes' % (len(names), len(region)))
