#!/usr/bin/env python3
"""Embed the client's authenticated NPU model and its input and reference vectors.

The default links entry --model from apps/os/npu-models.json. --models adds
prepared resident entries in the given order, with the client model first.
os_npu_catalogue_digest hashes the concatenated model digests in that order;
the session extension binds this value.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
MODELS = ROOT / 'apps/os/npu-models.json'

def catalogue_tool():
    """Load the shared catalogue and historical artifact-map compatibility."""
    spec = importlib.util.spec_from_file_location('npu_session_catalogue', ROOT / 'tools/npu-session-catalogue.py')
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    return tool

def models():
    """The OS application's catalogue entries, in model-list order."""
    return catalogue_tool().application_models([MODELS])

def generate(artifacts, output, model=0, queue_models=None):
    catalogue = models()
    paths = json.loads(Path(artifacts).read_text())
    if not 0 <= model < len(catalogue):
        raise ValueError('Unsupported OS client selection')
    text = '#include "npu.h"\n'
    digests, rows = b'', []
    def blob(name, path, expected):
        nonlocal text
        data = Path(path).read_bytes()
        if len(data) != expected['bytes'] or hashlib.sha256(data).hexdigest() != expected['sha256']:
            raise ValueError(f'{name}: reviewed artifact differs')
        text += f'static const uint8_t {name}[{len(data)}] __attribute__((aligned(64),section(".data.os_npu_assets")))={{' + ','.join(map(str, data)) + '};\n'
        return data
    selected = catalogue[model]
    files = catalogue_tool().artifact_files(paths, selected['name'])
    indices = [model] if queue_models is None else queue_models
    if not indices or indices[0] != model or len(indices) > 8 or len(set(indices)) != len(indices):
        raise ValueError('Queue entries must be distinct, begin with the client model and fit eight slots')
    for slot, index in enumerate(indices):
        if not 0 <= index < len(catalogue):
            raise ValueError('Unsupported queue model selection')
        entry = catalogue[index]
        entry_files = catalogue_tool().artifact_files(paths, entry['name'])
        data = blob(f'model_{slot}', entry_files['model'], entry['model'])
        rows.append(f'{{model_{slot},sizeof(model_{slot})}}')
        digests += hashlib.sha256(data).digest()
    text += 'const struct npu_runtime_model os_npu_models[OS_NPU_MODELS]={' + ','.join(rows) + '};\n'
    text += 'const uint8_t os_npu_catalogue_digest[32]={' + ','.join(map(str, hashlib.sha256(digests).digest())) + '};\n'
    for section, field in [('inputs', 'inputs'), ('references', 'outputs')]:
        rows = []
        for i, tensor in enumerate(selected[field]):
            name = f'{section}_{i}'
            blob(name, files[section][i], tensor['reference'])
            rows.append(f'{{{name},sizeof({name})}}')
        text += f'const struct os_npu_vector os_npu_{section}[]={{' + ','.join(rows) + '};\n'
        symbol = 'input' if section == 'inputs' else 'output'
        text += f'const unsigned os_npu_{symbol}_count={len(rows)};\n'
    text += f'const unsigned os_npu_client_model={model};\n'
    Path(output).write_text(text)

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--artifacts', required=True)
    p.add_argument('--output', required=True)
    p.add_argument('--model', type=int, default=0)
    p.add_argument('--models', help='Comma-separated prepared resident entries, client model first')
    a = p.parse_args()
    generate(a.artifacts, a.output, a.model, None if a.models is None else [int(i) for i in a.models.split(',')])
