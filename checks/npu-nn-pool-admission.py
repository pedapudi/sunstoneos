#!/usr/bin/env python3
from pathlib import Path
import importlib.util,json
import argparse
parser=argparse.ArgumentParser(description='Actual fixed NN model/family admission refusals; no hardware access.')
parser.add_argument('--image',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--case',choices=('all','over-model-bound','truncated-registered-model','nonempty-shader-slot','changed-model'),default='all')
a=parser.parse_args();r=Path(__file__).resolve().parents[1]
sp=importlib.util.spec_from_file_location('nn',r/'checks/npu-nn-pool-native.py');n=importlib.util.module_from_spec(sp);sp.loader.exec_module(n);image=n.base.Image(a.image);results={}
def machine():return n.Machine(image,{})
def config(m,bytes=964224):
 cfg=0x43200000;fields=image.layouts['npu_convolution_config'];bf=image.layouts['a733_npu_buffer']
 for field,value in [('model',image.address('a733_npu_application_model')),('model_bytes',bytes),('register_cpu',0x03600000),('register_physical',0x03600000),('arena',0x41911000),('arena_physical',0x41911000),('arena_bytes',20480),('fatal',0x43fff000)]:m.put(cfg+fields[field],value)
 cpus=(0x41600000,0x41606000,0x417eb000,0x4186b000,0x41890000,0x41891000,0x416e0000,0);devices=(0xe0000000,0xe0007000,0xe01ee000,0xe026f000,0xe0295000,0xe0297000,0xe00e2000,0);sizes=(24576,892928,524288,151552,4096,524288,1093632,0);roles=(4,4,2,0,1,3,1,0)
 for i in range(8):
  for field,value,width in [('cpu',cpus[i],8),('physical',cpus[i],8),('bytes',sizes[i],8),('device',devices[i],4),('role',roles[i],4)]:m.put(cfg+fields['buffers']+i*32+bf[field],value,width)
 return cfg
for name,model_bytes,mutate,failed in [('over-model-bound',1048577,False,0),('truncated-registered-model',964223,False,1),('nonempty-shader-slot',964224,True,0)]:
 if a.case not in ('all',name):continue
 m=machine();cfg=config(m,model_bytes)
 if mutate:m.put(cfg+image.layouts['npu_convolution_config']['buffers']+7*32+image.layouts['a733_npu_buffer']['cpu'],0x41a00000)
 assert m.call('npu_convolution_prepare',0x43000000,cfg)==0
 assert m.get(0x43000000+image.layouts['npu_convolution']['failed'],4)==failed and m.get(0x43000000+image.layouts['npu_convolution']['prepared'],4)==0
 assert m.translations==0;results[name]=True
if a.case in ('all','changed-model'):
 m=machine();cfg=config(m);p=image.address('a733_npu_application_model');m.u.mem_write(p+1000,bytes([m.get(p+1000,1)^1]));assert m.call('npu_convolution_prepare',0x43000000,cfg)==0 and m.get(0x43000000+image.layouts['npu_convolution']['failed'],4)==1 and m.translations==0;results['changed-registered-model-refused-by-whole-model-hash']=True
a.output.write_text(json.dumps(dict(result='pass',cases=results,scope='Actual wrapper new family/model-capacity/empty-slot admission; existing generic decoder/CRC/relocation malformed evidence reused.',physical_access=False),indent=2)+'\n');print('PASS NN admission',a.case)
