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

With --digits the layer returns exact integer partial sums instead. The CPU
splits each int16 activation code into four base-16 digits
x = d0 + 16 d1 + 256 d2 + 4096 d3 (d0, d1, d2 in [-8, 7], d3 in [-8, 8]) and
cuts the reduction into C chunks of at most 32 terms. Chunk c is one INT8
input of shape [4R, L], one fully connected layer with the int8 weight codes
at unit scale and no bias, and one INT16 output of shape [4R, N] at unit
scale, so every partial P[j,c,n] = sum over the chunk of d_j[k] w[k,n] is an
integer within +-L*8*max|w| <= 32,767. The CPU recombines
S[n] = sum_j 16^j sum_c P[j,c,n] and applies the mimus read-out with each
row's own scale. Rows therefore use different maxima in this form. --range
replaces the layer's weights and rows with codes that drive partials to
+-L*8*127 and include activation codes +-32,767.

With --unit the INT16 form takes the activation and weight codes at unit
scale with no bias, so its output codes are the integer sums clamped to the
output type. tools/npu-mimus-probe.py patches this graph's output encoding.

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
DIGITS = 4          # base-16 digits of an int16 activation code
MAX_CHUNK = 32      # reduction terms per chunk
MAX_CHUNKS = 8      # inputs and outputs the native assembler accepts
INT16_MAX = 32767


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


def split_digits(q):
    """Base-16 digits of int16 codes: three balanced low digits, then the remainder."""
    digits, rest = [], q.astype(np.int64)
    for _ in range(DIGITS - 1):
        d = (rest + 8) % 16 - 8
        digits.append(d)
        rest = (rest - d) // 16
    digits.append(rest)
    d = np.stack(digits)                        # [4, R, K]
    assert np.array_equal(sum(d[j] * 16 ** j for j in range(DIGITS)), q)
    assert d[:DIGITS - 1].min() >= -8 and d[:DIGITS - 1].max() <= 7 and np.abs(d[-1]).max() <= 8
    return d


