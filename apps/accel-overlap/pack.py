#!/usr/bin/env python3
"""Write the vectors region of the accel-overlap application (result.h, struct accel_overlap_vectors): a header, then each
relu graph's input tensor and its simulator reference output, each on its own page.

  pack.py --graphs DIR --output vectors.region

DIR holds artifacts.json (name -> {model, inputs, references}) and list.json ({"npu_models": [names]}). With one graph the
header is version 1; with two to four (the counter build) it is version 2 (struct accel_overlap_vectors2), whose entries
follow list.json, the order of the models region.
"""
import argparse
import json
import struct
from pathlib import Path

PAGE = 4096
MAGIC = 0x56564f41
GRAPHS = 4      # ACCEL_OVERLAP_NPU_GRAPHS
p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('--graphs', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
artifacts = json.loads((a.graphs / 'artifacts.json').read_text())
names = json.loads((a.graphs / 'list.json').read_text())['npu_models']
if not 1 <= len(names) <= GRAPHS:
    raise SystemExit('accel-overlap registers one to %d graphs; the list names %d' % (GRAPHS, len(names)))
blobs = []
for name in names:
    files = artifacts[name]
    if len(files['inputs']) != 1 or len(files['references']) != 1:
        raise SystemExit('%s: accel-overlap checks one input tensor and one output tensor' % name)
    blobs += [Path(files[k][0]).read_bytes() for k in ('inputs', 'references')]
offsets, at = [], PAGE
for blob in blobs:
    offsets.append(at)
    at += -(-len(blob) // PAGE) * PAGE
if len(names) == 1:
    region = bytearray(struct.pack('<6I', MAGIC, 1, offsets[0], len(blobs[0]), offsets[1], len(blobs[1])))
else:
    region = bytearray(struct.pack('<4I', MAGIC, 2, len(names), 0))
    for i in range(len(names)):
        region += struct.pack('<4I', offsets[2 * i], len(blobs[2 * i]), offsets[2 * i + 1], len(blobs[2 * i + 1]))
for offset, blob in zip(offsets, blobs):
    region.extend(bytes(offset - len(region)))
    region.extend(blob)
region.extend(bytes(-len(region) % PAGE))
a.output.write_bytes(bytes(region))
for i, name in enumerate(names):
    print('%s: input %d bytes, reference %d bytes' % (name, len(blobs[2 * i]), len(blobs[2 * i + 1])))
print('region %d bytes, version %d' % (len(region), 1 if len(names) == 1 else 2))
