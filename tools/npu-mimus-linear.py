#!/usr/bin/env python3
"""Build one mimus linear layer for the NPU compiler and compare the result.

Run in the pinned compiler image (Python 3.8, NumPy, ONNX, PyYAML). Reads a
mimus model file directly, without mimus code, using the layout documented
in mimus train/model_format.py. Three subcommands:

  fixture MODEL OUT   writes an ONNX MatMul+Add of the selected layer, whose
                      float weights are the mimus int8 codes times their fp16
                      per-row scales, test rows with one shared maximum, the
                      mimus CPU reference and a metadata record.
  quantize OUT FILE   rewrites the compiler's quantize file so the weights
                      use exactly the mimus codes and per-row scales and the
                      input uses the mimus activation codes and scale.
  compare OUT REPORT  compares simulator output with the mimus CPU result
                      and with the ideal single rounding of that result.

With --raw-sums the layer is built from the integer codes alone, with unit
scales, no bias and an INT32 output, so the NPU returns the exact integer
sums and the CPU applies the mimus float read-out to them.

The CPU reference repeats the mimus P4 arithmetic (train/quant.py and
MimusGemmS16FC): per-row activation scale s = max|x| / qmax in float32,
codes q = clamp(rne(x * (1 / s)), +-qmax), an exact integer sum over the
int8 weight codes, then (float(sum) * ws[n]) * s + bias[n] in float32.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

import numpy as np

HEADER = struct.Struct('<4sHHIIIIIIIIQ32s32s')
ENTRY = struct.Struct('<IBBHI4IIIIIIIIII')
ACT = {0: 'f32', 1: 'int8_token', 2: 'int8_group', 3: 'int16', 4: 'int8_expand'}


def tensors(path):
    """Name -> (dtype, act, shape, data, scales) for F32 and I8 tensors."""
    buf = Path(path).read_bytes()
    fields = HEADER.unpack_from(buf, 0)
    if fields[0] != b'MIMS' or fields[1] != 1 or fields[11] != len(buf):
        raise ValueError('not a complete mimus v1 model file')
    count, table, strings = fields[4], fields[5], fields[6]
    result = {}
    for i in range(count):
        (name_off, dtype, act, _, rank, s0, s1, s2, s3, kp, stride, d_off, d_size,
         s_off, s_size, crc, _, _) = ENTRY.unpack_from(buf, table + i * ENTRY.size)
        end = buf.index(b'\0', strings + name_off)
        name = buf[strings + name_off:end].decode()
        blob, scales = buf[d_off:d_off + d_size], buf[s_off:s_off + s_size]
        if zlib.crc32(blob + scales) != crc:
            raise ValueError(f'CRC mismatch in {name}')
        shape = (s0, s1, s2, s3)[:rank]
        if dtype == 0:
            result[name] = ('F32', ACT[act], shape, np.frombuffer(blob, '<f4').reshape(shape), None)
        elif dtype == 2:
            rows = np.frombuffer(blob, np.uint8).reshape(shape[0], stride)
            result[name] = ('I8', ACT[act], shape, rows[:, :kp].view(np.int8)[:, :int(np.prod(shape[1:]))],
                            np.frombuffer(scales, '<f2'))
    return result


def quantize_rows(x, qmax):
    """mimus activation quantization: float32 scale, multiply by its inverse, round half even."""
    peak = np.max(np.abs(x), axis=1).astype(np.float32)
    scale = (peak / np.float32(qmax)).astype(np.float32)
    inverse = np.where(scale > 0, np.float32(1) / np.where(scale > 0, scale, np.float32(1)), np.float32(0))
    codes = np.clip(np.rint((x * inverse[:, None]).astype(np.float32)), -qmax, qmax).astype(np.int64)
    return codes, scale.astype(np.float32)


def fixture(args):
    import onnx
    from onnx import TensorProto, helper, numpy_helper
    out = args.output
    out.mkdir(parents=True, exist_ok=False)
    model = tensors(args.model)
    kind, act, shape, codes, scales = model[args.layer + '.weight']
    if kind != 'I8' or act not in ('int16', 'int8_token'):
        raise ValueError('layer must hold int8 weights with int16 or int8 activations')
    bias = model[args.layer + '.bias'][3].astype(np.float32)
    qmax = 32767 if act == 'int16' else 127
    n, k = shape
    ws = scales.astype(np.float32)
    weights = (codes.astype(np.float32) * ws[:, None]).astype(np.float32)  # exact products
    assert np.array_equal(np.rint(weights / ws[:, None]).astype(np.int64), codes.astype(np.int64))
    rng = np.random.default_rng(args.seed)
    x = rng.standard_normal((args.rows, k)).astype(np.float32)
    # One shared row maximum gives every row the same activation scale, which
    # a static NPU input quantization can represent.
    peak = np.float32(args.peak)
    x = (x / np.max(np.abs(x), axis=1, keepdims=True) * peak).astype(np.float32)
    x[np.arange(args.rows), np.argmax(np.abs(x), axis=1)] = np.sign(
        x[np.arange(args.rows), np.argmax(np.abs(x), axis=1)]) * peak
    q, xs = quantize_rows(x, qmax)
    assert np.all(xs == xs[0])
    sums = q @ codes.astype(np.int64).T
    reference = ((sums.astype(np.float32) * ws[None, :]).astype(np.float32) * xs[:, None]).astype(np.float32)
    reference = (reference + bias[None, :]).astype(np.float32)
    if args.raw_sums:
        # Codes as values with unit scales: the layer computes the integer sums.
        weights, network_input = codes.astype(np.float32), q.astype(np.float32)
        network_bias = np.zeros(n, np.float32)
    else:
        network_input, network_bias = x, bias
    graph = helper.make_graph(
        [helper.make_node('MatMul', ['input', 'weights'], ['product']),
         helper.make_node('Add', ['product', 'bias'], ['output'])],
        'mimus_linear',
        [helper.make_tensor_value_info('input', TensorProto.FLOAT, [args.rows, k])],
        [helper.make_tensor_value_info('output', TensorProto.FLOAT, [args.rows, n])],
        [numpy_helper.from_array(weights.T.copy(), 'weights'),
         numpy_helper.from_array(network_bias, 'bias')])
    onnx_model = helper.make_model(graph, opset_imports=[helper.make_opsetid('', 11)])
    onnx_model.ir_version = 7
    onnx.checker.check_model(onnx_model)
    onnx.save(onnx_model, str(out / 'linear.onnx'))
    arrays = dict(input=network_input, codes=q.astype(np.int16 if qmax == 32767 else np.int8), sums=sums,
                  reference=reference, weight_codes=codes, weight_scales=ws, bias=bias)
    for name, value in arrays.items():
        np.save(str(out / (name + '.npy')), value, allow_pickle=False)
    # Raw little-endian copies for checks/npu-mimus-kernel.cc.
    raw = {'x.i16': q.astype('<i2'), 'w.i8': codes.astype(np.int8), 'ws.f32': ws.astype('<f4'),
           'xs.f32': xs.astype('<f4'), 'bias.f32': bias.astype('<f4'), 'reference.f32': reference.astype('<f4')}
    for name, value in raw.items():
        (out / name).write_bytes(np.ascontiguousarray(value).tobytes())
    meta = dict(model_sha256=hashlib.sha256(Path(args.model).read_bytes()).hexdigest(),
                layer=args.layer, activation=act, qmax=qmax, rows=args.rows, k=k, n=n,
                raw_sums=args.raw_sums,
                input_scale=float(xs[0]), peak=float(peak), seed=args.seed,
                max_abs_sum=int(np.max(np.abs(sums))),
                max_abs_output=float(np.max(np.abs(reference))),
                files={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(out.iterdir())})
    (out / 'meta.json').write_text(json.dumps(meta, indent=2) + '\n')
    print(f'Generated {args.layer}: {args.rows}x{k} {act} activations, {n} outputs, '
          f'max |sum| {meta["max_abs_sum"]}')


def quantize(args):
    import yaml
    out = args.output
    meta = json.loads((out / 'meta.json').read_text())
    ws = np.load(str(out / 'weight_scales.npy'))
    xs = meta['input_scale']
    qtype = 'i16' if meta['qmax'] == 32767 else 'i8'
    if meta['raw_sums']:
        ws, xs = np.ones_like(ws), 1.0
    data = yaml.safe_load(args.file.read_text())
    params = data['quantize_parameters']
    changed = []
    for key, value in params.items():
        if key.endswith(':weight'):
            params[key] = dict(qtype='i8', quantizer='perchannel_symmetric_affine', rounding='rtne',
                               scale=[float(v) for v in ws], channel_dim=value.get('channel_dim', 1))
        elif key.endswith(':bias'):
            params[key] = dict(qtype='i32', quantizer='perchannel_symmetric_affine', rounding='rtne',
                               scale=[float(np.float32(v) * np.float32(xs)) for v in ws], channel_dim=0)
        elif key.startswith('@input'):
            params[key] = dict(qtype=qtype, quantizer='asymmetric_affine', rounding='rtne',
                               scale=float(xs), zero_point=0,
                               max_value=float(xs * meta['qmax']), min_value=float(-xs * meta['qmax']))
        elif meta['raw_sums']:
            params[key] = dict(qtype='i32', quantizer='asymmetric_affine', rounding='rtne',
                               scale=1.0, zero_point=0, max_value=2.0 ** 31 - 1, min_value=-2.0 ** 31)
        else:
            # Intermediate and output tensors: int16 over the reference range.
            top = meta['max_abs_output'] * 1.0001
            params[key] = dict(qtype=args.output_qtype, quantizer='asymmetric_affine', rounding='rtne',
                               scale=top / (32767 if args.output_qtype == 'i16' else 127), zero_point=0,
                               max_value=top, min_value=-top)
        changed.append(key)
    args.file.write_text(yaml.safe_dump(data, sort_keys=False))
    print('rewrote', len(changed), 'quantize entries:', ', '.join(changed))


def compare(args):
    import yaml
    out = args.output
    meta = json.loads((out / 'meta.json').read_text())
    reference = np.load(str(out / 'reference.npy'))
    sums = np.load(str(out / 'sums.npy'))
    ws = np.load(str(out / 'weight_scales.npy')).astype(np.float64)
    bias = np.load(str(out / 'bias.npy')).astype(np.float64)
    params = yaml.safe_load((out / 'linear.quantize').read_text())['quantize_parameters']
    output_key = next(k for k in params if k.startswith('@attach_output'))
    q = params[output_key]
    dtype = {'i32': np.int32, 'i16': np.int16, 'i8': np.int8}[q['qtype']]
    outputs = sorted((out / 'export_nbg_unify').glob('output0_*.dat'))
    assert len(outputs) == 1, 'expected one simulator output'
    code = np.fromfile(str(outputs[0]), dtype=dtype).reshape(reference.shape).astype(np.int64)
    codes = np.load(str(out / 'codes.npy'))
    simulated_input = np.fromfile(str(out / 'export_nbg_unify/input_0.dat'), dtype=codes.dtype)
    assert np.array_equal(simulated_input, codes.ravel()), 'simulator input differs from the mimus codes'
    scale, zero = q['scale'], q['zero_point']
    if meta['raw_sums']:
        # The NPU returns integer sums; apply the mimus read-out on the CPU.
        f32 = np.float32
        readout = ((code.astype(f32) * ws.astype(f32)[None, :]).astype(f32) *
                   f32(meta['input_scale'])).astype(f32)
        readout = (readout + bias.astype(f32)[None, :]).astype(f32)
        report = dict(meta, output_quantization=q, simulator_output=outputs[0].name,
                      simulator_output_sha256=hashlib.sha256(outputs[0].read_bytes()).hexdigest(),
                      sums_equal=int(np.sum(code == sums)), codes_total=int(code.size),
                      bit_exact_with_cpu_float=bool(np.array_equal(
                          readout.view(np.uint32), reference.astype(np.float32).view(np.uint32))),
                      simulator_input_equals_mimus_codes=True, physical_access=False)
        Path(args.report).write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({k: report[k] for k in ('sums_equal', 'codes_total', 'bit_exact_with_cpu_float')}))
        return
    exact = sums * ws[None, :] * meta['input_scale'] + bias[None, :]
    lo, hi = np.iinfo(dtype).min, np.iinfo(dtype).max
    ideal = np.clip(np.rint(exact / scale) + zero, lo, hi).astype(np.int64)
    # The NPU adds the bias as int32 codes in accumulator units, the product
    # of the input scale and ws[n]; one rounding of that exact value is the
    # reference for its integer path.
    bias_scale = ws * meta['input_scale']
    accumulated = (sums + np.rint(bias[None, :] / bias_scale[None, :])) * bias_scale[None, :]
    integer = np.clip(np.rint(accumulated / scale) + zero, lo, hi).astype(np.int64)
    dequantized = (code - zero) * scale
    report = dict(meta, output_quantization=q, simulator_output=outputs[0].name,
                  simulator_output_sha256=hashlib.sha256(outputs[0].read_bytes()).hexdigest(),
                  bit_exact_with_cpu_float=bool(np.array_equal(dequantized.astype(np.float32), reference)),
                  codes_equal_to_ideal_rounding=int(np.sum(code == ideal)),
                  codes_total=int(code.size),
                  max_code_difference_from_ideal=int(np.max(np.abs(code - ideal))),
                  codes_equal_to_rounding_with_int32_bias=int(np.sum(code == integer)),
                  max_abs_error_vs_cpu=float(np.max(np.abs(dequantized - reference))),
                  rms_error_vs_cpu=float(np.sqrt(np.mean((dequantized - reference) ** 2))),
                  output_step=float(scale), simulator_input_equals_mimus_codes=True,
                  physical_access=False)
    Path(args.report).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('bit_exact_with_cpu_float', 'codes_equal_to_ideal_rounding',
                                             'codes_equal_to_rounding_with_int32_bias', 'codes_total', 'max_code_difference_from_ideal',
                                             'max_abs_error_vs_cpu', 'output_step')}))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    f = sub.add_parser('fixture')
    f.add_argument('model', type=Path)
    f.add_argument('output', type=Path)
    f.add_argument('--layer', default='encoder.layers.0.self_attn.linear_q')
    f.add_argument('--rows', type=int, default=8)
    f.add_argument('--peak', type=float, default=4.0)
    f.add_argument('--seed', type=int, default=20261021)
    f.add_argument('--raw-sums', action='store_true')
    q = sub.add_parser('quantize')
    q.add_argument('output', type=Path)
    q.add_argument('file', type=Path)
    q.add_argument('--output-qtype', choices=('i16', 'i8'), default='i16')
    c = sub.add_parser('compare')
    c.add_argument('output', type=Path)
    c.add_argument('report', type=Path)
    args = ap.parse_args()
    {'fixture': fixture, 'quantize': quantize, 'compare': compare}[args.command](args)


if __name__ == '__main__':
    main()
