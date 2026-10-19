"""Only new native copied-result, guard and fixed packaging contracts."""
import hashlib
import importlib.util
import json
import struct
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR

def setup(image,out,session_optional=False,fixture_optional=False):
    """Validate fixture prerequisites without executing native instructions."""
    errors=[];symbols={};symbol_candidates={};copy_entries=set();segments=[];layouts={};reference=b'';session=b''
    try:
        with image.open('rb') as f:
            elf=ELFFile(f)
            for symbol in elf.get_section_by_name('.symtab').iter_symbols():
                if symbol['st_value']:symbol_candidates.setdefault(symbol.name,set()).add(int(symbol['st_value']))
            symbols={name:next(iter(values)) for name,values in symbol_candidates.items() if len(values)==1}
            segments=[(int(s['p_vaddr']),bytes(s.data())) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
            layouts={}
            for cu in elf.get_dwarf_info().iter_CUs():
                unit=cu.get_top_DIE().attributes.get('DW_AT_name')
                native=unit and unit.value.decode().endswith('/apps/h264/native.c')
                for die in cu.iter_DIEs():
                    name=die.attributes.get('DW_AT_name')
                    if native and die.tag=='DW_TAG_subprogram' and name and name.value==b'h264_copied' and 'DW_AT_low_pc' in die.attributes:
                        copy_entries.add(int(die.attributes['DW_AT_low_pc'].value))
                    if die.tag=='DW_TAG_structure_type' and name:
                        fields={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in die.iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_name' in c.attributes and 'DW_AT_data_member_location' in c.attributes}
                        if fields:layouts[name.value.decode()]=fields
            if len(copy_entries)==1 and copy_entries<=symbol_candidates.get('h264_copied',set()):
                symbols['h264_copied']=next(iter(copy_entries))
            else:errors.append('Cannot uniquely resolve application copy routine from native.c DWARF')
    except Exception as error:errors.append('ELF/DWARF: '+str(error))
    required_symbols={
        'h264','board','h264_copied','h264_response','a733_smp_build_id','h264_task',
        'a733_h264_begin','a733_video_platform_h264_ops',
        'a733_iommu_init_decoder','board_application_start','board_usb0_task',
        'board_live','board_fail','a733_iommu_scan','a733_iommu_poll','device_memory_describe','h264_load',
        'vTaskDelay','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init',
        'tud_task_ext','tud_vendor_n_available','tud_vendor_n_read'}
    for name in sorted(required_symbols):
        if not symbols.get(name):errors.append('Missing ELF symbol: '+name)
        if name!='h264_copied' and len(symbol_candidates.get(name,set()))!=1:errors.append('Ambiguous ELF symbol: '+name)
    for name in ('a733_watchdog_hw_admit','a733_watchdog_hw_reload'):
        if name in symbols:errors.append('Unexpected armed watchdog symbol: '+name)
    members={'board_application':['fatal','watchdog_state'],
        'h264_application':['input','luma','chroma','buffers','iommu','copy_step','payload','digest','result'],
        'a733_iommu':['fault'],'a733_iommu_fault':['scanned_masters']}
    for structure,names in members.items():
        for name in names:
            if name not in layouts.get(structure,{}):errors.append('Missing DWARF member: '+structure+'.'+name)
    for name in (() if fixture_optional else ('reference.yuv','session.bin','slice.bin')):
        if name=='session.bin' and session_optional and not (out/name).exists():continue
        try:data=(out/name).read_bytes()
        except OSError as error:errors.append(str(error));continue
        if name=='reference.yuv':
            reference=data
            if len(data)!=2304 or hashlib.sha256(data).hexdigest()!='4398156e0c9291091780d378ff220793691a3dd38eabf483f9e2e9400d6af72e':errors.append('Independent H.264 reference differs')
        elif name=='slice.bin':
            oracle=json.loads((out/'oracle-result.json').read_text())
            if not 0<len(data)<=4096 or hashlib.sha256(data).hexdigest()!=oracle['files']['input.h264']['sha256']:errors.append('AnnexB input differs from independent oracle')
        else:
            session=data
            if len(data)!=856:errors.append('Session must be856bytes')
            else:
                if struct.unpack_from('<3Q',data)!=(0x4153373342535332,2,592) or struct.unpack_from('<I',data,400)[0]!=2:errors.append('Session requires supervised board selection2')
                if struct.unpack_from('<3Q',data,592)!=(0x4153373348324131,1,856):errors.append('H.264 session extension differs')
                if data[112:184]!=bytes(72):errors.append('Uploaded session must have zero timer/deadline fields')
    report={'result':'FAIL' if errors else 'PASS','errors':errors,'required_symbols':sorted(required_symbols),'required_members':members,'reference_bytes':len(reference),'session_bytes':len(session),'native_instructions_executed':False,
        'copy_entry_source':'apps/h264/native.c','copy_entry':symbols.get('h264_copied'),'copy_symbol_candidates':sorted(symbol_candidates.get('h264_copied',set()))}
    (out/('h264-preflight.json' if session_optional else 'h264-setup.json')).write_text(json.dumps(report,indent=2)+'\n')
    if errors:raise RuntimeError('H.264 instruction setup failed:\n'+'\n'.join(errors))
    return symbols,segments,layouts,reference,session

def run(image,out,source,compiler,linker,steps,steps_path):
    symbols,segments,layouts,reference,session=setup(image,out)
    def passed(name,**detail):
        steps[name]={'exit':0,**detail};steps_path.write_text(json.dumps(steps,indent=2)+'\n')
    def module(name,path):
        spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
    if steps.get('armed-parent-refused',{}).get('exit')!=0:
        import subprocess
        model=out/'armed-parent-model';model.mkdir(exist_ok=True)
        (model/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.24)\nproject(armed_parent NONE)\nadd_library(sunstone-a733-board INTERFACE)\nset(SUNSTONE_SOURCE_ROOT "'+str(source)+'")\ninclude("'+str(source/'cmake/a733-h264.cmake')+'")\n')
        result=subprocess.run(['cmake','-S',str(model),'-B',str(model/'build')],capture_output=True,text=True)
        (out/'armed-parent-refusal.log').write_text(result.stdout+result.stderr)
        assert result.returncode and 'H.264 cannot inherit an armed board target' in result.stderr
        passed('armed-parent-refused')
    layout=layouts['h264_application'];io=layouts['a733_iommu'];fault=layouts['a733_iommu_fault']
    supplied=layouts['a733_h264_input']
    (out/'h264-layouts.json').write_text(json.dumps(layouts,indent=2)+'\n')
    for variant in ['verified','pixel-mismatch','physical-guard','scratch-tail','iommu-fault','writeback-busy','post-copy-fault','post-copy-busy']:
        label='native-copy-'+variant
        if steps.get(label,{}).get('exit')==0:continue
        cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.mem_map(0x40000000,0x4000000);cpu.mem_map(0x50000000,0x1000000)
        for address,data in segments:cpu.mem_write(address,data)
        base=symbols['h264'];cpu.mem_write(base,(out/'session.bin').read_bytes())
        cpu.mem_write(symbols['board']+160,struct.pack('<Q',1000000))
        data_addresses=[0x50001000,0x50005000,0x50009000,0x5000d000,0x50031000,0x50038000]
        allocation_bases=[0x50000000,0x50004000,0x50008000,0x5000c000,0x50030000,0x50034000]
        capacities=[4096,4096,4096,0x21000,4096,0xc000]
        prefixes=[4096,4096,4096,4096,4096,16384]
        for field,address in zip(('input','luma','chroma'),data_addresses):cpu.mem_write(base+layout[field],struct.pack('<Q',address))
        cpu.mem_write(base+layout['buffers'],struct.pack('<6Q',*data_addresses))
        cpu.mem_write(base+layout['supplied']+supplied['oracle'],reference)
        cpu.mem_write(base+layout['supplied']+supplied['oracle_bytes'],struct.pack('<Q',2304))
        cpu.mem_write(base+layout['supplied']+supplied['committed'],struct.pack('<I',1))
        for address,capacity,prefix in zip(allocation_bases,capacities,prefixes):
            cpu.mem_write(address,bytes([0xa5])*(capacity+prefix+4096))
        for row in range(32):cpu.mem_write(0x50005000+row*64,reference[row*48:row*48+48])
        for plane in range(2):
            for row in range(16):cpu.mem_write(0x50009000+plane*512+row*32,reference[1536+plane*384+row*24:1536+plane*384+row*24+24])
        cpu.mem_write(base+layout['iommu']+io['fault']+fault['scanned_masters'],struct.pack('<H',0xc0))
        if variant=='pixel-mismatch':cpu.mem_write(0x50005000,bytes([reference[0]^1]))
        if variant=='physical-guard':cpu.mem_write(0x50004000,b'\x00')
        if variant=='scratch-tail':cpu.mem_write(0x5000d000+0x20800,b'\x00')
        calls=[];boundary_arguments=[];describes=[0];finished=[]
        hooks={symbols[n]:n for n in ['board_live','a733_iommu_scan','a733_iommu_poll','device_memory_describe','h264_load']}
        def returned(value):cpu.reg_write(UC_ARM64_REG_X0,value);cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR))
        def hook(machine,address,size,unused):
            if address==0x50fff000:finished.append(True);machine.emu_stop();return
            name=hooks.get(address)
            if not name:return
            calls.append(name)
            if name in ('device_memory_describe','h264_load'):
                boundary_arguments.append({'name':name,'address':address,'arguments':[machine.reg_read(r) for r in (UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3)]})
            if name=='device_memory_describe':
                i=describes[0]%6;extent=allocation_bases[i];describes[0]+=1
                machine.mem_write(machine.reg_read(UC_ARM64_REG_X2),struct.pack('<3Q',extent,extent,capacities[i]+prefixes[i]+4096))
                machine.mem_write(machine.reg_read(UC_ARM64_REG_X3),struct.pack('<I',2));returned(1)
            elif name=='h264_load':machine.mem_write(machine.reg_read(UC_ARM64_REG_X1),struct.pack('<I',1<<23 if variant=='writeback-busy' or (variant=='post-copy-busy' and int.from_bytes(machine.mem_read(base+layout['copy_step'],4),'little')==2) else 0));returned(1)
            elif name=='a733_iommu_poll':returned(0xffffffffffffffff if variant=='iommu-fault' or (variant=='post-copy-fault' and int.from_bytes(machine.mem_read(base+layout['copy_step'],4),'little')==2) else 1)
            else:returned(1)
        cpu.hook_add(UC_HOOK_CODE,hook)
        returns=[]
        saved_copy=variant=='verified' and steps.get('native-copy-payload',{}).get('exit')==0
        if saved_copy:
            cpu.mem_write(base+layout['payload'],(out/'native-copy-payload.bin').read_bytes());cpu.mem_write(base+layout['digest'],(out/'native-copy-digest.bin').read_bytes());returns=[0,1]
        for invocation in range(2 if variant in ('verified','post-copy-fault','post-copy-busy') else 1):
            if saved_copy:break
            cpu.reg_write(UC_ARM64_REG_X0,base);cpu.reg_write(UC_ARM64_REG_X1,13);cpu.reg_write(UC_ARM64_REG_X2,990000)
            cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);cpu.reg_write(UC_ARM64_REG_LR,0x50fff000)
            finished.clear();cpu.emu_start(symbols['h264_copied'],0,count=4000000);assert finished,'copy allowance exhausted'
            returns.append(cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff)
        (out/(label+'-observation.json')).write_text(json.dumps({'returns':returns,'calls':calls,
            'boundary_arguments':boundary_arguments,'describes':describes[0],'copy_step':int.from_bytes(cpu.mem_read(base+layout['copy_step'],4),'little')},indent=2)+'\n')
        assert returns==([0,1] if variant=='verified' else [0,0xffffffff] if variant.startswith('post-copy-') else [0xffffffff]),(label,returns,calls,describes[0])
        if variant=='verified':
            assert bytes(cpu.mem_read(base+layout['payload'],2304))==reference
            assert bytes(cpu.mem_read(base+layout['digest'],32))==hashlib.sha256(reference).digest()
        if variant=='verified':
            if not saved_copy:
                (out/'native-copy-payload.bin').write_bytes(bytes(cpu.mem_read(base+layout['payload'],2304)))
                (out/'native-copy-digest.bin').write_bytes(bytes(cpu.mem_read(base+layout['digest'],32)))
                passed('native-copy-payload',model='Actual native copy and digest before separate USB frame construction.')
            # Form the complete frame with the actual native USB callback code.
            cpu.mem_write(base+layout['result'],struct.pack('<I',1))
            cpu.mem_write(symbols['board']+24,struct.pack('<2Q',1,2))
            cpu.mem_write(base+768,hashlib.sha256((out/'slice.bin').read_bytes()).digest()+hashlib.sha256(reference).digest())
            def usb_hook(machine,address,size,unused):
                if address==symbols.get('vTaskDelay'):
                    finished.append(True);machine.emu_stop();return
                if address in {symbols.get(n) for n in ['a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','a733_usb_prepare']}:returned(1)
                elif address==symbols.get('tud_vendor_n_available') or address==symbols.get('tud_vendor_available'):returned(1)
                elif address==symbols.get('tud_vendor_n_read'):
                    # TinyUSB public convenience wrapper supplies interface0.
                    machine.mem_write(machine.reg_read(UC_ARM64_REG_X1),b'\x00');returned(1)
                elif address==symbols.get('tud_vendor_read'):
                    machine.mem_write(machine.reg_read(UC_ARM64_REG_X0),b'\x00');returned(1)
            usb=cpu.hook_add(UC_HOOK_CODE,usb_hook)
            cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);cpu.reg_write(UC_ARM64_REG_LR,0x50fff000)
            finished.clear();cpu.emu_start(symbols['board_usb0_task'],0,count=4000000);assert finished
            frame=bytes(cpu.mem_read(symbols['h264_response'],2432))
            observed=module('h264_native_observe',source/'apps/h264/observe.py')
            wire_session=bytes(cpu.mem_read(base,856))
            assert observed.decode(frame,wire_session,reference)['copied_verified']
            (out/'native-frame.bin').write_bytes(frame);cpu.hook_del(usb)
        passed(label,calls=calls,model='Real native copy/guards/comparison and full USB frame; admission, IOMMU observation and MMIO are explicit modeled boundaries.')
    if steps.get('native-usb-revocation',{}).get('exit')!=0:
        cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.mem_map(0x40000000,0x4000000);cpu.mem_map(0x50000000,0x1000000)
        for address,data in segments:cpu.mem_write(address,data)
        cpu.mem_write(symbols['h264']+layouts['h264_application']['supplied']+layouts['a733_h264_input']['committed'],struct.pack('<I',1))
        parked=[];failures=[]
        def usb_failure(machine,address,size,unused):
            def returned(value):
                machine.reg_write(UC_ARM64_REG_X0,value);machine.reg_write(UC_ARM64_REG_PC,machine.reg_read(UC_ARM64_REG_LR))
            if address==symbols['board_fail']:failures.append(address)
            if address==symbols['vTaskDelay']:
                fields=layouts['board_application'];base=symbols['board']
                assert int.from_bytes(machine.mem_read(base+fields['fatal'],4),'little')==1
                assert int.from_bytes(machine.mem_read(base+fields['watchdog_state'],4),'little')&4
                parked.append(True);machine.emu_stop()
            elif address in {symbols.get(n) for n in ['board_live','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','a733_usb_prepare','tud_vendor_n_available','tud_vendor_available']}:returned(1)
            elif address==symbols.get('tud_vendor_n_read'):
                machine.mem_write(machine.reg_read(UC_ARM64_REG_X1),b'\x01');returned(1)
            elif address==symbols.get('tud_vendor_read'):
                machine.mem_write(machine.reg_read(UC_ARM64_REG_X0),b'\x01');returned(1)
        cpu.hook_add(UC_HOOK_CODE,usb_failure)
        cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);cpu.reg_write(UC_ARM64_REG_LR,0x50fff000)
        cpu.emu_start(symbols['board_usb0_task'],0,count=100000)
        assert parked and failures, 'Invalid request did not execute actual board_fail before parking'
        passed('native-usb-revocation',model='Actual invalid-request stop and shared fatal/watchdog latches; TinyUSB and initial liveness modeled.')
    prepare=module('h264_prepare',source/'apps/h264/prepare.py');observe=module('h264_observe',source/'apps/h264/observe.py')
    if steps.get('h264-wire',{}).get('exit')!=0:
        raw=bytearray((out/'session.bin').read_bytes());raw[768:800]=hashlib.sha256((out/'slice.bin').read_bytes()).digest();raw[800:832]=hashlib.sha256(reference).digest()
        header=struct.pack('<4I2Q',0x34363248,1,1,2304,1,2)+raw[768:832]+hashlib.sha256(reference).digest()
        assert observe.decode(header+reference,raw,reference)['copied_verified']
        for bad in [header+reference[:-1],header+bytes([reference[0]^1])+reference[1:]]:
            try:observe.decode(bad,raw,reference)
            except ValueError:pass
            else:raise AssertionError('Malformed/mismatched H.264 bytes accepted')
        prepare.session_check(raw)
        for offset,value in [(160,1),(400,3),(848,900000),(852,2)]:
            altered=bytearray(raw);struct.pack_into('<Q' if offset==160 else '<I',altered,offset,value)
            try:prepare.session_check(altered)
            except Exception:pass
            else:raise AssertionError('Non-admitted H.264 profile accepted')
        passed('h264-wire',cases=['exact copied frame','truncation refused','pixel mismatch refused','uploaded deadline refused','radio workload refused','wrong voltage/rail refused'])
    assert steps.get('h264-launcher-build',{}).get('exit')==0, 'Launcher must be built during setup'
    if steps.get('h264-protected-packaging',{}).get('exit')!=0:
        # Physical-evidence authority is an explicit modeled boundary here;
        # geometry/sealing/ELF/session checks run from the actual public decoder.
        import shutil
        bundle=out/'packaging-model';bundle.mkdir(exist_ok=True)
        shutil.copy2(image,bundle/'image.elf');shutil.copy2(out/'launcher/launch.bin',bundle/'launch.bin')
        parts,address,identity=prepare.base.image_parts(bundle/'image.elf')
        template=bytearray((out/'session.bin').read_bytes())
        template[768:800]=hashlib.sha256((out/'slice.bin').read_bytes()).digest()
        template[800:832]=hashlib.sha256(reference).digest()
        raw=bytearray(template);raw[48:80]=identity
        (bundle/'session-template.bin').write_bytes(template);(bundle/'session.bin').write_bytes(raw)
        (bundle/'input.h264').write_bytes((out/'slice.bin').read_bytes())
        (bundle/'oracle.yuv').write_bytes(reference)
        (bundle/'admission.json').write_text('{}\n')
        (bundle/'image-input-manifest.json').write_text(json.dumps({'target':'sunstone-a733-h264','execution_profile':'h264-supervised-v1','final_elf':hashlib.sha256(image.read_bytes()).hexdigest()}))
        uploads=[]
        for i,part in enumerate(parts):
            if part['data']:
                name=f'segment-{i}.bin';(bundle/name).write_bytes(part['data']);uploads.append({'name':name,'address':part['address']})
        uploads.append({'name':'session.bin','address':0x41f00000})
        for name,data in zip(('request.bin','contract.bin'),prepare.request_bytes(address,identity,raw,1,2,3)):(bundle/name).write_bytes(data)
        (bundle/'result-zero.bin').write_bytes(bytes(16384));(bundle/'report-zero.bin').write_bytes(bytes(128))
        record={'schema':prepare.SCHEMA,'physical_access':False,'image_sha256':hashlib.sha256(image.read_bytes()).hexdigest(),
            'build_id':identity.hex(),'epoch':1,'generation':2,'lease':3,'entry_evidence':{},'segments':uploads,
            'backup_sizes':{name:n for name,a,n in prepare.base.REGIONS},'files':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in bundle.iterdir() if p.is_file() and p.name!='prepared.json'}}
        (bundle/'prepared.json').write_text(json.dumps(record))
        authority=prepare.validate_admission;prepare.validate_admission=lambda *a,**kw:{}
        try:
            prepare.validate_bundle(bundle)
            changed={**record,'backup_sizes':dict(record['backup_sizes'])};key=next(iter(changed['backup_sizes']));changed['backup_sizes'][key]+=4
            (bundle/'prepared.json').write_text(json.dumps(changed))
            try:prepare.validate_bundle(bundle)
            except Exception:pass
            else:raise AssertionError('Changed protected backup geometry accepted')
            (bundle/'prepared.json').write_text(json.dumps(record))
        finally:prepare.validate_admission=authority
        passed('h264-protected-packaging',cases=['exact ELF/session sealed upload','changed protected backup refused'],model='Physical admission boundary modeled; actual public bundle geometry validation.')

    if steps.get('h264-launcher-contract',{}).get('exit')!=0:
        import subprocess,sys
        launch=out/'launcher'
        identity_address=symbols['a733_smp_build_id']
        for address,data in segments:
            if address<=identity_address<address+len(data):identity=data[identity_address-address:identity_address-address+32]
        raw=bytearray((out/'session.bin').read_bytes());raw[48:80]=identity
        raw[768:800]=hashlib.sha256((out/'slice.bin').read_bytes()).digest()
        raw[800:832]=hashlib.sha256(reference).digest()
        request,contract=prepare.request_bytes(identity_address,identity,raw,1,2,3)
        for name,data in [('request.bin',request),('contract.bin',contract),('session.bin',raw),('identity.bin',identity)]:(launch/name).write_bytes(data)
        (launch/'prepared.json').write_text(json.dumps({'segments':[{'name':'session.bin','address':0x41f00000},{'name':'identity.bin','address':identity_address}]}))
        model=(source/'checks/a733-smp-supervised-launch.py').read_text().replace('0x4153373353554333','0x4153373348324331').replace('480000000','1440000000')
        model=model.replace('reason in (1,2,3,4,5,6,13)','reason in (1,2,3,4,5,6,13,14)')
        first=model.index('    if args.frequency_only:');last=model.index('    result =',first)
        model=model[:first]+"    cases=[check('H.264 full-session60secondhandoff',12),check('truncated H.264 extension refused',14,mutation=(0x41f00000+608,592))]\n"+model[last:]
        (out/'h264-launcher-model.py').write_text(model)
        command=[sys.executable,out/'h264-launcher-model.py','--bundle',launch,'--output',out/'h264-launcher-result.json']
        result=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        (out/'h264-launcher-check.log').write_text(result.stdout+result.stderr)
        result.check_returncode();passed('h264-launcher-contract',command=list(map(str,command)))
