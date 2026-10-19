#!/usr/bin/env python3
"""Run linked GPU client and USB framing with modeled service/device execution."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result
transport=module('gpu_runtime_transport',ROOT/'checks/gpu-usb-ordering.py')
host=module('gpu_runtime_host',ROOT/'apps/gpu-program/runtime.py')
class Machine(transport.Machine):
    def __init__(self,image,package,kind='affine'):
        super().__init__(image)
        self.kind=kind;self.package=package;self.total=struct.unpack_from('<I',package,32)[0]//4
        self.input_bytes=struct.unpack_from('<I',package,28)[0]
        self.input=package[4192:4192+self.input_bytes]
        self.sequence=0;self.operation=0;self.first=0;self.closed=False;self.operations=[]
        self.contexts={};self.incoming=bytearray();self.sent=bytearray();self.wire=[];self.copied=[]
        self.cpu.mem_write(0x43010000,package)
        for name in ('gpu_application_package','pvr_gpu_service_collect','pvr_gpu_service_compute_input',
                     'pvr_gpu_service_compute_read','pvr_gpu_service_compute_read_sequence'):
            self.hooks[image.address(name)]=name
    def expected(self):
        values=struct.unpack('<'+'I'*(len(self.input)//4),self.input)
        count,factor,bias=values[:3];data=values[3:]
        if self.kind=='transpose':result=[data[y*factor+x] for x in range(factor) for y in range(bias)]
        elif self.kind=='neighbor':result=[sum(data[y*factor+xx] for xx in range(max(0,x-1),min(factor,x+2)))&0xffffffff for y in range(bias) for x in range(factor)]
        else:result=[(x*factor+bias)&0xffffffff for x in data]
        return struct.pack('<'+'I'*count,*result)
    def hook(self,cpu,address,size,unused):
        name=self.hooks.get(address)
        args=[cpu.reg_read(UC_ARM64_REG_X0+n) for n in range(8)]
        if name=='vTaskDelay':
            self.ret();self.stop='yield';cpu.emu_stop();return
        if name=='gpu_application_package':self.ret(0x43010000);return
        if name=='pvr_gpu_service_send':
            self.operation=int.from_bytes(cpu.mem_read(args[1],4),'little');self.operations.append(self.operation)
            if self.operation==3:self.sequence+=1
            if self.operation==2:self.closed=True
            self.write(args[3],1,8);self.ret(1);return
        if name in ('pvr_gpu_service_compute_read','pvr_gpu_service_compute_read_sequence'):
            exact=name.endswith('_sequence');self.operation=6
            if exact:assert args[1]==self.sequence
            self.first=args[2] if exact else args[1]
            self.write(args[4] if exact else args[3],1,8);self.ret(1);return
        if name=='pvr_gpu_service_compute_input':
            assert args[1]==self.sequence==1 and args[3]==self.input_bytes
            self.input=bytes(cpu.mem_read(args[2],args[3]));self.copied.append(self.input)
            self.sequence+=1;self.operation=8;self.operations.append(8)
            self.write(args[6],1,8);self.ret(1);return
        if name=='pvr_gpu_service_collect':
            fields=self.image.layouts['pvr_gpu_service_reply'];p=args[2]
            cpu.mem_write(p,bytes(fields['render_pixels']+256))
            self.write(p+fields['phase'],5 if self.closed else 3)
            self.write(p+fields['compute_phase'],7 if self.sequence else 4)
            self.write(p+fields['compute_sequence'],self.sequence)
            if self.operation==6:
                count=min(32,self.total-self.first)
                self.write(p+fields['compute_first_word'],self.first)
                self.write(p+fields['compute_total_words'],self.total)
                self.write(p+fields['compute_word_count'],count)
                cpu.mem_write(p+fields['compute_words'],self.expected()[4*self.first:4*(self.first+count)])
            self.ret(1);return
        if name=='tud_vendor_n_available':self.ret(min(19,len(self.incoming)));return
        if name=='tud_vendor_n_read':
            n=min(args[2],7,len(self.incoming));cpu.mem_write(args[1],bytes(self.incoming[:n]));del self.incoming[:n];self.ret(n);return
        if name=='tud_vendor_n_write':
            n=min(args[2],23);data=bytes(cpu.mem_read(args[1],n));self.sent+=data;self.wire.append(n);self.ret(n);return
        if name=='tud_vendor_n_write_flush':self.ret();return
        super().hook(cpu,address,size,unused)
    def step(self,name):
        if name in self.contexts:self.cpu.context_restore(self.contexts[name]);start=self.cpu.reg_read(UC_ARM64_REG_PC)
        else:
            self.cpu.reg_write(UC_ARM64_REG_SP,0x43ffe000 if name=='gpu_application_usb' else 0x43ffd000)
            self.cpu.reg_write(UC_ARM64_REG_LR,0x43fff000);self.cpu.reg_write(UC_ARM64_REG_X0,0)
            start=self.image.address(name,'/apps/gpu-render/main.c' if name=='client' else None)
        self.stop=None;self.cpu.emu_start(start,0,count=200000)
        assert self.stop,(name,hex(self.cpu.reg_read(UC_ARM64_REG_PC)))
        self.contexts[name]=self.cpu.context_save()
    def exchange(self,request,remaining):
        assert not self.incoming and remaining>0
        self.incoming+=request;self.sent.clear()
        for _ in range(3000):
            self.step('gpu_application_usb')
            if len(self.sent)>=40 and len(self.sent)==40+4*struct.unpack_from('<I',self.sent,16)[0]:
                data=bytes(self.sent);self.sent.clear()
                # Actual TX completion publishes usb_observed only after all bytes.
                self.run('tud_vendor_tx_cb',0,len(data))
                assert len(data)>=40 and len(data)==40+4*struct.unpack_from('<I',data,16)[0]
                return data
            self.step('client')
        raise AssertionError('Native runtime response did not complete')
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','package','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--kind',choices=('affine','transpose','neighbor'),default='affine')
    a=parser.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    package=a.package.read_bytes();image=transport.layout.Image(a.image);m=Machine(image,package,a.kind)
    count=struct.unpack_from('<I',package,36)[0]
    shape=struct.unpack_from('<2I',m.input,4) if a.kind!='affine' else (7,19)
    changed=struct.pack('<'+'I'*(count+3),count,*shape,*[1000+11*i for i in range(count)])
    reference=m.expected()
    before,after=host.sequence(m.exchange,123,count,changed)
    assert before==reference and after==m.expected()
    assert before!=after and m.copied==[changed] and m.closed
    assert m.field('runtime_closed')==1 and not m.field('runtime_state') and not m.field('fatal')
    assert m.operations.count(1)==1 and m.operations.count(3)==1 and m.operations.count(8)==1 and m.operations.count(2)==1
    # READ of revoked sequence returns no copied bytes, while last output stays owned.
    raw=m.exchange(host.frame(host.READ,1,123),1)
    assert struct.unpack_from('<I',raw,8)[0]==4 and len(raw)==40
    (a.output/'before.bin').write_bytes(before);(a.output/'after.bin').write_bytes(after)
    result=dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),
        package_sha256=hashlib.sha256(package).hexdigest(),input_sha256=hashlib.sha256(changed).hexdigest(),
        kernel=a.kind,count=count,sequences=[1,2],operations=m.operations,split_receive_bytes=7,split_transmit_bytes=23,
        scope='Actual linked native GPU owner task, mailbox, USB parser/copied response and host consumer. FreeRTOS service replies, GPU arithmetic and USB transport completion modeled; separate native FreeRTOS service case covers actual staging/worker. No physical GPU claim.')
    (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS native two-input host sequence and healthy CLOSE')
if __name__=='__main__':main()
