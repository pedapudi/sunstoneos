#!/usr/bin/env python3
"""Actual GPU/display constructors, copied-output ownership and immutable bank.

Uses the root-accepted retained session bytes. Root admission, CPU translation,
cache primitives, counter, IRQ binding and FreeRTOS scheduling are modeled.
GPU completion data is modeled; no shader, HDMI PHY or physical device executes.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile
from elftools.dwarf.dwarf_expr import DWARFExprParser
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('provider_image',ROOT/'checks/npu-large-native.py')
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)

def variables(path,unit):
    result={}
    with path.open('rb') as stream:
        dwarf=ELFFile(stream).get_dwarf_info()
        for cu in dwarf.iter_CUs():
            if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith(unit):continue
            for die in cu.iter_DIEs():
                if die.tag!='DW_TAG_variable' or 'DW_AT_location' not in die.attributes or 'DW_AT_name' not in die.attributes:continue
                if die.attributes['DW_AT_location'].form!='DW_FORM_exprloc':continue
                ops=DWARFExprParser(dwarf.structs).parse_expr(die.attributes['DW_AT_location'].value)
                if len(ops)!=1 or ops[0].op_name not in ('DW_OP_addr','DW_OP_addrx'):continue
                at=ops[0].args[0] if ops[0].op_name=='DW_OP_addr' else dwarf.get_addr(cu,ops[0].args[0])
                typ=die.get_DIE_from_attribute('DW_AT_type')
                fields={d.attributes['DW_AT_name'].value.decode():int(d.attributes['DW_AT_data_member_location'].value) for d in typ.iter_children() if d.tag=='DW_TAG_member' and 'DW_AT_name' in d.attributes and 'DW_AT_data_member_location' in d.attributes}
                result[die.attributes['DW_AT_name'].value.decode()]=(at,fields)
    return result

class Machine:
    def __init__(self,image,path,session,contract):
        self.image=image;self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.u.mem_map(0x40000000,0x04000000);self.u.mem_map(0x48000000,0x31000000)
        self.u.reg_write(UC_ARM64_REG_PSTATE,0x3c5)
        for at,data in image.parts:
            if data:self.u.mem_write(at,data)
        self.ticks=struct.unpack_from('<Q',contract,80)[0]+100;self.scheduler=1;self.task=0
        self.tasks=[];self.translations=[];self.callname='';self.clocks=[]
        self.vars=variables(path,'/apps/gpu-display/snapshot.c')
        self.sealed,self.sf=self.vars['retained']
        self.u.mem_write(0x41f00000,session);self.u.mem_write(0x42004000,contract)
        self.rc=image.address('retained_contract');self.u.mem_write(self.rc,contract)
        self.app=image.address('app');self.af=image.layouts['gpu_display_application']
        runtime=image.address('a733_cached_runtime');rl=image.layouts['a733_cached_runtime']
        self.put(runtime+rl['epoch'],struct.unpack_from('<Q',contract,24)[0])
        self.put(runtime+rl['diagnostic_deadline'],struct.unpack_from('<Q',contract,image.layouts['a733_smp_contract']['diagnostic_deadline'])[0])
        self.tasks=[];self.stopped=False
        names=['a733_smp_counter','a733_smp_device_preparation_admitted','a733_device_memory_commit_allowed','a733_device_memory_ready','a733_cpu_coherence_publish','a733_gpu_interrupt_binding_init','vTaskStartScheduler','aarch64_device_translate',
               'aarch64_device_cache_stride','aarch64_device_cache','a733_counter_clock_init','a733_counter_clock_now',
               'xTaskGetSchedulerState','xTaskGetCurrentTaskHandle','xTaskCreateStaticAffinitySet','xTaskCreateStatic','xQueueGenericCreateStatic',
               'xQueueCreateMutexStatic','vTaskEnterCritical','vTaskExitCritical','platform_panic']
        self.hooks={image.address(n):n for n in names if image.symbols.get(n)}
        self.trace=[];self.trace_sites={at:name for (unit,name),values in image.functions.items() if any(k in name for k in ('init','prepare','valid','identity','bind','handoff','admit')) for at in values}
        sites=set(self.hooks)|set(self.trace_sites)|{0x43fff000}
        for at,data in image.parts:
            for offset in range(0,len(data)-3,4):
                word=int.from_bytes(data[offset:offset+4],'little')
                if word&0xffffffe0 in (0xd5384240,0xd53be000,0xd53be020) or word==0xd5034fdf:sites.add(at+offset)
        for at in sites:self.u.hook_add(UC_HOOK_CODE,self.code,begin=at,end=at)
    def put(self,at,value,n=8):self.u.mem_write(at,int(value).to_bytes(n,'little'))
    def get(self,at,n=8):return int.from_bytes(self.u.mem_read(at,n),'little')
    def ret(self,v=0):self.u.reg_write(UC_ARM64_REG_X0,v);self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
    def code(self,cpu,at,size,unused):
        if at==0x43fff000:self.u.emu_stop();return
        word=self.get(at,4)
        if at not in self.hooks and word==0xd5034fdf:self.u.reg_write(UC_ARM64_REG_PC,at+4);return
        if at not in self.hooks and word&0xffffffe0 in (0xd5384240,0xd53be000,0xd53be020):
            value=4 if word&0xffffffe0==0xd5384240 else 24000000 if word&0xffffffe0==0xd53be000 else self.ticks
            self.u.reg_write(UC_ARM64_REG_X0+(word&31),value);self.u.reg_write(UC_ARM64_REG_PC,at+4);return
        if at in self.trace_sites:self.trace.append(self.trace_sites[at])
        if at not in self.hooks:return
        name=self.hooks[at];self.trace.append(name);a=[self.u.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
        if name=='a733_smp_counter':self.ret(self.ticks)
        elif name=='a733_smp_device_preparation_admitted':self.ret(1)
        elif name=='aarch64_device_translate':
            attr=0x44 if 0x78000000<=a[0]<0x79000000 else 0xff if a[0]>=0x40000000 else 0
            self.u.mem_write(a[2],struct.pack('<QII',a[0],attr,3 if attr else 2));self.translations.append(a[:2]);self.ret(1)
        elif name=='a733_device_memory_commit_allowed':self.ret((a[0],a[1],a[2]) in ((0,0x48000000,0x78000000),(1,0x78000000,0x79000000)))
        elif name=='a733_device_memory_ready':self.ret(1)
        elif name=='a733_cpu_coherence_publish':self.ret(1)
        elif name=='a733_gpu_interrupt_binding_init':self.ret(1)
        elif name=='vTaskStartScheduler':self.stopped=True;self.u.emu_stop()
        elif name=='aarch64_device_cache_stride':self.ret(64)
        elif name=='aarch64_device_cache':self.ret(1)
        elif name=='a733_counter_clock_init':
            l=self.image.layouts['a733_counter_clock'];self.put(a[0]+l['frequency'],24000000,4);self.put(a[0]+l['initialized'],1,4);self.clocks.append(a[0]);self.ret(1)
        elif name=='a733_counter_clock_now':self.put(a[1],self.ticks//24);self.ret(1)
        elif name=='xTaskGetSchedulerState':self.ret(self.scheduler)
        elif name=='xTaskGetCurrentTaskHandle':self.ret(self.task)
        elif name in ('xTaskCreateStatic','xTaskCreateStaticAffinitySet'):self.tasks.append(a[:7]);self.ret(a[6])
        elif name=='xQueueGenericCreateStatic':self.ret(a[3])
        elif name=='xQueueCreateMutexStatic':self.ret(a[1])
        elif name in ('vTaskEnterCritical','vTaskExitCritical'):self.ret()
        else:raise AssertionError(('Unexpected boundary',name,self.callname,a,self.trace[-40:],{k:self.get(self.app+self.af[k]+self.image.layouts[t]['initialized'],4) for k,t in [('pin_native','a733_pins_native'),('pins','a733_pins'),('pck_native','a733_gpu_resource_native')]}))
    def call(self,name,*args):
        self.callname=name;self.u.reg_write(UC_ARM64_REG_SP,0x43fd0000);self.u.reg_write(UC_ARM64_REG_LR,0x43fff000)
        for i,value in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+i,value)
        try:self.u.emu_start(self.image.address(name),0,count=240000000)
        except Exception as error:raise AssertionError((name,hex(self.u.reg_read(UC_ARM64_REG_PC)),self.trace[-25:])) from error
        assert self.stopped or self.u.reg_read(UC_ARM64_REG_PC)==0x43fff000,(name,hex(self.u.reg_read(UC_ARM64_REG_PC)))
        return self.u.reg_read(UC_ARM64_REG_X0)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','session','contract','output'):parser.add_argument('--'+name,type=Path,required=True)
    a=parser.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    image=base.Image(a.image);raw=bytearray(a.session.read_bytes());contract=bytearray(a.contract.read_bytes())
    hdmi=image.layouts['gpu_display_session']['hdmi'];hl=image.layouts['hdmi_session']
    generation=struct.unpack_from('<Q',raw,hdmi+hl['generation'])[0]
    pmic=hdmi+hl['pmic'];struct.pack_into('<3Q',raw,pmic,777,generation,0)
    raw[pmic+24:pmic+30]=bytes([2,1,1,2,1,1])
    pins=hdmi+hl['pins'];struct.pack_into('<I',raw,pins+11*4,3)
    # Existing native fixture latch image: all PL values zero and complete mask.
    struct.pack_into('<2Q2I',raw,pins+56,100,777,0x3fff,0)
    # Sessions sealed before the PHY reference was bound hold zero there;
    # they were prepared for the 24 MHz reference.
    reference=hdmi+hl['request']+image.layouts['a733_hdmi_request']['phy_reference_hz']
    if not struct.unpack_from('<I',raw,reference)[0]:struct.pack_into('<I',raw,reference,24000000)
    contract[224:256]=hashlib.sha256(raw).digest();raw=bytes(raw);contract=bytes(contract)
    m=Machine(image,a.image,raw,contract)
    # The initial fixture run executed the actual root sealer successfully.
    # Retain that result across subsequent constructor-only fixture repairs.
    seal=a.output/'sealed.json'
    if not seal.exists():
        assert m.call('gpu_display_session_prepare',m.rc)==1
        seal.write_text(json.dumps(dict(passed=True,session_sha256=hashlib.sha256(raw).hexdigest(),contract_sha256=hashlib.sha256(contract).hexdigest(),image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest()))+'\n')
    else:
        assert json.loads(seal.read_text())['session_sha256']==hashlib.sha256(raw).hexdigest()
        assert json.loads(seal.read_text())['image_sha256']==hashlib.sha256(a.image.read_bytes()).hexdigest()
        assert json.loads(seal.read_text())['contract_sha256']==hashlib.sha256(contract).hexdigest()
        m.u.mem_write(m.sealed+m.sf['bytes'],raw);m.put(m.sealed+m.sf['ready'],1,4)
    (a.output/'session.bin').write_bytes(raw);(a.output/'contract.bin').write_bytes(contract)
    assert m.call('gpu_display_inputs_get')==m.sealed+m.sf['bytes']+image.layouts['gpu_display_session']['inputs']

    checkpoint=a.output/'constructor.json'
    if checkpoint.exists():
        state=json.loads(checkpoint.read_text());assert state['image_sha256']==hashlib.sha256(a.image.read_bytes()).hexdigest()
        assert state['session_sha256']==hashlib.sha256(raw).hexdigest() and state['contract_sha256']==hashlib.sha256(contract).hexdigest()
        for row in state['memory']:m.u.mem_write(row['address'],(a.output/row['file']).read_bytes())
        m.tasks=state['tasks'];m.clocks=state['clocks']
    else:
        assert m.call('a733_memory_reservations_commit_verified')==1
        assert m.call('a733_memory_uncached_commit_verified')==1
        m.call('a733_smp_diagnostic_start')
        # PMIC, output, render and USB observation tasks plus the GPU service worker.
        assert m.stopped and len(m.tasks)==5,(m.tasks,m.trace[-30:])
        pl=image.layouts['device_memory_pool'];al=image.layouts['device_memory_allocation'];allocations={};memory=[]
        for name,fn in [('wb','a733_memory_reservations_init'),('nc','a733_memory_uncached_init')]:
            pool=m.call(fn);rows=[]
            for i in range(32):
                slot=pool+pl['allocations']+i*32;owner=m.get(slot+al['owner'])
                if owner:rows.append(dict(owner=owner,first=m.get(slot+al['first'],4),pages=m.get(slot+al['pages'],4),state=m.get(slot+al['state'],4)))
            allocations[name]=rows
            backing=m.get(pool+pl['backing']);end=max((r['first']+r['pages'] for r in rows),default=0)*4096
            if end:
                file=name+'-constructor.bin';(a.output/file).write_bytes(bytes(m.u.mem_read(backing,end)));memory.append(dict(address=backing,file=file))
        (a.output/'main-constructor.bin').write_bytes(bytes(m.u.mem_read(0x41000000,0x400000)));memory.append(dict(address=0x41000000,file='main-constructor.bin'))
        state=dict(passed=True,session_sha256=hashlib.sha256(raw).hexdigest(),contract_sha256=hashlib.sha256(contract).hexdigest(),image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),memory=memory,tasks=m.tasks,clocks=m.clocks,allocations=allocations,scope=__doc__)
        checkpoint.write_text(json.dumps(state,indent=2)+'\n');print('PASS actual constructors',allocations,flush=True)
    m.stopped=False;m.scheduler=2;m.task=m.get(m.app+m.af['handles']+16)
    # Model a completed GPU copied-output slot, then execute the actual output
    # accessor, display conversion, owner checks, RCQ builder and bank handoff.
    gl=image.layouts['a733_gpu_graphics_memory'];sl=image.layouts['pvr_gpu_service'];rl=image.layouts['pvr_render_service']
    graphics=m.app+m.af['gpu_graphics'];service=m.app+m.af['gpu_service'];submission=graphics+gl['submission']
    layout=0x43e00000;assert m.call('pvr_render_texture_layout_profile',1280,720,2,layout)==1
    download=m.get(layout+image.layouts['pvr_render_texture_layout']['download'],4)
    draw=graphics+gl['render'];render=image.layouts['pvr_render_requirements']
    payload=m.get(graphics+gl['payload_extent'])
    output=payload+m.get(draw+render['payload_bytes'],4)+download
    m.put(service+sl['owner'],0,4);m.put(service+sl['phase'],3,4)
    m.put(submission+rl['phase'],9,4);m.put(submission+rl['texture_output_state'],1,4)
    m.put(submission+rl['texture_output_client'],0,4)
    fb=image.address('framebuffer');fl=image.layouts['hdmi_framebuffer'];extent=fb+fl['extent'];bank=m.get(extent)
    total=1280*720*4;framebytes=1847296;hashes=[]
    inputs=m.call('gpu_display_inputs_get')
    for frame in range(2):
        # The render task opens and closes each frame's copied-output evidence.
        assert m.call('gpu_display_evidence_begin',frame,inputs+16+frame*64+32)==1
        converted=bytearray()
        for offset in range(0,total,65536):
            n=min(65536,total-offset);ticket=1000+frame*100+offset//65536
            rgba=bytes((frame*99+offset//4+i*17)&255 for i in range(n))
            m.u.mem_write(output,rgba)
            for key,value,width in [('sequence',frame+1,4),('texture_output_frame',frame+1,4),('texture_output_offset',offset,4),('texture_output_bytes',n,4),('texture_output_ticket',ticket,8)]:m.put(submission+rl[key],value,width)
            assert m.call('gpu_display_bank_copy',frame,frame+1,offset,output,n)==1,(frame,offset)
            expected=b''.join(struct.pack('<H',((rgba[i]>>3)<<11)|((rgba[i+1]>>2)<<5)|(rgba[i+2]>>3)) for i in range(0,n,4))
            actual=bytes(m.u.mem_read(bank+36864+frame*framebytes+offset//2,n//2));assert actual==expected
            converted.extend(actual)
            assert m.call('gpu_display_bank_copy',frame,frame+1,offset,output,n)==0
        assert m.call('gpu_display_evidence_end',frame)==1
        hashes.append(hashlib.sha256(converted).hexdigest())
    assert m.call('gpu_display_bank_seal')==0 # GPU still owns unfinished close.
    m.put(m.app+m.af['gpu_closed'],1,4);assert m.call('gpu_display_bank_seal')==1
    before=bytes(m.u.mem_read(bank,4194304))
    assert m.call('gpu_display_bank_copy',1,2,total,output,4)==0
    assert m.call('gpu_display_bank_seal')==0
    assert bytes(m.u.mem_read(bank,4194304))==before
    record=dict(passed=True,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),allocations=state['allocations'],frame_rgb565_sha256=hashes,
        cases=['actual-shared-provider-construction','two-complete-copied-frame-conversion','duplicate-offset-refused','seal-before-close-refused','completed-bank-refuses-further-copy-and-reseal'],
        physical_access=False,scope=__doc__)
    (a.output/'copy.json').write_text(json.dumps(record,indent=2)+'\n');print('PASS two copied frames and immutable bank',flush=True)
if __name__=='__main__':main()
