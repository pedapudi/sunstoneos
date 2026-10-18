#!/usr/bin/env python3
"""One native DMA observer link and focused client/frame/host composition.

The image has refusal-only handoffs. Instruction execution models system
registers, completed DMA service replies and memory movement. No board is used.
"""
import argparse
import ctypes
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
from elftools.elf.elffile import ELFFile

ROOT=Path(__file__).resolve().parents[1]


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def save(path,value):path.write_text(json.dumps(value,indent=2)+'\n')
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def build(args,out):
    fixture=out/'refusal-handoff.c'
    fixture.write_text('''#include "a733_dma_copy_application.h"
/* Model identities only. Both established masks are zero: no board authority. */
const struct a733_dma_handoff a733_dma_copy_handoff={
    .allowed={.handoff=17},.controller=0x04601000,.evidence=17,
    .owned_channels=65535,.service_channels=3};
const struct a733_dma_copy_usb_handoff a733_dma_copy_usb_handoff={
    .epoch=5,.generation=7,.lease=9,.evidence=19,.dma_evidence=17,.dma_handoff=17};
''')
    commands=[]
    def run(name,command):
        command=list(map(str,command));commands.append(command);save(out/'commands.json',commands)
        result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True)
        (out/(name+'.log')).write_text(result.stdout+result.stderr)
        result.check_returncode()
    b=out/'build'
    run('configure',['cmake','-S',ROOT/'apps/dma-copy','-B',b,
        '-DCMAKE_TOOLCHAIN_FILE='+str(ROOT/'cmake/aarch64-clang.cmake'),
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON','-DCMAKE_C_FLAGS=-Werror',
        '-DFREERTOS_DIR='+str(args.freertos.resolve()),'-DTINYUSB_DIR='+str(args.tinyusb.resolve()),
        '-DSUNSTONE_LLD='+str(args.lld.absolute()),'-DA733_DMA_HANDOFF_SOURCE='+str(fixture)])
    run('build',['cmake','--build',b,'--target','a733-dma-copy','-j4'])
    image=out/'a733-dma-copy.elf';shutil.copy2(b/image.name,image)
    shutil.copy2(b/'a733-dma-copy.map',out/'a733-dma-copy.map')
    inputs={Path(p) for p in (b/'dma-copy/identity-inputs.txt').read_text().splitlines()}
    # Include assembly translation units explicitly; some compilers omit .S.d.
    for entry in json.loads((b/'compile_commands.json').read_text()):
        if 'a733-dma-copy.dir' in entry['command']:inputs.add(Path(entry['file']))
    for dep in (b/'CMakeFiles/a733-dma-copy.dir').rglob('*.obj.d'):
        inputs.update(Path(p) for p in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1]))
    inputs.update([Path(__file__),ROOT/'checks/a733-smp-cached.py',ROOT/'apps/dma-copy/observe.py',
        ROOT/'tools/check-a733-smp-observe.py',ROOT/'tools/check-a733-smp.py',
        ROOT/'cmake/aarch64-clang.cmake',b/'compile_commands.json',b/'dma-copy/identity-inputs.txt'])
    captured={}
    for index,p in enumerate(sorted(p.resolve() for p in inputs)):
        target=out/'inputs'/str(index)/p.name;target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(p,target);captured[str(p)]={'sha256':sha(target),'snapshot':str(target.relative_to(out))}
    save(out/'inputs.json',captured)
    with image.open('rb') as stream:
        elf=ELFFile(stream);symbols={s.name:s for s in elf.get_section_by_name('.symtab').iter_symbols()}
        assert not [s.name for s in symbols.values() if s.name and s['st_shndx']=='SHN_UNDEF']
        layout={name:symbols[name]['st_value'] for name in ('__bss_end','__device_pool_start','__device_pool_end')}
        assert layout['__bss_end']<=layout['__device_pool_start']<layout['__device_pool_end']<=0x41f00000
        assert layout['__device_pool_end']-layout['__device_pool_start']==2097152
    save(out/'layout.json',layout)
    save(out/'image-manifest.json',{'target':'a733-dma-copy','protocol_version':1,'final_elf':sha(image),
        'physical_access':False,'refusal_fixture':True})
    return image


