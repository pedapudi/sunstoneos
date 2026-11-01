#!/usr/bin/env python3
"""Embed the existing fixed convolution and reference data after authentication."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location('npu_fixed_catalogue', ROOT/'drivers/npu/catalogue.py')
_catalogue = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_catalogue)

CATALOGUE = json.loads((Path(__file__).resolve().parents[2]/'tools/npu-models.json').read_text())['models']
EXPECTED = tuple((name, CATALOGUE[0][name]['bytes'], CATALOGUE[0][name]['sha256'])
                 for name in ('model','input','reference'))


def prepare(model, input_path, reference, output, execution_family=None):
    _catalogue.verify()
    model_sha = hashlib.sha256(model.read_bytes()).hexdigest()
    selected = next((v for v in CATALOGUE if v['model']['sha256'] == model_sha), None)
    if selected is None: raise ValueError('Model is absent from reviewed build-time catalogue')
    if execution_family is not None and selected.get('family','nn')!=execution_family:
        raise ValueError('Compiled model execution family differs from selected application')
    expected = tuple((name, selected[name]['bytes'], selected[name]['sha256']) for name in ('model','input','reference'))
    lines = ['#include "a733_npu_application.h"']
    identities = {}
    for (name, size, expected_sha), path in zip(expected, (model, input_path, reference)):
        data = path.read_bytes()
        if len(data) != size or hashlib.sha256(data).hexdigest() != expected_sha:
            raise ValueError(f'Unaccepted {name}: {path}')
        identities[name] = {'path': str(path.resolve()), 'bytes': size, 'sha256': expected_sha}
        lines.append(f'const unsigned char a733_npu_application_{name}[{size}]={{' +
                     ','.join(str(x) for x in data) + '};')
    lines.append('const unsigned char a733_npu_application_artifact_hashes[3][32]={' +
                 ','.join('{' + ','.join(str(x) for x in bytes.fromhex(h)) + '}' for _, _, h in expected) + '};')
    for name, size, _ in expected:
        lines.append(f'const size_t a733_npu_application_{name}_bytes={size};')
    output.with_name('npu_models.inc').write_text(_catalogue.render())
    output.write_text('\n'.join(lines) + '\n')
    output.with_suffix('.json').write_text(json.dumps(identities, indent=2) + '\n')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('model', 'input', 'reference', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--execution-family',choices=('nn','sh-nt','nn-pool'))
    a = p.parse_args()
    prepare(a.model, a.input, a.reference, a.output, a.execution_family)
