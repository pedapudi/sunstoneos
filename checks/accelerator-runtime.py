#!/usr/bin/env python3
"""Linked retained clients and shared transport with modeled service/device I/O.

No startup, IRQ routing, architecture admission or hardware execution occurs.
GPU arithmetic and saved NPU SDK output are injected at service completion.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import time
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
base=module('accelerator_npu_fixture',ROOT/'checks/npu-runtime.py')
large=module('accelerator_image',ROOT/'checks/npu-large-native.py')
host=module('accelerator_host',ROOT/'apps/accelerators/host.py')

class Machine(base.Machine):
    def __init__(self,image,package,oracles):
        super().__init__(image)
        self.package=package;self.oracles=oracles;self.g=image.address('accelerator_gpu')
        self.gl=image.layouts['accelerator_gpu'];self.gsequence=0;self.gop=0;self.gfirst=0
        self.gclosed=False;self.gcontext=None;self.gsuspended=False;self.ginputs=[];self.goperations=[]
        self.ginput=package[4192:4192+struct.unpack_from('<I',package,28)[0]]
        self.u.mem_write(0x43010000,package);self.max_tx=512;self.shared_failed=False
        for name in ('a733_smp_counter','accelerator_shared_live','accelerator_gpu_package',
                     'pvr_gpu_service_send','pvr_gpu_service_collect',
                     'pvr_gpu_service_compute_input','pvr_gpu_service_compute_read_sequence',
                     'a733_observe_digest'):
            self.hooks[image.address(name)]=name
        self.active='call';self.client_kind=None;self.response_queued=bytearray()
        handle,=self.u._callbacks;self.u.hook_del(handle)
        interesting=set(self.hooks)|{0x43fff000}
        for address,data in image.parts:
            for offset in range(0,len(data)-3,4):
                word=int.from_bytes(data[offset:offset+4],'little')
                if word&0xffffffe0 in (0xd53be000,0xd53be020):interesting.add(address+offset)
        for address in interesting:self.u.hook_add(UC_HOOK_CODE,self.code,begin=address,end=address)
    def gf(self,name):return self.get(self.g+self.gl[name],4)
    def expected_gpu(self):
        words=struct.unpack('<'+'I'*(len(self.ginput)//4),self.ginput)
        count,factor,bias=words[:3]
        return struct.pack('<'+'I'*count,*[((v*factor+bias)&0xffffffff) for v in words[3:]])
    def code(self,cpu,at,size,unused):
        name=self.hooks.get(at)
        a=[self.u.reg_read(UC_ARM64_REG_X0+i) for i in range(8)] if name else []
        if name=='a733_smp_counter':self.ret(self.ticks);return
        if name=='accelerator_shared_live':self.ret(not self.shared_failed);return
        if name=='accelerator_gpu_package':self.ret(0x43010000);return
        if name=='a733_observe_digest':self.u.mem_write(a[2],hashlib.sha256(self.data(a[0],a[1])).digest());self.ret();return
        if name=='npu_convolution_output':
            value=self.oracles[self.inputs[-1]];assert a[2]==len(value)
            self.u.mem_write(a[1],value);self.ret(1);return
        if name=='pvr_gpu_service_send':
            self.gop=self.get(a[1],4);self.goperations.append(self.gop)
            if self.gop==3:self.gsequence+=1
            if self.gop==2:self.gclosed=True
            self.put(a[3],1);self.ret(1);return
        if name=='pvr_gpu_service_compute_input':
            assert a[1]==self.gsequence==1 and a[3]==len(self.ginput)
            self.ginput=self.data(a[2],a[3]);self.ginputs.append(self.ginput)
            self.gsequence+=1;self.gop=8;self.goperations.append(8);self.put(a[6],1);self.ret(1);return
        if name=='pvr_gpu_service_compute_read_sequence':
            assert a[1]==self.gsequence;self.gfirst=a[2];self.gop=6
            self.put(a[4],1);self.ret(1);return
        if name=='pvr_gpu_service_collect':
            f=self.image.layouts['pvr_gpu_service_reply'];p=a[2]
            self.u.mem_write(p,bytes(f['render_pixels']+256))
            self.put(p+f['phase'],5 if self.gclosed else 3,4)
            self.put(p+f['compute_phase'],7 if self.gsequence else 4,4)
            self.put(p+f['compute_sequence'],self.gsequence,4)
            if self.gop==6:
                total=len(self.expected_gpu())//4;n=min(32,total-self.gfirst)
                for key,value in [('compute_first_word',self.gfirst),('compute_total_words',total),('compute_word_count',n)]:self.put(p+f[key],value,4)
                self.u.mem_write(p+f['compute_words'],self.expected_gpu()[4*self.gfirst:4*(self.gfirst+n)])
            self.ret(1);return
        if name=='vTaskDelay' and self.active in ('gpu','npu'):
            self.ret();self.u.emu_stop();return
        if name=='vTaskSuspend' and self.active=='gpu':
            self.gsuspended=True;self.ret();self.u.emu_stop();return
        super().code(cpu,at,size,unused)
    def gpu_client(self):
        if self.gsuspended:return
        self.active='gpu'
        if self.gcontext:self.u.context_restore(self.gcontext)
        else:
            self.u.reg_write(UC_ARM64_REG_SP,0x43fe0000);self.u.reg_write(UC_ARM64_REG_LR,0x43fff000)
            self.u.reg_write(UC_ARM64_REG_PC,self.image.address('client',unit='/apps/accelerators/gpu.c'))
            self.u.reg_write(UC_ARM64_REG_X0,self.g)
        self.u.emu_start(self.u.reg_read(UC_ARM64_REG_PC),0,count=3000000)
        self.gcontext=self.u.context_save();self.active='call'
    def clients(self):
        self.active='npu';self.client();self.active='call';self.gpu_client()
    def usb(self,ack=True):
        before=len(self.tx);self.active='usb';self.call('usb_task');self.active='call'
        if ack:
            for raw in self.tx[before:]:
                self.response_queued.extend(raw)
                self.call('tud_vendor_tx_cb',0,len(raw))
                # Model TinyUSB's empty-FIFO terminal ZLP at full speed.
                frame=self.response_queued
                is_gpu=len(frame)>=4 and struct.unpack_from('<I',frame)[0]==0x50555047
                total=40+4*struct.unpack_from('<I',frame,16)[0] if is_gpu and len(frame)>=40 else 1040 if not is_gpu else None
                if total is not None and len(frame)==total:
                    if len(raw)%64==0:self.call('tud_vendor_tx_cb',0,0)
                    self.response_queued.clear()
    def exchange(self,packet,capacity,remaining):
        assert not self.incoming and remaining>0
        self.incoming.extend(packet);before=len(self.tx)
        gpu=struct.unpack_from('<I',packet)[0]==host.gpu.MAGIC
        for _ in range(3000):
            self.usb();self.clients();raw=b''.join(self.tx[before:])
            expected=40+4*struct.unpack_from('<I',raw,16)[0] if gpu and len(raw)>=40 else 1040 if not gpu else None
            if expected is not None and len(raw)==expected:
                assert len(raw)<=capacity;return raw
        raise AssertionError('Combined native response bound')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','package','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--input',type=Path,action='append',required=True)
    parser.add_argument('--reference',type=Path,action='append',required=True)
    parser.add_argument('--model',type=Path,required=True)
    a=parser.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    inputs=[p.read_bytes() for p in a.input];outputs=[p.read_bytes() for p in a.reference]
    assert len(inputs)==len(outputs)==2 and inputs[0]!=inputs[1]
    assert all((ref.parent/'input_0.dat').read_bytes()==data for ref,data in zip(a.reference,inputs))
    image=large.Image(a.image);package=a.package.read_bytes();m=Machine(image,package,dict(zip(inputs,outputs)))
    count=struct.unpack_from('<I',package,36)[0];before=m.expected_gpu()
    changed=struct.pack('<'+'I'*(count+3),count,7,19,*[1000+11*i for i in range(count)])
    result=host.sequence(m.exchange,m.prepared,count,changed,inputs,json.loads(a.model.read_text()),deadline=time.monotonic()+20)
    assert result['gpu_outputs']==[before,m.expected_gpu()] and result['npu_outputs']==outputs
    assert m.ginputs==[changed] and m.inputs==inputs and m.closed and m.gclosed
    assert m.gf('closed')==1 and m.gf('failed')==0 and m.state()==host.npu.CLOSED
    for name,values in [('gpu',result['gpu_outputs']),('npu',result['npu_outputs'])]:
        for i,value in enumerate(values,1):(a.output/f'{name}-{i}.bin').write_bytes(value)
    record=dict(passed=True,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),
        package_sha256=hashlib.sha256(package).hexdigest(),gpu_operations=m.goperations,npu_operations=m.operations,
        copied_gpu_outputs=2,copied_npu_outputs=2,healthy_close_both=True,physical_access=False,
        scope='Actual linked clients, common USB parser/copied responses, unchanged NPU runtime state and host interleave. Service results, GPU arithmetic, saved SDK inference, scheduler, USB completion and root/provider admission modeled. No startup/CPU/IRQ execution.')
    (a.output/'result.json').write_text(json.dumps(record,indent=2)+'\n')
    print('PASS combined linked retained clients and copied output sequence')
if __name__=='__main__':main()
