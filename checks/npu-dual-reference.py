#!/usr/bin/env python3
"""Compare saved compiler/simulator outputs of the two-input, two-output fixture.

This check reads artifacts only. It neither invokes the compiler nor executes
device commands. Run in the pinned compiler environment with NumPy and PyYAML.
Each output must match the exact float reference before quantization, and the
simulator's bytes must equal the compiler's quantized CPU inference.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

import numpy as np
import yaml

assert __debug__, 'run without -O: checks decide pass/fail with assert'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixture', type=Path)
    parser.add_argument('report', type=Path, help='new JSON evidence file')
    args = parser.parse_args()
    root = args.fixture
    inputs = []

    def path(name):
        p = root / name
        inputs.append(p)
        return p

    def single(directory, pattern):
        found = sorted((root / directory).glob(pattern))
        require(len(found) == 1, f'expected one {directory}/{pattern}')
        inputs.append(found[0])
        return found[0]

    quant = yaml.safe_load(path('dual.quantize').read_text())['quantize_parameters']
    qtype = quant['@input0_4:out0']['qtype']
    require(qtype in ('u8', 'i8'), 'unexpected tensor type')
    low, high, dtype = (0, 255, np.uint8) if qtype == 'u8' else (-128, 127, np.int8)
    weights = [v for k, v in quant.items() if k.endswith(':weight')]
    require(len(weights) == 2 and all(
        v['quantizer'] == ('asymmetric_affine' if qtype == 'u8' else 'perchannel_symmetric_affine')
        for v in weights), 'unexpected weight quantization')

    def tensor(p, shape, quantized=False):
        values = np.loadtxt(str(p), dtype=np.float64)
        require(np.all(np.isfinite(values)), 'nonfinite tensor')
        if quantized:
            require(np.all((values >= low) & (values <= high) & (values == np.floor(values))),
                    'invalid quantized tensor')
        return values.reshape(shape)

    meta = json.loads(path('export_nbg_unify/network_binary.nb').with_name('nbg_meta.json').read_text())
    summary = dict(input_tensors=[], output_tensors=[])
    for index in range(2):
        raw = np.load(str(path(f'input{index}.npy')), allow_pickle=False)
        stem = f'iter_0_input{index}_*_out0_' + '_'.join(map(str, raw.shape))
        floating = tensor(single('float-reference', stem + '.tensor'), raw.shape)
        require(np.array_equal(floating, raw), f'input {index} preprocessing changed values')
        quantized = tensor(single('quantized-reference', stem + '.qnt.tensor'), raw.shape, True)
        simulated = np.fromfile(str(path(f'export_nbg_unify/input_{index}.dat')), dtype=dtype)
        require(np.array_equal(simulated, quantized.ravel()), f'simulator input {index} differs')
        port = next(v for k, v in meta['Inputs'].items() if v['name'] == f'input{index}')
        summary['input_tensors'].append(dict(shape=list(raw.shape), bytes=int(raw.size), quantization=port['quantize']))
    for index in range(2):
        expected = np.load(str(path(f'expected{index}.npy')), allow_pickle=False)
        _, channels, height, width = expected.shape
        stem = f'iter_0_attach_output{index}_out0_{index}_out0_1_{channels}_{height}_{width}'
        floating = tensor(single('float-reference', stem + '.tensor'), expected.shape)
        require(np.array_equal(floating, expected), f'float output {index} differs from the exact reference')
        quantized = tensor(single('quantized-reference', stem + '.qnt.tensor'), expected.shape, True)
        simulated = np.fromfile(str(path(f'export_nbg_unify/output{index}_{width}_{height}_{channels}_1.dat')),
                                dtype=dtype)
        require(np.array_equal(simulated, quantized.ravel()), f'simulator output {index} differs')
        q = quant[f'@attach_output{index}/out0_{index}:out0']
        require(q['qtype'] == qtype and np.isfinite(q['scale']) and q['scale'] > 0 and
                isinstance(q['zero_point'], int) and low <= q['zero_point'] <= high,
                f'invalid output {index} quantization')
        error = (simulated.astype(np.float64) - q['zero_point']) * q['scale'] - expected.ravel()
        summary['output_tensors'].append(dict(shape=list(expected.shape), bytes=int(simulated.size),
                                       quantization=q, max_float_error=float(np.max(np.abs(error)))))
    model = path('export_nbg_unify/network_binary.nb').read_bytes()
    require(model[:4] == b'VPMN', 'missing NBG header')
    version, target = struct.unpack_from('<II', model, 4)
    require(version == 0x20000 and target == 0x1000003b, 'unexpected NBG version or target')
    report = dict(result='pass', tensor_type=qtype, float_reference_exact=True, simulator_inputs_exact=True,
                  simulator_outputs_match_quantized_cpu=True, nbg_version=hex(version),
                  target=hex(target), nbg_bytes=len(model),
                  nbg_sha256=hashlib.sha256(model).hexdigest(), **summary,
                  physical_access=False, native_loader_execution=False,
                  artifacts=[dict(path=str(p.relative_to(root)), bytes=p.stat().st_size,
                               sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in inputs])
    with args.report.open('x') as output:
        output.write(json.dumps(report, indent=2) + '\n')
    print('PASS dual references: exact float outputs and',
          sum(v['bytes'] for v in summary['output_tensors']), 'simulator output bytes')


if __name__ == '__main__':
    main()
