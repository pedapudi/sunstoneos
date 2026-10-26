#!/usr/bin/env python3
"""Embed the client's authenticated NPU model and its input and reference vectors.

Only the catalogue entry that the application client loads is linked; the
other entries have no caller. os_npu_catalogue_digest is the SHA-256 of the
linked model's SHA-256 digest, the value the session extension binds.
"""
import argparse
import hashlib
import json
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]

def generate(artifacts, output, model=0, ram_assets=False):
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
        section = ',section(".data.os_npu_assets")' if ram_assets else ''
        text += f'static const uint8_t {name}[{len(data)}] __attribute__((aligned(64){section}))={{' + ','.join(map(str, data)) + '};\n'
        return data
    selected = catalogue[model]
    data = blob('model_0', paths[selected['name']]['model'], selected['model'])
    rows.append('{model_0,sizeof(model_0)}')
    digests += hashlib.sha256(data).digest()
    text += 'const struct npu_runtime_model os_npu_models[OS_NPU_MODELS]={' + ','.join(rows) + '};\n'
    text += 'const uint8_t os_npu_catalogue_digest[32]={' + ','.join(map(str, hashlib.sha256(digests).digest())) + '};\n'
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
    p.add_argument('--ram-assets', action='store_true', help='Place resident const payloads in existing non-executable initialized RAM')
    a = p.parse_args()
    generate(a.artifacts, a.output, a.model, a.ram_assets)
