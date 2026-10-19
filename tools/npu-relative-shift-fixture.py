#!/usr/bin/env python3
"""Produce the fixed FP32 relative-shift graph and its bit-preserving reference.

Run in the pinned compiler Python environment. This creates no compiled NBG.
"""
from pathlib import Path
import argparse
import numpy as np
import onnx
from onnx import helper as h,numpy_helper as n,TensorProto as T
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('output',type=Path)
p=parser.parse_args().output;p.mkdir(parents=True,exist_ok=False)
heads,time=4,128
x=((np.arange(heads*time*(2*time-1),dtype=np.int32)*37)%251-125).astype(np.float32).reshape(heads,time,2*time-1)/16+np.float32(2**-15)
# Exact existing rel_shift + [:,:,:t] from mimus/train/conformer.py.
y=np.pad(x,((0,0),(0,0),(1,0))).reshape(heads,2*time,time)[:,1:].reshape(heads,time,2*time-1)[:,:,:time].copy()
constants=dict(pads=np.array([0,0,1,0,0,0],np.int64),zero=np.array(0,np.float32),shape0=np.array([heads,2*time,time],np.int64),start0=np.array([1],np.int64),end0=np.array([2*time],np.int64),axis0=np.array([1],np.int64),shape1=np.array([heads,time,2*time-1],np.int64),start1=np.array([0],np.int64),end1=np.array([time],np.int64),axis1=np.array([2],np.int64))
nodes=[h.make_node('Pad',['input','pads','zero'],['padded'],mode='constant'),h.make_node('Reshape',['padded','shape0'],['view0']),h.make_node('Slice',['view0','start0','end0','axis0'],['cropped']),h.make_node('Reshape',['cropped','shape1'],['view1']),h.make_node('Slice',['view1','start1','end1','axis1'],['output'])]
g=h.make_graph(nodes,'relative_shift',[h.make_tensor_value_info('input',T.FLOAT,list(x.shape))],[h.make_tensor_value_info('output',T.FLOAT,list(y.shape))],[n.from_array(v,k) for k,v in constants.items()]);m=h.make_model(g,opset_imports=[h.make_opsetid('',11)]);m.ir_version=7;onnx.checker.check_model(m);onnx.save(m,str(p/'conv3x3.onnx'));np.save(str(p/'input.npy'),x);np.save(str(p/'expected.npy'),y);(p/'input.f32').write_bytes(x.astype('<f4').tobytes());(p/'expected.f32').write_bytes(y.astype('<f4').tobytes());(p/'input.tensor').write_text('\n'.join(format(float(v),'.9g') for v in x.ravel())+'\n');print('Input',x.shape,'output',y.shape,'nonzero',np.count_nonzero(y))
