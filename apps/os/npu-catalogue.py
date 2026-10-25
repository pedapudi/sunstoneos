#!/usr/bin/env python3
"""Embed the authenticated NPU catalogue and one explicit application vector."""
import argparse
import hashlib
import json
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]

def generate(artifacts, output, model=0):
    catalogue = json.loads((ROOT / 'tools/npu-session-models.json').read_text())['models']
    paths = json.loads(Path(artifacts).read_text())
    if len(catalogue) != 5 or not 0 <= model < len(catalogue):
        raise ValueError('Unsupported OS catalogue/client selection')
    text = '#include "npu.h"\n'
    digests, rows = b'', []
    def blob(name, path, expected):
        nonlocal text
        data = Path(path).read_bytes()
        if len(data) != expected['bytes'] or hashlib.sha256(data).hexdigest() != expected['sha256']:
            raise ValueError(f'{name}: reviewed artifact differs')
        text += f'static const uint8_t {name}[{len(data)}] __attribute__((aligned(64)))={{' + ','.join(map(str, data)) + '};\n'
        return data
    for i, entry in enumerate(catalogue):
        data = blob(f'model_{i}', paths[entry['name']]['model'], entry['model'])
        rows.append(f'{{model_{i},sizeof(model_{i})}}')
        digests += hashlib.sha256(data).digest()
    text += 'const struct npu_runtime_model os_npu_models[OS_NPU_MODELS]={' + ','.join(rows) + '};\n'
    text += 'const uint8_t os_npu_catalogue_digest[32]={' + ','.join(map(str, hashlib.sha256(digests).digest())) + '};\n'
    selected = catalogue[model]
    for section, field in [('inputs', 'inputs'), ('references', 'outputs')]:
        rows = []
        for i, tensor in enumerate(selected[field]):
            name = f'{section}_{i}'
            blob(name, paths[selected['name']][section][i], tensor['reference'])
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
    a = p.parse_args()
    generate(a.artifacts, a.output, a.model)
