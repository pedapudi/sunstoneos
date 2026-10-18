"""New RTC boot binding and copied-result instructions; no physical hardware."""
import hashlib
import importlib.util
import json
import struct
from collections import defaultdict
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def setup(image,out):
    candidates=defaultdict(set);layouts={};definitions=defaultdict(set)
    with image.open('rb') as f:
        elf=ELFFile(f)
        for symbol in elf.get_section_by_name('.symtab').iter_symbols():
            if symbol['st_value']:candidates[symbol.name].add(int(symbol['st_value']))
        segments=[(int(s['p_vaddr']),bytes(s.data())) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
        for cu in elf.get_dwarf_info().iter_CUs():
            unit=cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
            for die in cu.iter_DIEs():
                name=die.attributes.get('DW_AT_name')
                if not name:continue
                name=name.value.decode()
                if die.tag=='DW_TAG_subprogram' and 'DW_AT_low_pc' in die.attributes:definitions[(unit,name)].add(int(die.attributes['DW_AT_low_pc'].value))
                if die.tag=='DW_TAG_structure_type':
                    fields={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in die.iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_name' in c.attributes and 'DW_AT_data_member_location' in c.attributes}
                    if fields:layouts[name]=fields
    names=['rtc_enable_irq','vTaskEnterCritical','vTaskExitCritical','rtc_session_prepare','rtc_admitted_session','rtc_boot_route','rtc_copy','rtc_application_irq',
           'a733_smp_counter','a733_cpu_coherence_publish','a733_rtc_session_valid','a733_smp_contract_valid',
           'a733_smp_build_id','board','board_live','board_fail','rtc','gicv3_spi_mask']
    symbols={}
    for name in names:
        assert len(candidates[name])==1,(name,candidates[name]);symbols[name]=next(iter(candidates[name]))
    assert all(name not in candidates for name in ('a733_watchdog_hw_admit','a733_watchdog_hw_reload'))
    for s,fields in {'rtc_application':['session','driver','result','cancel','armed','pending','output','digest','spi'],
                     'a733_rtc':['irq_fault'],'gicv3_spi':['distributor','intid']}.items():
        assert all(k in layouts[s] for k in fields),s
    identity=next(data[symbols['a733_smp_build_id']-address:symbols['a733_smp_build_id']-address+32] for address,data in segments if address<=symbols['a733_smp_build_id']<=address+len(data)-32)
    assert len(identity)==32
    (out/'native-preflight.json').write_text(json.dumps({'symbols':symbols,'layouts':layouts,'build_id':identity.hex(),'native_instructions_executed':False},indent=2)+'\n')
    return symbols,segments,layouts,identity

class Machine:
    def __init__(self,image,out,source,fail=0):
        self.symbols,self.segments,self.layouts,self.identity=setup(image,out)
        self.cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        for address,size in [(0x40000000,0x4000000),(0x50000000,0x1000000),(0x03400000,0x10000),(0x07010000,0x1000),(0x07090000,0x1000)]:self.cpu.mem_map(address,size)
        for address,data in self.segments:self.cpu.mem_write(address,data)
        self.publications=0;self.fail=fail;self.live=1;self.counter=100;self.masks=0;self.fatals=0;self.done=False;self.critical=False;self.defer_irq=False;self.race=False;self.mask_model=True
        self.session=bytearray((out/'session-template.bin').read_bytes());self.session[48:80]=self.identity
        self.cpu.mem_write(0x41f00000,bytes(self.session));self.cpu.mem_write(0x0701020c,struct.pack('<I',0xa5000000))
        prepare=module('rtc_case_prepare',source/'apps/rtc/prepare.py');prepare.session_check(self.session)
        epoch,generation,lease=struct.unpack_from('<3Q',self.session,24)
        _,contract=prepare.request_bytes(self.symbols['a733_smp_build_id'],self.identity,self.session,epoch,generation,lease)
        self.contract=bytearray(contract);struct.pack_into('<Q',self.contract,0,prepare.CONTRACT_MAGIC)
        struct.pack_into('<4Q',self.contract,80,24,24000024,1440000024,1440000024)
        self.cpu.mem_write(0x43200000,bytes(self.contract));self.cpu.hook_add(UC_HOOK_CODE,self.hook);self.cpu.hook_add(UC_HOOK_MEM_WRITE,self.write)
        assert self.call('a733_rtc_session_valid',0x41f00000)==1
        assert self.call('a733_smp_contract_valid',0x43200000,100)==1
    def ret(self,value):
        self.cpu.reg_write(UC_ARM64_REG_X0,value);self.cpu.reg_write(UC_ARM64_REG_PC,self.cpu.reg_read(UC_ARM64_REG_LR))
    def hook(self,cpu,address,size,unused):
        if address==0x50fff000:self.done=True;cpu.emu_stop();return
        s=self.symbols
        if address==s['a733_smp_counter']:self.ret(self.counter);return
        if address==s['board_live']:self.ret(self.live);return
        if address==s['board_fail']:self.fatals+=1;self.live=0;self.ret(0);return
        if address==s['a733_cpu_coherence_publish']:
            self.publications+=1;self.ret(0 if self.publications==self.fail else 1);return
        if address==s['gicv3_spi_mask']:
            pointer=cpu.reg_read(UC_ARM64_REG_X0);binding=bytes(cpu.mem_read(pointer,12))
            assert struct.unpack('<QI',binding)==(0x03400000,228)
            self.masks+=1
            if self.mask_model:self.ret(1);return
        if address==s['vTaskEnterCritical']:assert not self.critical;self.critical=True;self.ret(0);return
        if address==s['vTaskExitCritical']:
            assert self.critical;self.critical=False
            if self.defer_irq:
                self.defer_irq=False;cpu.reg_write(UC_ARM64_REG_PC,s['rtc_application_irq']);return
            self.ret(0);return
        # Boundaries above take precedence over MRS instructions at function entry.
        word=int.from_bytes(cpu.mem_read(address,4),'little');op=word&0xffffffe0
        if op in (0xd5384240,0xd53800a0):
            cpu.reg_write(UC_ARM64_REG_X0+(word&31),12 if op==0xd5384240 else 0);cpu.reg_write(UC_ARM64_REG_PC,address+4)
    def write(self,cpu,access,address,size,value,unused):
        if not self.race:return
        if address==0x0340011c and value&16:
            assert self.critical,'Immediate IRQ could interrupt enable/readback'
            self.defer_irq=True
        if address==0x0340019c and value&16:self.word(0x0340011c,0)
    def bind_irq(self):
        base=self.symbols['rtc'];layout=self.layouts['rtc_application'];spi=self.layouts['gicv3_spi']
        self.cpu.mem_write(base+layout['spi']+spi['distributor'],struct.pack('<Q',0x03400000))
        self.word(base+layout['spi']+spi['intid'],228)
        self.cpu.mem_write(self.symbols['board']+self.layouts['a733_board_session']['deadline_us'],struct.pack('<Q',1000000))
    def call(self,name,*args):
        for i,value in enumerate(args):self.cpu.reg_write(UC_ARM64_REG_X0+i,value)
        self.cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);self.cpu.reg_write(UC_ARM64_REG_LR,0x50fff000);self.done=False
        self.cpu.emu_start(self.symbols[name],0,count=1000000);assert self.done,name
        return self.cpu.reg_read(UC_ARM64_REG_X0)
    def word(self,address,value=None):
        if value is not None:self.cpu.mem_write(address,struct.pack('<I',value))
        return int.from_bytes(self.cpu.mem_read(address,4),'little')

def run(out,source,step):
    image=out/'build/sunstone-a733-rtc.elf'
    m=Machine(image,out,source)
    (out/'native-validator-preflight.json').write_text(json.dumps({'passed':True,'session_sha256':hashlib.sha256(m.session).hexdigest(),'linked_build_id':m.identity.hex(),'authority':'Explicit software model; nondeployable.'},indent=2)+'\n')
    for variant in ('retained','tamper','inherited-enabled','exposed-pending','publication-failure','route-publication-failure'):
        label='native-seal-'+variant
        if step(label):continue
        m=Machine(image,out,source,1 if variant=='publication-failure' else 2 if variant=='route-publication-failure' else 0)
        if variant=='tamper':m.word(0x41f00000+760,3)
        if variant=='inherited-enabled':m.word(0x0340011c,16)
        if variant=='exposed-pending':m.word(0x07090030,1)
        early=m.call('rtc_session_prepare',0x43200000)
        expected=variant in ('retained','route-publication-failure')
        assert early==int(expected),(variant,early)
        assert not m.call('rtc_admitted_session')
        if expected:
            m.word(0x03400000,0x33);assert m.call('rtc_boot_route')==int(variant=='retained')
            pointer=m.call('rtc_admitted_session');assert bool(pointer)==(variant=='retained')
            if pointer:
                m.cpu.mem_write(0x41f00000+616,struct.pack('<Q',999));assert bytes(m.cpu.mem_read(pointer,768))==bytes(m.session)
        if variant in ('tamper','inherited-enabled'):assert m.word(0x0701020c)==0xa5000000
        else:assert m.word(0x0701020c)==0xa5010001
        if variant=='exposed-pending':assert m.word(0x07090030)==1
        assert not m.call('rtc_session_prepare',0x43200000)
        step(label,{'publications':m.publications,'bus':m.word(0x0701020c),'inherited_pending':m.word(0x07090030)})
    for variant in ('notification','late','unarmed'):
        label='native-irq-'+variant
        if step(label):continue
        m=Machine(image,out,source);m.bind_irq();base=m.symbols['rtc'];layout=m.layouts['rtc_application']
        m.word(base+layout['armed'],0 if variant=='unarmed' else 1)
        if variant=='late':m.counter=990000*24
        m.call('rtc_application_irq')
        assert m.masks==1 and m.word(base+layout['pending'])==int(variant=='notification')
        assert m.fatals==int(variant!='notification')
        step(label,{'masked_first':True,'notification_only':variant=='notification','fatal':m.fatals})
    label='native-rearm-immediate-irq'
    if not step(label):
        m=Machine(image,out,source);m.bind_irq();m.race=True;m.mask_model=False
        base=m.symbols['rtc'];layout=m.layouts['rtc_application'];m.word(base+layout['armed'],1)
        assert m.call('rtc_enable_irq')==1
        assert not m.critical and not m.defer_irq and m.masks==1
        assert m.word(base+layout['pending'])==1 and not m.word(base+layout['result']) and not m.fatals
        assert not m.word(0x0340011c)
        step(label,{'interleaving':'Owned IRQ raised at ISENABLER write, deferred until critical exit after readback','notification_only':True,'decode_completion':False})
    label='native-private-copy'
    if not step(label):
        m=Machine(image,out,source);base=m.symbols['rtc'];layout=m.layouts['rtc_application']
        for result in (0,2):
            m.word(base+layout['result'],result);m.cpu.mem_write(0x50000000,b'\xa5'*120)
            assert m.call('rtc_copy',0x50000000,0x50000058)==result
            assert bytes(m.cpu.mem_read(0x50000000,120))==bytes(120)
        payload=struct.pack('<22I',1,0,0,0,1970,1,1,1,0,0,2,1970,1,1,1,2,0,0,0x4011,1,1,0)
        m.cpu.mem_write(base+layout['output'],payload);m.cpu.mem_write(base+layout['digest'],hashlib.sha256(payload).digest())
        board_layout=m.layouts['a733_board_session'];m.cpu.mem_write(m.symbols['board']+board_layout['deadline_us'],struct.pack('<Q',1000000))
        m.word(base+layout['result'],1);assert m.call('rtc_copy',0x50000000,0x50000058)==1
        assert bytes(m.cpu.mem_read(0x50000000,88))==payload
        m.live=0;assert m.call('rtc_copy',0x50000000,0x50000058)==2
        assert bytes(m.cpu.mem_read(0x50000000,120))==bytes(120)
        (out/'copied-snapshot.bin').write_bytes(payload);step(label,{'bytes':88,'private_pending_failure':True,'revocation_refusal':True})

    label='protected-packaging'
    if not step(label):
        import shutil
        prepare=module('rtc_packaging',source/'apps/rtc/prepare.py')
        bundle=out/'packaging-model';bundle.mkdir()
        shutil.copy2(image,bundle/'image.elf');shutil.copy2(out/'launcher/launch.bin',bundle/'launch.bin')
        parts,address,identity=prepare.base.image_parts(bundle/'image.elf')
        template=(out/'session-template.bin').read_bytes();raw=bytearray(template);raw[48:80]=identity
        epoch,generation,lease=struct.unpack_from('<3Q',raw,24)
        (bundle/'session-template.bin').write_bytes(template);(bundle/'session.bin').write_bytes(raw)
        (bundle/'admission.json').write_text('{}\n')
        (bundle/'image-input-manifest.json').write_text((out/'image-manifest.json').read_text())
        uploads=[]
        for i,part in enumerate(parts):
            if part['data']:
                name=f'segment-{i}.bin';(bundle/name).write_bytes(part['data']);uploads.append({'name':name,'address':part['address']})
        uploads.append({'name':'session.bin','address':0x41f00000})
        request,contract=prepare.request_bytes(address,identity,raw,epoch,generation,lease)
        for name,data in [('request.bin',request),('contract.bin',contract),('result-zero.bin',bytes(16384)),('report-zero.bin',bytes(128))]:(bundle/name).write_bytes(data)
        record={'schema':prepare.SCHEMA,'physical_access':False,'image_sha256':hashlib.sha256(image.read_bytes()).hexdigest(),
            'build_id':identity.hex(),'epoch':epoch,'generation':generation,'lease':lease,'entry_evidence':{},'segments':uploads,
            'backup_sizes':{name:n for name,a,n in prepare.base.REGIONS},'files':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in bundle.iterdir() if p.is_file()}}
        (bundle/'prepared.json').write_text(json.dumps(record))
        original=prepare.validate_admission;prepare.validate_admission=lambda *args,**kwargs:{}
        try:prepare.validate_bundle(bundle)
        finally:prepare.validate_admission=original
        launch=out/'launcher'
        for name,data in [('request.bin',request),('contract.bin',contract),('session.bin',raw),('identity.bin',identity)]:(launch/name).write_bytes(data)
        (launch/'prepared.json').write_text(json.dumps({'segments':[{'name':'session.bin','address':0x41f00000},{'name':'identity.bin','address':address}]}))
        step(label,{'scope':'Real public ELF/session/contract/upload binding; physical admission is modeled.'})
