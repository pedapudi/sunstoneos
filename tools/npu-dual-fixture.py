#!/usr/bin/env python3
"""Generate a two-input, two-output convolution graph with exact references.

Run with the pinned host compiler's Python environment (NumPy and ONNX). The
graph holds two independent 3x3 convolutions so that each output depends on
exactly one input. The float reference precedes compiler quantization; it is
not an NPU oracle.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper


def convolution(values, weights, bias):
    """Exact integer cross-correlation without padding at unit stride."""
    channels, size = len(weights), values.shape[2] - 2
    result = np.empty((1, channels, size, size), dtype=np.int64)
    for c in range(channels):
        for y in range(size):
            for x in range(size):
                result[0, c, y, x] = int(bias[c]) + sum(
                    int(values[0, z, y + ky, x + kx]) * int(weights[c, z, ky, kx])
                    for z in range(values.shape[1]) for ky in range(3) for kx in range(3))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, help='new fixture directory')
    out = parser.parse_args().output
    out.mkdir(parents=True, exist_ok=False)
    # Dyadic values keep every accumulator and every float result exact.
    x0 = ((np.arange(192, dtype=np.int32) * 7) % 33 - 16).reshape(1, 3, 8, 8)
    x1 = ((np.arange(36, dtype=np.int32) * 5) % 29 - 14).reshape(1, 1, 6, 6)
    w0 = (np.arange(54, dtype=np.int32) % 5 - 2).reshape(2, 3, 3, 3)
    w1 = (np.arange(18, dtype=np.int32) % 7 - 3).reshape(2, 1, 3, 3)
    b0 = np.array([16, -32], dtype=np.int32)
    b1 = np.array([-8, 24], dtype=np.int32)
    y0, y1 = convolution(x0, w0, b0), convolution(x1, w1, b1)
    arrays = dict(input0=x0.astype(np.float32) / 8, input1=x1.astype(np.float32) / 8,
                  weights0=w0.astype(np.float32) / 8, bias0=b0.astype(np.float32) / 64,
                  weights1=w1.astype(np.float32) / 8, bias1=b1.astype(np.float32) / 64,
                  expected0=y0.astype(np.float32) / 64, expected1=y1.astype(np.float32) / 64,
                  accumulator0=y0, accumulator1=y1)
    nodes = [
        helper.make_node('Conv', ['input0', 'weights0', 'bias0'], ['output0'],
                         kernel_shape=[3, 3], strides=[1, 1], pads=[0, 0, 0, 0]),
        helper.make_node('Conv', ['input1', 'weights1', 'bias1'], ['output1'],
                         kernel_shape=[3, 3], strides=[1, 1], pads=[0, 0, 0, 0]),
    ]
    graph = helper.make_graph(
        nodes, 'dual',
        [helper.make_tensor_value_info('input0', TensorProto.FLOAT, [1, 3, 8, 8]),
         helper.make_tensor_value_info('input1', TensorProto.FLOAT, [1, 1, 6, 6])],
        [helper.make_tensor_value_info('output0', TensorProto.FLOAT, [1, 2, 6, 6]),
         helper.make_tensor_value_info('output1', TensorProto.FLOAT, [1, 2, 4, 4])],
        [numpy_helper.from_array(arrays[n], n) for n in ('weights0', 'bias0', 'weights1', 'bias1')])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid('', 11)])
    model.ir_version = 7
    onnx.checker.check_model(model)
    onnx.save(model, str(out / 'dual.onnx'))
    for name, array in arrays.items():
        np.save(str(out / (name + '.npy')), array, allow_pickle=False)
    report = dict(graph='two independent Conv2D operations', inputs=[[1, 3, 8, 8], [1, 1, 6, 6]],
                  outputs=[[1, 2, 6, 6], [1, 2, 4, 4]],
                  reference='Exact integer cross-correlations including bias, divided by 64.',
                  quantization_applied=False, device_execution=False,
                  files={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(out.iterdir())})
    (out / 'fixture.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Generated dual graph:', [a.size for a in (y0, y1)], 'reference outputs')


if __name__ == '__main__':
    main()
