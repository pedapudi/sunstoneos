#!/usr/bin/env python3
"""Offline preparation of one supervised fixed PWM application session."""
import hashlib
import importlib.util
import json
from pathlib import Path
import secrets
import shutil
import struct
import subprocess
from types import SimpleNamespace
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
board=module('pwm_board_prepare',ROOT/'apps/board/prepare.py')
base=board.base
require=base.require
SCHEMA='a733-pwm-supervised-bundle-v2'
CONTRACT_MAGIC=0x4153373350574331
REQUEST_MAGIC=0x4153373350574c31

def session_check(raw):
    require(len(raw)==872,'PWM session must be 872 bytes.')
    require(board.session_check(raw[:592])==2,'PWM requires USB0/provider selection 2.')
    require(struct.unpack_from('<3Q',raw,592)==(0x4153373350574131,2,872),'Invalid pwm session extension.')
    owner,authority=struct.unpack_from('<2Q',raw,616)
    require(owner and authority and owner not in struct.unpack_from('<2Q',raw,456),'Invalid pwm owner.')
    require(all(any(raw[o:o+32]) for o in (632,664,696,728)),'Missing PWM authority.')
    ef,ep,ed,df,dp,dd,r0,r1=raw[760:768]
    require(ef in (0,15) and ep<=2 and ed<=3 and df==7 and dp<=2 and dd<=3 and not r0 and not r1,'Invalid fixed PD5 route.')
    apb,hz,operation=struct.unpack_from('<IIQ',raw,768)
    source=apb>>24;parent=24000000 if source==0 else 600000000 if source==3 else 0;div=(apb&31)+1
    require(not apb&~0x0300001f and parent and not parent%div and hz==parent//div and 200000<=operation<=50000000,'Invalid bounded PWM clock/profile.')
    led,latch_id,latch_owner,known,value=struct.unpack_from('<3Q2I',raw,784)
    ef,ep,ed=raw[816:819];apbs,hz=struct.unpack_from('<2I',raw,824);maximum,reserved=struct.unpack_from('<2I',raw,864)
    require(led in (0,1) and not any(raw[819:824]) and not reserved and 1<=maximum<=1000,'Invalid PWM service geometry.')
    if led:
        source=apbs>>24;div=(apbs&31)+1
        require(latch_id and latch_owner and known==0x3fff and not value&~0x3fff and ef in (0,1,15) and ep<=2 and ed<=3 and any(raw[832:864]),'Missing LED pin/latch authority.')
        require(not apbs&~0x0700001f and source in (0,4) and not 24000000%div and hz==24000000//div,'Unsupported RPIO clock profile.')
    else:require(not any(raw[792:864]),'Disabled LED carries authority/profile.')

def embedded_reference(image,session):
    """Bind actual immutable geometry and const build ID before behavioral use."""
    import io
    from elftools.elf.elffile import ELFFile
    session_check(session)
    elf=ELFFile(io.BytesIO(image));table=elf.get_section_by_name('.symtab')
    require(table is not None,'PWM image has no symbols.')
    symbols={s.name:(int(s['st_value']),int(s['st_size'])) for s in table.iter_symbols()}
    expected=struct.pack('<5Q',0x4153373350574131,872,512,25000,0)
    for name,body in [('pwm_geometry',expected)]:
        require(name in symbols,'Missing immutable PWM geometry.')
        first,size=symbols[name];require(size==len(body),'Wrong PWM geometry size.')
        parts=[p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and not int(p['p_flags'])&2 and int(p['p_vaddr'])<=first and first+size<=int(p['p_vaddr'])+int(p['p_filesz'])]
        require(len(parts)==1,'PWM geometry must be immutable/file-backed.')
        off=first-int(parts[0]['p_vaddr']);require(parts[0].data()[off:off+size]==body,'PWM geometry differs.')
    first,size=symbols.get('a733_smp_build_id',(0,0));require(size==32,'Const PWM build ID absent.')
    parts=[p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and not int(p['p_flags'])&2 and int(p['p_vaddr'])<=first and first+size<=int(p['p_vaddr'])+int(p['p_filesz'])]
    require(len(parts)==1,'PWM build ID must be immutable/file-backed.')
    off=first-int(parts[0]['p_vaddr']);require(parts[0].data()[off:off+32]==session[48:80],'Template and const PWM build ID differ.')
    return None

def request_bytes(address,identity,raw,epoch,generation,lease):
    request,contract=board.request_bytes(address,identity,epoch,generation,lease,raw)
    request=struct.pack('<Q',REQUEST_MAGIC)+request[8:]
    contract=bytearray(contract);struct.pack_into('<Q',contract,184,872)
    return request,bytes(contract)

def build_launcher(out,clang,lld):
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text()
    (out/'base-supervised-launch.c').write_text(text)
    text=text.replace('0x4153373353554c33','0x4153373350574c31').replace('REQUEST[1]==3','REQUEST[1]==2').replace('CONTRACT->version==3','CONTRACT->version==2')
    text=text.replace('CONTRACT->reserved[0] && !CONTRACT->reserved[1]','CONTRACT->reserved[0]==A733_BOARD_SESSION_ADDRESS && CONTRACT->reserved[1]==sizeof(struct pwm_session)')
    text=text.replace('20*A733_SMP_COUNTER_HZ','60*A733_SMP_COUNTER_HZ')
    (out/'pwm-launch-base.c').write_text(text)
    for name,path in [('launch.c',ROOT/'apps/pwm/launch.c'),('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h'),('pwm_session.h',ROOT/'apps/pwm/pwm_session.h'),('a733_board_application.h',ROOT/'include/a733_board_application.h'),('a733_pins.h',ROOT/'include/a733_pins.h')]:shutil.copy2(path,out/name)
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-Wall','-Wextra','-Werror','-DSUNSTONE_BOARD_SUPERVISED=1',f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC}ULL','-I'+str(out)]
    commands=[[clang,*flags,'-c',out/name,'-o',out/(name+'.o')] for name in ('launch.c','launch.S')]
    commands += [[lld,'-T',out/'launch.ld',out/'launch.c.o',out/'launch.S.o','-o',out/'launch.elf'],['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',out/'launch.elf',out/'launch.bin']]
    for command in commands:
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (out/'launcher-build.log').open('a') as f:f.write(r.stdout+r.stderr)
        require(r.returncode==0,'PWM launcher build failed; inspect retained log.')
    base.save_json(out/'launcher-commands.json',[[str(x) for x in c] for c in commands])
    require(0<(out/'launch.bin').stat().st_size<=0xc00,'Launcher exceeds SRAM.')

def validate_admission(record,image_hash,template_hash,raw,cache=None,frozen=False):
    # Existing DDR/platform/manual-recovery checks stay exact and byte-snapshotted.
    require(record.get('schema')=='a733-pwm-supervised-entry-v1','Wrong PWM admission schema.')
    common={**record,'schema':'a733-board-supervised-entry-v2'}
    cache=board.validate_admission(common,image_hash,template_hash,cache,frozen)
    for key,offset in [('initial',632),('memory',664),('power',696),('supervision',728)]:
        item=record[key];path=Path(item['path']);require(path.is_absolute(),'Evidence path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Missing frozen PWM evidence.');cache[str(path)]=path.read_bytes()
        data=cache[str(path)];require(hashlib.sha256(data).digest()==raw[offset:offset+32] and hashlib.sha256(data).hexdigest()==item['sha256'],'PWM evidence differs.')
        fact=json.loads(data)
        require(fact.get('schema')=='a733-pwm-'+key+'-v1' and fact.get('identity')==record['identity'] and fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and fact.get('reviewed') is True,'PWM authority is not bound to this image/target.')
        required={'initial':['cold_pwm0_controller','all_pwm0_clients_excluded','remote_firmware_excluded','irq67_owned_disabled_drained','no_clock_reset_writers','exclusive_pd5_route'],
            'memory':['identity_device_mappings','no_aliases','permanent_reservation'],
            'power':['bldo1_pd_1800000','source_qualified_settling','fan_5v_topology_reviewed','q2_r89_loading_reviewed','no_external_gate_driver','fan_operation_permitted','no_voltage_pad_writers'],
            'supervision':['whole_board_power_removal','permanent_resource_retention','one_attempt','no_watchdog_arm','no_reset_retry','no_claim_of_fault_fan_off','continuous_output_until_power_removal_on_fault']}[key]
        require(all(fact.get(k) is True for k in required),'Required PWM physical fact absent.')
    if struct.unpack_from('<Q',raw,784)[0]:
        item=record['led'];path=Path(item['path']);require(path.is_absolute(),'LED authority path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Missing frozen LED authority.');cache[str(path)]=path.read_bytes()
        data=cache[str(path)];fact=json.loads(data)
        require(hashlib.sha256(data).digest()==raw[832:864] and hashlib.sha256(data).hexdigest()==item['sha256'],'LED authority differs.')
        require(fact.get('schema')=='a733-pwm-led-v1' and fact.get('identity')==record['identity'] and fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and fact.get('reviewed') is True,'LED authority covers another image/instance.')
        require(all(fact.get(k) is True for k in ('pl7_gpio_route','complete_latch_authority','bldo1_1800000_dcdc1_3300000','r_apbs0_stable_profile','no_optical_off_claim','remote_gpio_writers_excluded')),'LED physical fact missing.')
    return cache

def prepare(image,manifest,admission,template,output,clang,lld):
    snapshots={name:Path(path).read_bytes() for name,path in [('image.elf',image),('image-input-manifest.json',manifest),('admission.json',admission),('session-template.bin',template)]}
    raw=bytearray(snapshots['session-template.bin']);session_check(raw)
    embedded_reference(snapshots['image.elf'],raw)
    ih=hashlib.sha256(snapshots['image.elf']).hexdigest();th=hashlib.sha256(raw).hexdigest()
    mf=json.loads(snapshots['image-input-manifest.json'])
    require(mf.get('target')=='sunstone-a733-pwm' and mf.get('execution_profile')=='pwm-service-supervised-v1' and mf.get('final_elf')==ih,'Tested PWM image manifest required.')
    record=json.loads(snapshots['admission.json']);cache=validate_admission(record,ih,th,raw)
    epoch,generation,lease=(secrets.randbits(64) or 1 for _ in range(3))
    struct.pack_into('<3Q',raw,24,epoch,generation,lease)
    out=Path(output);out.mkdir(parents=True,exist_ok=False)
    for name,data in snapshots.items():(out/name).write_bytes(data)
    segments,address,identity=base.image_parts(SimpleNamespace(read_bytes=lambda:snapshots['image.elf']))
    # Runtime authenticates linked build identity, not the ELF file hash.
    raw[48:80]=identity
    build_launcher(out,clang,lld)
    uploads=[]
    for i,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{i}.bin';(out/name).write_bytes(segment['data']);uploads.append({'name':name,'address':segment['address']})
    (out/'session.bin').write_bytes(raw);uploads.append({'name':'session.bin','address':0x41f00000})
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,epoch,generation,lease)):(out/name).write_bytes(data)
    for name,count in [('result-zero.bin',16384),('report-zero.bin',128)]:(out/name).write_bytes(bytes(count))
    evidence={}
    for i,(path,data) in enumerate(cache.items()):
        name=f'evidence-{i}.bin';(out/name).write_bytes(data);evidence[path]={'name':name,'sha256':hashlib.sha256(data).hexdigest()}
    p=dict(schema=SCHEMA,physical_access=False,image_sha256=ih,build_id=identity.hex(),epoch=epoch,generation=generation,lease=lease,reservation=record['reservation'],session='session.bin',profile=None,manifest='image-input-manifest.json',image='image.elf',segments=uploads,entry_evidence=evidence,backup_sizes={name:n for name,a,n in base.REGIONS},files={x.name:base.digest(x) for x in out.iterdir() if x.is_file()})
    base.save_json(out/'prepared.json',p);return validate_bundle(out)

