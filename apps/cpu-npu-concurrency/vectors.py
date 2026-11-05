#!/usr/bin/env python3
"""Embed one diagnostic's compiled graph, input and reference, selected by name. The application registers the graph at run
time (ss_npu_register_graph): the OS image links none."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--artifacts', type=Path, required=True)
p.add_argument('--models', type=Path, required=True)
p.add_argument('--name', required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
spec = importlib.util.spec_from_file_location('diagnostic_catalogue', ROOT/'tools/npu-test-models.py')
catalogue = importlib.util.module_from_spec(spec)
spec.loader.exec_module(catalogue)
models = catalogue.models(a.models)
entry = next((m for m in models if m['name'] == a.name), None)
if entry is None:
    raise ValueError('Diagnostic model name is absent from its selected model list.')
files = catalogue.catalogue_tool().artifact_files(json.loads(a.artifacts.read_text()), a.name)
text = '#include "vectors.h"\n'
graph = Path(files['model']).read_bytes()
if len(graph) != entry['model']['bytes'] or hashlib.sha256(graph).hexdigest() != entry['model']['sha256']:
    raise ValueError('Diagnostic graph differs from its reviewed identity.')
text += 'const uint8_t cpu_npu_graph[]={' + ','.join(map(str, graph)) + '};\n'
text += 'const size_t cpu_npu_graph_bytes=sizeof(cpu_npu_graph);\n'
for kind, paths in [('input', files['inputs']), ('reference', files['references'])]:
    tensors = entry['inputs' if kind == 'input' else 'outputs']
    if len(paths) != len(tensors):
        raise ValueError('Diagnostic vector tensor count differs.')
    data = b''
    for path, tensor in zip(paths, tensors):
        raw = Path(path).read_bytes()
        expected = tensor['reference']
        if len(raw) != expected['bytes'] or hashlib.sha256(raw).hexdigest() != expected['sha256']:
            raise ValueError('Diagnostic vector differs from its authenticated catalogue data.')
        data += raw
    if not data or len(data) > (1024*1024 if kind == 'input' else 8):
        raise ValueError('Diagnostic vector exceeds its bounded receipt or SDK window.')
    text += f'const uint8_t cpu_npu_{kind}[]={{' + ','.join(map(str,data)) + '};\n'
    text += f'const size_t cpu_npu_{kind}_bytes=sizeof(cpu_npu_{kind});\n'
a.output.write_text(text)
