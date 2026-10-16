#!/usr/bin/env python3
"""Compare saved compiler/simulator outputs with the deterministic convolution.

This check reads artifacts only. It neither invokes the compiler nor executes
device commands. Run in the pinned compiler environment with NumPy and PyYAML.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

import numpy as np
import yaml


def require(condition, message):
    if not condition:
        raise ValueError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("report", type=Path, help="new JSON evidence file")
    args = parser.parse_args()
    root = args.fixture
    inputs = []

    def path(name):
        p = root / name
        inputs.append(p)
        return p

    def tensor(directory, name, shape, quantized=False):
        suffix = ".qnt.tensor" if quantized else ".tensor"
        p = path(directory + "/iter_0_" + name + suffix)
        values = np.loadtxt(str(p), dtype=np.float64)
        require(np.all(np.isfinite(values)), "nonfinite tensor")
        if quantized:
            require(np.all((values >= 0) & (values <= 255) & (values == np.floor(values))),
                    "invalid UINT8 tensor")
        return values.reshape(shape)

    in_name = "input_2_out0_1_3_8_8"
    out_name = "attach_output_out0_0_out0_1_2_6_6"
    expected = np.load(str(path("expected.npy")), allow_pickle=False).reshape(1, 2, 6, 6)
    raw_input = np.load(str(path("input.npy")), allow_pickle=False).reshape(1, 3, 8, 8)
    require(np.array_equal(tensor("float-reference", in_name, raw_input.shape), raw_input),
            "floating-point input preprocessing changed values")
    require(np.array_equal(tensor("float-reference", out_name, expected.shape), expected),
            "floating-point output differs from the exact reference")
    quantized_input = tensor("quantized-reference", in_name, raw_input.shape, True)
    quantized_output = tensor("quantized-reference", out_name, expected.shape, True)
    simulator_input = np.fromfile(str(path("export_nbg_unify/input_0.dat")), dtype=np.uint8)
    simulator_output = np.fromfile(str(path("export_nbg_unify/output0_6_6_2_1.dat")), dtype=np.uint8)
    require(np.array_equal(simulator_input, quantized_input.ravel()), "simulator input differs")
    require(np.array_equal(simulator_output, quantized_output.ravel()), "simulator output differs")
    quant = yaml.safe_load(path("conv3x3_uint8.quantize").read_text())["quantize_parameters"]
    output_quant = quant["@attach_output/out0_0:out0"]
    scale, zero = output_quant["scale"], output_quant["zero_point"]
    require(output_quant["qtype"] == "u8" and np.isfinite(scale) and scale > 0 and
            isinstance(zero, int) and 0 <= zero <= 255, "invalid output quantization")
    difference = (simulator_output.astype(np.float64) - zero) * scale - expected.ravel()
    model = path("export_nbg_unify/network_binary.nb").read_bytes()
    require(len(model) >= 12 and model[:4] == b"VPMN", "missing NBG header")
    version, target = struct.unpack_from("<II", model, 4)
    require(target == 0x1000003b, "wrong NPU target")
    identities = []
    for p in inputs:
        blob = p.read_bytes()
        identities.append(dict(path=str(p.relative_to(root)), bytes=len(blob),
                               sha256=hashlib.sha256(blob).hexdigest()))
    report = dict(result="pass", float_input_values=192, float_output_values=72,
                  float_reference_exact=True, simulator_input_exact=True,
                  quantized_reference_exact=True, simulator_output_bytes=72,
                  max_code_difference=0, max_float_error=float(np.max(np.abs(difference))),
                  rms_float_error=float(np.sqrt(np.mean(difference * difference))),
                  nbg_version=hex(version), target=hex(target), nbg_bytes=len(model),
                  input_quantization=quant["@input_2:out0"], output_quantization=output_quant,
                  physical_access=False, native_loader_execution=False, inputs=identities)
    with args.report.open("x") as output:
        output.write(json.dumps(report, indent=2) + "\n")
    print("PASS convolution references: exact float calculation and 72 simulator output bytes")


if __name__ == "__main__":
    main()
