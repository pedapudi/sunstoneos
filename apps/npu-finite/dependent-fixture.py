#!/usr/bin/env python3
"""Generate a convolution whose input is the admitted conv12 output tensor.

Run in the pinned compiler container. The producer's saved quantized output
supplies calibration data; its quantization is preserved at the boundary.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
import onnx
from onnx import helper, numpy_helper, TensorProto


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('producer', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    source = args.producer
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output
    check = json.loads((source / 'reference-check.json').read_text())
    assert check['result'] == 'pass' and check['simulator_input_exact'] and check['quantized_reference_exact']
    quantization = check['output_quantization']
    raw = next((source / 'quantized-reference').glob('*attach_output*.qnt.tensor'))
    # SDK tensor text is NCHW. The native flat tensor has reversed dimensions
    # in its descriptor, with the same contiguous channel/row/column order.
    codes = np.loadtxt(str(raw), dtype=np.float32).reshape(1, 4, 10, 10)
    assert codes.size == 400 and np.all(codes == np.rint(codes))
    x = (codes - quantization['zero_point']) * quantization['scale']
    weights = ((np.arange(72, dtype=np.int32) % 7) - 3).reshape(2, 4, 3, 3).astype(np.float32) / 32
    bias = np.array([0.125, -0.0625], dtype=np.float32)
    graph = helper.make_graph([
        helper.make_node('Conv', ['input', 'weights', 'bias'], ['output'],
                         kernel_shape=[3, 3], strides=[1, 1], pads=[0, 0, 0, 0])],
        'dependent_conv', [helper.make_tensor_value_info('input', TensorProto.FLOAT, [1, 4, 10, 10])],
        [helper.make_tensor_value_info('output', TensorProto.FLOAT, [1, 2, 8, 8])],
        [numpy_helper.from_array(weights, 'weights'), numpy_helper.from_array(bias, 'bias')])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid('', 11)])
    model.ir_version = 7
    onnx.checker.check_model(model)
    onnx.save(model, str(output / 'conv3x3.onnx'))
    np.save(str(output / 'input.npy'), x.astype(np.float32), allow_pickle=False)
    (output / 'producer-output.bin').write_bytes(codes.astype(np.uint8).tobytes())
    (output / 'producer-quantization.json').write_text(json.dumps(quantization, indent=2) + '\n')
    (output / 'fixture.json').write_text(json.dumps({
        'producer': 'conv12', 'producer_model_sha256': hashlib.sha256((source / 'export_nbg_unify/network_binary.nb').read_bytes()).hexdigest(),
        'input_nchw': [1, 4, 10, 10], 'output_nchw': [1, 2, 8, 8],
        'boundary_quantization': quantization, 'physical_access': False,
        'files': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in output.iterdir()}}, indent=2) + '\n')
    print('Output:', output)


if __name__ == '__main__':
    main()
