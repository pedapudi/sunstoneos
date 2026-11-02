#!/usr/bin/env python3
"""Pack models, inputs and references into runner input regions, and write the maps that name them.

An image can take its NPU models, and the vectors of a graphs application, from RAM that something else filled instead of
from linked bytes: a runner input region (this tool), or a large region that the large-memory loader reads from the card.
The image names each model by address, length and SHA-256 and the runtime checks the digest at prepare; it does not care
which source filled the memory. This tool makes the first source:

  npu-external-pack.py --artifacts ARTIFACTS.json --models LIST.json --models-address 0x44000000 --vectors-address 0x45000000 \\
      --output DIR

It writes DIR/models.region and DIR/vectors.region (the files for the two input regions, which the manifest declares at
those addresses with those sizes), DIR/models-map.json ({name: {address, bytes, sha256}}, for npu-catalogue.py --external
and the cmake parameter SUNSTONE_OS_NPU_EXTERNAL_MODELS), DIR/vectors-map.json (for apps/npu-graphs/vectors.py --external)
and DIR/regions.json (the region names, addresses, sizes and digests for the manifest). Every item starts on 4096 bytes.
"""
import argparse
import hashlib
import json
from pathlib import Path

PAGE = 4096
p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('--artifacts', type=Path, required=True)
p.add_argument('--models', type=Path, required=True)
p.add_argument('--models-address', type=lambda x: int(x, 0), required=True)
p.add_argument('--vectors-address', type=lambda x: int(x, 0), required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
paths = json.loads(a.artifacts.read_text())
names = json.loads(a.models.read_text())['npu_models']
a.output.mkdir(parents=True, exist_ok=False)


def align(n):
    return -(-n // PAGE) * PAGE


models, vectors, model_map, vector_map = bytearray(), bytearray(), {}, {}
for name in names:
    files = paths[name]
    blob = Path(files['model']).read_bytes()
    models += bytes(align(len(models)) - len(models))
    model_map[name] = {'address': a.models_address + len(models), 'bytes': len(blob), 'sha256': hashlib.sha256(blob).hexdigest()}
    models += blob
    entry = {}
    for kind, key in (('input', 'inputs'), ('reference', 'references')):
        data = b''.join(Path(f).read_bytes() for f in files[key])
        vectors += bytes(align(len(vectors)) - len(vectors))
        entry[kind] = {'address': a.vectors_address + len(vectors), 'bytes': len(data)}
        vectors += data
    vector_map[name] = entry
models += bytes(align(len(models)) - len(models))
vectors += bytes(align(len(vectors)) - len(vectors))
(a.output / 'models.region').write_bytes(models)
(a.output / 'vectors.region').write_bytes(vectors)
(a.output / 'models-map.json').write_text(json.dumps(model_map, indent=1) + '\n')
(a.output / 'vectors-map.json').write_text(json.dumps(vector_map, indent=1) + '\n')
(a.output / 'regions.json').write_text(json.dumps([
    {'name': 'models', 'kind': 'input', 'access': ['read'], 'bytes': len(models), 'address': a.models_address,
     'sha256': hashlib.sha256(models).hexdigest()},
    {'name': 'vectors', 'kind': 'input', 'access': ['read'], 'bytes': len(vectors), 'address': a.vectors_address,
     'sha256': hashlib.sha256(vectors).hexdigest()}], indent=1) + '\n')
print('%d models, %d bytes at %#x; vectors %d bytes at %#x' % (len(names), len(models), a.models_address, len(vectors), a.vectors_address))
