"""Only new supervised admission and native startup/cutoff binding checks."""
import hashlib
import json
import struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_X0

def run(image,out,source,compiler,command,steps,steps_path):
    command('supervised-admission-build',[compiler,'-std=c11','-O1','-g','-fsanitize=address,undefined',
        '-DSUNSTONE_BOARD_SUPERVISED=1','-I'+str(source/'include'),'-I'+str(source/'platform/a733-smp-cached'),
        source/'apps/board/admission.c',source/'drivers/storage/storage_digest.c',
        source/'checks/board-supervised-admission.c','-o',out/'supervised-admission'])
    command('supervised-admission',[out/'supervised-admission',out/'session-fixture.bin'])
    with image.open('rb') as f:
        elf=ELFFile(f);symbols={s.name:int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
        segments=[(int(s['p_vaddr']),bytes(s.data())) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
        offsets={}
        for cu in elf.get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                name=die.attributes.get('DW_AT_name')
                if die.tag=='DW_TAG_structure_type' and name and name.value==b'board_application':
                    members={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value)
                        for c in die.iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_data_member_location' in c.attributes}
                    if 'radio_result' in members:offsets=members;break
            if offsets:break
    assert offsets
    assert 'a733_watchdog_hw_admit' not in symbols and 'a733_watchdog_hw_reload' not in symbols
    assert {'a733_typec_native_init','a733_usb2_native_init','board_radio_startup','tusb_rhport_init','board_host_tusb_rhport_init'}<=set(symbols)
    (out/'supervised-layout.json').write_text(json.dumps(offsets,indent=2)+'\n')
    device=None
    for address,data in segments:
        if address<=symbols['device']<address+len(data):device=data[symbols['device']-address:symbols['device']-address+18]
    assert device and struct.unpack_from('<HHH',device,8)==(0x1f3a,0xefe8,0x0100)
    raw=(out/'session-fixture.bin').read_bytes()
    import importlib.util
    spec=importlib.util.spec_from_file_location('board_prepare',source/'apps/board/prepare.py')
    prepare=importlib.util.module_from_spec(spec);spec.loader.exec_module(prepare)
    if steps.get('supervised-profile-spans',{}).get('exit')!=0:
        profile=bytearray(1872);struct.pack_into('<QII',profile,0,0x3152464f52545353,4,1872)
        addresses=[0x43010000,0x43011000,0x43012000,0x43013000];lengths=[32]*4
        struct.pack_into('<4Q',profile,432,*addresses);struct.pack_into('<4Q',profile,464,*lengths)
        session=bytearray(raw);struct.pack_into('<QQ',session,352,0x43000000,1872)
        session[368:400]=hashlib.sha256(profile).digest();spans=list(zip(addresses,lengths))
        assert prepare.profile_spans(profile,session,spans)==spans
        for bad in [spans[:-1],spans[:-1]+[(addresses[-1],33)]]:
            try:prepare.profile_spans(profile,session,bad)
            except ValueError:pass
            else:raise AssertionError('missing/mismatched firmware span accepted')
        steps['supervised-profile-spans']={'exit':0,'cases':['exact-four-spans','missing-blob-refused','mismatched-span-refused']}
        steps_path.write_text(json.dumps(steps,indent=2)+'\n')
    launch=out/'launcher'
    if not launch.exists():
        launch.mkdir();prepare.build_launcher(launch,compiler,Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
        identity_address=symbols['a733_smp_build_id']
        for address,data in segments:
            if address<=identity_address<address+len(data):identity=data[identity_address-address:identity_address-address+32]
        request,contract=prepare.request_bytes(identity_address,identity,1,2,3,raw)
        (launch/'request.bin').write_bytes(request);(launch/'contract.bin').write_bytes(contract)
        (launch/'session.bin').write_bytes(raw);(launch/'identity.bin').write_bytes(identity)
        (launch/'prepared.json').write_text(json.dumps({'segments':[{'name':'session.bin','address':0x41f00000},{'name':'identity.bin','address':identity_address}]}))
    model=(source/'checks/a733-smp-supervised-launch.py').read_text()
    # Count-checked edits: both 20-second deadlines become the board's 60 seconds.
    model=prepare.adapt_launcher(model,{'0x4153373353554333':'0x4153373342534332','480000000':'1440000000',
        'reason in (1,2,3,4,5,6,13)':'reason in (1,2,3,4,5,6,13,14)'},{'480000000':2})
    selection='    if args.accelerator_profile:\n        cases'
    assert model.count(selection)==1 and model.count('    result =')==1, 'Launcher model case selection changed.'
    first=model.index(selection);last=model.index('    result =',first)
    model=model[:first]+'''    cases=[check('board60second supervised handoff',12,sysreg={0xee1e0f10:0}),
        check('nonzero uploaded timer deadline refused',14,mutation=(0x41f00000+160,1)),
        check('armed contract cannot enter supervised board',3,mutation=(CONTRACT+8,1))]
'''+model[last:]
    (out/'launcher-model.py').write_text(model)
    command('supervised-launcher',[__import__('sys').executable,out/'launcher-model.py','--bundle',launch,'--output',out/'launcher-result.json'])
    for mutation,label in [(None,'profile-runtime-derived'),(200,'profile-nonzero-time-refused'),(192,'profile-nonzero-handoff-refused')]:
        if steps.get(label,{}).get('exit')==0:continue
        cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.mem_map(0x40000000,0x4000000);cpu.mem_map(0x50000000,0x10000)
        for address,data in segments:cpu.mem_write(address,data)
        profile=bytearray(1872);struct.pack_into('<QII',profile,0,0x3152464f52545353,4,1872)
        profile[88:120]=bytes(range(32)) # Preserve actual qualification bytes exactly; not proof of qualification.
        if mutation is not None:struct.pack_into('<Q',profile,mutation,1)
        session=bytearray(raw);struct.pack_into('<Q',session,160,60000001);struct.pack_into('<Q',session,168,24)
        session[368:400]=hashlib.sha256(profile).digest()
        cpu.mem_write(0x43000000,bytes(profile));cpu.mem_write(0x43200000,bytes([0xcc])*1872);cpu.mem_write(0x43300000,bytes(session))
        for index,value in enumerate([0x43200000,0x43000000,0x43300000,0x43201000]):cpu.reg_write(UC_ARM64_REG_X0+index,value)
        cpu.reg_write(UC_ARM64_REG_SP,0x5000fff0);cpu.reg_write(UC_ARM64_REG_LR,0x50000000)
        stopped=[]
        def finish(machine,address,size,unused):
            if address==0x50000000:stopped.append(True);machine.emu_stop()
        cpu.hook_add(UC_HOOK_CODE,finish);cpu.emu_start(symbols['board_radio_profile_runtime'],0,count=1000000)
        assert stopped
        if mutation is None:
            expected=bytearray(profile);struct.pack_into('<I',expected,8,3)
            for offset,value in [(192,2),(344,2),(200,40000001),(208,59990001),(360,40000001),
                                 (1232,40000001),(1288,50000001),(1320,59990001)]:struct.pack_into('<Q',expected,offset,value)
            assert cpu.reg_read(UC_ARM64_REG_X0)==1 and bytes(cpu.mem_read(0x43200000,1872))==expected
            assert bytes(cpu.mem_read(0x43201000,32))==hashlib.sha256(expected).digest()
        else:assert cpu.reg_read(UC_ARM64_REG_X0)==0 and bytes(cpu.mem_read(0x43200000,1872))==bytes([0xcc])*1872
        steps[label]={'exit':0};steps_path.write_text(json.dumps(steps,indent=2)+'\n')
    labels=['supervised-usb0-startup','supervised-radio-binding','supervised-software-cutoff','supervised-radio-retention']
    for mode,label in enumerate(labels):
        if steps.get(label,{}).get('exit')==0:continue
        uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM);uc.mem_map(0x40000000,0x4000000);uc.mem_map(0x50000000,0x10000)
        for address,data in segments:uc.mem_write(address,data)
        uc.mem_write(0x41f00000,raw)
        uc.reg_write(UC_ARM64_REG_SP,0x5000fff0)
        # Startup context comes from the admitted cached all-core foundation.
        uc.mem_write(symbols['a733_cached_runtime'],struct.pack('<QQQQ',1,24000024,1440000024,0))
        calls=[];entries=[];done=[]
        def returned(value=1):uc.reg_write(UC_ARM64_REG_X0,value);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
        def hook(machine,address,size,unused):
            name=by_address.get(address)
            if not name:return
            calls.append(name)
            if name=='platform_panic':raise AssertionError((label,'native panic',uc.reg_read(UC_ARM64_REG_X0)))
            if name in ('a733_typec_native_init','a733_usb2_native_init'):
                raise AssertionError('disabled source/host constructed')
            if name=='xTaskCreateStatic':
                entries.append(uc.reg_read(UC_ARM64_REG_X0));returned(0x60000000+len(entries)*256)
            elif name=='a733_native_usb_bind':
                assert uc.reg_read(UC_ARM64_REG_X0)==symbols['providers'];returned()
            elif name=='a733_smp_counter':returned(1440000000 if mode>=2 else 100)
            elif name=='radio_profile':returned(0x43000000)
            elif name=='board_radio_chip_id':returned(0x43001000)
            elif name in ('a733_smp_device_preparation_admitted','a733_native_usb_init','board_radio_startup','board_radio_external_failure'):returned()
            elif name=='vTaskStartScheduler':done.append('startup');uc.emu_stop()
            elif name=='vTaskDelay':done.append('cutoff');uc.emu_stop()
        names=['platform_panic','a733_typec_native_init','a733_usb2_native_init','xTaskCreateStatic',
               'a733_native_usb_bind','a733_smp_counter','radio_profile','board_radio_chip_id',
               'a733_smp_device_preparation_admitted','a733_native_usb_init','board_radio_startup',
               'board_radio_external_failure','vTaskStartScheduler','vTaskDelay']
        by_address={symbols[name]:name for name in names if name in symbols}
        uc.hook_add(UC_HOOK_CODE,hook)
        if mode==1:
            record=bytearray(raw);struct.pack_into('<I',record,400,3)
            # Raw admission is separately tested; this case exercises wiring.
            uc.mem_write(0x41f00000,bytes(record))
            by_address[symbols['a733_board_session_valid']]='a733_smp_device_preparation_admitted'
        if mode<2:start=symbols['a733_smp_diagnostic_start']
        else:
            base=symbols['board'];uc.mem_write(base,raw)
            uc.mem_write(base+offsets['live_until'],struct.pack('<Q',1))
            uc.mem_write(base+offsets['watchdog_state'],struct.pack('<I',1))
            start=symbols['supervise'] if mode==2 else symbols['radio_owner']
        uc.emu_start(start,0,count=100000)
        assert done,(label,'instruction allowance exhausted')
        if mode<2:
            expected=['pmic','board_usb0_task','supervise']
            if mode==1:expected+=['radio_owner','radio_client']
            assert set(entries)=={symbols[n] for n in expected},(label,entries)
            assert ('board_radio_startup' in calls)==(mode==1)
            assert int.from_bytes(uc.mem_read(symbols['board']+offsets['result'],4),'little')==3
        elif mode==2:
            assert int.from_bytes(uc.mem_read(symbols['board']+offsets['fatal'],4),'little')==1
        else:assert 'board_radio_external_failure' in calls
        steps[label]={'exit':0,'calls':calls,'task_entries':entries,'model':'Provider constructors are binding boundaries; no modeled return is physical admission.'}
        steps_path.write_text(json.dumps(steps,indent=2)+'\n')
    return {'result':'PASS','mode':'supervised','image_sha256':hashlib.sha256(image.read_bytes()).hexdigest(),
        'cases':['supervised-admission',*labels,'no-watchdog-symbols'],'hardware_access':False,
        'limit':'Native startup/client ownership and cutoff binding only; component provider/protocol evidence reused. No physical operation or autonomous recovery claim.'}
