#!/usr/bin/env python3
"""Check descriptor identities, register layout, code bounds and C compilation."""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True)
p.add_argument('--cc',default='clang')
a=p.parse_args();a.build.mkdir(parents=True,exist_ok=True)
spec=importlib.util.spec_from_file_location('program_compile',ROOT/'apps/gpu-program/compile.py')
c=importlib.util.module_from_spec(spec);spec.loader.exec_module(c)
m=json.loads((ROOT/'apps/queue-add/sources.json').read_text())
header=(ROOT/'apps/queue-add/program.h').read_text()
words=[int(w,16) for w in re.findall(r'0x([0-9a-f]{8})u',header)]
code=struct.pack('<'+'I'*len(words),*words)
blob=bytes([m['metadata'][0],5,3,0,0])+struct.pack('<H',len(words))+bytes(1)+code
assert hashlib.sha256((ROOT/'apps/queue-add/vector_add.cl').read_bytes()).hexdigest()==m['source_sha256']
assert c.emit_queue_program(blob,m)==header
cases=['reproducible-real-grid-descriptor']

def refused(name,binary,manifest):
    try:c.emit_queue_program(binary,manifest)
    except ValueError:cases.append(name)
    else:raise AssertionError(name+' accepted')

for field,value in [('profile',2),('mesa_revision','0'*40),('schema','wrong'),
                    ('program_sha256','0'*64),('compiler_sha256','x'*64),
                    ('source_sha256','a'),('explicit_vtxin_mask',31),('abi_vtxins',6),
                    ('sysval_ranges',[4,1,0,3,4,3])]:
    other=copy.deepcopy(m);other[field]=value;refused('reject-'+field,blob,other)
for index,value in [(0,253),(1,6),(2,4),(3,1),(4,4),(5,1),(6,1),(7,1),(8,1),(9,1),
                    (10,64),(11,2),(12,2),(13,1),(14,1),(15,1),(16,1),(17,1)]:
    other=copy.deepcopy(m);other['metadata'][index]=value
    refused('reject-metadata-'+str(index),blob,other)
for name,binary in [('empty',bytes(8)),('short',blob[:-1]),('header',bytes([0])+blob[1:]),
                    ('reserved',blob[:7]+b'\1'+blob[8:]),('extent',blob[:5]+b'\1\0'+blob[7:])]:
    other=copy.deepcopy(m);other['program_sha256']=c.digest(binary);refused('reject-'+name,binary,other)
# Both coefficient allocations are supported. Code identity binds the complete
# compiler binary while the emitted instruction array excludes its header.
for size,coeff in [(8128,3),(4,7)]:
    other=copy.deepcopy(m);other['metadata'][2]=coeff
    other['sysval_ranges']=[4,1,0,3]+([4,3] if coeff==7 else [0,0])
    binary=bytes([10,5,coeff,0,0])+struct.pack('<H',size//4)+bytes(1)+bytes(size)
    other['program_sha256']=c.digest(binary)
    assert '.coefficients = %uu'%coeff in c.emit_queue_program(binary,other)
    cases.append('accept-%d-bytes-%d-coefficients'%(size,coeff))
other=copy.deepcopy(m)
binary=bytes([10,5,3,0,0])+struct.pack('<H',2033)+bytes(1)+bytes(8132)
other['program_sha256']=c.digest(binary);refused('reject-over-two-pages',binary,other)
(a.build/'descriptor.c').write_text(header+'\nconst void *descriptor(void);\nconst void *descriptor(void) { return &gpu_queue_program; }\n')
for flags,suffix in [([], 'host'),(['--target=aarch64-none-elf','-ffreestanding'],'aarch64')]:
    subprocess.run([a.cc,*flags,'-std=c11','-Wall','-Wextra','-Werror','-Wpedantic',
                    '-I'+str(ROOT/'include'),'-c',str(a.build/'descriptor.c'),
                    '-o',str(a.build/(suffix+'.o'))],check=True)
result={'schema':'sunstone-gpu-queue-program-host-v1','physical_access':False,'cases':cases,
        'program_sha256':m['program_sha256']}
(a.build/'result.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({'cases':len(cases),'program_sha256':m['program_sha256']}))
