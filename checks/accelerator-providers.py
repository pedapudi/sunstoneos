#!/usr/bin/env python3
"""Actual compact allocations and provider binding after modeled root admission.

Root's separately accepted sealed session is copied byte-for-byte into retained
storage. CPU admission, translations/cache primitives, IRQ binding and FreeRTOS
constructors are modeled; no startup, CPU architecture or device I/O executes.
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
        self.image=image;self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.u.mem_map(0x40000000,0x04000000)
        self.u.reg_write(UC_ARM64_REG_PSTATE,0x3c5)
        for at,data in image.parts:
            if data:self.u.mem_write(at,data)
        self.ticks=struct.unpack_from('<Q',contract,80)[0]+100;self.scheduler=1;self.task=0
        self.tasks=[];self.translations=[];self.callname='';self.clocks=[]
        self.vars=variables(path,'/apps/npu-convolution/native.c')
        self.app,self.af=self.vars['app'];self.g=image.address('accelerator_gpu');self.gl=image.layouts['accelerator_gpu']
        retained=variables(path,'/apps/accelerators/admission.c')
        self.sealed=retained['session'][0];self.u.mem_write(self.sealed,session);self.put(image.address('copied'),1,4)
        identity=image.address('a733_observe_identity');layout=image.layouts['a733_observe_identity']
        for key,offset in [('epoch',24),('generation',32),('lease',112),('armed_at',80),('deadline',104)]:self.put(identity+layout[key],struct.unpack_from('<Q',contract,offset)[0])
        names=['a733_smp_counter','a733_smp_device_preparation_admitted','aarch64_device_translate',
               'aarch64_device_cache_stride','aarch64_device_cache','a733_counter_clock_init','a733_counter_clock_now',
               'xTaskGetSchedulerState','xTaskGetCurrentTaskHandle','xTaskCreateStatic','xQueueGenericCreateStatic',
               'xQueueCreateMutexStatic','vTaskEnterCritical','vTaskExitCritical','npu_application_irq_bind','platform_panic']
        self.hooks={image.address(n):n for n in names if image.symbols.get(n)}
        self.hooks[image.address('digest',unit='/drivers/npu/npu_convolution.c')]='modeled_digest'
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
            attr=0x44 if 0x41c00000<=a[0]<0x41e00000 else 0xff if a[0]>=0x40000000 else 0
            self.u.mem_write(a[2],struct.pack('<QII',a[0],attr,3 if attr else 2));self.translations.append(a[:2]);self.ret(1)
        elif name=='modeled_digest':self.u.mem_write(a[2],hashlib.sha256(bytes(self.u.mem_read(a[0],a[1]))).digest());self.ret()
        elif name=='aarch64_device_cache_stride':self.ret(64)
        elif name=='aarch64_device_cache':self.ret(1)
        elif name=='a733_counter_clock_init':
            l=self.image.layouts['a733_counter_clock'];self.put(a[0]+l['frequency'],24000000,4);self.put(a[0]+l['initialized'],1,4);self.clocks.append(a[0]);self.ret(1)
        elif name=='a733_counter_clock_now':self.put(a[1],self.ticks//24);self.ret(1)
        elif name=='xTaskGetSchedulerState':self.ret(self.scheduler)
        elif name=='xTaskGetCurrentTaskHandle':self.ret(self.task)
        elif name=='xTaskCreateStatic':self.tasks.append(a[:7]);self.ret(a[6])
        elif name=='xQueueGenericCreateStatic':self.ret(a[3])
        elif name=='xQueueCreateMutexStatic':self.ret(a[1])
        elif name in ('vTaskEnterCritical','vTaskExitCritical'):self.ret()
        elif name=='npu_application_irq_bind':self.ret(1)
        else:raise AssertionError(('Unexpected boundary',name,self.callname,a))
    def call(self,name,*args):
        self.callname=name;self.u.reg_write(UC_ARM64_REG_SP,0x43fd0000);self.u.reg_write(UC_ARM64_REG_LR,0x43fff000)
        for i,value in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+i,value)
        self.u.emu_start(self.image.address(name),0,count=240000000)
        assert self.u.reg_read(UC_ARM64_REG_PC)==0x43fff000,(name,hex(self.u.reg_read(UC_ARM64_REG_PC)))
        return self.u.reg_read(UC_ARM64_REG_X0)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','session','contract','output'):parser.add_argument('--'+name,type=Path,required=True)
    a=parser.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    image=base.Image(a.image);raw=a.session.read_bytes();contract=a.contract.read_bytes();m=Machine(image,a.image,raw,contract)
    sealed=m.sealed;assert m.call('accelerator_session_valid',sealed)==1
    assert m.call('npu_session_get'), ('missing retained copy',m.get(image.address('copied'),4))
    client=m.call('accelerator_npu_client_create');result=m.call('a733_npu_application_init',client)
    assert client and result==1,dict(client=client,scheduler=m.scheduler,tasks=m.tasks,result=result,failed=m.get(image.address('a733_npu_application_failed'),4),used=m.get(m.app+m.af['used'],4),trace=m.trace[-30:])
    assert m.call('accelerator_gpu_prepare')==1
    view=0x43e00000;assert m.call('accelerator_providers_get',view)==1
    p=image.layouts['accelerator_providers'];res=m.app+m.af['resources'];rs=image.layouts['a733_npu_startup_resources']
    expected=dict(counter=m.app+m.af['clock'],clocks=m.app+m.af['registry'],locks=m.app+m.af['locks'],
                  gpu=res+rs['manager'],gpu_native=res+rs['native'],pmic=m.app+m.af['pmic'])
    assert all(m.get(view+p[key])==value for key,value in expected.items())
    assert m.clocks==[expected['counter']] and len(m.tasks)==6,dict(clocks=m.clocks,tasks=m.tasks)
    wb=m.call('a733_memory_reservations_init');nc=m.call('a733_memory_uncached_init');pl=image.layouts['device_memory_pool'];al=image.layouts['device_memory_allocation']
    stride=32;allocations={}
    for name,pool in [('wb',wb),('nc',nc)]:
        rows=[]
        for i in range(32):
            slot=pool+pl['allocations']+i*stride
            owner=m.get(slot+al['owner'])
            if owner:rows.append(dict(owner=owner,first=m.get(slot+al['first'],4),pages=m.get(slot+al['pages'],4),state=m.get(slot+al['state'],4)))
        allocations[name]=rows
    assert [r['pages'] for r in allocations['wb']]==[790,34,88,12],allocations
    assert [r['pages'] for r in allocations['nc']]==[45,6],allocations
    assert [r['owner'] for r in allocations['wb']]==[1,9,10,10]
    assert m.call('accelerator_shared_live')==1
    snapshot=bytes(m.u.mem_read(0x41200000,0xc00000));cases={}
    for name,field,typ,member in [('counter','clock','a733_counter_clock','failed'),
                                  ('registry','registry','a733_clock_leases','failed'),
                                  ('lock','locks','a733_platform_locks','retained'),
                                  ('manager','resources','a733_npu_startup_resources',None)]:
        branch=Machine(image,a.image,raw,contract);branch.u.mem_write(0x41200000,snapshot)
        address=m.app+m.af[field]+(rs['manager']+image.layouts['a733_gpu_resource_manager']['failed'] if member is None else image.layouts[typ][member])
        branch.put(address,1,4)
        assert branch.call('accelerator_shared_live')==0
        assert branch.get(image.address('a733_npu_application_failed'),4) and branch.get(m.g+m.gl['failed'],4)
        cases[name+'-fault-revokes-both']=True
    record=dict(passed=True,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),session_sha256=hashlib.sha256(raw).hexdigest(),
                actual_allocations=allocations,exact_provider_view={k:hex(v) for k,v in expected.items()},created_tasks=len(m.tasks),
                cases=cases,physical_access=False,scope=__doc__)
    (a.output/'result.json').write_text(json.dumps(record,indent=2)+'\n');print('PASS actual combined allocations and borrowed provider identity')
if __name__=='__main__':main()
