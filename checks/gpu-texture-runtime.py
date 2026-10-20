#!/usr/bin/env python3
"""Check the linked texture client/host through pinned TinyUSB FIFO completion."""
import argparse
import hashlib
import importlib.util
import json
import struct
import types
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn.arm64_const import *

ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
transport=module('texture_transport',ROOT/'checks/gpu-usb-ordering.py')
layout=module('texture_layout',ROOT/'checks/storage-read-native.py')
host=module('texture_host',ROOT/'apps/gpu-render/texture.py')

class Machine(transport.Machine):
    def __init__(self,image,speed,profile=2):
        super().__init__(image);self.speed=speed;self.profile=profile;self.states=[];self.contexts={};self.incoming=bytearray()
        self.pending=None;self.claimed=False;self.closed=False;self.frame=0;self.ticket=0;self.generation=0
        self.draft=bytearray();self.shape=None;self.snapshot=b'';self.borrow=None;self.reply={}
        self.zlps=0;self.packets=0;self.inputs=[];self.operations=[];self.reads=0
        self.cpu.mem_map(0x48000000,65536)
        self.reply_size=0
        with image.path.open('rb') as f:
            for cu in ELFFile(f).get_dwarf_info().iter_CUs():
                for d in cu.iter_DIEs():
                    if d.tag=='DW_TAG_structure_type' and d.attributes.get('DW_AT_name') and d.attributes['DW_AT_name'].value==b'pvr_gpu_service_reply' and 'DW_AT_byte_size' in d.attributes:
                        self.reply_size=int(d.attributes['DW_AT_byte_size'].value)
        assert self.reply_size
        for name in ('tud_vendor_n_write','tud_vendor_n_write_flush'):
            del self.hooks[image.address(name)]
        for name in ('pvr_gpu_service_collect','pvr_gpu_service_texture','pvr_gpu_service_texture_output',
            'tud_speed_get','usbd_edpt_open','usbd_edpt_claim','usbd_edpt_release','usbd_edpt_xfer'):
            self.hooks[image.address(name)]=name
        sealed=image.address('texture_session');self.write(image.address('session_sealed'),1)
        self.cpu.mem_write(sealed,bytes(632));self.write(sealed+600,profile-1,8);self.write(sealed+616,1920,8);self.write(sealed+624,1080,8)
        self.call('vendord_init')
        configuration=self.call('tud_descriptor_configuration_cb',0)
        assert self.call('vendord_open',0,configuration+9,23)==23
    def call(self,name,*args):
        saved=self.cpu.context_save();old_stop=self.stop
        self.cpu.reg_write(UC_ARM64_REG_SP,0x43ffc000);self.cpu.reg_write(UC_ARM64_REG_LR,0x43fff000)
        for i,arg in enumerate(args):self.cpu.reg_write(UC_ARM64_REG_X0+i,arg)
        self.stop=None;self.cpu.emu_start(self.image.address(name),0,count=1000000)
        assert self.stop=='returned',(name,self.stop,hex(self.cpu.reg_read(UC_ARM64_REG_PC)))
        value=self.cpu.reg_read(UC_ARM64_REG_X0);self.cpu.context_restore(saved);self.stop=old_stop;return value
    def step(self,name):
        if name in self.contexts:self.cpu.context_restore(self.contexts[name]);start=self.cpu.reg_read(UC_ARM64_REG_PC)
        else:
            self.cpu.reg_write(UC_ARM64_REG_SP,0x43ffe000 if name=='gpu_application_usb' else 0x43ffd000)
            self.cpu.reg_write(UC_ARM64_REG_LR,0x43fff000);self.cpu.reg_write(UC_ARM64_REG_X0,0)
            start=self.image.address(name,'/apps/gpu-render/main.c' if name=='client' else None)
        self.stop=None;self.cpu.emu_start(start,0,count=1000000)
        assert self.stop=='yield',(name,self.stop,hex(self.cpu.reg_read(UC_ARM64_REG_PC)))
        assert not self.field('fatal');self.contexts[name]=self.cpu.context_save()
    def pump(self):
        self.now+=100;self.step('gpu_application_usb');self.step('client')
    def render(self):
        width,height,stride,count,index_count=self.shape
        vertices=list(struct.iter_unpack('<6f',self.draft[:count*24]))
        indices=struct.unpack('<'+'H'*index_count,self.draft[count*24:count*24+index_count*2])
        assert count==4 and index_count==6 and all(i<count for i in indices)
        texels=self.draft[count*24+index_count*2:];assert len(texels)==stride*height*4
        # Model only the GPU result for the two supplied indexed full-screen
        # quads. This does not execute or qualify USC/rasterization.
        flip=vertices[0][4]==1
        rows=[]
        for y in range(height):
            row=bytearray()
            for x in range(1920):
                tx=x*width//1920;tx=width-1-tx if flip else tx
                row.extend(texels[(y*stride+tx)*4:(y*stride+tx+1)*4])
            rows.append(bytes(row))
        self.snapshot=b''.join(rows[y*height//1080] for y in range(1080))
    def hook(self,cpu,address,size,unused):
        if address==0x43fff000:self.stop='returned';cpu.emu_stop();return
        name=self.hooks.get(address)
        if name is None:return
        args=[cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
        if name=='vTaskDelay':self.ret();self.stop='yield';cpu.emu_stop();return
        if name=='tud_speed_get':self.ret(self.speed);return
        if name=='usbd_edpt_open':self.ret(1);return
        if name=='usbd_edpt_claim':
            if args[1]&0x80:
                if self.claimed:self.ret(0);return
                self.claimed=True
            self.ret(1);return
        if name=='usbd_edpt_release':self.claimed=False;self.ret(1);return
        if name=='usbd_edpt_xfer':
            if args[1]&0x80:
                assert self.pending is None and self.claimed
                self.pending=bytes(cpu.mem_read(args[2],args[3])) if args[3] else b''
            self.ret(1);return
        if name=='pvr_gpu_service_send':
            op=int.from_bytes(cpu.mem_read(args[1],4),'little');self.operations.append(op)
            assert op in (0,1,2)
            if op==2:assert self.borrow is None;self.closed=True
            self.ticket+=1;self.write(args[3],self.ticket,8);self.reply={};self.ret(1);return
        if name=='pvr_gpu_service_texture':
            assert args[1]==123 and not self.closed
            generation,op,previous,offset,n,*fields=struct.unpack('<Q4I6I',cpu.mem_read(args[2],48))
            shape,state=fields[:5],fields[5]
            assert previous==self.frame;self.ticket+=1;self.write(args[6],self.ticket,8)
            self.reply={};self.operations.append(10)
            if op==0:
                assert not self.borrow;self.shape=shape;self.state=state;self.states.append(state);self.draft=bytearray();self.generation+=1
            elif op==1:
                assert generation==self.generation and offset==len(self.draft) and n<=65536
                self.draft.extend(cpu.mem_read(args[3],n))
            elif op==2:
                assert not self.borrow and generation==self.generation
                self.inputs.append(bytes(self.draft));self.render();self.frame+=1
            elif op==3:
                assert self.borrow is None and n and n<=65536 and offset+n<=len(self.snapshot)
                cpu.mem_write(0x48000000,self.snapshot[offset:offset+n]);self.borrow=(self.ticket,offset,n)
            else:
                assert op==4 and self.borrow and generation==self.borrow[0] and offset==self.borrow[1]
                self.borrow=None
            self.ret(1);return
        if name=='pvr_gpu_service_texture_output':
            assert self.borrow and args[1]==self.borrow[0]
            self.write(args[2],0x48000000,8);self.write(args[3],self.borrow[2]);self.ret(1);return
        if name=='pvr_gpu_service_collect':
            fields=self.image.layouts['pvr_gpu_service_reply'];p=args[2];cpu.mem_write(p,bytes(self.reply_size))
            for k,v in dict(epoch=123,ticket=args[1],phase=5 if self.closed else 3,
                render_sequence=self.frame,render_phase=9 if self.frame else 2,
                render_checked=len(self.snapshot)+128 if self.frame else 0,
                texture_generation=self.generation,texture_received=len(self.draft)).items():
                self.write(p+fields[k],v,8 if k in ('epoch','ticket','texture_generation') else 4)
            self.ret(1);return
        if name=='tud_vendor_n_available':self.ret(min(19,len(self.incoming)));return
        if name=='tud_vendor_n_read':
            n=min(args[2],7,len(self.incoming));cpu.mem_write(args[1],bytes(self.incoming[:n]));del self.incoming[:n];self.ret(n);return
        super().hook(cpu,address,size,unused)
    def write_request(self,data,remaining):
        assert not self.incoming;self.incoming+=data;return len(data)
    def read(self,n,remaining):
        self.reads+=1;result=bytearray();packet=512 if self.speed==2 else 64
        for _ in range(100000):
            if self.pending is None:self.pump();continue
            data=self.pending;assert len(data)<=n-len(result),(len(data),n,len(result))
            self.pending=None;self.claimed=False;result.extend(data);self.packets+=1
            if not data:self.zlps+=1
            assert self.call('vendord_xfer_cb',0,0x81,0,len(data))==1
            if len(result)==n or len(data)%packet or not data:return bytes(result)
            assert self.now<60000000
        raise AssertionError('Native transport made no progress')

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--image',type=Path,required=True)
    ap.add_argument('--profile',type=int,choices=(2,3),default=2)
    ap.add_argument('--output',type=Path,required=True);ap.add_argument('--speed',choices=('high','full'),required=True)
    a=ap.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    image=layout.Image(a.image);image.path=a.image;m=Machine(image,2 if a.speed=='high' else 1,a.profile)
    host.time=types.SimpleNamespace(monotonic=lambda:m.now/1000000)
    inputs=[]
    for scene in range(2):
        width,height=(16,8) if scene==0 else (32,16)
        vertices=[[-1,-1,0,1,scene,0],[1,-1,0,1,1-scene,0],[-1,1,0,1,scene,1],[1,1,0,1,1-scene,1]]
        pixels=bytes(c for y in range(height) for x in range(width) for c in ((x*17+scene*31)&255,y*13,scene*73,255))
        if a.profile==3:
            for vertex in vertices:vertex[2]=0.25+scene*0.25
        inputs.append(host.input_bytes(vertices,[0,1,3,0,3,2],width,height,pixels,
            profile=a.profile,state=(5 if scene==0 else 10) if a.profile==3 else 0))
    outputs=host.frames(m.write_request,m.read,123,inputs,timeout=50,profile=a.profile)
    for _ in range(20):
        if not m.field('runtime_state'):break
        m.pump()
    assert len(outputs)==2 and outputs[0]['pixels']!=outputs[1]['pixels'] and m.closed and m.borrow is None
    assert m.states==([5,10] if a.profile==3 else [0,0])
    assert m.inputs==[item[1] for item in inputs] and not m.field('fatal') and m.field('runtime_closed')==1
    assert m.zlps and m.pending is None
    record=dict(passed=True,physical_access=False,profile=a.profile,states=m.states,speed=a.speed,frames=2,width=1920,height=1080,
        image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),zlps=m.zlps,endpoint_completions=m.packets,
        input_sha256=[hashlib.sha256(x).hexdigest() for x in m.inputs],
        output_sha256=[hashlib.sha256(x['pixels']).hexdigest() for x in outputs],
        scope='Actual linked native client/parser, pinned TinyUSB FIFO/write/xfer/ZLP path and host consumer; GPU service replies, framebuffer and USB endpoint hardware modeled. No startup replay or physical rasterization.')
    (a.output/'result.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record,indent=2))
if __name__=='__main__':main()
