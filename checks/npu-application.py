#!/usr/bin/env python3
"""One native NPU application link and focused constructor/client/frame/host composition.

The image has refusal-only handoffs. Instruction execution models system
registers, NPU service replies and copied reference output. No board is used.
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
import sys
from elftools.elf.elffile import ELFFile

ROOT=Path(__file__).resolve().parents[1]


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def save(path,value):path.write_text(json.dumps(value,indent=2)+'\n')
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def build(args,out):
    fixture=ROOT/'checks/npu-application.c'
    commands=[]
    def run(name,command):
        command=list(map(str,command));commands.append(command);save(out/'commands.json',commands)
        result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True)
        (out/(name+'.log')).write_text(result.stdout+result.stderr)
        result.check_returncode()
    b=out/'build'
    run('configure',['cmake','-S',ROOT/'apps/npu-convolution','-B',b,
        '-DCMAKE_TOOLCHAIN_FILE='+str(ROOT/'cmake/aarch64-clang.cmake'),
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON','-DCMAKE_C_FLAGS=-Werror',
        '-DFREERTOS_DIR='+str(args.freertos.resolve()),'-DTINYUSB_DIR='+str(args.tinyusb.resolve()),
        '-DSUNSTONE_LLD='+str(args.lld.absolute()),'-DA733_NPU_HANDOFF_SOURCE='+str(fixture),
        '-DA733_NPU_MODEL='+str(args.artifacts/'fixture_model.bin'),
        '-DA733_NPU_INPUT='+str(args.artifacts/'fixture_input.bin'),
        '-DA733_NPU_REFERENCE='+str(args.artifacts/'fixture_output.bin')])
    run('build',['cmake','--build',b,'--target','a733-npu-convolution','-j4'])
    image=out/'a733-npu-convolution.elf';shutil.copy2(b/image.name,image)
    shutil.copy2(b/'a733-npu-convolution.map',out/'a733-npu-convolution.map')
    inputs={Path(p) for p in (b/'npu-convolution/identity-inputs.txt').read_text().splitlines()}
    # Include assembly translation units explicitly; some compilers omit .S.d.
    for entry in json.loads((b/'compile_commands.json').read_text()):
        if 'a733-npu-convolution.dir' in entry['command']:inputs.add(Path(entry['file']))
    for dep in (b/'CMakeFiles/a733-npu-convolution.dir').rglob('*.obj.d'):
        inputs.update(Path(p) for p in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1]))
    inputs.update([Path(__file__),ROOT/'checks/a733-smp-cached.py',ROOT/'apps/npu-convolution/observe.py',
        ROOT/'tools/check-a733-smp-observe.py',ROOT/'tools/check-a733-smp.py',
        ROOT/'cmake/aarch64-clang.cmake',ROOT/'tools/npu-models.json',ROOT/'drivers/npu/npu_models.inc',b/'compile_commands.json',b/'npu-convolution/identity-inputs.txt'])
    import capstone, unicorn, elftools
    inputs.update([Path(sys.executable),Path(shutil.which('clang')),args.lld,
                   Path(capstone.__file__),Path(unicorn.__file__),Path(elftools.__file__)])
    for package in (capstone,unicorn):
        inputs.update(Path(package.__file__).parent.rglob('*.so*'))
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
    save(out/'image-manifest.json',{'target':'a733-npu-convolution','protocol_version':2,'final_elf':sha(image),
        'physical_access':False,'refusal_fixture':True})
    return image


def execute(image,out,launcher_bundle,reuse_preparation=False):
    host=module('npu_host',ROOT/'apps/npu-convolution/observe.py')
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
    from collections import deque
    recent=deque(maxlen=128)
    class Machine(model.Machine):
        translations=0
        device_io=0
        def memory(self,u,access,a,size,value,c):
            if a<0x41000000:self.device_io+=1
            super().memory(u,access,a,size,value,c)
        def instruction(self,u,pc,size,c):
            recent.append((c,hex(pc)))
            if pc==self.symbols['digest']:
                address,size,destination=(self.reg(c,r) for r in ('x0','x1','x2'))
                assert 0<size<=524288 and 0x41000000<=address and address+size<=0x42000000
                assert 0x41200000<=destination and destination+32<=0x42000000
                value=hashlib.sha256(self.read(address,size)).digest()
                ctypes.memmove(self.ram_pointer+destination-0x41000000,value,32)
                u.reg_write(model.UC_ARM64_REG_PC,self.reg(c,'x30'));return
            if pc==self.symbols['platform_panic']:
                raise AssertionError(('native panic',self.reg(c,'x1'),hex(self.reg(c,'x30'))))
            if stops.get(c)==pc:
                u.emu_stop();return
            if pc not in self.decoded:self.decoded[pc]=next(self.decoder.disasm(bytes(u.mem_read(pc,4)),pc))
            instruction=self.decoded[pc]
            if instruction.mnemonic=='at':
                operation,register=instruction.op_str.split(', ')
                assert operation in ('s1e1r','s1e1w')
                address=self.reg(c,register);page=address&~4095
                # Architectural translation/cache effects are modeled. Real C
                # page checks execute, but this is not a new mapping proof.
                if 0x41000000<=page<0x42000000:par=(0xff<<56)|page|(3<<7)
                elif page==0x03600000:par=page|(2<<7)
                else:par=1
                self.regs[c]['par_el1']=par;self.translations+=1
                u.reg_write(model.UC_ARM64_REG_PC,pc+4);return
            super().instruction(u,pc,size,c)
    m=Machine(image);sym=m.symbols
    scratch=sym['__device_pool_end']-4096;return_pc=sym['a733_smp_entry']
    def write(address,data):ctypes.memmove(m.ram_pointer+address-0x41000000,data,len(data))
    def word(address,value):write(address,struct.pack('<Q',value))
    def uint(address,value):write(address,struct.pack('<I',value))
    def q(address):return struct.unpack('<Q',m.read(address,8))[0]
    def init_cpu(c,el=1):
        m.regs[c].update(currentel=el*4,spsel=0 if el==0 else 1,daif=0x3c0,tpidr_el1=c,
            sctlr_el1=0x30d01805,par_el1=0,ctr_el0=(4<<24)|(4<<16),
            sp_el0=sym['a733_cpu_el1_stacks']+(c+1)*32768-4096,
            sp_el1=sym['a733_cpu_el1_stacks']+(c+1)*32768,
            vbar_el1=sym['a733_cached_el1_vectors'])
        m.cpus[c].reg_write(model.UC_ARM64_REG_SP,m.regs[c][m.spname(c)])
    def run(c,entry,stop,limit=30000000):
        u=m.cpus[c];stops[c]=stop
        try:u.emu_start(entry,0,count=limit)
        except Exception:
            save(out/'instruction-failure.json',{'pc':hex(u.reg_read(model.UC_ARM64_REG_PC)),
                'lr':hex(u.reg_read(model.UC_ARM64_REG_LR)),'recent':list(recent),'registers':m.regs[c]})
            raise
        finally:stops.pop(c)
        if u.reg_read(model.UC_ARM64_REG_PC)!=stop:
            m.save(out/'interrupted-instructions.pkl')
            raise AssertionError(('unexpected native stop',hex(u.reg_read(model.UC_ARM64_REG_PC))))
    def call(name,*args):
        init_cpu(0);u=m.cpus[0]
        for reg,value in zip(model.GPR,args):u.reg_write(reg,value)
        u.reg_write(model.UC_ARM64_REG_LR,return_pc)
        run(0,sym[name] if isinstance(name,str) else name,return_pc)
        return u.reg_read(model.UC_ARM64_REG_X0)
    with image.open('rb') as stream:
        dwarf=ELFFile(stream).get_dwarf_info()
        dies=[d for cu in dwarf.iter_CUs() for d in cu.iter_DIEs()]
        app_dies=[d for cu in dwarf.iter_CUs() if cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith('/apps/npu-convolution/native.c') for d in cu.iter_DIEs()]
        def layout(name,variable=False):
            candidates=[d for d in (app_dies if variable else dies) if d.attributes.get('DW_AT_name') and
                d.attributes['DW_AT_name'].value.decode()==name and
                d.tag==('DW_TAG_variable' if variable else 'DW_TAG_structure_type') and (variable or 'DW_AT_byte_size' in d.attributes)]
            assert candidates,('Missing actual producer DWARF',name)
            if variable:assert len(candidates)==1,('Ambiguous actual producer',name)
            die=candidates[0]
            if variable:die=die.get_DIE_from_attribute('DW_AT_type')
            fields={d.attributes['DW_AT_name'].value.decode():d.attributes['DW_AT_data_member_location'].value
                for d in die.iter_children() if d.tag=='DW_TAG_member'}
            return die.attributes['DW_AT_byte_size'].value,fields
        _,app=layout('app',True);_,reserved=layout('a733_npu_reserved');_,conv=layout('npu_convolution')
        _,service=layout('npu_service');reply_size,reply=layout('npu_service_reply')
        _,result=layout('a733_npu_application_result');_,runtime=layout('a733_cached_runtime')
        _,pool=layout('device_memory_pool');_,allocation=layout('device_memory_allocation')
        allocation_size,_=layout('device_memory_allocation');_,extent=layout('device_memory_extent')
        _,registry=layout('a733_clock_leases');_,buffer=layout('a733_npu_buffer')
        buffer_size,_=layout('a733_npu_buffer')
    checkpoint=out/'constructed.pkl'
    checkpoint_identity=out/'constructed.json'
    producer={'runner_sha256':sha(Path(__file__)),'model_sha256':sha(Path(model.__file__)),
              'fixture_sha256':sha(ROOT/'checks/npu-application.c')}
    if reuse_preparation and checkpoint.exists():
        recorded=json.loads(checkpoint_identity.read_text())
        assert recorded['image_sha256']==sha(image) and recorded['checkpoint_sha256']==sha(checkpoint)
        assert recorded['producer']['model_sha256']==producer['model_sha256']
        assert recorded['producer']['fixture_sha256']==producer['fixture_sha256']
        m.restore(checkpoint)
    else:
        write(sym['a733_observe_identity'],struct.pack('<5Q',5,7,9,24,48000024));m.clock=100
        assert call('a733_npu_application_observer_admit')==0
        assert call('a733_smp_device_preparation_admitted')==0
        # Modeled entry admission only; immutable ELF remains refusal-only. Boot and
        # coherence observations are inherited prerequisites, not simulated proofs.
        word(sym['a733_npu_application_boot']+96,127);word(sym['a733_npu_application_boot']+104,15)
        for name in ('probe_arrived','probe_finished'):write(sym[name],struct.pack('<8I',*([1]*8)))
        uint(sym['probe_release'],1)
        # Runtime is an array; use its element type's startup_deadline offset.
        word(sym['a733_cached_runtime']+runtime['startup_deadline'],48000024)
        assert call('a733_smp_device_preparation_admitted')==1
        init_cpu(0)
        run(0,sym['a733_smp_diagnostic_start'],sym['vTaskStartScheduler'])
    app_address=sym['app'];r=app_address+app['reserved'];c=r+reserved['convolution']
    assert struct.unpack('<I',m.read(r+reserved['admitted'],4))[0]==1
    assert struct.unpack('<I',m.read(c+conv['bound'],4))[0]==1
    assert struct.unpack('<I',m.read(c+conv['prepared'],4))[0]==1
    assert q(app_address+app['client']) and q(app_address+app['pmic_task']) and q(app_address+app['bootstrap_task'])
    assert q(c+conv['service']+service['worker'])
    actual_pool=q(r+reserved['pool']);allocation_address=actual_pool+pool['allocations']
    assert q(r+reserved['extent']+extent['bytes'])==265*4096
    assert struct.unpack('<I',m.read(allocation_address+allocation['state'],4))[0]==2
    assert m.translations>512 and m.device_io==0
    # The following client cases model service responses only. The actual
    # constructor, client wrappers, input/output copies, reap and encoder run.
    uint(app_address+app['ready'],1);uint(sym['xSchedulerRunning'],1)
    word(sym['pxCurrentTCBs']+8,q(app_address+app['client']))
    if not checkpoint.exists():
        m.save(checkpoint)
        save(checkpoint_identity,{'image_sha256':sha(image),'checkpoint_sha256':sha(checkpoint),
                                 'producer':producer})
    constructed=bytes(m.ram)
    input_address=q(c+conv['buffers']+3*buffer_size+buffer['cpu'])
    output_address=q(c+conv['buffers']+4*buffer_size+buffer['cpu'])
    input_bytes=q(sym['a733_npu_application_input_bytes']);output_bytes=q(sym['a733_npu_application_reference_bytes'])
    assert (q(c+conv['input_bytes']),q(c+conv['output_bytes']))==(input_bytes,output_bytes)
    assert output_bytes==400 and input_bytes==432, 'This focused batch selects the largest reviewed model'
    expected=m.read(sym['a733_npu_application_reference'],output_bytes)
    challenge=hashlib.sha256(b'bounded NPU application publication').digest()
    def frame(sequence):
        write(scratch+1536,challenge);write(scratch+2048,host.request(sequence,prepared,challenge))
        assert call('a733_npu_application_request',scratch+2048,sym['a733_observe_identity'],scratch+2200,scratch+2304)==1
        assert m.read(scratch+2304,32)==challenge
        assert call('a733_npu_application_frame',scratch,sym['a733_observe_identity'],scratch+1536,sym['a733_smp_build_id'])==1
        return struct.pack('<4I',host.MESSAGE_MAGIC,sequence,0x80000033,1024)+m.read(scratch,1024)
    frames={};operations_by_case={}
    for case in ('match','first-mismatch','second-reap-ticket'):
        saved_case=out/('case-'+case+'.json')
        if reuse_preparation and saved_case.exists():
            prior=json.loads(saved_case.read_text());assert prior['image_sha256']==sha(image)
            operations_by_case[case]=prior['operations']
            for label,expected_hash in prior['frames'].items():
                path=out/(label+'.bin');assert sha(path)==expected_hash;frames[label]=path.read_bytes()
            continue
        ctypes.memmove(m.ram_pointer,constructed,len(constructed));m.clock=100
        init_cpu(1,el=0);u=m.cpus[1];operations=[];pending=[None];pause=[case=='match'];job=[0]
        def boundary(cpu,pc,size,unused):
            def returned(value):
                cpu.reg_write(model.UC_ARM64_REG_X0,value)
                cpu.reg_write(model.UC_ARM64_REG_PC,cpu.reg_read(model.UC_ARM64_REG_LR))
            if pc==sym['npu_service_send']:
                request=cpu.reg_read(model.UC_ARM64_REG_X1);op,command=struct.unpack('<2I',m.read(request,8))
                operations.append(op);pending[0]=op
                word(cpu.reg_read(model.UC_ARM64_REG_X3),len(operations))
                if op==2:
                    job[0]=len(operations)
                    assert command==1 and m.read(input_address,input_bytes)==m.read(sym['a733_npu_application_input'],input_bytes)
                returned(1)
            elif pc==sym['npu_service_collect']:
                op=pending[0]
                if op==0 and pause[0]:pause[0]=False;cpu.emu_stop();return
                dest=cpu.reg_read(model.UC_ARM64_REG_X2);write(dest,bytes(reply_size))
                state={1:2,2:3,0:6,4:2}[op]
                uint(dest+reply['state'],state);uint(dest+reply['owns_session'],1)
                word(dest+reply['job_ticket'],job[0])
                if op==0:
                    write(output_address,bytes([expected[0]^1])+expected[1:] if case=='first-mismatch' and operations.count(2)==1 else expected)
                if op==4 and case=='second-reap-ticket' and operations.count(4)==2:word(dest+reply['job_ticket'],job[0]+1)
                returned(1)
            elif pc==sym['vTaskDelay']:returned(0)
        hook=u.hook_add(model.UC_HOOK_CODE,boundary)
        try:
            if case=='match':
                u.emu_start(sym['client'],0,count=30000000)
                assert u.reg_read(model.UC_ARM64_REG_PC)==sym['npu_service_collect'] and not pause[0]
                frames['pending']=frame(1);assert host.response(frames['pending'],1,prepared,challenge)['status']=='pending'
                m.clock=200
                run(1,u.reg_read(model.UC_ARM64_REG_PC),sym['vTaskSuspend'])
            else:run(1,sym['client'],sym['vTaskSuspend'])
        finally:u.hook_del(hook)
        complete=struct.unpack('<I',m.read(sym['a733_npu_application_result']+result['complete'],4))[0]
        assert complete=={'match':1,'first-mismatch':2,'second-reap-ticket':0}[case]
        assert operations==[1,2,0,4,2,0,4]
        encoded=frame(2);decoded=host.response(encoded,2,prepared,challenge)
        assert decoded['status']=={'match':'success','first-mismatch':'mismatch','second-reap-ticket':'fault'}[case]
        frames[case]=encoded;operations_by_case[case]=operations
        labels=['pending','match'] if case=='match' else [case]
        for label in labels:(out/(label+'.bin')).write_bytes(frames[label])
        save(saved_case,{'image_sha256':sha(image),'producer':producer,'operations':operations,
                        'frames':{label:sha(out/(label+'.bin')) for label in labels}})
    capture=out/'capture';capture.mkdir();record={'npu_result':'unobserved'}
    replies=iter((frames['pending'],frames['match']))
    def save_record():
        if record['npu_result']=='observed-success':
            item=record['durable_result'];assert sha(capture/item['path'])==item['sha256']
    host.capture(lambda packet,timeout:next(replies),prepared,challenge,capture,record,save_record,10,now=lambda:1,pause=lambda _:None)
    assert record['npu_result']=='observed-success'
    for label,data in frames.items():(out/(label+'.bin')).write_bytes(data)
    bad=bytearray(frames['match']);bad[16+280]^=1
    bad[16+992:]=hashlib.sha256(bad[16:16+992]).digest()
    try:host.response(bytes(bad),2,prepared,challenge)
    except ValueError:pass
    else:raise AssertionError('Changed output escaped its independent digest')
    return {'passed':True,'operations':operations_by_case,'parent_bytes':265*4096,
        'actual_native_construction':True,'native_at_instructions':m.translations,'device_mmio_operations':m.device_io,
        'sealed_launcher_sha256':sha(launcher_bundle/'launch.bin'),'sealed_bundle_sha256':sealed_before,
        'modeled_inputs':'Architectural registers, AT/cache effects, admitted boot flags, SHA-256 via hashlib over actual bytes, and service replies/output. Reference output is injected only at the modeled device boundary; no numerical inference is executed.',
        'limits':'No physical boot/coherence/USB/NPU/PMIC/IRQ/watchdog qualification. No scheduler, old controller or numerical workload replay.'}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('freertos','tinyusb','lld','launcher-bundle','artifacts','evidence'):
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
    print('PASS native NPU application preparation, client publication and durable host capture')


if __name__=='__main__':main()