def validate_bundle(bundle):
    out=Path(bundle);metadata=(out/'prepared.json').read_bytes();p=json.loads(metadata);require(p.get('schema')==SCHEMA and p.get('physical_access') is False,'Wrong PWM bundle.')
    required={'image.elf','image-input-manifest.json','admission.json','session-template.bin','session.bin','request.bin','contract.bin','launch.bin','result-zero.bin','report-zero.bin'}
    require(required<=set(p['files']) and all(Path(item['name']).name==item['name'] and item['name'] in p['files'] for item in p['entry_evidence'].values()),'Unsealed PWM authority or transport input.')
    data={}
    for name,h in p['files'].items():
        require(Path(name).name==name,'Invalid PWM filename.');data[name]=(out/name).read_bytes()
        require(hashlib.sha256(data[name]).hexdigest()==h,'PWM input changed.')
    require(hashlib.sha256(data['image.elf']).hexdigest()==p['image_sha256'],'PWM image changed.')
    manifest=json.loads(data['image-input-manifest.json'])
    require(manifest.get('target')=='sunstone-a733-pwm' and manifest.get('execution_profile')=='pwm-service-supervised-v1' and manifest.get('final_elf')==p['image_sha256'],'PWM image manifest differs.')
    sizes={name:len(value) for name,value in data.items()}
    require(p.get('backup_sizes')=={name:n for name,a,n in base.REGIONS} and 0<sizes.get('launch.bin',0)<=0xc00,'PWM protected SRAM geometry changed.')
    require(data['result-zero.bin']==bytes(16384) and data['report-zero.bin']==bytes(128),'PWM initial report backing changed.')
    raw=data['session.bin'];session_check(raw)
    template=data['session-template.bin'];cache={path:data[item['name']] for path,item in p['entry_evidence'].items()}
    validate_admission(json.loads(data['admission.json']),p['image_sha256'],hashlib.sha256(template).hexdigest(),template,cache,True)
    segments,address,identity=base.image_parts(SimpleNamespace(read_bytes=lambda:data['image.elf']));require(identity.hex()==p['build_id'],'PWM build identity changed.')
    expected_raw=bytearray(template);struct.pack_into('<3Q',expected_raw,24,p['epoch'],p['generation'],p['lease']);expected_raw[48:80]=identity
    require(raw==expected_raw,'PWM session differs from admitted derivation.')
    expected=[]
    for i,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{i}.bin';require(data[name]==segment['data'],'PWM ELF bytes changed.');expected.append({'name':name,'address':segment['address']})
    expected.append({'name':'session.bin','address':0x41f00000});require(p['segments']==expected and all(item['name'] in sizes for item in expected),'PWM upload spans changed or unsealed.')
    for name,value in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,p['epoch'],p['generation'],p['lease'])):require(data[name]==value,'PWM launch contract changed.')
    p['_reference']=embedded_reference(data['image.elf'],raw)
    p['_sizes']={name:len(value) for name,value in data.items()};p['_frozen']=data;p['_prepared_sha256']=hashlib.sha256(metadata).hexdigest();return p

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','admission','template','output','clang','lld'):parser.add_argument('--'+name,type=Path,required=True)
    a=parser.parse_args();prepare(a.image,a.manifest,a.admission,a.template,a.output,a.clang,a.lld)
