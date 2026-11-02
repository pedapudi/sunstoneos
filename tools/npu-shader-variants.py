#!/usr/bin/env python3
"""Write single-operation ONNX graphs that the compiler may send to the shader cores or the tensor processor.

Run inside the pinned compiler image (Python 3.8 with NumPy and ONNX; no network):

  npu-shader-variants.py OUTDIR '[{"kind":"gelu","name":"gelu64","rows":16,"width":64}, ...]'

Each item names one graph directory under OUTDIR with fragment.onnx, one .npy file per graph input and inputs.json,
the layout that tools/npu-graph-compile.sh and tools/npu-graph-simulate.sh read. Kinds and their parameters:

  elt     op = Erf, Exp, Sqrt, Reciprocal, Abs, Neg, Floor, Ceil, Round, Sin, Cos, Log, Softplus, Elu, LeakyRelu,
          Clip, HardSigmoid, HardSwish, Sign (unary on a rows x width tensor)
  gelu    Gelu as the erf formula (x * 0.5 * (1 + erf(x / sqrt 2))); form = erf or tanh or native (opset 20 Gelu)
  pow     x ** exponent
  gather  Gather of rows of a table by constant indices (axis 0 or 1)
  gatherd Gather of rows by an index input
  embed   Gather of rows of a constant table (vocab x dim) by an index input of seq entries (an embedding lookup)
  resize  Resize of an NCHW tensor; mode = nearest, linear; scale
  pad     Pad of an NCHW tensor by pad elements on every side
  tr      Transpose of a rank-3 or rank-4 tensor by perm
  concat  Concat of count inputs along axis
  split   Split along axis into two outputs
  maxpool, avgpool  NCHW pooling with kernel and stride
  gpool   GlobalAveragePool
  lrn     LocalResponseNormalization over size channels
  s2d     SpaceToDepth with blocksize; d2s DepthToSpace
  slice   Slice along axis
  where   Where(cond, x, z)
  reduce  ReduceMean/ReduceMax/ReduceSum over the last axis
  topk    TopK along the last axis
  argmax  ArgMax along the last axis
  cumsum  CumSum along the last axis
  expand  Expand of a rows x 1 tensor to rows x width
  tile    Tile
  reshape Reshape with an interior Transpose chain
  conv    Convolution (kernel k, channels cin to cout)
  matmul  MatMul with constant weights (rows x inner by inner x cols)
  misc    one of the less common operators, by op: OneHot, NonZero, ScatterND, GatherND, Mod, ReduceProd, NonMaxSuppression, RoiAlign,
          GridSample, Einsum, Unique, Round, Not, Shrink; probes whether the compiler emits software operations
  seq     a chain of operations on a rows x width tensor; ops lists names: a unary operator name (Exp, Sin, Floor, Reciprocal,
          Relu, ...) or MatMul (a width x width constant matrix, a neural-network operation)

Weights and inputs come from a fixed seed, so two variants that differ in one parameter differ in nothing else.
"""
import json, sys
from pathlib import Path
import numpy as np, onnx
from onnx import helper, TensorProto, numpy_helper
OUT = Path(sys.argv[1])
F = TensorProto.FLOAT
H = helper


def save(name, nodes, inputs, outputs, inits, arrays, opset=17):
    d = OUT / name; d.mkdir(parents=True, exist_ok=True)
    m = H.make_model(H.make_graph(nodes, name, inputs, outputs, inits), opset_imports=[H.make_opsetid('', opset)])
    m.ir_version = 8
    onnx.save(m, str(d / 'fragment.onnx'))
    for k, v in arrays.items(): np.save(str(d / (k + '.npy')), v.astype(np.float32))
    json.dump(list(arrays), open(d / 'inputs.json', 'w'))


def vi(n, shape, t=F): return H.make_tensor_value_info(n, t, list(shape))
def rng(): return np.random.default_rng(7)
def init(n, a): return numpy_helper.from_array(np.asarray(a), n)


def unary(name, node_op, shape, attrs=None, opset=17, scale=1.0, inits=(), extra_in=(), offset=0.0, out_shape=None):
    r = rng()
    nodes = [H.make_node(node_op, ['x'] + list(extra_in), ['y'], **(attrs or {}))]
    save(name, nodes, [vi('x', shape)], [vi('y', out_shape or shape)], list(inits),
         {'x': offset + scale * r.standard_normal(shape)}, opset)


