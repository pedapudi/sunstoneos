#!/usr/bin/env python3
"""tools/npu-app-image.py packs graphs and writes the manifest of an NPU application: the index page, the digests, the bounds.

The tool is run with --no-build on a synthetic application: three graphs of known sizes with known tensor files, a data region
and a manifest template. The check reads the models region back through its index, compares every graph with its source, and
requires the manifest to carry the regions' own digests and the declaration the graphs imply (count, largest graph and tensor
group, each rounded to pages plus a page for the tensors). A changed graph or data file must change its region's digest.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--output', '{output}']]
CHECK_INPUTS = ('tools/npu-app-image.py', 'tools/npu-external-pack.py')
PAGE = 4096

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)


def tool(app, graphs, data, out):
    subprocess.run([sys.executable, str(ROOT / 'tools/npu-app-image.py'), '--app', str(app), '--graphs', str(graphs), '--data', str(data),
                    '--out', str(out), '--no-build'], check=True, capture_output=True)


app, graphs = a.output / 'app', a.output / 'graphs'
app.mkdir()
graphs.mkdir()
(app / 'host.py').write_text('# decoder\n')
(app / 'manifest.json').write_text(json.dumps({
    'name': 'synthetic', 'regions': [
        {'name': 'models', 'kind': 'input', 'access': ['read'], 'bytes': 0, 'address': '0x0', 'sha256': '0' * 64},
        {'name': 'vectors', 'kind': 'input', 'access': ['read'], 'bytes': 0, 'address': '0x0', 'sha256': '0' * 64}],
    'npu': {'slots': 1, 'in_flight': 8, 'graphs': 1, 'links': 0}}))
sizes = [(5000, 3000, 700), (40960, 4096, 300), (123, 100000, 5)]     # graph bytes, inputs bytes, references bytes
artifacts, names = {}, []
for i, (graph, inputs, references) in enumerate(sizes):
    name = 'g%d' % i
    names.append(name)
    files = {'model': graphs / (name + '.nb'), 'inputs': [graphs / (name + '.in0')], 'references': [graphs / (name + '.ref0')]}
    files['model'].write_bytes(bytes([i + 1]) * graph)
    files['inputs'][0].write_bytes(bytes(inputs))
    files['references'][0].write_bytes(bytes(references))
    artifacts[name] = {k: str(v) if k == 'model' else [str(x) for x in v] for k, v in files.items()}
(graphs / 'artifacts.json').write_text(json.dumps(artifacts))
(graphs / 'list.json').write_text(json.dumps({'npu_models': names}))
data = a.output / 'vectors.region'
data.write_bytes(bytes(range(256)) * 64)

out = a.output / 'out'
tool(app, graphs, data, out)
models = (out / 'pack/models.region').read_bytes()
magic, version, count = struct.unpack_from('<4sIQ', models)
assert (magic, version, count) == (b'NMDL', 1, 3), (magic, version, count)
for i, (graph, _, _) in enumerate(sizes):
    offset, length = struct.unpack_from('<QQ', models, 16 + 16 * i)
    assert offset % PAGE == 0 and length == graph and models[offset:offset + length] == bytes([i + 1]) * graph, i
manifest = json.loads((out / 'manifest.json').read_text())
regions = {r['name']: r for r in manifest['regions']}
assert regions['models']['sha256'] == hashlib.sha256(models).hexdigest() and regions['models']['bytes'] == len(models)
assert regions['vectors']['sha256'] == hashlib.sha256(data.read_bytes()).hexdigest()
assert regions['models']['address'] == '0x44000000' and regions['vectors']['address'] == '0x45200000'
npu = manifest['npu']
largest_group = max(max(i, r) for _, i, r in sizes)
assert (npu['slots'], npu['graphs'], npu['in_flight'], npu['links']) == (3, 3, 8, 0), npu
assert npu['graph_bytes'] == 40960 and npu['tensor_bytes'] == -(-largest_group // PAGE) * PAGE + PAGE, npu
assert (out / 'recipe.txt').read_text().count('\n') == 4

# a changed graph or data file changes the digest of its region
(graphs / 'g1.nb').write_bytes(bytes([9]) * 40960)
tool(app, graphs, data, a.output / 'out2')
assert json.loads((a.output / 'out2/manifest.json').read_text())['regions'][0]['sha256'] != regions['models']['sha256']
(graphs / 'g1.nb').write_bytes(bytes([2]) * 40960)
data.write_bytes(bytes(reversed(data.read_bytes())))
tool(app, graphs, data, a.output / 'out3')
changed = {r['name']: r for r in json.loads((a.output / 'out3/manifest.json').read_text())['regions']}
assert changed['vectors']['sha256'] != regions['vectors']['sha256'] and changed['models']['sha256'] == regions['models']['sha256']
print('PASS NPU application image tool: index, digests, declaration and sensitivity to the graphs and the data')
