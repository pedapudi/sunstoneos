#!/usr/bin/env python3
"""Write controlled single-operation ONNX graphs for differential compilation of the NPU command formats.

Run inside the pinned compiler image (Python 3.8 with NumPy and ONNX; no network):

  npu-graph-variants.py OUTDIR '[{"kind":"ln","name":"ln176","rows":16,"width":176}, ...]'

Each item names one graph directory under OUTDIR with fragment.onnx, one .npy file per graph input and inputs.json.
Kinds: ln (LayerNormalization; eps, gamma, beta, scale), sm (Softmax over the last axis; scale), un (op =
Sigmoid, Tanh, Relu or Swish; scale), bin (op = Add, Sub or Mul of two inputs), deep (a chain of four operations with intermediate results in VIP SRAM), chain (first and second operation of three inputs; swap puts the first result in the second operand), mm (batched matrix product of two inputs), glu (gated linear unit). Weights and inputs come from a
fixed seed (an item may name skip, the number of draws to discard first), so two variants that differ in one parameter differ in nothing else. The compile and simulate steps are
tools/npu-graph-compile.sh and tools/npu-graph-simulate.sh; docs/hardware/npu-command-formats.md says how the
results are read.
"""
import json, sys
from pathlib import Path
import numpy as np, onnx
from onnx import helper, TensorProto, numpy_helper
OUT = Path(sys.argv[1])
F = TensorProto.FLOAT
def save(name, nodes, inputs, outputs, inits, arrays, opset=17):
    d = OUT / name; d.mkdir(parents=True, exist_ok=True)
    m = helper.make_model(helper.make_graph(nodes, name, inputs, outputs, inits), opset_imports=[helper.make_opsetid('', opset)])
    m.ir_version = 8
    onnx.save(m, str(d / 'fragment.onnx'))
    for k, v in arrays.items(): np.save(str(d / (k + '.npy')), v.astype(np.float32))
    json.dump(list(arrays), open(d / 'inputs.json', 'w'))
SKIP = 0
def stream():
    """The generator every graph draws from: the fixed seed, after SKIP draws. Graphs that were first made one after another from
    a single stream differ from the same graph made alone only in SKIP (tools/npu-fixtures.json)."""
    rng = np.random.default_rng(7)
    rng.standard_normal(SKIP)
    return rng
def vi(n, shape): return helper.make_tensor_value_info(n, F, list(shape))
def layer_norm(name, rows, width, eps=1e-5, gamma=None, beta=None, scale=1.0):
    rng = stream()
    g = numpy_helper.from_array((np.ones(width) if gamma == 'one' else 1 + 0.1 * rng.standard_normal(width)).astype(np.float32), 'g')
    b = numpy_helper.from_array((np.zeros(width) if beta == 'zero' else 0.1 * rng.standard_normal(width)).astype(np.float32), 'b')
    n = helper.make_node('LayerNormalization', ['x', 'g', 'b'], ['y'], axis=-1, epsilon=eps)
    save(name, [n], [vi('x', [rows, width])], [vi('y', [rows, width])], [g, b], {'x': scale * rng.standard_normal((rows, width))})
def softmax(name, rows, width, scale=1.0):
    rng = stream()
    n = helper.make_node('Softmax', ['x'], ['y'], axis=-1)
    save(name, [n], [vi('x', [rows, width])], [vi('y', [rows, width])], [], {'x': scale * rng.standard_normal((rows, width))})
def unary(name, op, rows, width, scale=1.0):
    rng = stream()
    nodes = [helper.make_node(op, ['x'], ['y'])] if op != 'Swish' else [helper.make_node('Sigmoid', ['x'], ['s']), helper.make_node('Mul', ['x', 's'], ['y'])]
    save(name, nodes, [vi('x', [rows, width])], [vi('y', [rows, width])], [], {'x': scale * rng.standard_normal((rows, width))})
def binary(name, op, rows, width):
    rng = stream()
    save(name, [helper.make_node(op, ['x', 'z'], ['y'])], [vi('x', [rows, width]), vi('z', [rows, width])], [vi('y', [rows, width])], [],
         {'x': rng.standard_normal((rows, width)), 'z': rng.standard_normal((rows, width))})
def chain(name, rows, width, first='Mul', second='Add', swap=False):
    """t = first(x, z); y = second(t, w) (second(w, t) when swap): one operand of the second operation is in VIP SRAM."""
    rng = stream()
    nodes = [helper.make_node(first, ['x', 'z'], ['t']), helper.make_node(second, ['w', 't'] if swap else ['t', 'w'], ['y'])]
    save(name, nodes, [vi('x', [rows, width]), vi('z', [rows, width]), vi('w', [rows, width])], [vi('y', [rows, width])], [],
         {k: rng.standard_normal((rows, width)) for k in ('x', 'z', 'w')})
def deep(name, rows, width):
    """t1 = x * z; t2 = t1 + w; t3 = w * t2; y = relu(t3): the third operation reads a result in VIP SRAM as its
    second operand and writes its own result to VIP SRAM."""
    rng = stream()
    nodes = [helper.make_node('Mul', ['x', 'z'], ['t1']), helper.make_node('Add', ['t1', 'w'], ['t2']),
             helper.make_node('Mul', ['w', 't2'], ['t3']), helper.make_node('Relu', ['t3'], ['y'])]
    save(name, nodes, [vi('x', [rows, width]), vi('z', [rows, width]), vi('w', [rows, width])], [vi('y', [rows, width])], [],
         {k: rng.standard_normal((rows, width)) for k in ('x', 'z', 'w')})
def batched_matmul(name, batch, rows, inner, cols):
    """y[b] = x[b] @ z[b] for two graph inputs."""
    rng = stream()
    save(name, [helper.make_node('MatMul', ['x', 'z'], ['y'])], [vi('x', [batch, rows, inner]), vi('z', [batch, inner, cols])],
         [vi('y', [batch, rows, cols])], [], {'x': rng.standard_normal((batch, rows, inner)), 'z': rng.standard_normal((batch, inner, cols))})
def glu(name, rows, width):
    """Gated linear unit: split the last axis in two halves, y = a * sigmoid(b)."""
    rng = stream()
    sizes = numpy_helper.from_array(np.array([width // 2, width // 2], np.int64), 'sizes')
    nodes = [helper.make_node('Split', ['x', 'sizes'], ['a', 'b'], axis=-1), helper.make_node('Sigmoid', ['b'], ['s']),
             helper.make_node('Mul', ['a', 's'], ['y'])]
    save(name, nodes, [vi('x', [rows, width])], [vi('y', [rows, width // 2])], [sizes], {'x': rng.standard_normal((rows, width))})
spec = json.loads(sys.argv[2])
for item in spec:
    kind = item.pop('kind'); name = item.pop('name'); SKIP = item.pop('skip', 0)
    {'ln': layer_norm, 'sm': softmax, 'un': unary, 'bin': binary, 'chain': chain, 'deep': deep, 'mm': batched_matmul, 'glu': glu}[kind](name, **item)