def elt(name, op, rows=16, width=64, scale=1.0):
    attrs, inits, extra, offset = {}, [], [], 0.0
    if op in ('Sqrt', 'Log'): offset = 3.0   # positive inputs
    if op == 'Reciprocal': offset = 3.0
    if op == 'Clip': inits, extra = [init('lo', np.float32(-0.5)), init('hi', np.float32(0.7))], ['lo', 'hi']
    if op == 'Softplus': pass
    unary(name, op, [rows, width], attrs, 17, scale, inits, extra, offset)


def gelu(name, rows=16, width=64, form='erf'):
    r = rng(); shape = [rows, width]; x = r.standard_normal(shape)
    if form == 'native':
        nodes = [H.make_node('Gelu', ['x'], ['y'])]; inits = []; opset = 20
    elif form == 'erf':
        inits = [init('c', np.float32(1 / np.sqrt(2))), init('h', np.float32(0.5)), init('o', np.float32(1.0))]
        nodes = [H.make_node('Mul', ['x', 'c'], ['a']), H.make_node('Erf', ['a'], ['b']), H.make_node('Add', ['b', 'o'], ['d']),
                 H.make_node('Mul', ['x', 'd'], ['e']), H.make_node('Mul', ['e', 'h'], ['y'])]; opset = 17
    else:
        inits = [init('k0', np.float32(0.044715)), init('k1', np.float32(np.sqrt(2 / np.pi))), init('h', np.float32(0.5)), init('o', np.float32(1.0))]
        nodes = [H.make_node('Mul', ['x', 'x'], ['x2']), H.make_node('Mul', ['x2', 'x'], ['x3']), H.make_node('Mul', ['x3', 'k0'], ['a']),
                 H.make_node('Add', ['x', 'a'], ['b']), H.make_node('Mul', ['b', 'k1'], ['c']), H.make_node('Tanh', ['c'], ['d']),
                 H.make_node('Add', ['d', 'o'], ['e']), H.make_node('Mul', ['x', 'e'], ['f']), H.make_node('Mul', ['f', 'h'], ['y'])]; opset = 17
    save(name, nodes, [vi('x', shape)], [vi('y', shape)], inits, {'x': x}, opset)


def power(name, rows=16, width=64, exponent=3.0):
    unary(name, 'Pow', [rows, width], None, 17, 1.0, [init('e', np.float32(exponent))], ['e'])


def gather(name, rows=32, width=64, indices=(3, 1, 7, 5), axis=0):
    r = rng(); shape = [rows, width]
    idx = init('idx', np.array(indices, np.int64))
    out = list(shape); out[axis] = len(indices)
    save(name, [H.make_node('Gather', ['x', 'idx'], ['y'], axis=axis)], [vi('x', shape)], [vi('y', out)], [idx], {'x': r.standard_normal(shape)})


def gather_dynamic(name, rows=32, width=64, count=4):
    r = rng()
    save(name, [H.make_node('Gather', ['x', 'idx'], ['y'])], [vi('x', [rows, width]), vi('idx', [count], TensorProto.INT64)], [vi('y', [count, width])], [],
         {'x': r.standard_normal((rows, width)), 'idx': np.array([3, 1, 7, 5][:count])})


def embed(name, vocab=64, dim=32, seq=8):
    r = rng()
    table = init('table', (0.5 * r.standard_normal((vocab, dim))).astype(np.float32))
    save(name, [H.make_node('Gather', ['table', 'idx'], ['y'])], [vi('idx', [seq], TensorProto.INT64)], [vi('y', [seq, dim])], [table],
         {'idx': (np.arange(seq) * 7 + 3) % vocab})


def resize(name, c=4, h=8, w=8, mode='nearest', scale=2.0):
    r = rng(); shape = [1, c, h, w]
    roi = init('roi', np.zeros(0, np.float32)); sc = init('sc', np.array([1, 1, scale, scale], np.float32))
    oh, ow = int(h * scale), int(w * scale)
    n = H.make_node('Resize', ['x', 'roi', 'sc'], ['y'], mode=mode, coordinate_transformation_mode='asymmetric' if mode == 'nearest' else 'half_pixel',
                    nearest_mode='floor')
    save(name, [n], [vi('x', shape)], [vi('y', [1, c, oh, ow])], [roi, sc], {'x': r.standard_normal(shape)})


