#!/usr/bin/env python3
"""Embed verified models from a caller-supplied application model list.

The default profile links only the diagnostic selection. --all links every
listed model in data order. Vectors belong to the selected diagnostic model.
The session digest remains SHA-256 of the linked model SHA-256 digests.
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

def models(model_list=MODELS):
    """The OS application's catalogue entries, in model-list order."""
    return catalogue_tool().application_models([model_list])

def generate(artifacts, output, model=0, model_list=MODELS, all_models=False, model_name=None, queue_models=None):
    # Keep the resident queue producer's positional list compatible.
    if isinstance(model_list, (list, tuple)):
        queue_models, model_list = model_list, MODELS
    catalogue = models(model_list)
    if model_name is not None:
        names = [m['name'] for m in catalogue]
        if model_name not in names:
            raise ValueError('Unsupported OS client name: ' + model_name)
        model = names.index(model_name)
    paths = json.loads(Path(artifacts).read_text())
    if not 0 <= model < len(catalogue):
        raise ValueError('Unsupported OS client selection')
    if queue_models is not None:
        if all_models or not queue_models or queue_models[0]!=model or len(queue_models)>8 or len(set(queue_models))!=len(queue_models):
            raise ValueError('Queue entries must be distinct, begin with the client model and fit eight slots')
        if any(not 0<=i<len(catalogue) for i in queue_models):
            raise ValueError('Unsupported queue model selection')
        linked = [catalogue[i] for i in queue_models]
    else:
        linked = catalogue if all_models else [catalogue[model]]
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
    metadata = []
    for i, entry in enumerate(linked):
        name = entry['name']
        if not name or len(name.encode('utf-8')) >= 64 or '\0' in name:
            raise ValueError('Model name exceeds SS_NPU_MODEL_NAME_BYTES capacity')
        files = catalogue_tool().artifact_files(paths, name)
        data = blob(f'model_{i}', files['model'], entry['model'])
        rows.append(f'{{model_{i},sizeof(model_{i})}}')
        digests += hashlib.sha256(data).digest()
        sizes = [sum(t['logical_bytes'] for t in entry[field]) for field in ('inputs', 'outputs')]
        if not all(0 < n <= 1024 * 1024 for n in sizes):
            raise ValueError('Model tensors exceed SS_NPU_MAX_TENSOR_BYTES capacity')
        metadata.append('{' + json.dumps(name, ensure_ascii=True) +
                        f",{len(entry['inputs'])}u,{len(entry['outputs'])}u,{sizes[0]}u,{sizes[1]}u" + '}')
    text += 'const struct npu_runtime_model os_npu_models[]={' + ','.join(rows) + '};\n'
    text += 'const struct ss_npu_model os_npu_model_info[]={' + ','.join(metadata) + '};\n'
    text += f'const unsigned os_npu_model_count={len(linked)}u;\n'
    text += f'const unsigned os_npu_client_entry={model if all_models else 0}u;\n'
    files = catalogue_tool().artifact_files(paths, selected['name'])
    if len(files['inputs']) != len(selected['inputs']) or len(files['references']) != len(selected['outputs']):
        raise ValueError('One artifact is required per selected model tensor')
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
    p.add_argument('--model-name', help='semantic diagnostic model name; overrides --model')
    p.add_argument('--models', default=str(MODELS), help='application model-list JSON or legacy comma-separated queue indices')
    p.add_argument('--queue-models', help='comma-separated resident entries, client first')
    p.add_argument('--all', action='store_true', help='link all listed models')
    a = p.parse_args()
    queue = None if a.queue_models is None else [int(i) for i in a.queue_models.split(',')]
    model_list = Path(a.models)
    if not model_list.is_file() and all(i.strip().isdigit() for i in a.models.split(',')):
        if queue is not None:p.error('Specify queue indices once')
        queue = [int(i) for i in a.models.split(',')]; model_list = MODELS
    generate(a.artifacts, a.output, a.model, model_list, a.all, a.model_name, queue)