def chunk_bounds(k):
    """C = ceil(k / 32) chunks whose lengths differ by at most one."""
    count = -(-k // MAX_CHUNK)
    sizes = [k // count + (i < k % count) for i in range(count)]
    starts = np.cumsum([0] + sizes)
    return [(int(starts[i]), int(starts[i + 1])) for i in range(count)]


def partials(digits, codes, bounds):
    """P[c, 4r + j, n] = sum over chunk c of d_j[r, k] w[n, k]."""
    _, rows, _ = digits.shape
    w = codes.astype(np.int64)
    out = []
    for lo, hi in bounds:
        stacked = digits[:, :, lo:hi].transpose(1, 0, 2).reshape(rows * DIGITS, hi - lo)
        out.append(stacked @ w[:, lo:hi].T)
    return np.stack(out)                          # [C, 4R, N]


def recombine(p):
    """S[r, n] = sum_j 16^j sum_c P[c, 4r + j, n]."""
    _, stacked, n = p.shape
    by_digit = p.sum(axis=0).reshape(stacked // DIGITS, DIGITS, n)
    return sum(by_digit[:, j, :] * 16 ** j for j in range(DIGITS))


def readout(sums, ws, xs, bias):
    f32 = np.float32
    value = ((sums.astype(f32) * ws.astype(f32)[None, :]).astype(f32) * xs.astype(f32)[:, None]).astype(f32)
    return (value + bias.astype(f32)[None, :]).astype(f32)


def range_fixture(rows, k, n, rng):
    """Codes that reach the partial bound with both signs and include +-32,767."""
    patterns = [np.full(k, 127), np.full(k, -127), np.where(np.arange(k) % 2, 127, -127)]
    w = np.stack([patterns[i % 4] if i % 4 < 3 else rng.integers(-127, 128, k) for i in range(n)])
    # 30,584 has digits (-8, -8, -8, 8) and -2,184 has digits (-8, -8, -8, 0).
    fixed = [32767, -32767, 30584, -2184]
    q = np.stack([np.full(k, fixed[r]) if r < len(fixed) else
                  np.where(np.arange(k) % 2, 32767, -32767) if r == len(fixed) else
                  rng.integers(-32767, 32768, k) for r in range(rows)])
    xs = np.array([4.0 * (r + 1) / 32767 for r in range(rows)], np.float32)
    return w.astype(np.int8), q.astype(np.int64), xs


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
    rng = np.random.default_rng(args.seed)
    if args.range:
        if not args.digits or qmax != 32767:
            raise ValueError('--range needs --digits and an int16-activation layer')
        codes, q, xs = range_fixture(args.rows, k, n, rng)
        peak = np.float32(0)
    else:
        x = rng.standard_normal((args.rows, k)).astype(np.float32)
        peak = np.float32(args.peak)
        # One shared row maximum gives every row the same activation scale,
        # which a static NPU input quantization can represent. The digit form
        # applies each row's scale on the CPU, so its rows differ.
        peaks = peak * (np.float32(1) + np.arange(args.rows, dtype=np.float32) / np.float32(4)
                        if args.digits else np.ones(args.rows, np.float32))
        top = np.argmax(np.abs(x), axis=1)
        x = (x / np.max(np.abs(x), axis=1, keepdims=True) * peaks[:, None]).astype(np.float32)
        x[np.arange(args.rows), top] = np.sign(x[np.arange(args.rows), top]) * peaks
        q, xs = quantize_rows(x, qmax)
        assert args.digits or np.all(xs == xs[0])
    sums = q @ codes.astype(np.int64).T
    reference = readout(sums, ws, xs, bias)
    meta = dict(model_sha256=hashlib.sha256(Path(args.model).read_bytes()).hexdigest(),
                layer=args.layer, activation=act, qmax=qmax, rows=args.rows, k=k, n=n,
                digits=args.digits, range_fixture=args.range, unit=args.unit, peak=float(peak), seed=args.seed,
                max_abs_sum=int(np.max(np.abs(sums))), max_abs_output=float(np.max(np.abs(reference))))
    if args.digits:
        if qmax != 32767:
            raise ValueError('the digit form splits int16 activation codes')
        bounds = chunk_bounds(k)
        longest, largest = max(hi - lo for lo, hi in bounds), int(np.max(np.abs(codes.astype(np.int64))))
        if np.any(codes == -128) or longest * 8 * largest > INT16_MAX or len(bounds) > MAX_CHUNKS:
            raise ValueError(f'partials may leave INT16 or exceed {MAX_CHUNKS} chunks: '
                             f'{len(bounds)} chunks, L={longest}, max|w|={largest}')
        digits = split_digits(q)
        expected = partials(digits, codes, bounds)
        assert np.array_equal(recombine(expected), sums)
        inputs = [digits[:, :, lo:hi].transpose(1, 0, 2).reshape(args.rows * DIGITS, hi - lo)
                  .astype(np.float32) for lo, hi in bounds]
        nodes = [helper.make_node('MatMul', [f'input{c}', f'weights{c}'], [f'output{c}'])
                 for c in range(len(bounds))]
        graph = helper.make_graph(
            nodes, 'mimus_linear_digits',
            [helper.make_tensor_value_info(f'input{c}', TensorProto.FLOAT, [args.rows * DIGITS, hi - lo])
             for c, (lo, hi) in enumerate(bounds)],
            [helper.make_tensor_value_info(f'output{c}', TensorProto.FLOAT, [args.rows * DIGITS, n])
             for c in range(len(bounds))],
            [numpy_helper.from_array(codes[:, lo:hi].astype(np.float32).T.copy(), f'weights{c}')
             for c, (lo, hi) in enumerate(bounds)])
        meta.update(chunks=[hi - lo for lo, hi in bounds], max_abs_partial=int(np.max(np.abs(expected))),
                    partial_bound=longest * 8 * largest)
        arrays = dict(expected_partials=expected)
        for c, value in enumerate(inputs):
            arrays[f'input{c}'] = value
            arrays[f'digits{c}'] = value.astype(np.int8)
    else:
        weights = (codes.astype(np.float32) * ws[:, None]).astype(np.float32)  # exact products
        assert np.array_equal(np.rint(weights / ws[:, None]).astype(np.int64), codes.astype(np.int64))
        network_bias = bias
        if args.unit:
            weights, x, network_bias = codes.astype(np.float32), q.astype(np.float32), np.zeros(n, np.float32)
        graph = helper.make_graph(
            [helper.make_node('MatMul', ['input', 'weights'], ['product']),
             helper.make_node('Add', ['product', 'bias'], ['output'])],
            'mimus_linear',
            [helper.make_tensor_value_info('input', TensorProto.FLOAT, [args.rows, k])],
            [helper.make_tensor_value_info('output', TensorProto.FLOAT, [args.rows, n])],
            [numpy_helper.from_array(weights.T.copy(), 'weights'), numpy_helper.from_array(network_bias, 'bias')])
        meta.update(input_scale=float(xs[0]))
        arrays = dict(input=x, codes=q.astype(np.int16 if qmax == 32767 else np.int8))
    onnx_model = helper.make_model(graph, opset_imports=[helper.make_opsetid('', 11)])
    onnx_model.ir_version = 7
    onnx.checker.check_model(onnx_model)
    onnx.save(onnx_model, str(out / 'linear.onnx'))
    arrays.update(sums=sums, reference=reference, weight_codes=codes, weight_scales=ws, bias=bias)
    for name, value in arrays.items():
        np.save(str(out / (name + '.npy')), value, allow_pickle=False)
    # Raw little-endian copies for checks/npu-mimus-kernel.cc.
    raw = {'x.i16': q.astype('<i2'), 'w.i8': codes.astype(np.int8), 'ws.f32': ws.astype('<f4'),
           'xs.f32': xs.astype('<f4'), 'bias.f32': bias.astype('<f4'), 'reference.f32': reference.astype('<f4')}
    if args.digits:
        raw['chunks.u32'] = np.array(meta['chunks'], '<u4')
    for name, value in raw.items():
        (out / name).write_bytes(np.ascontiguousarray(value).tobytes())
    meta['files'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(out.iterdir())}
    (out / 'meta.json').write_text(json.dumps(meta, indent=2) + '\n')
    print(f'Generated {args.layer}: {args.rows}x{k} {act} activations, {n} outputs, '
          f'max |sum| {meta["max_abs_sum"]}' +
          (f', {len(meta["chunks"])} chunks, max |partial| {meta["max_abs_partial"]}' if args.digits else ''))


def entry(qtype, scale, zero=0):
    lo, hi = {'i8': (-128, 127), 'u8': (0, 255), 'i16': (-32768, 32767)}[qtype]
    return dict(qtype=qtype, quantizer='asymmetric_affine', rounding='rtne', scale=float(scale),
                zero_point=int(zero), max_value=float((hi - zero) * scale), min_value=float((lo - zero) * scale))


def quantize(args):
    import yaml
    out = args.output
    meta = json.loads((out / 'meta.json').read_text())
    ws = np.load(str(out / 'weight_scales.npy'))
    data = yaml.safe_load(args.file.read_text())
    params = data['quantize_parameters']
    changed = []
    for key, value in params.items():
        if meta['digits']:
            # Codes at unit scale: digits in, exact integer partials out.
            if key.endswith(':weight'):
                params[key] = dict(qtype='i8', quantizer='perchannel_symmetric_affine', rounding='rtne',
                                   scale=[1.0] * len(ws), channel_dim=value.get('channel_dim', 1))
            elif key.endswith(':bias'):
                params[key] = dict(qtype='i32', quantizer='perchannel_symmetric_affine', rounding='rtne',
                                   scale=[1.0] * len(ws), channel_dim=0)
            elif key.startswith('@input'):
                params[key] = entry('i8', 1.0)
            else:
                params[key] = entry('i16', 1.0)
        elif meta['unit']:
            if key.endswith(':weight') or key.endswith(':bias'):
                params[key] = dict(qtype='i8' if key.endswith(':weight') else 'i32',
                                   quantizer='perchannel_symmetric_affine', rounding='rtne',
                                   scale=[1.0] * len(ws),
                                   channel_dim=value.get('channel_dim', 1) if key.endswith(':weight') else 0)
            elif key.startswith('@input'):
                params[key] = entry('i16', 1.0)
            else:
                params[key] = entry(args.output_qtype, 1.0)
        elif key.endswith(':weight'):
            params[key] = dict(qtype='i8', quantizer='perchannel_symmetric_affine', rounding='rtne',
                               scale=[float(v) for v in ws], channel_dim=value.get('channel_dim', 1))
        elif key.endswith(':bias'):
            xs = meta['input_scale']
            params[key] = dict(qtype='i32', quantizer='perchannel_symmetric_affine', rounding='rtne',
                               scale=[float(np.float32(v) * np.float32(xs)) for v in ws], channel_dim=0)
        elif key.startswith('@input'):
            xs = meta['input_scale']
            qtype = 'i16' if meta['qmax'] == 32767 else 'i8'
            params[key] = dict(entry(qtype, xs), max_value=float(xs * meta['qmax']),
                               min_value=float(-xs * meta['qmax']))
        else:
            # Intermediate and output tensors over the reference range, or at
            # a given step and zero point for descriptor comparisons.
            top = meta['max_abs_output'] * 1.0001
            step = args.output_step or top / {'i16': 32767, 'i8': 127, 'u8': 127}[args.output_qtype]
            params[key] = entry(args.output_qtype, step, args.output_zero)
        changed.append(key)
    args.file.write_text(yaml.safe_dump(data, sort_keys=False))
    print('rewrote', len(changed), 'quantize entries:', ', '.join(changed))


def compare_digits(out, meta, args):
    expected = np.load(str(out / 'expected_partials.npy'))
    sums = np.load(str(out / 'sums.npy'))
    chunks, stacked, n = expected.shape
    unify = out / 'export_nbg_unify'
    got = []
    for c in range(chunks):
        found = sorted(unify.glob(f'output{c}_*.dat'))
        assert len(found) == 1, f'expected one simulator output {c}'
        got.append(np.fromfile(str(found[0]), dtype='<i2').reshape(stacked, n).astype(np.int64))
        digits = np.load(str(out / f'digits{c}.npy'))
        simulated = np.fromfile(str(unify / f'input_{c}.dat'), dtype=np.int8)
        assert np.array_equal(simulated, digits.ravel()), f'simulator input {c} differs from the digits'
    got = np.stack(got)
    np.ascontiguousarray(got.astype('<i2')).tofile(str(out / 'partials.i16'))
    combined = recombine(got)
    report = dict(meta, partials_equal=int(np.sum(got == expected)), partials_total=int(got.size),
                  max_abs_partial_simulated=int(np.max(np.abs(got))),
                  partial_range_simulated=[int(got.min()), int(got.max())],
                  sums_equal=int(np.sum(combined == sums)), sums_total=int(sums.size),
                  partials_sha256=hashlib.sha256((out / 'partials.i16').read_bytes()).hexdigest(),
                  simulator_inputs_equal_digits=True, physical_access=False)
    Path(args.report).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('partials_equal', 'partials_total', 'partial_range_simulated',
                                             'sums_equal', 'sums_total')}))


