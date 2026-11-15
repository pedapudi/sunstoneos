#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare native reset/default writes with the pinned libVE decoder policy."""
import argparse
import hashlib
import json
import pathlib
import struct
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X30,UC_ARM64_REG_SP,UC_ARM64_REG_PC
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('libraries',type=pathlib.Path)
p.add_argument('native',type=pathlib.Path)
p.add_argument('output',type=pathlib.Path)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
pins={'libVE.so':'3af49a5eb24f6832518a858274fb206d60e490985bff02f5d5be36d053f65be4',
      'libvideoengine.so':'abfea1fa75f6a3ca5413d16eb69fcb87692f3e3b6552d83a222a998e909cef82'}
CTX,NODE1,NODE2,LEVEL,MMIO,STACK=0x100000,0x101000,0x102000,0x103000,0x110000,0x130000
STOP,STRCMP,LEGACY,PLUS=0x13f000,0x13f010,0x13f020,0x13f030
IOCTL=0x13f040
trace=[];reset_calls=[]

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
                if s.name in ('pthread_mutex_lock','pthread_mutex_unlock'):v=LEGACY
                if s.name=='ioctl':v=IOCTL
                u.mem_write(r['r_offset'],struct.pack('<Q',v))
    u.mem_write(LEVEL,struct.pack('<I',7))
    def hook(uc,pc,size,data):
        if pc==STOP:uc.emu_stop();return
        if pc==IOCTL:
            cmd=uc.reg_read(UC_ARM64_REG_X1)
            assert cmd in (0x104,0x310,0x311),hex(cmd)
            if cmd==0x104:
                # This is the pinned kernel IOCTL_RESET_VE sequence, separate
                # from the executed library instructions. No hardware reset
                # effects are simulated. The C model checks the 10us hold.
                reset_calls.append(cmd)
                trace.extend([[0x02002a8c,4,4],[0x02002a8c,4,0x40004]])
                old=struct.unpack('<I',uc.mem_read(MMIO+0x804,4))[0]
                for val in (old|0x11,old&~0x11):
                    trace.append([0x01c0e804,4,val]);uc.mem_write(MMIO+0x804,struct.pack('<I',val))
            uc.reg_write(UC_ARM64_REG_X0,0);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_X30));return
        if pc not in (STRCMP,LEGACY,PLUS):return
        if pc==STRCMP:
            def string(address):
                b=bytes(uc.mem_read(address,64));return b.split(b'\0')[0]
            v=int(string(uc.reg_read(UC_ARM64_REG_X0))!=string(uc.reg_read(UC_ARM64_REG_X1)))
        else:v=0
        uc.reg_write(UC_ARM64_REG_X0,v);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_X30))
    u.hook_add(UC_HOOK_CODE,hook)
    return u,syms

def put(u,off,v,fmt='I',base=CTX):u.mem_write(base+off,struct.pack('<'+fmt,v))
def call(u,syms,name,x1=0):
    for reg,value in [(UC_ARM64_REG_X0,CTX),(UC_ARM64_REG_X1,x1),(UC_ARM64_REG_X2,0),(UC_ARM64_REG_SP,STACK),(UC_ARM64_REG_X30,STOP)]:u.reg_write(reg,value)
    u.emu_start(syms[name],STOP,count=100000)
    assert u.reg_read(UC_ARM64_REG_PC)==STOP
    return u.reg_read(UC_ARM64_REG_X0)


inputs={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [pathlib.Path(__file__),a.libraries/'libVE.so',*sorted(a.native.glob('*.bin'))]}
(a.output/'inputs.json').write_text(json.dumps(inputs,indent=2)+'\n')
results=[]
for index,version in enumerate([0x0003332100021321,0x0003331000021320,0x0003052000012011]):
    u,syms=load('libVE.so');trace=[];reset_calls=[]
    policy=next(i for i in range(53) if struct.unpack('<Q',u.mem_read(syms['policy_list']+i*48,8))[0]==version)
    put(u,0,1);put(u,8,0x101);put(u,0xc,48);put(u,0x74,3)
    put(u,0x48,MMIO,'Q');put(u,0x78,MMIO,'Q');put(u,0x98,version,'Q')
    put(u,0xa8,5);put(u,0xb8,policy)
    initial=bytearray(struct.pack('<I',0x80808080)*1024)
    for off,val in [(0,0),(4,0),(0x804,0),(0xe0,version>>32),(0xe4,version&0xffffffff),(0xf0,0),(0x11c,0)]:struct.pack_into('<I',initial,off,val)
    u.mem_write(MMIO,bytes(initial))
    def write(uc,access,address,size,value,data):
        if MMIO<=address<MMIO+0x1000:trace.append([0x01c0e000+address-MMIO,size,value])
    u.hook_add(UC_HOOK_MEM_WRITE,write)
    call(u,syms,'VeReset')
    native=list(struct.iter_unpack('<III',(a.native/(str(index)+'.bin')).read_bytes()))
    actual=bytearray(initial)
    for address,width,value in native:
        if 0x01c0e000<=address<0x01c0f000:actual[address-0x01c0e000:address-0x01c0e000+width]=value.to_bytes(width,'little')
    expected=bytes(u.mem_read(MMIO,0x1000))
    assert actual==expected,[(hex(i),x,y)for i,(x,y)in enumerate(zip(actual,expected))if x!=y]
    pulse_addresses={0x02002a8c,0x01c0e804,0x01c0e004}
    assert [tuple(x)for x in trace if x[0]in pulse_addresses]==[x for x in native if x[0]in pulse_addresses]
    assert len(reset_calls)==(0 if index==1 else 1)
    (a.output/(str(index)+'-reference.json')).write_text(json.dumps(trace,indent=2)+'\n')
    (a.output/(str(index)+'-native.json')).write_text(json.dumps(native,indent=2)+'\n')
    results.append({'identity':hex(version),'policy_index':policy,'kernel_reset_calls':reset_calls,'register_image_matches':True})
(a.output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
print('PASS: native preparation final register image and ordered reset pulses match admitted libVE policies plus pinned kernel reset sequence. No device behavior emulated.')
