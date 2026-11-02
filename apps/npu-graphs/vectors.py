#!/usr/bin/env python3
"""Embed the authenticated input and reference tensors of every graph in a model list."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--artifacts', type=Path, required=True)
p.add_argument('--models', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--runtime-models', type=Path, help='a model list whose graphs the application registers at run time')
a = p.parse_args()
spec = importlib.util.spec_from_file_location('graph_catalogue', ROOT / 'apps/os/npu-catalogue.py')
catalogue = importlib.util.module_from_spec(spec)
spec.loader.exec_module(catalogue)
entries = catalogue.models(a.models)
artifacts = json.loads(a.artifacts.read_text())
text = '#include "vectors.h"\n'
rows = []
for number, entry in enumerate(entries):
    files = catalogue.catalogue_tool().artifact_files(artifacts, entry['name'])
    blobs = {}
    for kind, paths, tensors in (('input', files['inputs'], entry['inputs']), ('reference', files['references'], entry['outputs'])):
        if len(paths) != len(tensors):
            raise ValueError(entry['name'] + ': vector tensor count differs')
        data = b''
        for path, tensor in zip(paths, tensors):
            raw = Path(path).read_bytes()
            expected = tensor['reference']
            if len(raw) != expected['bytes'] or hashlib.sha256(raw).hexdigest() != expected['sha256']:
                raise ValueError(entry['name'] + ': vector differs from its authenticated catalogue data')
            data += raw
        if not 0 < len(data) <= 1024 * 1024:
            raise ValueError(entry['name'] + ': vector size outside the SDK window')
        blobs[kind] = data
        text += f'static const uint8_t {kind}_{number}[{len(data)}]={{' + ','.join(map(str, data)) + '};\n'
    rows.append(f'{{{json.dumps(entry["name"])},input_{number},sizeof(input_{number}),reference_{number},sizeof(reference_{number})}}')
text += 'const struct npu_graph_vector npu_graph_vectors[]={' + ','.join(rows) + '};\n'
text += f'const unsigned npu_graph_count={len(entries)}u;\n'
# Graphs for run-time registration: the model bytes are embedded too (the OS image links no entry for them).
runtime_rows = []
if a.runtime_models:
    for number, entry in enumerate(catalogue.models(a.runtime_models)):
        files = catalogue.catalogue_tool().artifact_files(artifacts, entry['name'])
        model = Path(files['model']).read_bytes()
        if len(model) != entry['model']['bytes'] or hashlib.sha256(model).hexdigest() != entry['model']['sha256']:
            raise ValueError(entry['name'] + ': model differs from its catalogue data')
        text += f'static const uint8_t runtime_model_{number}[{len(model)}]={{' + ','.join(map(str, model)) + '};\n'
        data = {}
        for kind, paths, tensors in (('input', files['inputs'], entry['inputs']), ('reference', files['references'], entry['outputs'])):
            data[kind] = b''.join(Path(p).read_bytes() for p in paths)
            text += f'static const uint8_t runtime_{kind}_{number}[{len(data[kind])}]={{' + ','.join(map(str, data[kind])) + '};\n'
        runtime_rows.append('{{%s,runtime_input_%d,sizeof(runtime_input_%d),runtime_reference_%d,sizeof(runtime_reference_%d)},runtime_model_%d,sizeof(runtime_model_%d)}' % (
            json.dumps(entry['name']), number, number, number, number, number, number))
text += 'const struct npu_graph_runtime npu_graph_runtime[]={' + (','.join(runtime_rows) or '{{0}}') + '};\n'
text += f'const unsigned npu_graph_runtime_count={len(runtime_rows)}u;\n'
a.output.write_text(text)
