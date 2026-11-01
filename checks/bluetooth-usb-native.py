#!/usr/bin/env python3
"""Shared native Bluetooth USB emulator for modeled controller and transport checks."""
import argparse
import hashlib
import importlib.util
import json
import shutil
import struct
import sys
from collections import defaultdict
from pathlib import Path
from elftools.elf.elffile import ELFFile
from elftools.dwarf.dwarf_expr import DWARFExprParser
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PC

ELF_HASH = None
CLOCK_HOOK = None
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def save(p,value):p.write_text(json.dumps(value,indent=2)+"\n")

def setup(out):
    image=out/'bluetooth.elf';assert digest(image)==ELF_HASH
    with image.open('rb') as f:
        elf=ELFFile(f);dw=elf.get_dwarf_info();candidates=defaultdict(set);symbols={};variables={};types={};enums={};sizes={}
        for symbol in elf.get_section_by_name('.symtab').iter_symbols():
            if symbol['st_value']:candidates[symbol.name].add(int(symbol['st_value']))
        def resolved(d):
            while d.tag in ('DW_TAG_typedef','DW_TAG_const_type','DW_TAG_volatile_type','DW_TAG_atomic_type'):d=d.get_DIE_from_attribute('DW_AT_type')
            return d
        def members(d,base=0):
            d=resolved(d);result={}
            for child in d.iter_children():
                if child.tag!='DW_TAG_member':continue
                offset=base+int(child.attributes.get('DW_AT_data_member_location',type('V',(),{'value':0})()).value)
                name=child.attributes.get('DW_AT_name');t=resolved(child.get_DIE_from_attribute('DW_AT_type'))
                if name:result[name.value.decode()]=offset
                if t.tag in ('DW_TAG_structure_type','DW_TAG_union_type'):
                    nested=members(t,offset)
                    for n,o in nested.items():result[(name.value.decode()+'.' if name else '')+n]=o
            return result
        for cu in dw.iter_CUs():
            unit=cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
            for die in cu.iter_DIEs():
                if die.tag=='DW_TAG_enumeration_type':
                    for child in die.iter_children():
                        if child.tag=='DW_TAG_enumerator':enums[child.attributes['DW_AT_name'].value.decode()]=int(child.attributes['DW_AT_const_value'].value)
                name=die.attributes.get('DW_AT_name')
                if not name:continue
                n=name.value.decode()
                if die.tag in ('DW_TAG_structure_type','DW_TAG_typedef'):
                    try:
                        t=resolved(die)
                        if t.tag in ('DW_TAG_structure_type','DW_TAG_union_type'):
                            if 'DW_AT_byte_size' in t.attributes:types[n]=members(t)
                            if 'DW_AT_byte_size' in t.attributes:sizes[n]=int(t.attributes['DW_AT_byte_size'].value)
                    except KeyError:pass
                if die.tag=='DW_TAG_variable' and ((unit.endswith('/apps/bluetooth/usb0.c') and n in ('identity_frame','identity_bytes','identity_sent','identity_transferred','transcript_delivered','transcript_delivered_bytes')) or (unit.endswith('/apps/bluetooth/recognition.cc') and n in ('retained','terminal','stopped','finished')) or (unit.endswith('/apps/bluetooth/headset.c') and n=='voice')):
                    loc=die.attributes.get('DW_AT_location');assert loc,(unit,n)
                    ops=DWARFExprParser(dw.structs).parse_expr(loc.value)
                    assert len(ops)==1 and ops[0].op_name in ('DW_OP_addr','DW_OP_addrx'),(unit,n)
                    address=ops[0].args[0] if ops[0].op_name=='DW_OP_addr' else dw.get_addr(cu,ops[0].args[0])
                    variables[n]={'address':address,'unit':unit,'members':members(die.get_DIE_from_attribute('DW_AT_type')) if n in ('retained','voice') else {}}
        required=['board_usb0_task','board','bluetooth','providers','board_live','board_fail','vTaskDelay','a733_smp_counter',
          'xTaskGetCurrentTaskHandle','a733_clock_token_status','a733_rail_status','a733_pmic_live',
          'a733_native_usb_step','a733_native_usb_health','a733_usb_platform_poll','bluetooth_recognition_copy',
          'tusb_rhport_init','tud_task_ext','vendord_init','vendord_open','vendord_xfer_cb',
          'tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush',
          'dcd_event_handler','dcd_init','dcd_int_enable','dcd_int_disable','dcd_edpt_open','dcd_edpt_xfer',
          'dcd_edpt_close_all','dcd_edpt0_setup_begin','dcd_edpt_stall','dcd_sof_enable']
        if CLOCK_HOOK:required.append(CLOCK_HOOK)
        for name in required:
            assert len(candidates[name])==1,(name,candidates[name]);symbols[name]=next(iter(candidates[name]))
        assert all(n in variables for n in ('identity_frame','identity_bytes','identity_sent','identity_transferred','retained','terminal','stopped'))
        if 'finished' in variables:
            for name in ('bluetooth_recognition_segment_copy','bluetooth_recognition_segment_ack','bluetooth_recognition_finished',
                         'bluetooth_headset_segment_status','bluetooth_headset_segment_stop'):
                assert len(candidates[name])==1,(name,candidates[name]);symbols[name]=next(iter(candidates[name]))
        # Notification-free images have no scheduler give boundary to model.
        if candidates.get('xTaskGenericNotify'):
            assert len(candidates['xTaskGenericNotify']) == 1
            symbols['xTaskGenericNotify'] = next(iter(candidates['xTaskGenericNotify']))
        fields={'board_application':['session','fatal','watchdog_state','live_until'],
          'bluetooth_application':['self','profile','shared','failed','recovery_state'],'a733_native_usb':['clocks','rails','pmic','usb','usb.token','usb.token.id','usb_rail','usb_rail.id','boot.usb.owner','usb.token.owner','usb.registry','usb.self','usb.initialized','usb.stage','usb.fault','usb_rail.owner','usb_power.owner','usb_power.provider','live','fail','context','usb_task','usb_step','usb_rail_index','pmic_ready','usb_prepared','clocks.failed','locks.retained'],
          'dcd_event_t':['rhport','event_id','setup_received','bus_reset.speed','xfer_complete.ep_addr','xfer_complete.result','xfer_complete.len']}
        for name,names in fields.items():assert all(n in types[name] for n in names),(name,names)
        for name in ('app','deadline','log','snapshot','used'):assert name in variables['retained']['members']
        assert enums['DCD_EVENT_BUS_RESET_END']==2 and enums['TUSB_SPEED_HIGH']==2
        segments=[(int(s['p_vaddr']),bytes(s.data())) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
    report={'symbols':symbols,'variables':variables,'types':types,'sizes':sizes,'enums':enums,'elf_sha256':ELF_HASH,'native_instructions_executed':False}
    save(out/'preflight.json',report);return report,segments

class Machine:
    def __init__(self,out,variant):
        self.out=out;self.variant=variant;self.report,self.segments=setup(out);self.s=self.report['symbols'];self.t=self.report['types'];self.variables=self.report['variables']
        # Model architectural counter reads in linked code as well as calls
        # through the board counter helper. No application clock symbol is needed.
        self.counter_reads = {}
        for base, data in self.segments:
            for offset in range(0, len(data) - 3, 4):
                word = int.from_bytes(data[offset:offset + 4], 'little')
                if word & ~31 in (0xd53be020, 0xd53be040, 0xd53be000):
                    self.counter_reads[base + offset] = (word & 31, word & ~31)
        self.cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        for address,size in [(0x40000000,0x4000000),(0x50000000,0x1000000)]:self.cpu.mem_map(address,size)
        for address,data in self.segments:self.cpu.mem_write(address,data)
        self.time=1000;self.task=0x123400;self.paused=False;self.returned=False;self.started=False;self.pc=0;self.log=[];self.pending={};self.opened={};self.completed=[];self.seen=defaultdict(int);self.poison=False;self.injected=False;self.write_returns=[];self.write_return_address=None
        self.cpu.hook_add(UC_HOOK_CODE,self.hook);self.cpu.hook_add(UC_HOOK_MEM_READ,self.read)
        session=(out/'admitted-session.bin').read_bytes();self.session=session
        self.cpu.mem_write(self.s['board']+self.t['board_application']['session'],session[:592])
        self.field('board_application','board','session.deadline_us',10000000,8)
        self.field('board_application','board','live_until',10000000,8)
        owner=int.from_bytes(session[self.t['a733_board_session']['usb0_owner']:self.t['a733_board_session']['usb0_owner']+8],'little')
        self.usb=self.s['providers']+self.t['board_provider']['shared'];u=self.t['a733_native_usb'];self.u=u
        def put(name,value,n=8):self.write(self.usb+u[name],value,n)
        for name,value,n in [('boot.usb.owner',owner,8),('usb.token.owner',owner,8),('usb.registry',self.usb+u['clocks'],8),
          ('usb.self',self.usb+u['usb'],8),('usb.initialized',1,4),('usb.stage',18,4),('usb_rail.owner',owner,8),
          ('usb_power.owner',owner,8),('usb_power.provider',self.usb+u['pmic'],8),('live',self.s['board_live'],8),('fail',self.s['board_fail'],8),
          ('usb_task',self.task,8),('usb_step',5,4),('usb_rail_index',4,4),('pmic_ready',1,4)]:put(name,value,n)
        for name in ('usb.token.id','usb_rail.id'):put(name,1)
        # Prepared-provider lower statuses are explicit model boundaries. Actual
        # step/poll/health still check self/task/owner/readiness and exact pointers.
        self.service=self.variables['retained']['address'];self.sf=self.variables['retained']['members']
        self.cpu.mem_write(self.service+self.sf['log'],(out/'transcript.bin').read_bytes())
        self.cpu.mem_write(self.service+self.sf['snapshot'],(out/'snapshot.bin').read_bytes())
        self.write(self.service+self.sf['used'],len((out/'transcript.bin').read_bytes()),4)
        self.write(self.service+self.sf['app'],self.s['bluetooth'],8)
        self.write(self.service+self.sf['deadline'],10000000,8)
        # The recognizer task that a transcript acknowledgement wakes.
        self.recognizer=0x567800;self.notified=[]
        if 'owner' in self.sf:self.write(self.service+self.sf['owner'],self.recognizer,8)
        profile=(out/'profile.bin').read_bytes();self.cpu.mem_write(0x43000000,profile)
        a=self.s['bluetooth'];t=self.t['bluetooth_application']
        for name,value,n in [('self',a,8),('profile',0x43000000,8),('shared',self.usb,8),('recovery_state',self.s['board']+self.t['board_application']['watchdog_state'],8),('failed',0,4)]:self.write(a+t[name],value,n)
    def field(self,type_name,base,name,value,n=4):self.write(self.s[base]+self.t[type_name][name],value,n)
    def write(self,address,value,n=4):self.cpu.mem_write(address,value.to_bytes(n,'little'))
    def word(self,address,n=4):return int.from_bytes(self.cpu.mem_read(address,n),'little')
    def ret(self,value=0):self.cpu.reg_write(UC_ARM64_REG_X0,value);self.cpu.reg_write(UC_ARM64_REG_PC,self.cpu.reg_read(UC_ARM64_REG_LR))
    def hook(self,cpu,address,size,unused):
        args=lambda n:cpu.reg_read(UC_ARM64_REG_X0+n)
        if address in self.counter_reads:
            target, operation = self.counter_reads[address]
            assert target < 31
            cpu.reg_write(UC_ARM64_REG_X0 + target, 24000000 if operation == 0xd53be000 else self.time * 24)
            cpu.reg_write(UC_ARM64_REG_PC, address + 4)
            return
        if address==self.write_return_address:
            self.write_returns.append(args(0));self.write_return_address=None
        if address==self.s['tud_vendor_n_write']:self.write_return_address=cpu.reg_read(UC_ARM64_REG_LR)
        if address==0x50fff000:self.returned=True;cpu.emu_stop();return
        if address==self.s['vTaskDelay']:
            self.time+=1000;self.pc=cpu.reg_read(UC_ARM64_REG_LR);self.paused=True;cpu.emu_stop();return
        if address==self.s['a733_smp_counter']:self.ret(self.time*24);return
        if CLOCK_HOOK and address==self.s.get(CLOCK_HOOK):self.ret(self.time*1000);return
        if address==self.s['xTaskGetCurrentTaskHandle']:self.ret(self.task);return
        if address==self.s.get('xTaskGenericNotify'):
            # Scheduler boundary: one give on index 0 to the recognizer task.
            assert (args(0),args(1),args(3),args(4))==(self.recognizer,0,2,0);self.notified.append(args(0));self.ret(1);return
        if address==self.s['a733_clock_token_status']:
            assert (args(0),args(1))==(self.usb+self.u['clocks'],self.usb+self.u['usb.token']);self.ret(0);return
        if address==self.s['a733_rail_status']:
            assert (args(0),args(1))==(self.usb+self.u['rails'],self.usb+self.u['usb_rail']);self.ret(0);return
        if address==self.s['a733_pmic_live']:
            assert (args(0),args(1))==(self.usb+self.u['pmic'],self.usb+self.u['usb_power']);self.ret(1);return
        for name in ('a733_native_usb_step','a733_native_usb_health','a733_usb_platform_poll','bluetooth_recognition_copy','tud_task_ext','vendord_xfer_cb','tud_vendor_n_write'):
            if address==self.s[name]:self.seen[name]+=1
        name=next((n for n in ('dcd_init','dcd_int_enable','dcd_int_disable','dcd_edpt_open','dcd_edpt_xfer','dcd_edpt_close_all','dcd_edpt0_setup_begin','dcd_edpt_stall','dcd_sof_enable') if address==self.s[n]),None)
        if not name:return
        self.log.append({'call':name,'args':[args(i) for i in range(5)]})
        assert args(0)&255==0
        if name=='dcd_init':
            # tusb_rhport_init_t is two enum fields, role=device1/speed=high2.
            assert bytes(cpu.mem_read(args(1),8))==struct.pack('<2I',1,2)
        elif name=='dcd_edpt_open':
            desc=bytes(cpu.mem_read(args(1),7));ep=desc[2];mps=int.from_bytes(desc[4:6],'little')
            assert desc[:2]==b'\x07\x05' and desc[3]==2 and ep in (2,0x81) and mps==512
            self.opened[ep]=mps
        elif name=='dcd_edpt_xfer':
            ep,pointer,count,isr=args(1)&255,args(2),args(3)&65535,args(4)&255
            assert not isr and ep in (0,0x80,2,0x81) and count<=512 and ep not in self.pending,(ep,count,{e:p['count'] for e,p in self.pending.items()})
            if ep in (2,0x81):assert ep in self.opened
            self.pending[ep]={'pointer':pointer,'count':count,'bytes':bytes(cpu.mem_read(pointer,count)) if count else b''}
        elif name=='dcd_edpt_stall':raise AssertionError('Unexpected stall')
        self.ret(1)
    def read(self,cpu,access,address,size,value,unused):
        if self.poison and not self.injected and address==self.service+self.sf['log']:
            self.write(self.variables['stopped']['address'],1,1);self.injected=True
            self.log.append({'injection':'Recognition stopped during actual output copy','source':address,'instruction':cpu.reg_read(UC_ARM64_REG_PC)})
    def call(self,name,*arguments):
        context=self.cpu.context_save()
        self.returned=False
        for i,a in enumerate(arguments):self.cpu.reg_write(UC_ARM64_REG_X0+i,a)
        self.cpu.reg_write(UC_ARM64_REG_SP,0x50efeff0);self.cpu.reg_write(UC_ARM64_REG_LR,0x50fff000)
        self.cpu.emu_start(self.s[name],0,count=4000000);assert self.returned,name
        result=self.cpu.reg_read(UC_ARM64_REG_X0);self.cpu.context_restore(context);return result
    def tick(self):
        for p in self.pending.values():
            if p['count'] and 0x81 in self.pending and p is self.pending[0x81]:assert bytes(self.cpu.mem_read(p['pointer'],p['count']))==p['bytes'],'Backing changed before completion'
        self.paused=False
        if not self.started:
            self.started=True;self.pc=self.s['board_usb0_task'];self.cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);self.cpu.reg_write(UC_ARM64_REG_X0,0)
        self.cpu.emu_start(self.pc,0,count=4000000);assert self.paused,'USB task did not reach bounded scheduler wait'
        if self.word(self.s['board']+self.t['board_application']['fatal']):
            assert self.word(self.s['board']+self.t['board_application']['watchdog_state'])&4
            return False
        return True
    def event(self,kind,**fields):
        t=self.t['dcd_event_t'];raw=bytearray(self.report['sizes']['dcd_event_t']);raw[t['event_id']]=self.report['enums']['DCD_EVENT_'+kind]
        for name,value in fields.items():
            if name=='setup_received':raw[t[name]:t[name]+8]=value
            else:
                n=1 if name in ('xfer_complete.ep_addr','xfer_complete.result') else 4
                raw[t[name]:t[name]+n]=value.to_bytes(n,'little')
        self.cpu.mem_write(0x50e00000,bytes(raw));self.call('dcd_event_handler',0x50e00000,0)
    def complete(self,ep,data=None):
        p=self.pending.pop(ep);n=p['count'] if data is None else len(data)
        assert n<=p['count']
        if data is not None:self.cpu.mem_write(p['pointer'],data)
        if ep==0x81:
            assert bytes(self.cpu.mem_read(p['pointer'],n))==p['bytes'][:n]
            self.completed.append(p['bytes'][:n])
        self.event('XFER_COMPLETE',**{'xfer_complete.ep_addr':ep,'xfer_complete.result':0,'xfer_complete.len':n})
    def enumerate(self):
        assert self.tick()
        assert self.seen['a733_native_usb_step'] and self.seen['a733_usb_platform_poll']
        self.event('BUS_RESET_END',**{'bus_reset.speed':2});assert self.tick()
        self.event('SETUP_RECEIVED',setup_received=struct.pack('<BBHHH',0,9,1,0,0));assert self.tick()
        assert self.opened=={2:512,0x81:512} and 2 in self.pending
        assert 0x80 in self.pending and not self.pending[0x80]['count']
        self.complete(0x80);assert self.tick()
    def request(self,value):
        assert 2 in self.pending;self.complete(2,value);assert self.tick()
    def drain(self,expected):
        first=len(self.completed)
        for _ in range(40):
            if 0x81 in self.pending:self.complete(0x81)
            assert self.tick()
            if len(b''.join(self.completed[first:]))==expected:
                for _ in range(2):assert self.tick()
                return b''.join(self.completed[first:])
        raise AssertionError(('Bounded transfer progress exhausted', expected, [len(v) for v in self.completed[first:]]))
    def observation(self):
        return {'variant':self.variant,'calls':self.log,'executed':dict(self.seen),'packet_lengths':list(map(len,self.completed)),
          'fatal':self.word(self.s['board']+self.t['board_application']['fatal']),'poison_injected':self.injected,'actual_vendor_write_returns':self.write_returns,'time_us_model':self.time}