def pad(name, c=4, h=8, w=8, pad=1):
    r = rng(); shape = [1, c, h, w]
    pads = init('pads', np.array([0, 0, pad, pad, 0, 0, pad, pad], np.int64))
    save(name, [H.make_node('Pad', ['x', 'pads'], ['y'], mode='constant')], [vi('x', shape)], [vi('y', [1, c, h + 2 * pad, w + 2 * pad])], [pads], {'x': r.standard_normal(shape)})


def tr(name, shape=(1, 8, 4, 6), perm=(0, 2, 3, 1)):
    r = rng(); shape = list(shape); out = [shape[p] for p in perm]
    save(name, [H.make_node('Transpose', ['x'], ['y'], perm=list(perm))], [vi('x', shape)], [vi('y', out)], [], {'x': r.standard_normal(shape)})


def concat(name, count=2, rows=16, width=64, axis=1):
    r = rng(); shape = [rows, width]; out = list(shape); out[axis] *= count
    ins = ['x%d' % i for i in range(count)]
    save(name, [H.make_node('Concat', ins, ['y'], axis=axis)], [vi(i, shape) for i in ins], [vi('y', out)], [], {i: r.standard_normal(shape) for i in ins})


def split(name, rows=16, width=64, axis=1):
    r = rng(); shape = [rows, width]; out = list(shape); out[axis] //= 2
    sizes = init('sizes', np.array([shape[axis] // 2] * 2, np.int64))
    save(name, [H.make_node('Split', ['x', 'sizes'], ['a', 'b'], axis=axis)], [vi('x', shape)], [vi('a', out), vi('b', out)], [sizes], {'x': r.standard_normal(shape)})


def pool(name, op, c=4, h=8, w=8, kernel=2, stride=2):
    r = rng(); shape = [1, c, h, w]; oh, ow = (h - kernel) // stride + 1, (w - kernel) // stride + 1
    save(name, [H.make_node(op, ['x'], ['y'], kernel_shape=[kernel, kernel], strides=[stride, stride])], [vi('x', shape)], [vi('y', [1, c, oh, ow])], [], {'x': r.standard_normal(shape)})


def gpool(name, c=8, h=8, w=8):
    r = rng(); shape = [1, c, h, w]
    save(name, [H.make_node('GlobalAveragePool', ['x'], ['y'])], [vi('x', shape)], [vi('y', [1, c, 1, 1])], [], {'x': r.standard_normal(shape)})


def lrn(name, c=8, h=6, w=6, size=3):
    unary(name, 'LRN', [1, c, h, w], {'size': size, 'alpha': 1e-3, 'beta': 0.75, 'bias': 1.0})


def s2d(name, op='SpaceToDepth', c=4, h=8, w=8, block=2):
    r = rng(); shape = [1, c, h, w]
    out = [1, c * block * block, h // block, w // block] if op == 'SpaceToDepth' else [1, c // (block * block), h * block, w * block]
    save(name, [H.make_node(op, ['x'], ['y'], blocksize=block)], [vi('x', shape)], [vi('y', out)], [], {'x': r.standard_normal(shape)})


def slice_(name, rows=16, width=64, start=8, end=40, axis=1):
    r = rng(); shape = [rows, width]; out = list(shape); out[axis] = end - start
    inits = [init('s', np.array([start], np.int64)), init('e', np.array([end], np.int64)), init('a', np.array([axis], np.int64))]
    save(name, [H.make_node('Slice', ['x', 's', 'e', 'a'], ['y'])], [vi('x', shape)], [vi('y', out)], inits, {'x': r.standard_normal(shape)})


def where(name, rows=16, width=64):
    r = rng(); shape = [rows, width]
    inits = [init('t', np.float32(0.0))]
    nodes = [H.make_node('Greater', ['x', 't'], ['c']), H.make_node('Where', ['c', 'x', 'z'], ['y'])]
    save(name, nodes, [vi('x', shape), vi('z', shape)], [vi('y', shape)], inits, {'x': r.standard_normal(shape), 'z': r.standard_normal(shape)})


def reduce(name, op='ReduceMean', rows=16, width=64):
    r = rng()
    n = H.make_node(op, ['x'], ['y'], axes=[1], keepdims=1)
    save(name, [n], [vi('x', [rows, width])], [vi('y', [rows, 1])], [], {'x': r.standard_normal((rows, width))})


def topk(name, rows=16, width=64, k=4):
    r = rng()
    kk = init('k', np.array([k], np.int64))
    save(name, [H.make_node('TopK', ['x', 'k'], ['v', 'i'], axis=-1)], [vi('x', [rows, width])], [vi('v', [rows, k])], [kk], {'x': r.standard_normal((rows, width))})


def argmax(name, rows=16, width=64):
    r = rng()
    save(name, [H.make_node('ArgMax', ['x'], ['y'], axis=1, keepdims=0)], [vi('x', [rows, width])], [vi('y', [rows], TensorProto.INT64)], [], {'x': r.standard_normal((rows, width))})


def cumsum(name, rows=16, width=64):
    r = rng()
    ax = init('ax', np.array(1, np.int64))
    save(name, [H.make_node('CumSum', ['x', 'ax'], ['y'])], [vi('x', [rows, width])], [vi('y', [rows, width])], [ax], {'x': r.standard_normal((rows, width))})


def expand(name, rows=16, width=64):
    r = rng()
    sh = init('sh', np.array([rows, width], np.int64))
    save(name, [H.make_node('Expand', ['x', 'sh'], ['y'])], [vi('x', [rows, 1])], [vi('y', [rows, width])], [sh], {'x': r.standard_normal((rows, 1))})


def tile(name, rows=8, width=32, reps=(2, 2)):
    r = rng()
    rp = init('rp', np.array(reps, np.int64))
    save(name, [H.make_node('Tile', ['x', 'rp'], ['y'])], [vi('x', [rows, width])], [vi('y', [rows * reps[0], width * reps[1]])], [rp], {'x': r.standard_normal((rows, width))})


def conv(name, k=3, cin=4, cout=8, h=8, w=8, group=1, stride=1):
    r = rng()
    wt = init('w', (0.2 * r.standard_normal((cout, cin // group, k, k))).astype(np.float32))
    b = init('b', (0.1 * r.standard_normal(cout)).astype(np.float32))
    oh, ow = (h - k) // stride + 1, (w - k) // stride + 1
    n = H.make_node('Conv', ['x', 'w', 'b'], ['y'], kernel_shape=[k, k], strides=[stride, stride], group=group)
    save(name, [n], [vi('x', [1, cin, h, w])], [vi('y', [1, cout, oh, ow])], [wt, b], {'x': r.standard_normal((1, cin, h, w))})


def matmul(name, rows=16, inner=64, cols=32):
    r = rng()
    wt = init('w', (0.2 * r.standard_normal((inner, cols))).astype(np.float32))
    save(name, [H.make_node('MatMul', ['x', 'w'], ['y'])], [vi('x', [rows, inner])], [vi('y', [rows, cols])], [wt], {'x': r.standard_normal((rows, inner))})


def misc(name, op):
    r = rng(); I64 = TensorProto.INT64
    if op == 'OneHot':
        d = [init('depth', np.array(10, np.int64)), init('vals', np.array([0, 1], np.float32))]
        save(name, [H.make_node('OneHot', ['idx', 'depth', 'vals'], ['y'])], [vi('idx', [8], I64)], [vi('y', [8, 10])], d, {'idx': np.arange(8)}, 11)
    elif op == 'NonZero':
        save(name, [H.make_node('NonZero', ['x'], ['y'])], [vi('x', [4, 8])], [vi('y', [2, 16], I64)], [], {'x': r.standard_normal((4, 8))}, 13)
    elif op == 'ScatterND':
        d = [init('i', np.array([[1], [3]], np.int64))]
        save(name, [H.make_node('ScatterND', ['x', 'i', 'u'], ['y'])], [vi('x', [8, 8]), vi('u', [2, 8])], [vi('y', [8, 8])], d,
             {'x': r.standard_normal((8, 8)), 'u': r.standard_normal((2, 8))}, 16)
    elif op == 'GatherND':
        d = [init('i', np.array([[1], [3]], np.int64))]
        save(name, [H.make_node('GatherND', ['x', 'i'], ['y'])], [vi('x', [8, 8])], [vi('y', [2, 8])], d, {'x': r.standard_normal((8, 8))}, 13)
    elif op == 'Mod':
        d = [init('m', np.float32(0.7))]
        save(name, [H.make_node('Mod', ['x', 'm'], ['y'], fmod=1)], [vi('x', [8, 8])], [vi('y', [8, 8])], d, {'x': 3 * r.standard_normal((8, 8))}, 13)
    elif op == 'ReduceProd':
        save(name, [H.make_node('ReduceProd', ['x'], ['y'], axes=[1], keepdims=1)], [vi('x', [8, 8])], [vi('y', [8, 1])], [], {'x': 1 + 0.1 * r.standard_normal((8, 8))}, 13)
    elif op == 'NonMaxSuppression':
        d = [init('mo', np.array([5], np.int64)), init('iou', np.array([0.5], np.float32)), init('st', np.array([0.1], np.float32))]
        save(name, [H.make_node('NonMaxSuppression', ['b', 's', 'mo', 'iou', 'st'], ['y'])], [vi('b', [1, 16, 4]), vi('s', [1, 1, 16])], [vi('y', [5, 3], I64)], d,
             {'b': np.sort(r.random((1, 16, 4)), axis=2), 's': r.random((1, 1, 16))}, 11)
    elif op == 'RoiAlign':
        d = [init('rois', np.array([[0, 0, 4, 4], [1, 1, 6, 6]], np.float32)), init('bi', np.array([0, 0], np.int64))]
        save(name, [H.make_node('RoiAlign', ['x', 'rois', 'bi'], ['y'], output_height=2, output_width=2)], [vi('x', [1, 4, 8, 8])], [vi('y', [2, 4, 2, 2])], d,
             {'x': r.standard_normal((1, 4, 8, 8))}, 16)
    elif op == 'GridSample':
        save(name, [H.make_node('GridSample', ['x', 'g'], ['y'], mode='bilinear', align_corners=0)], [vi('x', [1, 4, 8, 8]), vi('g', [1, 6, 6, 2])], [vi('y', [1, 4, 6, 6])], [],
             {'x': r.standard_normal((1, 4, 8, 8)), 'g': np.clip(r.standard_normal((1, 6, 6, 2)), -1, 1)}, 16)
    elif op == 'Einsum':
        save(name, [H.make_node('Einsum', ['x', 'z'], ['y'], equation='bij,bjk->bik')], [vi('x', [2, 4, 8]), vi('z', [2, 8, 4])], [vi('y', [2, 4, 4])], [],
             {'x': r.standard_normal((2, 4, 8)), 'z': r.standard_normal((2, 8, 4))}, 13)
    elif op == 'Unique':
        save(name, [H.make_node('Unique', ['x'], ['y'])], [vi('x', [16])], [vi('y', [16])], [], {'x': np.round(3 * r.standard_normal(16))}, 11)
    else:
        save(name, [H.make_node(op, ['x'], ['y'])], [vi('x', [8, 8])], [vi('y', [8, 8])], [], {'x': 3 * r.standard_normal((8, 8))}, 13)


def seq(name, ops, rows=16, width=64):
    r = rng(); nodes, inits, cur = [], [], 'x'
    for i, op in enumerate(ops):
        out = 'y' if i == len(ops) - 1 else 't%d' % i
        if op == 'MatMul':
            inits.append(init('w%d' % i, (0.2 * r.standard_normal((width, width))).astype(np.float32)))
            nodes.append(H.make_node('MatMul', [cur, 'w%d' % i], [out]))
        else:
            nodes.append(H.make_node(op, [cur], [out]))
        cur = out
    save(name, nodes, [vi('x', [rows, width])], [vi('y', [rows, width])], inits, {'x': r.standard_normal((rows, width))})


KINDS = {'seq': seq, 'misc': misc, 'elt': elt, 'gelu': gelu, 'pow': power, 'gather': gather, 'gatherd': gather_dynamic, 'embed': embed, 'resize': resize, 'pad': pad, 'tr': tr,
         'concat': concat, 'split': split, 'gpool': gpool, 'lrn': lrn, 's2d': s2d, 'd2s': lambda name, **k: s2d(name, op='DepthToSpace', **k),
         'maxpool': lambda name, **k: pool(name, 'MaxPool', **k), 'avgpool': lambda name, **k: pool(name, 'AveragePool', **k),
         'slice': slice_, 'where': where, 'reduce': reduce, 'topk': topk, 'argmax': argmax, 'cumsum': cumsum, 'expand': expand, 'tile': tile,
         'conv': conv, 'matmul': matmul}
for item in json.loads(sys.argv[2]):
    kind = item.pop('kind'); name = item.pop('name')
    try:
        KINDS[kind](name, **item)
    except Exception as e:
        print('FAILED to write', name, repr(e))
