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
        self.operation=0;self.first=0;self.closed=False;self.draw=None;self.operations=[]
        for name in ('pvr_gpu_service_collect','pvr_gpu_service_draw','pvr_gpu_service_render_read'):
            self.hooks[image.address(name)]=name
    def expected(self):
        # Software framebuffer supplied at the modeled GPU boundary. This is
        # an ownership/framing oracle, not a USC/tiler execution model.
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
        return struct.pack('<4096I',*result)
    def hook(self,cpu,address,size,unused):
        name=self.hooks.get(address);args=[cpu.reg_read(UC_ARM64_REG_X0+n) for n in range(8)]
        if name=='vTaskDelay':self.ret();self.stop='yield';cpu.emu_stop();return
        if name=='pvr_gpu_service_send':
            self.operation=int.from_bytes(cpu.mem_read(args[1],4),'little');self.operations.append(self.operation)
            if self.operation==2:self.closed=True
            self.write(args[3],1,8);self.ret(1);return
        if name=='pvr_gpu_service_draw':
            assert args[1]==77 and 96<=args[3]<=4032 and args[4]*32==args[3] and args[4]%3==0
            assert self.draw is None and not self.closed
            self.draw=bytes(cpu.mem_read(args[2],args[3]));self.operation=9;self.operations.append(9)
            self.write(args[7],1,8);self.ret(1);return
        if name=='pvr_gpu_service_render_read':
            self.operation=5;self.first=args[1];assert self.draw is not None and not self.first%64
            self.write(args[3],1,8);self.ret(1);return
        if name=='pvr_gpu_service_collect':
            f=self.image.layouts['pvr_gpu_service_reply'];p=args[2]
            cpu.mem_write(p,bytes(f['render_pixels']+256));self.write(p+f['epoch'],77,8)
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
class RetryMachine(Machine):
    def __init__(self,image):
        super().__init__(image);self.refused=False;self.released=False;self.pause_release=False
        self.cpu.hook_add(UC_HOOK_MEM_WRITE,self.store)
    def store(self,cpu,access,address,size,value,unused):
        if self.refused and not self.released and address==self.app+self.fields['runtime_state'] and value==0:
            assert self.field('result')==4,'Mailbox became available before prior refusal publication'
            self.released=True;self.pause_release=True
    def hook(self,cpu,address,size,unused):
        if getattr(self,'pause_release',False):
            self.pause_release=False;self.stop='yield';cpu.emu_stop();return
        name=self.hooks.get(address)
        if name=='pvr_gpu_service_draw' and not self.refused:
            self.refused=True;self.ret(0);return
        super().hook(cpu,address,size,unused)
def retry_case(image,scene):
    m=RetryMachine(image)
    assert host.decode(m.exchange(host.frame(host.READ,0,123),1),123,0)[:2]==(0,0)
    assert host.decode(m.exchange(host.frame(host.SUBMIT,0,123,data=scene),1),123,0)[:2]==(0,0)
    for _ in range(20):
        state,seq,_=host.decode(m.exchange(host.frame(host.READ,0,123),1),123,0)
        if state==4:break
    assert m.released and state==4 and seq==0
    assert host.decode(m.exchange(host.frame(host.SUBMIT,0,123,data=scene),1),123,0)[:2]==(0,0)
    output=bytearray()
    for first in range(0,4096,64):
        for _ in range(30):
            state,seq,page=host.decode(m.exchange(host.frame(host.READ,1,123,first),1),123,first)
            if state==1 and seq==1:output+=page;break
            assert state==0
        else:raise AssertionError('Retried draw did not finish')
    assert output==m.expected()
    assert host.decode(m.exchange(host.frame(host.CLOSE,1,123),1),123,0)[:2]==(0,1)
    for _ in range(30):
        state,seq,_=host.decode(m.exchange(host.frame(host.READ,1,123),1),123,0)
        if state==3:break
        assert state==0
    assert state==3 and seq==1 and m.closed
    return dict(passed=True,refusal_published_before_mailbox_release=True,output_sha256=hashlib.sha256(output).hexdigest())
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--retry-only',action='store_true',help='Run only the changed refusal/retry publication case')
    a=ap.parse_args();a.output.mkdir(parents=True,exist_ok=False);image=transport.layout.Image(a.image)
    scenes=[host.vertices([(-.8,-.8,0,1,1,0,0,1),(.8,-.8,0,1,1,0,0,1),(0,.8,0,1,1,0,0,1)]),
        host.vertices([(-.5,-.5,0,1,0,1,0,1),(.5,-.5,0,1,0,0,1,1),(.5,.5,0,1,1,0,0,1),
                       (-.5,-.5,0,1,0,1,0,1),(.5,.5,0,1,1,0,0,1),(-.5,.5,0,1,1,1,1,1)])]
    if a.retry_only:
        result=retry_case(image,scenes[0]);result.update(physical_access=False,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest())
        (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        print('PASS native refusal publication, interleaved retry and healthy CLOSE');return
    results=[]
    for i,scene in enumerate(scenes):
        m=Machine(image);output=host.sequence(m.exchange,123,scene)
        assert output==m.expected() and m.draw==scene and m.closed
        assert m.field('runtime_closed')==1 and not m.field('runtime_state') and not m.field('fatal')
        assert m.operations.count(1)==1 and m.operations.count(9)==1 and m.operations.count(2)==1
        raw=m.exchange(host.frame(host.SUBMIT,0,123,data=scene),1)
        assert struct.unpack_from('<I',raw,8)[0]==4 and len(raw)==40
        (a.output/f'scene-{i}.bin').write_bytes(scene);(a.output/f'framebuffer-{i}.rgba').write_bytes(output)
        results.append(dict(vertices=len(scene)//32,output_sha256=hashlib.sha256(output).hexdigest(),operations=m.operations))
    assert results[0]['output_sha256']!=results[1]['output_sha256']
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,
        image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),scenes=results,
        split_receive_bytes=7,split_transmit_bytes=23,
        scope='Actual linked native client, USB framing and host consumer; service replies, GPU framebuffer and USB completion modeled. No physical rasterization claim.'),indent=2)+'\n')
    print('PASS native copied triangle caller, two scene inputs, copied framebuffers and healthy CLOSE')
if __name__=='__main__':main()