def execute(image,out,launcher_bundle,reuse_preparation=False):
    host=module('dma_host',ROOT/'apps/dma-copy/observe.py')
    model=module('cached_model',ROOT/'checks/a733-smp-cached.py')
    sealed_before=sha(launcher_bundle/'prepared.json')
    bundle=out/'refusal-bundle'
    if not reuse_preparation:
        host.prepare(image,out/'image-manifest.json',bundle,launcher_bundle,refusal=True)
    prepared=host.validate_bundle(bundle)
    assert host.digest(bundle/'launch.bin')==host.digest(launcher_bundle/'launch.bin')
    assert sha(launcher_bundle/'prepared.json')==sealed_before
    args=['launch','--bundle',str(bundle),'--confirm-bridge-stable','--claim','absent','--owner','absent']
    for name in ('fel','qualification','input-evidence','ddr-contract','platform-contract','evidence','database'):
        args.extend(['--'+name,str(out/('unused-'+name))])
    if not reuse_preparation:
        try:host.main(args)
        except ValueError as error:assert 'refusal bundle' in str(error)
        else:raise AssertionError('Refusal bundle reached physical launch')
    stops={}
    class Machine(model.Machine):
        def instruction(self,u,pc,size,c):
            if stops.get(c)==pc:
                u.emu_stop();return
            super().instruction(u,pc,size,c)
    m=Machine(image)
    sym=m.symbols
    scratch=sym['__device_pool_start'];return_pc=sym['a733_smp_entry']
    def write(address,data):
        ctypes.memmove(m.ram_pointer+address-0x41000000,data,len(data))
    def word(address,value):write(address,struct.pack('<Q',value))
    def uint(address,value):write(address,struct.pack('<I',value))
    def init_cpu(c,el=1,sp=None):
        m.regs[c].update(currentel=el*4,spsel=0 if el==0 else 1,daif=0x3c0,tpidr_el1=c,
            sp_el0=sym['a733_cpu_el1_stacks']+(c+1)*32768-4096,
            sp_el1=sym['a733_cpu_el1_stacks']+(c+1)*32768,
            vbar_el1=sym['a733_cached_el1_vectors'])
        m.cpus[c].reg_write(model.UC_ARM64_REG_SP,sp or m.regs[c][m.spname(c)])
    def run(c,entry,stop,limit=500000):
        u=m.cpus[c]
        stops[c]=stop
        try:u.emu_start(entry,0,count=limit)
        finally:stops.pop(c)
        assert u.reg_read(model.UC_ARM64_REG_PC)==stop,hex(u.reg_read(model.UC_ARM64_REG_PC))
    def call(name,*args):
        init_cpu(0)
        u=m.cpus[0]
        for reg,value in zip(model.GPR,args):u.reg_write(reg,value)
        u.reg_write(model.UC_ARM64_REG_LR,return_pc)
        run(0,sym[name],return_pc)
        return u.reg_read(model.UC_ARM64_REG_X0)
    write(sym['a733_observe_identity'],struct.pack('<5Q',5,7,9,24,48000024));m.clock=100
    assert call('a733_dma_copy_observer_admit')==0
    # Model setup only: the disk image retains refusal flags and is never run.
    word(sym['a733_dma_copy_usb_handoff']+48,15)
    assert call('a733_dma_copy_observer_admit')==1
    with image.open('rb') as stream:
        dwarf=ELFFile(stream).get_dwarf_info()
        dies=[d for cu in dwarf.iter_CUs() for d in cu.iter_DIEs()]
        def layout(name,variable=False):
            die=next(d for d in dies if d.attributes.get('DW_AT_name') and
                d.attributes['DW_AT_name'].value.decode()==name and
                d.tag==('DW_TAG_variable' if variable else 'DW_TAG_structure_type'))
            if variable:die=die.get_DIE_from_attribute('DW_AT_type')
            fields={d.attributes['DW_AT_name'].value.decode():d.attributes['DW_AT_data_member_location'].value
                for d in die.iter_children() if d.tag=='DW_TAG_member'}
            return die.attributes['DW_AT_byte_size'].value,fields
        _,view=layout('client_view',True);reply_size,reply=layout('dma_service_reply')
    u=m.cpus[1];init_cpu(1,el=0)
    source=[scratch+4096,scratch+8192];destination=[scratch+12288,scratch+16384]
    address=sym['client_view'];state=scratch+20480
    uint(state,2) # A733_DMA_POOL_READY, after the existing service composition.
    word(address+view['state'],state);uint(address+view['count'],2);uint(address+view['bytes'],1024)
    for i in range(2):
        uint(address+view['channels']+4*i,i)
        word(address+view['source']+8*i,source[i]);word(address+view['destination']+8*i,destination[i])
    operations=[];pending=[None];pause=[True];corrupt=[False]
    def service_boundary(cpu,pc,size,unused):
        if pc==sym['dma_service_send']:
            req=cpu.reg_read(model.UC_ARM64_REG_X1)
            op,job=struct.unpack('<2I',m.read(req,8))
            if op==1 and job==1 and pause[0]:pause[0]=False;cpu.emu_stop();return
            operations.append([op,job]);pending[0]=(op,job)
            word(cpu.reg_read(model.UC_ARM64_REG_X3),len(operations))
            cpu.reg_write(model.UC_ARM64_REG_X0,1);cpu.reg_write(model.UC_ARM64_REG_PC,cpu.reg_read(model.UC_ARM64_REG_LR))
        elif pc==sym['dma_service_collect']:
            op,job=pending[0];dest=cpu.reg_read(model.UC_ARM64_REG_X2)
            write(dest,bytes(reply_size))
            uint(dest+reply['phase'],{1:2,2:5,4:2,5:0}[op])
            if op==2:
                write(destination[job],m.read(source[job],1024))
                if corrupt[0]:write(destination[job],bytes([m.read(destination[job],1)[0]^1]))
                uint(dest+reply['outcome'],1);uint(dest+reply['holds_buffers'],1)
            if op==4:
                assert m.read(destination[job],4096)==m.read(source[job],1024)+bytes([0xa5])*3072
            cpu.reg_write(model.UC_ARM64_REG_X0,1);cpu.reg_write(model.UC_ARM64_REG_PC,cpu.reg_read(model.UC_ARM64_REG_LR))
        elif pc==sym['platform_panic'] and not corrupt[0]:raise AssertionError('Native client assertion failed')
    hook=u.hook_add(model.UC_HOOK_CODE,service_boundary)
    challenge=hashlib.sha256(b'bounded DMA observer composition').digest()
    write(scratch+512,challenge)
    def frame(sequence):
        request=host.request(sequence,prepared,challenge);write(scratch+1024,request)
        assert call('a733_dma_copy_request',scratch+1024,sym['a733_observe_identity'],scratch+1100,scratch+1152)==1
        assert m.read(scratch+1152,32)==challenge
        assert call('a733_dma_copy_frame',scratch,sym['a733_observe_identity'],scratch+512,sym['a733_smp_build_id'])==1
        return struct.pack('<4I',host.MESSAGE_MAGIC,sequence,0x80000032,512)+m.read(scratch,512)
    try:
        u.emu_start(sym['client'],0,count=500000)
        assert u.reg_read(model.UC_ARM64_REG_PC)==sym['dma_service_send'] and not pause[0]
        assert struct.unpack('<2I',m.read(sym['a733_dma_copy_result'],8))==(1,0)
        first=frame(1);assert host.response(first,1,prepared,challenge)['status']=='pending'
        m.clock=200
        run(1,u.reg_read(model.UC_ARM64_REG_PC),sym['vTaskSuspend'])
    finally:u.hook_del(hook)
    assert operations==[[op,job] for job in range(2) for op in (1,2,4,5)]
    assert struct.unpack('<2I',m.read(sym['a733_dma_copy_result'],8))==(3,1)
    second=frame(2);assert host.response(second,2,prepared,challenge)['status']=='success'
    uint(sym['a733_dma_copy_result'],1)
    assert call('a733_dma_copy_frame',scratch,sym['a733_observe_identity'],scratch+512,sym['a733_smp_build_id'])==0
    uint(sym['a733_dma_copy_result'],3)
    completed_operations=operations.copy()
    write(sym['a733_dma_copy_result'],bytes(24));operations.clear();corrupt[0]=True
    init_cpu(1,el=0)
    hook=u.hook_add(model.UC_HOOK_CODE,service_boundary)
    try:run(1,sym['client'],sym['platform_panic'])
    finally:u.hook_del(hook)
    assert struct.unpack('<2I',m.read(sym['a733_dma_copy_result'],8))==(0,0)
    assert operations==[[1,0],[2,0]]
    # New SVC route: an EL1t impostor faults; the exact EL0 owner reaches the
    # existing USB initializer. USB MMIO and transport are not executed here.
    def svc(el,owner,target):
        init_cpu(0,el=el)
        m.regs[0].update(spsel=0,sp_el0=sym['task_stack']+2048*8)
        m.cpus[0].reg_write(model.UC_ARM64_REG_SP,m.regs[0]['sp_el0'])
        word(sym['pxCurrentTCBs'],sym['task_state'] if owner else 0)
        run(0,sym['a733_observe_svc'],sym[target],2000)
    svc(1,False,'a733_observe_failure')
    svc(0,True,'tusb_rhport_init')
    capture=out/'capture';capture.mkdir()
    record={'dma_result':'unobserved'};frames=iter((first,second));saves=[]
    def exchange(packet,timeout):
        sequence=struct.unpack_from('<I',packet,4)[0]
        assert packet==host.request(sequence,prepared,challenge)
        return next(frames)
    def saved():
        if record['dma_result']=='observed-success':
            item=record['durable_result'];assert sha(capture/item['path'])==item['sha256']
        saves.append(json.loads(json.dumps(record)))
    host.capture(exchange,prepared,challenge,capture,record,saved,10,now=lambda:1,pause=lambda _:None)
    assert record['dma_result']=='observed-success' and 'scheduler_result' not in record
    wrong=bytearray(first);struct.pack_into('<Q',wrong,16+144,7)
    wrong[16+480:]=hashlib.sha256(wrong[16:16+480]).digest()
    attempts=[];bad={'dma_result':'unobserved'}
    try:host.capture(lambda *args:attempts.append(1) or bytes(wrong),prepared,challenge,capture,bad,lambda:None,10,now=lambda:1)
    except ValueError:pass
    else:raise AssertionError('Malformed DMA profile accepted')
    assert len(attempts)==1 and bad['dma_result']=='unobserved'
    return {'passed':True,'native_client_operations':completed_operations,'payload_failure_operations':operations,'native_frames':2,
        'sealed_launcher_sha256':sha(launcher_bundle/'launch.bin'),'sealed_bundle_sha256':sealed_before,
        'modeled_inputs':'Only memory copies/completed service replies, architectural registers and exception entry are modeled. USB admission flags are set only in model memory after testing refusal.',
        'limits':'No physical boot/coherence/IRQ/USB/DMA or watchdog qualification; no scheduler or prior DMA protocol replay.'}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('freertos','tinyusb','lld','launcher-bundle','evidence'):
        parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();out=args.evidence.resolve();out.mkdir(parents=True,exist_ok=False)
    try:
        image=build(args,out)
        result=execute(image,out,args.launcher_bundle.resolve())
        save(out/'result.json',result)
        save(out/'artifacts.json',{str(p.relative_to(out)):sha(p) for p in out.rglob('*')
            if p.is_file() and ('build' not in p.relative_to(out).parts)})
    except Exception as error:
        (out/'failure.txt').write_text(repr(error)+'\n');raise
    print('PASS native DMA client publication, USB frame/privilege routing and durable host capture')


if __name__=='__main__':main()
