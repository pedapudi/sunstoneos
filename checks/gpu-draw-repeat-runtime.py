#!/usr/bin/env python3
"""Exercise the linked native draw caller, USB mailbox and host consumer offline."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from unicorn import UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PC
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
transport=module('draw_transport',ROOT/'checks/gpu-usb-ordering.py')
host=module('draw_host',ROOT/'apps/gpu-render/draw.py')
class Machine(transport.Machine):
    def __init__(self,image):
        super().__init__(image)
        self.contexts={};self.incoming=bytearray();self.sent=bytearray();self.wire=[]
        self.operation=0;self.first=0;self.closed=False;self.draw=None;self.operations=[];self.frame=0;self.copied=[];self.cached=None
        for name in ('pvr_gpu_service_collect','pvr_gpu_service_draw_next','pvr_gpu_service_render_read_sequence'):
            self.hooks[image.address(name)]=name
    def expected(self):
        # Software framebuffer supplied at the modeled GPU boundary. This is
        # an ownership/framing oracle, not a USC/tiler execution model.
        if self.cached is not None:return self.cached
        result=[0xffc08020]*4096
        rows=list(struct.iter_unpack('<8f',self.draw))
        for at in range(0,len(rows),3):
            a,b,c=rows[at:at+3]
            def edge(p,q,x,y):return (q[0]-p[0])*(y-p[1])-(q[1]-p[1])*(x-p[0])
            area=edge(a,b,c[0],c[1])
            if not area:continue
            for y in range(64):
                for x in range(64):
                    px,py=(x+.5)/32-1,(y+.5)/32-1
                    u,v,w=edge(b,c,px,py)/area,edge(c,a,px,py)/area,edge(a,b,px,py)/area
                    if min(u,v,w)<0:continue
                    color=[min(255,max(0,round(255*(u*a[i]+v*b[i]+w*c[i])))) for i in range(4,8)]
                    result[y*64+x]=sum(n<<(8*i) for i,n in enumerate(color))
        self.cached=struct.pack('<4096I',*result);return self.cached
    def hook(self,cpu,address,size,unused):
        name=self.hooks.get(address)
        if name is None:
            super().hook(cpu,address,size,unused);return
        args=[cpu.reg_read(UC_ARM64_REG_X0+n) for n in range(8)]
        if name=='vTaskDelay':self.ret();self.stop='yield';cpu.emu_stop();return
        if name=='pvr_gpu_service_send':
            self.operation=int.from_bytes(cpu.mem_read(args[1],4),'little');self.operations.append(self.operation)
            if self.operation==2:self.closed=True
            self.write(args[3],1,8);self.ret(1);return
        if name=='pvr_gpu_service_draw_next':
            assert args[1]==77 and args[2]==self.frame and 96<=args[4]<=4032 and args[5]*32==args[4] and args[5]%3==0
            assert not self.closed
            self.draw=bytes(cpu.mem_read(args[3],args[4]));self.copied.append(self.draw);self.cached=None
            self.frame+=1;self.operation=9;self.operations.append(9)
            ticket=int.from_bytes(cpu.mem_read(cpu.reg_read(UC_ARM64_REG_SP),8),'little')
            self.write(ticket,1,8);self.ret(1);return
        if name=='pvr_gpu_service_render_read_sequence':
            self.operation=5;self.first=args[2];assert self.draw is not None and args[1]==self.frame and not self.first%64
            self.write(args[4],1,8);self.ret(1);return
        if name=='pvr_gpu_service_collect':
            f=self.image.layouts['pvr_gpu_service_reply'];p=args[2]
            cpu.mem_write(p,bytes(f['render_pixels']+256));self.write(p+f['epoch'],77,8);self.write(p+f['render_sequence'],self.frame)
            self.write(p+f['phase'],5 if self.closed else 3)
            self.write(p+f['render_phase'],9 if self.draw is not None else 2)
            self.write(p+f['render_checked'],16512 if self.draw is not None else 0)
            if self.operation==5:
                self.write(p+f['render_first_pixel'],self.first);self.write(p+f['render_pixel_count'],64)
                cpu.mem_write(p+f['render_pixels'],self.expected()[self.first*4:(self.first+64)*4])
            self.ret(1);return
        if name=='tud_vendor_n_available':self.ret(min(19,len(self.incoming)));return
        if name=='tud_vendor_n_read':
            n=min(args[2],7,len(self.incoming));cpu.mem_write(args[1],bytes(self.incoming[:n]));del self.incoming[:n];self.ret(n);return
        if name=='tud_vendor_n_write':
            n=min(args[2],23);self.sent+=bytes(cpu.mem_read(args[1],n));self.wire.append(n);self.ret(n);return
        if name=='tud_vendor_n_write_flush':self.ret();return
        super().hook(cpu,address,size,unused)
    def step(self,name):
        if name in self.contexts:self.cpu.context_restore(self.contexts[name]);start=self.cpu.reg_read(UC_ARM64_REG_PC)
        else:
            self.cpu.reg_write(UC_ARM64_REG_SP,0x43ffe000 if name=='gpu_application_usb' else 0x43ffd000)
            self.cpu.reg_write(UC_ARM64_REG_LR,0x43fff000);self.cpu.reg_write(UC_ARM64_REG_X0,0)
            start=self.image.address(name,'/apps/gpu-render/main.c' if name=='client' else None)
        self.stop=None;self.cpu.emu_start(start,0,count=300000)
        assert self.stop,(name,hex(self.cpu.reg_read(UC_ARM64_REG_PC)))
        self.contexts[name]=self.cpu.context_save()
    def exchange(self,request,remaining):
        assert not self.incoming and remaining>0
        self.incoming+=request;self.sent.clear()
        for _ in range(4000):
            self.step('gpu_application_usb')
            if len(self.sent)>=40 and len(self.sent)==40+4*struct.unpack_from('<I',self.sent,16)[0]:
                data=bytes(self.sent);self.sent.clear();self.run('tud_vendor_tx_cb',0,len(data));return data
            self.step('client')
        raise AssertionError('Native draw response did not complete')
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
    a=ap.parse_args();a.output.mkdir(parents=True,exist_ok=False);image=transport.layout.Image(a.image)
    scenes=[host.vertices([(-.8,-.8,0,1,1,0,0,1),(.8,-.8,0,1,1,0,0,1),(0,.8,0,1,1,0,0,1)]),
        host.vertices([(-.5,-.5,0,1,0,1,0,1),(.5,-.5,0,1,0,0,1,1),(.5,.5,0,1,1,0,0,1),
                       (-.5,-.5,0,1,0,1,0,1),(.5,.5,0,1,1,0,0,1),(-.5,.5,0,1,1,1,1,1)])]
    inputs=[scenes[i&1] for i in range(12)]
    m=Machine(image)
    # Use a logical transport clock: interpreter wall time is not device time.
    # The production deadline checks still execute, with positive bounded time.
    class Clock:
        now=1.0
        def monotonic(self):return self.now
        def sleep(self,seconds):self.now+=seconds
    clock=Clock();host.time=clock;transactions=0
    def exchange(request,remaining):
        nonlocal transactions
        transactions+=1
        assert transactions<1500,(m.frame,m.field('result'),m.field('runtime_state'),m.operations[-5:])
        clock.now+=.00001;m.now=int(clock.now*1000000)
        raw=m.exchange(request,remaining)
        if struct.unpack_from('<I',request,24)[0]==4032 and struct.unpack_from('<I',raw,8)[0]==1:
            (a.output/'progress.json').write_text(json.dumps(dict(completed_frame=m.frame,transactions=transactions))+'\n')
        return raw
    outputs=host.frames(exchange,123,inputs,timeout=30)
    assert m.copied==inputs and len(outputs)==12 and m.closed
    assert m.field('runtime_closed')==1 and not m.field('runtime_state') and not m.field('fatal')
    assert m.operations.count(1)==1 and m.operations.count(9)==12 and m.operations.count(2)==1
    for i,(scene,output) in enumerate(zip(inputs,outputs)):
        m.draw=scene;m.cached=None;assert output==m.expected()
        (a.output/f'frame-{i+1}.rgba').write_bytes(output)
    assert outputs[0]!=outputs[1] and outputs[0]==outputs[2]
    raw=m.exchange(host.frame(host.READ,11,123),1)
    assert struct.unpack_from('<I',raw,8)[0]==4 and len(raw)==40
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,
        image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),frames=12,
        input_sha256=[hashlib.sha256(x).hexdigest() for x in inputs],
        output_sha256=[hashlib.sha256(x).hexdigest() for x in outputs],operations=m.operations,
        split_receive_bytes=7,split_transmit_bytes=23,
        scope='Actual linked native client, USB framing and host repeated-frame consumer; service replies, GPU framebuffer and USB completion modeled. No physical rasterization claim.'),indent=2)+'\n')
    print('PASS native copied twelve-frame caller, changed scenes, stale READ and healthy CLOSE')
if __name__=='__main__':main()
