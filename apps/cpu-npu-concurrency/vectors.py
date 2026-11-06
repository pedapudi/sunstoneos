#!/usr/bin/env python3
"""Authenticate one diagnostic's compiled graph, input and reference, selected by name, and write what the application needs.

The OS image links no graph and the application links neither the graph nor the input: both arrive in input regions that the
runner uploads (the graph behind an index in the models region, the input in the vectors region), and the application registers
the graph at run time (ss_npu_register_graph). This tool checks the graph, the input and the reference against the catalogue's
digests and writes

  --output  a C source that holds the reference output (at most eight bytes, which the receipt carries), and
  --region  (optional) the vectors region: the input tensors, concatenated, with no padding.

tools/npu-app-image.py packs the models region from the same artifacts."""
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
p.add_argument('--region', type=Path)
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
    if kind == 'input':
        if a.region:
            a.region.write_bytes(data)
        continue
    text += 'const uint8_t cpu_npu_reference[]={' + ','.join(map(str, data)) + '};\n'
    text += 'const size_t cpu_npu_reference_bytes=sizeof(cpu_npu_reference);\n'
a.output.write_text(text)
