#!/usr/bin/env python3
"""Run application/storage-copy instructions with explicit hardware boundaries.

No boot, identity preparation, CPU startup, MMIO or hardware recognition is run.
The scheduler pauses at vTaskDelay. USB host enumeration, read10 bus completion,
controller health and USB0 FIFO calls are modeled. The linked read-sector,
read-result, application ownership, serializer and retention instructions run.
Recognition-active and successful transcript return are injected task states;
model parity is a separate unchanged-recognizer check.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile
from elftools.dwarf.dwarf_expr import DWARFExprParser
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
with a.image.open('rb') as f:
    elf=ELFFile(f);segments=[(s['p_vaddr'],s.data()) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
    symbols={s.name:int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols() if s['st_value']}
    dwarf=elf.get_dwarf_info();fields={};local={}
    def resolve(d):
        while d.tag in ('DW_TAG_typedef','DW_TAG_const_type','DW_TAG_volatile_type','DW_TAG_atomic_type'):d=d.get_DIE_from_attribute('DW_AT_type')
        return d
    def members(d,base=0):
        result={}
        for child in resolve(d).iter_children():
            if child.tag!='DW_TAG_member' or 'DW_AT_name' not in child.attributes:continue
            name=child.attributes['DW_AT_name'].value.decode();offset=base+int(child.attributes['DW_AT_data_member_location'].value)
            result[name]=offset;t=resolve(child.get_DIE_from_attribute('DW_AT_type'))
            if t.tag=='DW_TAG_structure_type':
                result.update({name+'.'+n:v for n,v in members(t,offset).items()})
        return result
    for cu in dwarf.iter_CUs():
        unit=cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
        for d in cu.iter_DIEs():
            if 'DW_AT_name' not in d.attributes:continue
            name=d.attributes['DW_AT_name'].value.decode()
            if d.tag=='DW_TAG_structure_type' and name in ('board_application','a733_usb2_native','a733_typec_token') and 'DW_AT_byte_size' in d.attributes:fields[name]=members(d)
            if d.tag=='DW_TAG_variable' and 'DW_AT_location' in d.attributes:
                wanted=(unit.endswith('/a733_xhci_tinyusb.c') and name=='host') or (unit.endswith('/apps/mimus-board/recognition.cc') and name in ('state','started','overlap','transcript','used'))
                if wanted:
                    ops=DWARFExprParser(dwarf.structs).parse_expr(d.attributes['DW_AT_location'].value)
                    assert len(ops)==1 and ops[0].op_name in ('DW_OP_addr','DW_OP_addrx')
                    local[name]=ops[0].args[0] if ops[0].op_name=='DW_OP_addr' else dwarf.get_addr(cu,ops[0].args[0])
class Machine:
    def __init__(self):
        self.cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.cpu.mem_map(0x41000000,0x7000000);self.cpu.mem_map(0x50000000,0x200000)
        for address,data in segments:self.cpu.mem_write(address,data)
        self.b=symbols['board'];self.bf=fields['board_application'];self.h=self.b+self.bf['host'];self.hf=fields['a733_usb2_native'];self.time=1000
        self.current=1;self.live=1;self.detach=False;self.rx=bytearray();self.tx=bytearray();self.pending=0;self.paused=False;self.taskpc={};self.polls=0;self.reads=0;self.copies=0;self.usb_polls=0;self.snapshot_result=1
        self.sector=bytes((i*7+3)&255 for i in range(512));self.text=b'TEXT accepted unchanged recorded input\nFORMATTED Accepted unchanged recorded input.\n'
        self.write(self.b+self.bf['live_until'],10000000);self.write(self.b+self.bf['session.deadline_us'],10000000)
        self.write(self.b+self.bf['session.epoch'],19);self.write(self.b+self.bf['session.generation'],23)
        self.write(local['host'],self.h);self.write(self.h+self.hf['self'],self.h)
        for field in ('core_started','mounted','msc_mounted'):self.write(self.h+self.hf[field],1,4)
        self.write(self.h+self.hf['kind'],1,4);self.write(self.h+self.hf['blocks'],64,4)
        self.write(self.h+self.hf['platform.config.lifetime_deadline'],9000000)
        self.write(self.h+self.hf['core.context'],self.h);self.write(self.h+self.hf['core.generation'],29)
        for name,index in (('current',1),('now',2),('live',3)):self.write(self.h+self.hf['core.ops.'+name],0x50000000+index*4)
        self.cpu.mem_write(self.h+self.hf['sector'],self.sector)
        self.token=struct.pack('<QQQQI',self.b+self.bf['typec.provider'],31,29,self.time,1)+bytes(4)
        self.cpu.mem_write(self.h+self.hf['platform.attachment'],self.token)
        self.cpu.hook_add(UC_HOOK_CODE,self.hook)
    def write(self,p,n,z=8):self.cpu.mem_write(p,n.to_bytes(z,'little'))
    def word(self,p,z=8):return int.from_bytes(self.cpu.mem_read(p,z),'little')
    def ret(self,n=0):self.cpu.reg_write(UC_ARM64_REG_X0,n&((1<<64)-1));self.cpu.reg_write(UC_ARM64_REG_PC,self.cpu.reg_read(UC_ARM64_REG_LR))
    def hook(self,cpu,address,size,unused):
        arg=lambda n:cpu.reg_read(UC_ARM64_REG_X0+n)
        if address==0x501ff000:self.returned=True;cpu.emu_stop();return
        if address==0x50000004:self.ret(1);return
        if address==0x50000008:self.write(arg(1),self.time);self.ret(1);return
        if address==0x5000000c:self.ret(self.live);return
        if address==symbols['a733_smp_counter']:self.ret(self.time*24);return
        if address==symbols['vTaskDelay']:
            self.paused=True;self.pc=cpu.reg_read(UC_ARM64_REG_LR);cpu.emu_stop();return
        if address==symbols['a733_usb2_native_poll']:
            self.polls+=1
            self.cpu.mem_write(self.h+self.hf['platform.attachment'],self.token[:24]+struct.pack('<Q',self.time)+self.token[32:]);self.ret(1);return
        if address==symbols['tuh_msc_read10']:
            self.reads+=1;assert self.word(local['started'],4)==1
            self.write(self.h+self.hf['read_complete'],1,4);self.ret(1);return
        if address==symbols['a733_usb2_read_result']:self.copies+=1
        if address==symbols['a733_usb2_platform_outside']:self.ret(1);return
        if address==symbols['a733_typec_snapshot']:
            if self.detach:self.ret(-1)
            elif self.snapshot_result!=1:self.ret(self.snapshot_result)
            else:self.cpu.mem_write(arg(1),self.token[:24]+struct.pack('<Q',self.time)+self.token[32:]);self.ret(1)
            return
        if address==symbols['a733_typec_revoke']:self.detach=True;self.ret();return
        if address in (symbols['a733_native_usb_step'],symbols['a733_native_usb_health'],symbols['tusb_rhport_init']):self.ret(1);return
        if address==symbols['tud_task_ext']:self.usb_polls+=1;self.ret();return
        if address in (symbols['dcd_int_handler'],symbols['tud_vendor_n_write_flush']):self.ret();return
        if address==symbols['tud_vendor_n_available']:self.ret(len(self.rx));return
        if address==symbols['tud_vendor_n_read']:
            count=min(arg(2),len(self.rx));cpu.mem_write(arg(1),bytes(self.rx[:count]));del self.rx[:count];self.ret(count);return
        if address==symbols['tud_vendor_n_write']:
            count=min(37,arg(2));self.tx.extend(cpu.mem_read(arg(1),count));self.pending+=count;self.ret(count);return
    def call(self,name,*args):
        context=self.cpu.context_save();self.returned=False
        for i,value in enumerate(args):self.cpu.reg_write(UC_ARM64_REG_X0+i,value)
        self.cpu.reg_write(UC_ARM64_REG_SP,0x501ee000);self.cpu.reg_write(UC_ARM64_REG_LR,0x501ff000)
        self.cpu.emu_start(symbols[name],0,count=1000000);assert self.returned,name
        result=self.cpu.reg_read(UC_ARM64_REG_X0);self.cpu.context_restore(context);return result
    def tick(self,name):
        if name not in self.taskpc:
            self.cpu.reg_write(UC_ARM64_REG_SP,0x50100000 if name=='board_host_task' else 0x501a0000);self.cpu.reg_write(UC_ARM64_REG_X0,0)
            self.taskpc[name]=(symbols[name],self.cpu.context_save())
        pc,context=self.taskpc[name];self.cpu.context_restore(context);self.paused=False
        self.cpu.emu_start(pc,0,count=1000000);assert self.paused,name
        self.taskpc[name]=(self.pc,self.cpu.context_save())
    def response(self,op,offset=0,count=0):
        packet=struct.pack('<BBHIQ',op,0,count,offset,0);start=len(self.tx)
        # Real serializer accumulates requests fragmented across task polls.
        for part in (packet[:1],packet[1:5],packet[5:]):self.rx.extend(part);self.tick('board_usb0_task')
        expected=None
        for _ in range(80):
            self.tick('board_host_task');self.tick('board_usb0_task')
            if self.pending:
                n=self.pending;self.pending=0;self.call('tud_vendor_tx_cb',0,n)
            data=self.tx[start:]
            if len(data)>=64:expected=64+struct.unpack_from('<I',data,12)[0]
            if expected is not None and len(data)==expected:return bytes(data)
        raise AssertionError('Response budget exceeded')
results={}
m=Machine();m.tick('board_host_task');assert not m.reads
# Native storage is held while the recognition task authenticates inputs.
m.write(local['started'],1,4);m.tick('board_host_task')
assert m.reads==1 and m.copies==1 and m.word(local['overlap'],4)==1 and m.word(m.b+m.bf['result'],4)==1
assert bytes(m.cpu.mem_read(m.b+m.bf['sector'],512))==m.sector
results['actual_read_result_copy_during_recognition']=True
m.cpu.mem_write(local['transcript'],m.text);m.write(local['used'],len(m.text),4);m.write(local['state'],1,4)
first=m.response(1,0,len(m.text));second=m.response(1,0,len(m.text));assert first==second and first[64:]==m.text
assert m.response(2,0,512)[64:]==m.sector
before=m.polls;m.tick('board_host_task');assert m.polls>before
results.update(fragmented_immutable_delivery=True,continued_storage_polling=True)
for label,mutate in [('expiry',lambda:setattr(m,'time',11000000)),('detach',lambda:setattr(m,'detach',True)),('fault',lambda:m.write(m.b+m.bf['result'],2,4))]:
    # Direct actual export guard demonstrates refusal without another transfer.
    m.time=1000;m.detach=False;m.write(m.b+m.bf['result'],1,4);mutate()
    assert (m.call('mimus_board_export_live')&0xffffffff)==0xffffffff,label;results[label+'_refused']=True
m.time=1000;m.detach=False;m.write(m.b+m.bf['result'],1,4)
m.tick('board_host_task');before=m.usb_polls;m.tick('board_usb0_task');assert m.usb_polls>before
results['continued_usb0_polling']=True
# Provider contention preserves the earlier bounded observation.
m.live=0;m.call('mimus_board_host_observed',1);assert m.call('mimus_board_live')==1;m.live=1
m.snapshot_result=0;assert m.call('mimus_board_export_live')==0;m.snapshot_result=1
results['publication_busy_preserves_bounded_observation']=True
# Detach while a copied response is only partly queued closes session admission.
m.rx.extend(struct.pack('<BBHIQ',1,0,len(m.text),0,0));m.tick('board_usb0_task');m.tick('board_usb0_task')
assert m.pending and m.pending<64+len(m.text)
m.detach=True;m.tick('board_usb0_task');assert m.word(m.b+m.bf['fatal'],4)==1
results['detach_during_fragmented_send_closes_session']=True
results['image_sha256']=hashlib.sha256(a.image.read_bytes()).hexdigest()
results['boundaries']=__doc__;a.output.write_text(json.dumps(results,indent=2)+'\n');print(json.dumps({k:v for k,v in results.items() if k!='boundaries'}))
