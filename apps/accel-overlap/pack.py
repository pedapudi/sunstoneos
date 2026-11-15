#!/usr/bin/env python3
"""Write the vectors region of the accel-overlap application (result.h, struct accel_overlap_vectors): a header, then the
relu graph's input tensor and its simulator reference output, each on its own page.

  pack.py --graphs DIR --output vectors.region

DIR holds artifacts.json (name -> {model, inputs, references}) and list.json ({"npu_models": [names]}) with one graph.
"""
import argparse
import json
import struct
from pathlib import Path

PAGE = 4096
MAGIC = 0x56564f41
p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('--graphs', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
artifacts = json.loads((a.graphs / 'artifacts.json').read_text())
names = json.loads((a.graphs / 'list.json').read_text())['npu_models']
if len(names) != 1:
    raise SystemExit('accel-overlap registers one graph; the list names %d' % len(names))
files = artifacts[names[0]]
if len(files['inputs']) != 1 or len(files['references']) != 1:
    raise SystemExit('accel-overlap checks one input tensor and one output tensor')
data = [Path(files[k][0]).read_bytes() for k in ('inputs', 'references')]
offsets = [PAGE, PAGE + -(-len(data[0]) // PAGE) * PAGE]
region = bytearray(struct.pack('<6I', MAGIC, 1, offsets[0], len(data[0]), offsets[1], len(data[1])))
for offset, blob in zip(offsets, data):
    region.extend(bytes(offset - len(region)))
    region.extend(blob)
region.extend(bytes(-len(region) % PAGE))
a.output.write_bytes(bytes(region))
print('%s: input %d bytes, reference %d bytes, region %d bytes' % (names[0], len(data[0]), len(data[1]), len(region)))