def compare(args):
    import yaml
    out = args.output
    meta = json.loads((out / 'meta.json').read_text())
    if meta['digits']:
        return compare_digits(out, meta, args)
    reference = np.load(str(out / 'reference.npy'))
    sums = np.load(str(out / 'sums.npy'))
    ws = np.load(str(out / 'weight_scales.npy')).astype(np.float64)
    bias = np.load(str(out / 'bias.npy')).astype(np.float64)
    params = yaml.safe_load((out / 'linear.quantize').read_text())['quantize_parameters']
    output_key = next(k for k in params if k.startswith('@attach_output'))
    q = params[output_key]
    dtype = {'i16': np.int16, 'i8': np.int8, 'u8': np.uint8}[q['qtype']]
    outputs = sorted((out / 'export_nbg_unify').glob('output0_*.dat'))
    assert len(outputs) == 1, 'expected one simulator output'
    code = np.fromfile(str(outputs[0]), dtype=dtype).reshape(reference.shape).astype(np.int64)
    codes = np.load(str(out / 'codes.npy'))
    simulated_input = np.fromfile(str(out / 'export_nbg_unify/input_0.dat'), dtype=codes.dtype)
    assert np.array_equal(simulated_input, codes.ravel()), 'simulator input differs from the mimus codes'
    scale, zero = q['scale'], q['zero_point']
    lo, hi = np.iinfo(dtype).min, np.iinfo(dtype).max
    if meta['unit']:
        clamped = np.clip(sums, lo, hi)
        report = dict(meta, output_quantization=q, simulator_output=outputs[0].name,
                      simulator_output_sha256=hashlib.sha256(outputs[0].read_bytes()).hexdigest(),
                      codes_equal_to_clamped_sums=int(np.sum(code == clamped)), codes_total=int(code.size),
                      sums_inside_output_range=int(np.sum(clamped == sums)),
                      simulator_input_equals_mimus_codes=True, physical_access=False)
        Path(args.report).write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({k: report[k] for k in ('codes_equal_to_clamped_sums', 'codes_total',
                                                 'sums_inside_output_range')}))
        return
    exact = sums * ws[None, :] * meta['input_scale'] + bias[None, :]
    ideal = np.clip(np.rint(exact / scale) + zero, lo, hi).astype(np.int64)
    # The NPU adds the bias as int32 codes in accumulator units, the product
    # of the input scale and ws[n]; one rounding of that exact value is the
    # reference for its integer path.
    bias_scale = ws * meta['input_scale']
    accumulated = (sums + np.rint(bias[None, :] / bias_scale[None, :])) * bias_scale[None, :]
    integer = np.clip(np.rint(accumulated / scale) + zero, lo, hi).astype(np.int64)
    dequantized = ((code - zero) * np.float32(scale)).astype(np.float32)
    report = dict(meta, output_quantization=q, simulator_output=outputs[0].name,
                  simulator_output_sha256=hashlib.sha256(outputs[0].read_bytes()).hexdigest(),
                  bit_exact_with_cpu_float=bool(np.array_equal(dequantized, reference)),
                  readout_mismatches=int(np.sum(dequantized.view(np.uint32) != reference.view(np.uint32))),
                  codes_equal_to_ideal_rounding=int(np.sum(code == ideal)),
                  codes_total=int(code.size),
                  max_code_difference_from_ideal=int(np.max(np.abs(code - ideal))),
                  codes_equal_to_rounding_with_int32_bias=int(np.sum(code == integer)),
                  max_abs_error_vs_cpu=float(np.max(np.abs(dequantized - reference))),
                  rms_error_vs_cpu=float(np.sqrt(np.mean((dequantized.astype(np.float64) - reference) ** 2))),
                  output_step=float(scale), simulator_input_equals_mimus_codes=True,
                  physical_access=False)
    Path(args.report).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('bit_exact_with_cpu_float', 'readout_mismatches',
                                             'codes_equal_to_ideal_rounding',
                                             'codes_equal_to_rounding_with_int32_bias', 'codes_total',
                                             'max_code_difference_from_ideal', 'max_abs_error_vs_cpu',
                                             'output_step')}))


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
    f.add_argument('--digits', action='store_true')
    f.add_argument('--range', action='store_true')
    f.add_argument('--unit', action='store_true')
    q = sub.add_parser('quantize')
    q.add_argument('output', type=Path)
    q.add_argument('file', type=Path)
    q.add_argument('--output-qtype', choices=('i16', 'i8', 'u8'), default='i16')
    q.add_argument('--output-step', type=float, default=0.0)
    q.add_argument('--output-zero', type=int, default=0)
    c = sub.add_parser('compare')
    c.add_argument('output', type=Path)
    c.add_argument('report', type=Path)
    args = ap.parse_args()
    {'fixture': fixture, 'quantize': quantize, 'compare': compare}[args.command](args)


if __name__ == '__main__':
    main()
