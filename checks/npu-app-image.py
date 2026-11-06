#!/usr/bin/env python3
"""tools/npu-app-image.py packs graphs and writes the manifest of an NPU application: the index page, the digests, the bounds.

The tool is run with --no-build on a synthetic application with compiled graphs: graph-add and graph-relu of the test graphs
and shufflenet-v2-published, the graph the CPU/NPU diagnostic registers. The check reads the models region back through its
index, compares every graph with its source, and requires the manifest to carry the regions' own digests and the declaration
the graphs imply: the count, the largest graph in whole pages, and tensor_bytes the largest read or write region of the OS
layout. For shufflenet that is its write region, the 1,093,632-byte memory pool and the 4,096-byte output (1,097,728 bytes),
which is also what apps/cpu-npu-concurrency/manifest-npu.json declares; graph-add and graph-relu alone give 16,384 (graph-add's
two inputs). A changed graph or data file must change its region's digest.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ('tools/npu-app-image.py', 'tools/npu-external-pack.py', 'tools/npu_graph_layout.py', 'tools/npu_graph_layout.c',
                'drivers/npu/nbg_model.c', 'drivers/npu/nbg_assembly.c', 'apps/cpu-npu-concurrency/manifest-npu.json')
GATE_RUNS = [['--output', '{output}', '--graphs', '{fixture:npu-graphs}', '--catalogue', '{cache}/build/npu-board/session-artifacts.json']]
PAGE = 4096
CACHE = Path.home() / '.cache/sunstoneos'

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--graphs', type=Path, default=CACHE / 'build/npu-graph-variants', help='the test graphs (artifacts.json)')
p.add_argument('--catalogue', type=Path, default=CACHE / 'build/npu-board/session-artifacts.json',
               help='the artifacts of shufflenet-v2-published')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)


def tool(app, graphs, data, out, *extra, check=True):
    return subprocess.run([sys.executable, str(ROOT / 'tools/npu-app-image.py'), '--app', str(app), '--graphs', str(graphs), '--data', str(data),
                           '--out', str(out), '--no-build', *extra], check=check, capture_output=True, text=True)


app, graphs = a.output / 'app', a.output / 'graphs'
app.mkdir()
graphs.mkdir()
(app / 'host.py').write_text('# decoder\n')
(app / 'manifest.json').write_text(json.dumps({
    'name': 'synthetic', 'regions': [
        {'name': 'models', 'kind': 'input', 'access': ['read'], 'bytes': 0, 'address': '0x0', 'sha256': '0' * 64},
        {'name': 'vectors', 'kind': 'input', 'access': ['read'], 'bytes': 0, 'address': '0x0', 'sha256': '0' * 64}],
    'npu': {'slots': 1, 'in_flight': 8, 'graphs': 1, 'links': 0}}))
tests = json.loads((a.graphs / 'artifacts.json').read_text())
sources = [tests['graph-add'], tests['graph-relu'], json.loads(a.catalogue.read_text())['shufflenet-v2-published']]
artifacts, names = {}, []
for i, source in enumerate(sources):
    name = 'g%d' % i
    names.append(name)
    model = graphs / (name + '.nb')
    model.write_bytes(Path(source['model']).read_bytes())
    artifacts[name] = {'model': str(model), 'inputs': source['inputs'], 'references': source['references']}
(graphs / 'artifacts.json').write_text(json.dumps(artifacts))
(graphs / 'list.json').write_text(json.dumps({'npu_models': names}))
sizes = [Path(artifacts[n]['model']).stat().st_size for n in names]
data = a.output / 'vectors.region'
data.write_bytes(bytes(range(256)) * 64)

out = a.output / 'out'
tool(app, graphs, data, out)
models = (out / 'pack/models.region').read_bytes()
magic, version, count = struct.unpack_from('<4sIQ', models)
assert (magic, version, count) == (b'NMDL', 1, 3), (magic, version, count)
for i, graph in enumerate(sizes):
    offset, length = struct.unpack_from('<QQ', models, 16 + 16 * i)
    assert offset % PAGE == 0 and length == graph and models[offset:offset + length] == Path(artifacts[names[i]]['model']).read_bytes(), i
manifest = json.loads((out / 'manifest.json').read_text())
regions = {r['name']: r for r in manifest['regions']}
assert regions['models']['sha256'] == hashlib.sha256(models).hexdigest() and regions['models']['bytes'] == len(models)
assert regions['vectors']['sha256'] == hashlib.sha256(data.read_bytes()).hexdigest()
assert regions['models']['address'] == '0x44000000' and regions['vectors']['address'] == '0x45200000'
npu = manifest['npu']
assert (npu['slots'], npu['graphs'], npu['in_flight'], npu['links']) == (3, 3, 8, 0), npu
assert sizes[2] == 964224 and npu['graph_bytes'] == 966656 and npu['tensor_bytes'] == 1097728, npu
diagnostic = json.loads((ROOT / 'apps/cpu-npu-concurrency/manifest-npu.json').read_text())['npu']
assert diagnostic['tensor_bytes'] == npu['tensor_bytes'] and diagnostic['graph_bytes'] >= npu['graph_bytes'], diagnostic
# Without shufflenet the largest region is graph-add's two inputs, 16,384 bytes in whole pages.
(graphs / 'list.json').write_text(json.dumps({'npu_models': names[:2]}))
tool(app, graphs, data, a.output / 'small')
small = json.loads((a.output / 'small/manifest.json').read_text())['npu']
assert (small['graphs'], small['tensor_bytes']) == (2, 16384), small
(graphs / 'list.json').write_text(json.dumps({'npu_models': names}))
assert (out / 'recipe.txt').read_text().count('\n') == 4

# a changed graph or data file changes the digest of its region
relu = (graphs / 'g1.nb').read_bytes()
(graphs / 'g1.nb').write_bytes(Path(tests['graph-sub']['model']).read_bytes())
tool(app, graphs, data, a.output / 'out2')
assert json.loads((a.output / 'out2/manifest.json').read_text())['regions'][0]['sha256'] != regions['models']['sha256']
(graphs / 'g1.nb').write_bytes(relu)
data.write_bytes(bytes(reversed(data.read_bytes())))
tool(app, graphs, data, a.output / 'out3')
changed = {r['name']: r for r in json.loads((a.output / 'out3/manifest.json').read_text())['regions']}
assert changed['vectors']['sha256'] != regions['vectors']['sha256'] and changed['models']['sha256'] == regions['models']['sha256']

# an application whose decoder binds to the image: the recipe freezes it and submits the frozen decoder; the unfrozen form is refused
(app / 'host.py').write_text('# decoder\n# host.py --freeze-image ELF --decoder-output FILE --mode N\n')
unfrozen = tool(app, graphs, data, a.output / 'unfrozen', check=False)
assert unfrozen.returncode != 0 and '--freeze-decoder' in unfrozen.stderr, unfrozen
tool(app, graphs, data, a.output / 'frozen', '--freeze-decoder', '--mode 1')
lines = (a.output / 'frozen/recipe.txt').read_text().splitlines()
assert len(lines) == 5, lines
frozen = str(a.output.resolve() / 'frozen/decoder-frozen.py')
assert '--freeze-image' in lines[2] and '--mode 1' in lines[2] and '--decoder-output ' + frozen in lines[2], lines[2]
assert '--decoder ' + frozen in lines[3] and '--decoder ' + frozen in lines[4], lines[3:]
assert 'session/decoder.py' not in lines[4], lines[4]
(app / 'host.py').write_text('# decoder\n')
print('PASS NPU application image tool: index, digests, declaration and sensitivity to the graphs and the data')