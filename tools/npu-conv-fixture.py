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
    parser.add_argument('--width', type=int, default=8, choices=(8,10,12))
    parser.add_argument('--channels', type=int, default=2, choices=(2,4))
    args = parser.parse_args()
    w,c=args.width,args.channels
    n=w-2

    # All values are dyadic fractions. The accumulator below is integer-exact;
    # division by 64 is exactly representable for every generated output.
    input_integer = ((np.arange(3*w*w, dtype=np.int32) * 7) % 33 - 16).reshape(1, 3, w, w)
    weight_integer = (np.arange(c*27, dtype=np.int32) % 5 - 2).reshape(c, 3, 3, 3)
    bias_integer = np.array(([16,-32]*c)[:c], dtype=np.int32)
    input_tensor = input_integer.astype(np.float32) / 8
    weights = weight_integer.astype(np.float32) / 8
    bias = bias_integer.astype(np.float32) / 64
    accumulator = np.empty((1, c, n, n), dtype=np.int32)
    for channel in range(c):
        for y in range(n):
            for x in range(n):
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
        [helper.make_tensor_value_info("input", TensorProto.FLOAT, [1, 3, w, w])],
        [helper.make_tensor_value_info("output", TensorProto.FLOAT, [1, c, n, n])],
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
        operator="Conv", input_nchw=[1, 3, w, w], weights_oihw=[c, 3, 3, 3],
        output_nchw=[1, c, n, n], stride=[1, 1], padding=[0, 0, 0, 0],
        input_formula="((linear_index * 7) % 33 - 16) / 8",
        weight_formula="(linear_index % 5 - 2) / 8", bias=bias.tolist(),
        reference="exact integer cross-correlation, including bias, divided by 64",
        expected_min=float(expected.min()), expected_max=float(expected.max()),
        quantization_applied=False, device_execution=False,
        numpy_version=np.__version__, onnx_version=onnx.__version__, files=files,
    )
    (args.output / "fixture.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Generated 3x3 convolution: {3*w*w} input values, {c*27} weights, {c*n*n} reference outputs")


if __name__ == "__main__":
    main()
