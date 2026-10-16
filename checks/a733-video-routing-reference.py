#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute package codec selection and register routing for admitted VE IDs."""
import argparse
import hashlib
import json
import pathlib
import struct
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X30,UC_ARM64_REG_SP,UC_ARM64_REG_PC
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('libraries',type=pathlib.Path)
p.add_argument('output',type=pathlib.Path)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
pins={'libVE.so':'3af49a5eb24f6832518a858274fb206d60e490985bff02f5d5be36d053f65be4',
      'libvideoengine.so':'abfea1fa75f6a3ca5413d16eb69fcb87692f3e3b6552d83a222a998e909cef82'}
CTX,NODE1,NODE2,LEVEL,MMIO,STACK=0x100000,0x101000,0x102000,0x103000,0x110000,0x130000
STOP,STRCMP,LEGACY,PLUS=0x13f000,0x13f010,0x13f020,0x13f030

def load(name):
    path=a.libraries/name
    assert hashlib.sha256(path.read_bytes()).hexdigest()==pins[name]
    u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0,0x20000);u.mem_map(0x100000,0x40000)
    syms={}
    with path.open('rb') as f:
        e=ELFFile(f)
        for s in e.iter_segments():
            if s['p_type']=='PT_LOAD':u.mem_write(s['p_vaddr'],s.data())
        for s in e.get_section_by_name('.symtab').iter_symbols():
            if s['st_value']:syms[s.name]=s['st_value']
        for n in ('.rela.dyn','.rela.plt'):
            rs=e.get_section_by_name(n);ss=e.get_section(rs['sh_link'])
            for r in rs.iter_relocations():
                s=ss.get_symbol(r['r_info_sym'])
                v=r['r_addend'] if r['r_info_type']==1027 else s['st_value']
                if s.name=='GLOBAL_LOG_LEVEL':v=LEVEL
                if s.name=='strcmp':v=STRCMP
                u.mem_write(r['r_offset'],struct.pack('<Q',v))
    u.mem_write(LEVEL,struct.pack('<I',7))
    def hook(uc,pc,size,data):
        if pc==STOP:uc.emu_stop();return
        if pc not in (STRCMP,LEGACY,PLUS):return
        if pc==STRCMP:
            def string(address):
                b=bytes(uc.mem_read(address,64));return b.split(b'\0')[0]
            v=int(string(uc.reg_read(UC_ARM64_REG_X0))!=string(uc.reg_read(UC_ARM64_REG_X1)))
        else:v=2 if pc==LEGACY else 1
        uc.reg_write(UC_ARM64_REG_X0,v);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_X30))
    u.hook_add(UC_HOOK_CODE,hook)
    return u,syms

def put(u,off,v,fmt='I',base=CTX):u.mem_write(base+off,struct.pack('<'+fmt,v))
def call(u,syms,name,x1=0):
    for reg,value in [(UC_ARM64_REG_X0,CTX),(UC_ARM64_REG_X1,x1),(UC_ARM64_REG_X2,0),(UC_ARM64_REG_SP,STACK),(UC_ARM64_REG_X30,STOP)]:u.reg_write(reg,value)
    u.emu_start(syms[name],STOP,count=100000)
    assert u.reg_read(UC_ARM64_REG_PC)==STOP
    return u.reg_read(UC_ARM64_REG_X0)

ids=[0x0003332100021321,0x0003331000021320,0x0003052000012011]
record=[]
for version in ids:
    u,syms=load('libVE.so')
    put(u,0,1);put(u,0x74,3);put(u,0x48,MMIO,'Q');put(u,0x98,version,'Q')
    routes={}
    for group,offset in [(0,0),(1,0x100),(6,0xe00)]:
        routes[group]=call(u,syms,'VeGetGroupRegAddr',group)-MMIO
        assert routes[group]==offset,(group,routes[group],offset)
    u,syms=load('libvideoengine.so');head=syms['gVDecoderList']
    put(u,0,NODE1,'Q',head);put(u,0x10,2,base=head)
    for node,nextnode,name,callback in [(NODE1,NODE2,b'mjpegplus\0',PLUS),(NODE2,head,b'mjpeg\0',LEGACY)]:
        put(u,0,nextnode,'Q',node);put(u,0x10,callback,'Q',node)
        put(u,0x18,0,base=node);put(u,0x5c,0x101,base=node)
        u.mem_write(node+0x1c,name)
    put(u,0xe0,0x101);put(u,0x1e0,version,'Q')
    selection=call(u,syms,'CreateSpecificDecoder')
    assert selection==2
    record.append({'version':hex(version),'selected':'mjpeg','map3_offsets':routes})
(a.output/'routing.json').write_text(json.dumps({'libraries':pins,'results':record},indent=2)+'\n')
print('PASS: admitted compound IDs select legacy MJPEG; split-map MPEG/JPEG/top offsets match package instructions. Physical identity remains unobserved.')
