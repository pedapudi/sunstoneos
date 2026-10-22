#!/usr/bin/env python3
"""Execute the frozen Bluetooth recognition serializer and TinyUSB device stack."""
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
ROOT=Path(__file__).resolve().parents[1]
ORIGINAL=Path.home()/'.cache/sunstoneos/evidence/board-session-seal-mimus-20261018'
ELF_HASH='7e01cd78e899a5a45fa9074fc8f188bd835c87a53615b444d7f69128b1fe6d0b'
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def save(p,value):p.write_text(json.dumps(value,indent=2)+'\n')
def module(name,p):
    spec=importlib.util.spec_from_file_location(name,p);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

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
          '_ZN5mimus3asr8platform14MonotonicNanosEv','xTaskGetCurrentTaskHandle','a733_clock_token_status','a733_rail_status','a733_pmic_live',
          'a733_native_usb_step','a733_native_usb_health','a733_usb_platform_poll','bluetooth_recognition_copy',
          'tusb_rhport_init','tud_task_ext','vendord_init','vendord_open','vendord_xfer_cb',
          'tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush',
          'dcd_event_handler','dcd_init','dcd_int_enable','dcd_int_disable','dcd_edpt_open','dcd_edpt_xfer',
          'dcd_edpt_close_all','dcd_edpt0_setup_begin','dcd_edpt_stall','dcd_sof_enable']
        for name in required:
            assert len(candidates[name])==1,(name,candidates[name]);symbols[name]=next(iter(candidates[name]))
        assert all(n in variables for n in ('identity_frame','identity_bytes','identity_sent','identity_transferred','retained','terminal','stopped'))
        if 'finished' in variables:
            for name in ('bluetooth_recognition_segment_copy','bluetooth_recognition_segment_ack','bluetooth_recognition_finished',
                         'bluetooth_headset_segment_status','bluetooth_headset_segment_stop','xTaskGenericNotify'):
                assert len(candidates[name])==1,(name,candidates[name]);symbols[name]=next(iter(candidates[name]))
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
        if address==self.write_return_address:
            self.write_returns.append(args(0));self.write_return_address=None
        if address==self.s['tud_vendor_n_write']:self.write_return_address=cpu.reg_read(UC_ARM64_REG_LR)
        if address==0x50fff000:self.returned=True;cpu.emu_stop();return
        if address==self.s['vTaskDelay']:
            self.time+=1000;self.pc=cpu.reg_read(UC_ARM64_REG_LR);self.paused=True;cpu.emu_stop();return
        if address==self.s['a733_smp_counter']:self.ret(self.time*24);return
        if address==self.s['_ZN5mimus3asr8platform14MonotonicNanosEv']:self.ret(self.time*1000);return
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
        raise AssertionError('Bounded transfer progress exhausted')
    def observation(self):
        return {'variant':self.variant,'calls':self.log,'executed':dict(self.seen),'packet_lengths':list(map(len,self.completed)),
          'fatal':self.word(self.s['board']+self.t['board_application']['fatal']),'poison_injected':self.injected,'actual_vendor_write_returns':self.write_returns,'time_us_model':self.time}

