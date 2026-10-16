#!/usr/bin/env python3
"""Generate a deterministic ONNX convolution and an exact CPU reference.

Run with the pinned host compiler's Python environment (NumPy and ONNX).
The float reference precedes compiler quantization; it is not an NPU oracle.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="new directory for the fixture")
    args = parser.parse_args()

    # All values are dyadic fractions. The accumulator below is integer-exact;
    # division by 64 is exactly representable for every generated output.
    input_integer = ((np.arange(192, dtype=np.int32) * 7) % 33 - 16).reshape(1, 3, 8, 8)
    weight_integer = (np.arange(54, dtype=np.int32) % 5 - 2).reshape(2, 3, 3, 3)
    bias_integer = np.array([16, -32], dtype=np.int32)
    input_tensor = input_integer.astype(np.float32) / 8
    weights = weight_integer.astype(np.float32) / 8
    bias = bias_integer.astype(np.float32) / 64
    accumulator = np.empty((1, 2, 6, 6), dtype=np.int32)
    for channel in range(2):
        for y in range(6):
            for x in range(6):
                total = int(bias_integer[channel])
                for z in range(3):
                    for ky in range(3):
                        for kx in range(3):
                            total += int(input_integer[0, z, y + ky, x + kx]) * int(
                                weight_integer[channel, z, ky, kx])
                accumulator[0, channel, y, x] = total
    expected = accumulator.astype(np.float32) / 64

    graph = helper.make_graph(
        [helper.make_node("Conv", ["input", "weights", "bias"], ["output"],
                          kernel_shape=[3, 3], strides=[1, 1], pads=[0, 0, 0, 0])],
        "conv3x3",
        [helper.make_tensor_value_info("input", TensorProto.FLOAT, [1, 3, 8, 8])],
        [helper.make_tensor_value_info("output", TensorProto.FLOAT, [1, 2, 6, 6])],
        [numpy_helper.from_array(weights, "weights"), numpy_helper.from_array(bias, "bias")],
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 11)])
    model.ir_version = 7
    onnx.checker.check_model(model)
    args.output.mkdir(parents=True, exist_ok=False)
    onnx.save(model, str(args.output / "conv3x3.onnx"))
    arrays = dict(input=input_tensor, weights=weights, bias=bias, expected=expected,
                  accumulator=accumulator, input_nhwc=input_tensor.transpose(0, 2, 3, 1))
    for name, value in arrays.items():
        np.save(str(args.output / (name + ".npy")), value, allow_pickle=False)
        if value.dtype == np.float32:
            (args.output / (name + ".f32")).write_bytes(value.astype("<f4").tobytes())
    files = {}
    for path in sorted(args.output.iterdir()):
        blob = path.read_bytes()
        files[path.name] = dict(bytes=len(blob), sha256=hashlib.sha256(blob).hexdigest())
    report = dict(
        operator="Conv", input_nchw=[1, 3, 8, 8], weights_oihw=[2, 3, 3, 3],
        output_nchw=[1, 2, 6, 6], stride=[1, 1], padding=[0, 0, 0, 0],
        input_formula="((linear_index * 7) % 33 - 16) / 8",
        weight_formula="(linear_index % 5 - 2) / 8", bias=[0.25, -0.5],
        reference="exact integer cross-correlation, including bias, divided by 64",
        expected_min=float(expected.min()), expected_max=float(expected.max()),
        quantization_applied=False, device_execution=False,
        numpy_version=np.__version__, onnx_version=onnx.__version__, files=files,
    )
    (args.output / "fixture.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Generated 3x3 convolution: 192 input values, 54 weights, 72 reference outputs")


if __name__ == "__main__":
    main()
