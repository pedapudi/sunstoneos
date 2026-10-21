#!/usr/bin/env python3
"""Check linked NPU interrupt routing and ownership without accessing hardware."""
import argparse
import hashlib
import json
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PSTATE

D=0x03400000
STOP=0x50fff000
SNAP=0x50010000
SERVICE=0x50020000
CASES=['route-roundtrip','capture-enabled','capture-pending','capture-active','capture-lpi',
       'install-ignored','restore-active','expired','lifecycle','rearm-ignored',
       'stop-before-close','stop-pending','dispatch','unexpected-irq','bind-priority','mask-ignored',
       'monitor-return','monitor-pending']

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--case',action='append',choices=CASES)
    a=p.parse_args();digest=hashlib.sha256(a.image.read_bytes()).hexdigest()
    with a.image.open('rb') as f:
        elf=ELFFile(f)
        parts=[(int(x['p_vaddr']),x.data()) for x in elf.iter_segments() if x['p_type']=='PT_LOAD']
        symbols={};locals={};unit=''
        for s in elf.get_section_by_name('.symtab').iter_symbols():
            if s['st_info']['type']=='STT_FILE':unit=s.name
            if s['st_value']:
                symbols.setdefault(s.name,set()).add(int(s['st_value']))
                if unit=='irq.c':locals[s.name]=int(s['st_value'])
        offsets=set();layouts={}
        for cu in elf.get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                if die.tag=='DW_TAG_structure_type' and die.attributes.get('DW_AT_name') and die.attributes['DW_AT_name'].value==b'a733_npu_controller':
                    for child in die.iter_children():
                        if child.tag=='DW_TAG_member' and child.attributes['DW_AT_name'].value==b'state':
                            offsets.add(child.attributes['DW_AT_data_member_location'].value)
                if die.tag=='DW_TAG_structure_type':
                    members={child.attributes['DW_AT_name'].value.decode():child.attributes['DW_AT_data_member_location'].value for child in die.iter_children() if child.tag=='DW_TAG_member' and 'DW_AT_data_member_location' in child.attributes and 'DW_AT_name' in child.attributes}
                    name=die.attributes.get('DW_AT_name')
                    if name and name.value in (b'npu_finite_receipt',b'a733_smp_contract'):layouts[name.value.decode()]=members
                    if 'npu_irq' in members and 'spi_enabled' in members:layouts['saved']=members
        assert len(offsets)==1,offsets
        state_offset=offsets.pop()
    def addr(name):
        values=symbols.get(name,set());assert len(values)==1,(name,values)
        return next(iter(values))
    # Resolve every hook before any case; local callback names use their object.
    names=['npu_irq_route_capture','npu_irq_route_install','npu_irq_route_restore',
           'npu_application_irq_bind','npu_application_irq','vApplicationFPUSafeIRQHandler',
           'vPortEnterCritical','vPortExitCritical','uxPortSetInterruptMask','vPortClearInterruptMask',
           'xTaskGetSchedulerState','npu_service_bind_irq','npu_service_irq_notify_from_isr','platform_panic',
           'sd_single_core_exception','a733_smp_stop','npu_finite_rom_return','npu_finite_receipt','admission','saved','__npu_clean_start','__device_pool_end']
    syms={n:addr(n) for n in names}
    for n in ['mask','rearm','stop','binding']:assert n in locals,n
    a.output.mkdir(parents=True,exist_ok=True)
    for case in a.case or CASES:
        cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        for base,size in [(D,0x10000),(0x03460000,0x20000),(0x07050000,0x10000),(0x41000000,0x1200000),(0x50000000,0x1000000)]:cpu.mem_map(base,size)
        for base,data in parts:
            assert 0x41000000<=base and base+len(data)<=0x42200000
            if data:cpu.mem_write(base,data)
        regs={D:0x30,D+4:7,0x03460000:0,D+0x8c:0x55550000,D+0xd0c:0xaaaa0002,
              D+0xc18:0xaaaaaaa8,D+0x460:0x18382858,D+0x6308:0x10000}
        original=dict(regs);writes=[];calls=[];clock=10;ignored=None;panic=False;current_el=1;terminal=None
        def read(uc,access,address,size,value,unused):
            uc.mem_write(address,regs.get(address,0).to_bytes(size,'little'))
        def write(uc,access,address,size,value,unused):
            writes.append((address,size,value))
            if address==ignored:return
            if address==D+0x10c:regs[D+0x10c]=regs.get(D+0x10c,0)|value
            elif address==D+0x18c:regs[D+0x10c]=regs.get(D+0x10c,0)&~value
            elif address==D+0x28c:regs[D+0x20c]=regs.get(D+0x20c,0)&~value
            else:regs[address]=value
        def ret(value=0):
            cpu.reg_write(UC_ARM64_REG_X0,value);cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR))
        def code(uc,address,size,unused):
            nonlocal clock,panic,terminal
            ins=int.from_bytes(uc.mem_read(address,4),'little')
            if case.startswith('monitor-'):
                if address in (syms['a733_smp_stop'],syms['npu_finite_rom_return']):
                    terminal='retained' if address==syms['a733_smp_stop'] else 'returned';uc.emu_stop();return
                kind=ins&0xffffffe0
                if kind in (0xd51fe220,0xd51be220,0xd51be320):uc.reg_write(UC_ARM64_REG_PC,address+4);return
                if kind in (0xd53e5200,0xd5381000):
                    uc.reg_write(UC_ARM64_REG_X0+(ins&31),(0x17<<26) if kind==0xd53e5200 else 0);uc.reg_write(UC_ARM64_REG_PC,address+4);return
            if ins&0xffffffe0==0xd5384240:  # Unicorn runs at EL1; model the selected entry's EL.
                uc.reg_write(UC_ARM64_REG_X0+(ins&31),current_el*4);uc.reg_write(UC_ARM64_REG_PC,address+4);return
            if ins&0xffffffe0==0xd53be020:  # CNTPCT_EL0, deterministic deadline only.
                uc.reg_write(UC_ARM64_REG_X0+(ins&31),clock);clock+=1;uc.reg_write(UC_ARM64_REG_PC,address+4);return
            if address==syms['platform_panic']:
                panic=True;uc.emu_stop();return
            if address==syms['xTaskGetSchedulerState']:ret(1);return  # taskSCHEDULER_NOT_STARTED
            if address in [syms[n] for n in ['vPortEnterCritical','vPortExitCritical','uxPortSetInterruptMask','vPortClearInterruptMask']]:ret();return
            if address==syms['npu_service_bind_irq']:
                assert uc.reg_read(UC_ARM64_REG_X0)==SERVICE
                assert uc.reg_read(UC_ARM64_REG_X2)==locals['binding']
                assert uc.reg_read(UC_ARM64_REG_X3)==1  # Route and worker on CPU0 only.
                ops=uc.reg_read(UC_ARM64_REG_X1)
                assert [int.from_bytes(uc.mem_read(ops+i*8,8),'little') for i in range(3)]==[locals[n] for n in ['mask','rearm','stop']]
                ret(1);return
            if address==syms['npu_service_irq_notify_from_isr']:
                calls.append(('notify',uc.reg_read(UC_ARM64_REG_X1)));ret();return
            if case=='dispatch' and address==syms['npu_application_irq']:
                calls.append(('dispatch',97));ret()
        cpu.hook_add(UC_HOOK_MEM_READ,read,begin=D,end=D+0x7ffff)
        cpu.hook_add(UC_HOOK_MEM_WRITE,write,begin=D,end=D+0x7ffff)
        cpu.hook_add(UC_HOOK_CODE,code)
        def call(name,x0=0,x1=0,el=1):
            nonlocal current_el
            current_el=el
            cpu.reg_write(UC_ARM64_REG_PSTATE,13 if el==3 else 5)
            cpu.reg_write(UC_ARM64_REG_SP,0x50ffe000);cpu.reg_write(UC_ARM64_REG_LR,STOP)
            cpu.reg_write(UC_ARM64_REG_X0,x0);cpu.reg_write(UC_ARM64_REG_X1,x1)
            cpu.emu_start(syms.get(name,locals.get(name)),STOP,count=30000)
            assert panic or terminal or cpu.reg_read(UC_ARM64_REG_PC)==STOP,'instruction limit'
            value=cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff
            return value if value<0x80000000 else value-(1<<32)
        def route(name):return call('npu_irq_route_'+name,SNAP,100000,el=3)
        if case.startswith('monitor-'):
            assert route('capture')==1 and route('install')==1
            def put(address,value,size=8):cpu.mem_write(address,value.to_bytes(size,'little'))
            saved=syms['saved'];sl=layouts['saved']
            for name,value in [('ready',1),('banks',4),('ctlr',0x33)]:put(saved+sl[name],value,4)
            put(saved+sl['spi_enabled']+4,0x100,4)
            cpu.mem_write(saved+sl['npu_irq'],bytes(cpu.mem_read(SNAP,32)))
            for name,value in [('epoch',7),('generation',8),('lease',9),('diagnostic_deadline',100000)]:put(syms['admission']+layouts['a733_smp_contract'][name],value)
            start,end=syms['__npu_clean_start'],syms['__device_pool_end']
            # Receipt fields are those published by unchanged return.c after quiescence.
            for name,value in [('magic',0x4e50555245543031),('epoch',7),('generation',8),('lease',9),('complete',1),('clean_start',start),('clean_end',end),('clean_lines',(end-start)//64),('deadline',100000)]:put(syms['npu_finite_receipt']+layouts['npu_finite_receipt'][name],value)
            if case=='monitor-pending':regs[D+0x20c]=2
            before=len(writes);cpu.reg_write(UC_ARM64_REG_X2,1)
            call('sd_single_core_exception',0x733,0,el=3)
            assert terminal==('retained' if case=='monitor-pending' else 'returned'),terminal
            tail=writes[before:]
            if case=='monitor-return':
                route_write=next(i for i,x in enumerate(tail) if x[0]==D+0x6308)
                first_enable=next(i for i,x in enumerate(tail) if D+0x104<=x[0]<=D+0x10c)
                assert route_write<first_enable and regs[D+0x6308]==original[D+0x6308]
                assert all(regs[D+o]==original[D+o] for o in [0x8c,0xd0c,0xc18,0x460])
            else:assert not any(D+0x104<=x[0]<=D+0x10c or x[0]==D+0x6308 for x in tail)
        elif case.startswith('capture-') or case=='expired':
            if case=='expired':clock=100001
            else:
                location={'enabled':D+0x10c,'pending':D+0x20c,'active':D+0x30c,'lpi':0x03460000}[case[8:]]
                regs[location]=1 if case=='capture-lpi' else 2
            assert route('capture')==0 and not writes
        elif case in ['route-roundtrip','install-ignored','restore-active']:
            assert route('capture')==1 and not writes
            if case=='install-ignored':ignored=D+0x460
            assert route('install')==(0 if ignored else 1)
            if not ignored:
                expected={D+0x8c:(original[D+0x8c]|2),D+0xd0c:(original[D+0xd0c]&~2),D+0xc18:(original[D+0xc18]&~12),D+0x460:(original[D+0x460]&~0xff00)|0xd800,D+0x6308:0}
                assert all(regs[k]==v for k,v in expected.items())
                if case=='restore-active':
                    regs[D+0x30c]=2;before=len(writes);assert route('restore')==0 and len(writes)==before
                else:
                    # Another disabled source's fields may have changed since capture.
                    for offset in [0x8c,0xd0c,0xc18,0x460]:regs[D+offset]^=0x10000000
                    assert route('restore')==1
                    assert all(regs[D+o]==(original[D+o]^0x10000000) for o in [0x8c,0xd0c,0xc18,0x460])
                    assert regs[D+0x6308]==original[D+0x6308]
        elif case=='dispatch':
            call('vApplicationFPUSafeIRQHandler',97);assert calls==[('dispatch',97)] and not writes
        elif case=='unexpected-irq':
            call('npu_application_irq');assert panic and not calls and writes==[(D+0x18c,4,2)]
        else:
            # Nonsecure view: IGROUPR is RAZ/WI and d8 Secure priority reads b0.
            regs.update({D+0x8c:0,D+0xc18:0,D+0x460:0xb000,D+0x6308:0})
            if case=='bind-priority':regs[D+0x460]=0
            assert call('npu_application_irq_bind',SERVICE)==(0 if case=='bind-priority' else 1)
            if case!='bind-priority':
                cookie=locals['binding']
                if case=='rearm-ignored':ignored=D+0x10c
                regs[D+0x20c]=2
                assert call('rearm',cookie)==(-1 if ignored else 1)
                assert regs[D+0x20c]==2  # Rearm must preserve an arrived event.
                if case=='rearm-ignored':assert call('mask',cookie)==-1 and writes[-1]==(D+0x18c,4,2)
                elif case=='mask-ignored':
                    ignored=D+0x18c;assert call('mask',cookie)==-1
                else:
                    regs[D+0x30c]=2
                    call('npu_application_irq');assert calls==[('notify',1)]
                    assert call('mask',cookie)==0
                    regs[D+0x30c]=0;regs[D]|=1<<31;assert call('mask',cookie)==0
                    regs[D]&=~(1<<31);assert call('mask',cookie)==1
                    before=len(writes)
                    if case=='stop-before-close':assert call('stop',cookie)==-1 and len(writes)==before
                    else:
                        cpu.mem_write(SERVICE+state_offset,(9).to_bytes(4,'little'))
                        if case=='stop-pending':ignored=D+0x28c
                        assert call('stop',cookie)==(-1 if ignored else 1)
                        assert call('rearm',cookie)==-1
                        assert writes[-1]==(D+0x28c,4,2)
        result={'case':case,'passed':True,'image_sha256':digest,'physical_access':False,'writes':writes,'calls':calls,'terminal':terminal,
                'scope':'Actual linked route/callback/dispatch instructions. GIC register propagation, CurrentEL and deadlines modeled; scheduler critical sections, service binding/notification substituted. No hardware interrupt, privilege transition, timing or full return evidence.'}
        (a.output/(case+'.json')).write_text(json.dumps(result,indent=2)+'\n')
        print(case,'PASS')
if __name__=='__main__':main()
