#!/usr/bin/env python3
"""Generate two Conv2D/ReLU stages with an exact integer reference.

Run in the pinned compiler environment. This small diagnostic covers the
stride-two padded convolution/activation sequence used by mimus subsampling;
it is not a compiled mimus model or a native inference result.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
import onnx
from onnx import helper, numpy_helper, TensorProto


def convolution(a, weights, bias):
    padded = np.pad(a, ((0,0),(0,0),(1,1),(1,1)))
    height, width = (a.shape[2]+1)//2, (a.shape[3]+1)//2
    result = np.empty((1,len(weights),height,width), dtype=np.int64)
    for channel in range(len(weights)):
        for row in range(height):
            for col in range(width):
                result[0,channel,row,col] = int(bias[channel]) + sum(
                    int(padded[0,ic,2*row+ky,2*col+kx]) * int(weights[channel,ic,ky,kx])
                    for ic in range(a.shape[1]) for ky in range(3) for kx in range(3))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, help='new fixture directory')
    out = parser.parse_args().output
    out.mkdir(parents=True, exist_ok=False)
    x = ((np.arange(64,dtype=np.int32)*7)%33-16).reshape(1,1,8,8)
    w0 = (np.arange(18,dtype=np.int32)%5-2).reshape(2,1,3,3)
    b0 = np.array([16,-32],dtype=np.int32)
    w1 = (np.arange(36,dtype=np.int32)%7-3).reshape(2,2,3,3)
    b1 = np.array([32,-16],dtype=np.int32)
    first_raw = convolution(x,w0,b0)
    first = np.maximum(first_raw,0)
    second_raw = convolution(first,w1,b1)
    second = np.maximum(second_raw,0)
    assert all(np.any(v<0) and np.any(v>0) for v in (first_raw,second_raw))
    arrays = dict(input=x.astype(np.float32)/8,
                  weights0=w0.astype(np.float32)/8, bias0=b0.astype(np.float32)/64,
                  weights1=w1.astype(np.float32)/8, bias1=b1.astype(np.float32)/512,
                  expected=second.astype(np.float32)/512,
                  first_raw=first_raw, first=first, second_raw=second_raw)
    nodes = [
        helper.make_node('Conv',['input','weights0','bias0'],['conv0'],
                         kernel_shape=[3,3],strides=[2,2],pads=[1,1,1,1]),
        helper.make_node('Relu',['conv0'],['hidden']),
        helper.make_node('Conv',['hidden','weights1','bias1'],['conv1'],
                         kernel_shape=[3,3],strides=[2,2],pads=[1,1,1,1]),
        helper.make_node('Relu',['conv1'],['output']),
    ]
    graph = helper.make_graph(nodes,'subsample2',
        [helper.make_tensor_value_info('input',TensorProto.FLOAT,[1,1,8,8])],
        [helper.make_tensor_value_info('output',TensorProto.FLOAT,[1,2,2,2])],
        [numpy_helper.from_array(arrays[n],n) for n in ('weights0','bias0','weights1','bias1')])
    model = helper.make_model(graph,opset_imports=[helper.make_opsetid('',11)])
    model.ir_version = 7
    onnx.checker.check_model(model)
    onnx.save(model,str(out/'conv3x3.onnx'))
    for name,array in arrays.items(): np.save(str(out/(name+'.npy')),array,allow_pickle=False)
    report = dict(graph='Conv2D/ReLU/Conv2D/ReLU',input_shape=[1,1,8,8],output_shape=[1,2,2,2],
                  first_active=int(np.count_nonzero(first)),second_active=int(np.count_nonzero(second)),
                  reference='Integer cross-correlations at stride2/padding1, ReLU after each, first numerator/64 and final numerator/512.',
                  physical_access=False,
                  files={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.iterdir()})
    (out/'fixture.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Generated two-stage graph; output',arrays['expected'].ravel().tolist())


if __name__ == '__main__': main()
