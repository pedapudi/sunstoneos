#!/usr/bin/env python3
"""Write the inputs of a queue image that holds one reviewed graph in many resident slots.

The EL0 queue keeps one slot, and so one resident catalogue entry, for each handle. To study 31 slots with a graph that
is already reviewed, this tool lists that graph under N names (graph-add-00 ...): the model database, the artifact paths
and the application model list. The entries name the same files and the same digests, so the compiler output and the
reference vectors are the reviewed ones; only the names differ. SUNSTONE_NPU_MODEL_DB selects the database at build."""
import argparse
import copy
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--graph', default='graph-add')
p.add_argument('--count', type=int, default=31)
p.add_argument('--artifacts', type=Path, required=True, help='the artifact paths of the reviewed graphs')
p.add_argument('--output', type=Path, required=True, help='a directory for models.json, artifacts.json and list.json')
a = p.parse_args()
if not 1 <= a.count <= 31:
    p.error('the command mask allows 1 to 31 slots')
database = json.loads((ROOT / 'tools/npu-fixtures.json').read_text())
entry = next(m for m in database['models'] if m['name'] == a.graph)
artifacts = json.loads(a.artifacts.read_text())
names = ['%s-%02d' % (a.graph, i) for i in range(a.count)]
database['models'] = [dict(copy.deepcopy(entry), name=n) for n in names]
a.output.mkdir(parents=True, exist_ok=True)
(a.output / 'models.json').write_text(json.dumps(database, indent=1) + '\n')
(a.output / 'artifacts.json').write_text(json.dumps({n: artifacts[a.graph] for n in names}, indent=1) + '\n')
(a.output / 'list.json').write_text(json.dumps({'npu_models': names}, indent=1) + '\n')
print(len(names), 'names for', a.graph)