def run(out,step):
    observe=module('bluetooth_frozen_observe',out/'frozen-source/apps/bluetooth/observe.py')
    profile=(out/'profile.bin').read_bytes();text=(out/'transcript.bin').read_bytes()
    count=min(1024,len(text));request=struct.pack('<BBHI',3,0,count,0)
    for variant in ('fragment-repeat','cancel','provider-failure','deadline','copy-stop'):
        if step(variant):continue
        m=Machine(out,variant)
        try:
            m.enumerate()
            if variant=='fragment-repeat':
                m.request(request);pending=m.drain(208);(out/'pending-frame.bin').write_bytes(pending)
                assert observe.decode_recognition(pending,m.session,profile,0,count)[0]['state']=='pending'
                m.write(m.variables['terminal']['address'],1)
                m.request(request);assert m.tick()
                for _ in range(3):assert m.tick()
                assert m.word(m.variables['identity_sent']['address'])==208+count
                assert m.word(m.variables['identity_transferred']['address'])==0
                before=m.seen['bluetooth_recognition_copy'];m.request(request)
                for _ in range(3):assert m.tick()
                assert m.seen['bluetooth_recognition_copy']==before
                start=len(m.completed);frames=[]
                for _ in range(50):
                    if 0x81 in m.pending:m.complete(0x81)
                    assert m.tick()
                    wire=b''.join(m.completed[start:])
                    if len(wire)==2*(208+count):frames=[wire[:208+count],wire[208+count:]];break
                assert len(frames)==2 and frames[0]==frames[1]
                for i,frame in enumerate(frames):
                    (out/f'success-{i}.bin').write_bytes(frame)
                    metadata,body=observe.decode_recognition(frame,m.session,profile,0,count)
                    assert body==text[:count] and metadata['text_sha256']==hashlib.sha256(text).hexdigest()
                # Copy all remaining chunks through the same actual USB path.
                copied=frames[0][208:]
                for offset in range(count,len(text),1024):
                    n=min(1024,len(text)-offset)
                    m.request(struct.pack('<BBHI',3,0,n,offset));frame=m.drain(208+n)
                    metadata,body=observe.decode_recognition(frame,m.session,profile,offset,n);copied+=body
                assert copied==text
                (out/'copied-transcript.bin').write_bytes(copied)
                assert all(m.seen[n] for n in ('bluetooth_recognition_copy','tud_task_ext','vendord_xfer_cb','tud_vendor_n_write'))
            elif variant=='cancel':
                m.complete(2,struct.pack('<BBHI',3,1,count,0));assert not m.tick()
                assert not m.completed and 0x81 not in m.pending
            elif variant=='copy-stop':
                m.write(m.variables['terminal']['address'],1);m.poison=True;m.request(request)
                frame=m.drain(208);(out/'stopped-frame.bin').write_bytes(frame)
                assert m.injected and observe.decode_recognition(frame,m.session,profile,0,count)[0]['state']=='retained-failure'
            else:
                m.write(m.variables['terminal']['address'],1);m.request(request);assert m.tick() and 0x81 in m.pending
                issued=sum(x['call']=='dcd_edpt_xfer' for x in m.log)
                if variant=='provider-failure':m.write(m.usb+m.u['usb.fault'],1)
                else:m.time=10000000
                assert not m.tick()
                assert sum(x['call']=='dcd_edpt_xfer' for x in m.log)==issued
            save(out/(variant+'-result.json'),{'passed':True,**m.observation()});step(variant,True)
        except BaseException:
            import traceback
            save(out/(variant+'-failed-observation.json'),m.observation());(out/(variant+'-failure.log')).write_text(traceback.format_exc());raise

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--resume-setup',action='store_true')
    mode=p.add_mutually_exclusive_group(required=True);mode.add_argument('--setup-only',action='store_true');mode.add_argument('--run-only',action='store_true')
    a=p.parse_args();out=a.output.resolve()
    if a.setup_only:
        out.mkdir(parents=True,exist_ok=a.resume_setup)
        assert digest(ORIGINAL/'build/sunstone-a733-bluetooth.elf')==ELF_HASH
        dependencies=json.loads((ORIGINAL/'compiled-dependencies.json').read_text())
        for path,h in dependencies.items():assert digest(Path(path))==h,path
        for source,target in [('build/sunstone-a733-bluetooth.elf','bluetooth.elf'),('bt-session.bin','admitted-session.bin'),('bt-profile.bin','profile.bin'),('image.json','original-image-manifest.json'),('compiled-dependencies.json','original-compiled-dependencies.json')]:shutil.copy2(ORIGINAL/source,out/target)
        previous=Path.home()/'.cache/sunstoneos/evidence/bluetooth-mimus-native-20261017/composition'
        for source,target in [('success-transcription.log','transcript.bin'),('success-transcription.log.snapshot.bin','snapshot.bin')]:shutil.copy2(previous/source,out/target)
        assert len((out/'snapshot.bin').read_bytes())==144
        assert (out/'snapshot.bin').read_bytes()[112:]==hashlib.sha256((out/'transcript.bin').read_bytes()).digest()
        assert int.from_bytes((out/'profile.bin').read_bytes()[720:724],'little')==int.from_bytes((out/'snapshot.bin').read_bytes()[36:40],'little')
        for name in ('apps/bluetooth/observe.py','apps/bluetooth/usb0.c','apps/bluetooth/recognition.cc','apps/bluetooth/bluetooth.c','platform/a733/native_usb.c','drivers/usb/a733_usb_platform.c'):
            dst=out/'frozen-source'/name;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(ORIGINAL/'source'/name,dst)
        shutil.copy2(Path(__file__),out/'check.py');setup(out)
        save(out/'scope.json',{'physical_access':False,'image_rebuilt':False,'reuse':str(ORIGINAL),
            'modeled':'DCD endpoint/event hardware, lower clock/rail/PMIC statuses, scheduler yields/time; adopted final recognition transcript/snapshot and ready USB/application state.',
            'executed':'Actual recognition copy/live/output boundaries, Bluetooth serializer/USB task, USB PREPARED/health consumer and TinyUSB initialization/configuration/vendor FIFO/completion.'})
        import unicorn,elftools
        paths=[out/n for n in ('bluetooth.elf','transcript.bin','snapshot.bin','admitted-session.bin','profile.bin','original-image-manifest.json','original-compiled-dependencies.json','check.py','scope.json','preflight.json')]
        paths += [x for x in (out/'frozen-source').rglob('*') if x.is_file()]
        paths+=[Path(sys.executable)]+[x for package in (unicorn,elftools) for x in Path(package.__file__).parent.rglob('*') if x.is_file() and '__pycache__' not in x.parts]
        save(out/'inputs.json',{str(x):digest(x) for x in dict.fromkeys(paths)})
        print('Setup passed; frozen recognition image reused.');return
    for path,h in json.loads((out/'inputs.json').read_text()).items():assert digest(Path(path))==h,path
    steps_path=out/'steps.json';steps=json.loads(steps_path.read_text()) if steps_path.exists() else {}
    def step(name,passed=False):
        if passed:steps[name]={'passed':True};save(steps_path,steps);return
        if steps.get(name,{}).get('passed'):return True
        steps[name]={'state':'running'};save(steps_path,steps);return False
    run(out,step)
    save(out/'result.json',{'passed':True,'physical_access':False,'image_rebuilt':False,'elf_sha256':ELF_HASH,'cases':list(steps),'scope':'Native Bluetooth recognition copy/serializer/TinyUSB over modeled hardware and adopted prior recognition output.'})
    print('PASS actual frozen Bluetooth recognition USB serialization')
if __name__=='__main__':main()
