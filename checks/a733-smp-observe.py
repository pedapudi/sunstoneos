#!/usr/bin/env python3
"""Focused observer SVC and USB service checks on a preserved native ELF.

USB API stubs model fragmented FIFO transfers and task events. This is not a
MUSB electrical/controller model or a repeat of the eight-core scheduler test.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_MEM_WRITE
from unicorn.arm64_const import *
GPR=[UC_ARM64_REG_X0+i for i in range(29)]+[UC_ARM64_REG_X29,UC_ARM64_REG_LR]
MASK=(1<<64)-1
class Model:
    def __init__(self,image):
        self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.u.mem_map(0x41000000,0x1200000)
        with image.open('rb') as f:
            elf=ELFFile(f);self.sym={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
            for s in elf.iter_segments():
                if s['p_type']=='PT_LOAD':self.u.mem_write(s['p_vaddr'],s.data())
        self.dec=Cs(CS_ARCH_ARM64,CS_MODE_ARM);self.decoded={};self.clock=240000;self.clock_step=24
        self.mmio=False;self.writes=[]
        self.regs={'currentel':4,'tpidr_el1':0,'daif':0x3c0,'elr_el1':self.sym['a733_observe_svc_return'],
            'spsr_el1':0,'sp_el0':self.sym['task_stack']+16368,'esr_el1':(0x15<<26)|120}
        self.failed=None;self.ret=False;self.pending=b'';self.tx=bytearray();self.drain=False
        self.usb_calls=0;self.releases=0;self.acquires=0;self.stack=0x41fff000;self.stop=0x411ff000
        self.u.hook_add(UC_HOOK_CODE,self.instruction)
        self.u.hook_add(UC_HOOK_MEM_READ,self.memory);self.u.hook_add(UC_HOOK_MEM_WRITE,self.memory)
        self.write('a733_observe_identity',struct.pack('<5Q',5,7,9,24,72000000))
        self.write('pxCurrentTCBs',struct.pack('<Q',self.sym['task_state']))
    def write(self,s,data):self.u.mem_write(self.sym[s],data)
    def r(self,name):
        if name in ('xzr','wzr'):return 0
        return self.u.reg_read(GPR[int(name[1:])])&((1<<32)-1 if name[0]=='w' else MASK)
    def w(self,name,value):
        if name not in ('xzr','wzr'):self.u.reg_write(GPR[int(name[1:])],value)
    def return_(self,value=0):self.u.reg_write(GPR[0],value);self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
    def memory(self,u,access,address,size,value,unused):
        if self.mmio and address in (0x02003300,0x02003304,0x04100410,0x04100420):
            assert size==4
            if access==UC_MEM_WRITE:self.writes.append((address,value))
            return
        assert 0x41000000<=address and address+size<=0x42000000,('unexpected memory/secure monitor access',hex(address))
    def instruction(self,u,pc,size,unused):
        if pc==self.sym['a733_observe_failure']:
            self.failed=self.r('x0');u.emu_stop();return
        hooks={
            'tusb_rhport_init':lambda:1,'a733_usb_poll':lambda:0,
            'tud_mounted':lambda:1,'tud_vendor_n_available':lambda:len(self.pending),
            'tud_vendor_n_write_available':lambda:1024,'tud_vendor_n_write_flush':lambda:0,
            'ucPortGetCoreID':lambda:0,
        }
        for name,fn in hooks.items():
            if pc==self.sym[name]:
                if name in ('tusb_rhport_init','a733_usb_poll'):self.usb_calls+=1
                self.return_(fn());return
        if pc==self.sym['tud_task_ext']:
            assert self.r('x0')==0 and self.r('x1')==0
            if self.drain:
                self.drain=False;u.reg_write(GPR[0],0);u.reg_write(GPR[1],528)
                u.reg_write(UC_ARM64_REG_PC,self.sym['tud_vendor_tx_cb'])
            else:self.return_()
            return
        if pc==self.sym['tud_vendor_n_read']:
            count=min(self.r('x2'),len(self.pending));u.mem_write(self.r('x1'),self.pending[:count]);self.pending=self.pending[count:]
            self.return_(count);return
        if pc==self.sym['tud_vendor_n_write']:
            count=min(127,self.r('x2'));self.tx.extend(u.mem_read(self.r('x1'),count));self.return_(count);return
        if pc==self.sym['xTaskCreateStaticAffinitySet']:
            assert self.r('x4')==1 and self.r('x7')==1
            assert self.r('x5')==self.sym['task_stack'] and self.r('x6')==self.sym['task_state']
            self.return_(self.sym['task_state']);return
        if pc not in self.decoded:self.decoded[pc]=next(self.dec.disasm(bytes(u.mem_read(pc,4)),pc))
        ins=self.decoded[pc];op=ins.mnemonic;args=ins.op_str.replace(' ','').split(',')
        if op=='mrs':
            sys=args[1]
            if sys=='cntpct_el0':self.clock+=self.clock_step;value=self.clock
            else:value=self.regs[sys]
            self.w(args[0],value);u.reg_write(UC_ARM64_REG_PC,pc+4)
        elif op=='msr':
            assert args[0] in ('elr_el1','spsr_el1');self.regs[args[0]]=self.r(args[1]);u.reg_write(UC_ARM64_REG_PC,pc+4)
        elif op=='eret':self.ret=True;u.emu_stop()
        elif op in ('ldar','stlr'):
            address=self.r(args[1].strip('[]'))
            if address==self.sym['ready']:
                if op=='ldar':self.acquires+=1
                else:self.releases+=1
        elif op=='svc':raise AssertionError('unexpected nested SVC')
    def call(self,name,args=()):
        self.ret=False;self.failed=None;self.u.reg_write(UC_ARM64_REG_SP,self.stack)
        for i,arg in enumerate(args):self.u.reg_write(GPR[i],arg)
        self.u.reg_write(UC_ARM64_REG_LR,self.stop)
        self.u.emu_start(self.sym[name],self.stop,count=1000000)
        assert self.failed is not None or self.u.reg_read(UC_ARM64_REG_PC)==self.stop,('did not return',name,hex(self.u.reg_read(UC_ARM64_REG_PC)))
    def service(self,**overrides):
        self.ret=False;self.failed=None;self.regs.update(overrides)
        original=[0x1234000000000000+i*0x10001 for i in range(31)];original[30]=self.stop
        for reg,value in zip(GPR,original):self.u.reg_write(reg,value)
        for i in range(32):self.u.reg_write(UC_ARM64_REG_Q0+i,(i+1)*0x102030405060708090a0b0c0d0e0f)
        self.u.reg_write(UC_ARM64_REG_SP,self.stack)
        # Enter the actual lower-EL synchronous vector with a modeled SVC trap.
        self.u.emu_start(self.sym['a733_cached_el1_vectors']+0x400,self.stop,count=1000000)
        if self.failed is not None:return
        assert self.ret and self.u.reg_read(UC_ARM64_REG_SP)==self.stack
        assert [self.u.reg_read(r) for r in GPR]==original
        for i in range(32):assert self.u.reg_read(UC_ARM64_REG_Q0+i)==(i+1)*0x102030405060708090a0b0c0d0e0f
        assert self.regs['elr_el1']==self.sym['a733_observe_svc_return']
    def freeze(self):
        self.call('a733_observe_service_start')
        for c in range(8):
            self.u.mem_write(self.sym['a733_smp_core_results']+128*c,struct.pack('<5Q',32,c+1,1,0,32 if not c else 0))
        self.call('a733_observe_freeze');assert self.releases==1
def req(seq=1,challenge=bytes(range(1,33))):
    return struct.pack('<4I2Q',0x53534f53,seq,0x31,48,5,7)+challenge
def exchange(m,request,fragment=False):
    m.tx.clear();m.pending=request[:17] if fragment else request;m.service();assert m.failed is None
    if fragment:
        assert not m.tx;m.pending=request[17:];m.service();assert m.failed is None
    for _ in range(8):
        m.service();assert m.failed is None
        if len(m.tx)==528:break
    assert len(m.tx)==528
    # Completion is explicitly a later event, not queue acceptance.
    assert m.u.mem_read(m.sym['response_bytes'],1)!=b'\0'
    m.drain=True;m.service();assert m.failed is None
    assert m.u.mem_read(m.sym['response_bytes'],1)==b'\0'
    data=bytes(m.tx);assert struct.unpack('<4I',data[:16])==(0x53534f53,struct.unpack_from('<I',request,4)[0],0x80000031,512)
    frame=data[16:];assert hashlib.sha256(frame[:480]).digest()==frame[480:]
    assert frame[80:112]==request[32:64]
    assert struct.unpack_from('<2Q',frame,24)==(5,7)
    return frame
def followup(image):
    cases=[]
    m=Model(image);m.pending=req();m.service();m.service();assert len(m.tx)==127
    m.clock=72000000;m.service();assert m.failed==1 and len(m.tx)==127
    cases.append('partial IN frame expires without further publication')
    m=Model(image);m.service(spsr_el1=0xf00003c0)
    assert m.failed is None and m.regs['spsr_el1']==0xf00003c0
    cases.append('original masked IRQ/FIQ/debug/SError and condition flags restored')
    m=Model(image);m.mmio=True;m.clock_step=24000
    m.u.mem_map(0x02003000,0x1000);m.u.mem_map(0x04100000,0x1000)
    m.u.mem_write(0x02003300,struct.pack('<I',0x12345678))
    m.u.mem_write(0x02003304,struct.pack('<I',0x08000055))
    m.u.mem_write(0x04100410,struct.pack('<I',0x1828))
    m.u.mem_write(0x04100420,struct.pack('<I',0x40))
    start=m.clock;m.call('a733_usb_prepare');m.call('tusb_time_delay_ms_api',(10,))
    assert m.failed is None and m.clock-start>=161*24000
    assert m.writes==[(0x02003300,0x52345678),(0x02003304,0x08000155),(0x02003304,0x09000155),
        (0x04100420,0x41),(0x04100410,0x1820)]
    cases.append('actual bounded native USB preparation preserves adjacent fields and never writes watchdog')
    m=Model(image);m.clock=72000000-100;m.call('tusb_time_delay_ms_api',(150,));assert m.failed==3
    cases.append('initialization delay refuses insufficient remaining lease')
    return cases
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--followup',action='store_true')
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);shutil.copy2(__file__,a.output/'executed-check.py')
    if a.followup:
        cases=followup(a.image)
        (a.output/'result.json').write_text(json.dumps({'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),
            'cases':cases,'physical_access':False},indent=2)+'\n');print('PASS',len(cases),'focused followup cases');return
    cases=[]
    m=Model(a.image);m.regs['spsr_el1']=0xa0000000
    pending=exchange(m,req(),True);assert struct.unpack_from('<Q',pending,16)[0]==1;assert not any(pending[160:480])
    assert m.regs['spsr_el1']==0xa0000000;cases.append('fragmented pending transfer and register/FP/flags restoration')
    m.freeze();success=exchange(m,req(2));assert struct.unpack_from('<Q',success,16)[0]==0
    assert struct.unpack_from('<Q',success,72)[0]==255 and m.acquires>=2
    (a.output/'success-frame.bin').write_bytes(success);cases.append('release/acquire snapshot and pinned core0 task with same-challenge success')
    m.pending=req(3,bytes(range(2,34)));m.service();assert m.failed==7;cases.append('changed challenge refusal')
    for label,override in [('foreign core',{'tpidr_el1':1}),('foreign call site',{'elr_el1':0x41000000}),('EL1 caller',{'spsr_el1':5}),('invalid task stack',{'sp_el0':0x41000000}),('unmasked privileged IRQ',{'daif':0})]:
        m=Model(a.image);m.service(**override);assert m.failed==9 and not m.usb_calls;cases.append(label+' refusal before USB')
    m=Model(a.image);m.write('pxCurrentTCBs',struct.pack('<Q',0x41200000));m.service();assert m.failed==9 and not m.usb_calls;cases.append('foreign task refusal')
    m=Model(a.image);m.pending=req()[:7];m.service();assert m.failed is None
    m.clock=72000000;m.service();assert m.failed==1 and not m.tx;cases.append('partial request expires without response')
    m=Model(a.image);m.pending=req()+b'x';m.service();assert m.failed==11;cases.append('oversized receive refusal')
    m=Model(a.image);m.pending=req(2);m.service();assert m.failed==6;cases.append('sequence refusal')
    m=Model(a.image);m.clock=72000000;m.service();assert m.failed==1 and not m.usb_calls;cases.append('deadline refusal before USB')
    result={'image':str(a.image.resolve()),'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),
        'cases':cases,'physical_access':False,'limits':'USB FIFO/task-event stubs; no physical cache, interconnect, USB enumeration, scheduler replay or recovery proof'}
    (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS',len(cases),'observer instruction/service cases')
if __name__=='__main__':main()
