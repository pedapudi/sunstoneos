#!/usr/bin/env python3
"""Offline preparation of one supervised fixed THERMAL application session."""
import io
from elftools.elf.elffile import ELFFile
import hashlib
import importlib.util
import json
from pathlib import Path
import secrets
import shutil
import struct
import subprocess
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
board=module('thermal_board_prepare',ROOT/'apps/board/prepare.py')
base=board.base
require=base.require
SCHEMA='a733-thermal-supervised-bundle-v1'
CONTRACT_MAGIC=0x4153373354484331
REQUEST_MAGIC=0x4153373354484c31

PROFILES={'sunstone-a733-thermal':'thermal-supervised-v1',
          'sunstone-a733-thermal-single-core':'thermal-single-core-supervised-v1'}
def image_manifest_check(manifest,raw):
    target=manifest.get('target');profile=PROFILES.get(target)
    require(profile and manifest.get('execution_profile')==profile and
            manifest.get('final_elf')==hashlib.sha256(raw).hexdigest(),'Tested THERMAL image manifest required.')
    elf=ELFFile(io.BytesIO(raw));symbols={v.name:v for v in elf.get_section_by_name('.symtab').iter_symbols()}
    marker=symbols.get('thermal_single_core_geometry')
    if target=='sunstone-a733-thermal-single-core':
        require(marker is not None and marker['st_size']==32 and
                symbols.get('sd_single_core_entry') and symbols['sd_single_core_entry']['st_value']==elf['e_entry'] and
                not any(n in symbols for n in ('a733_cpu_start_init','a733_cpu_primary_continue',
                    'a733_smp_pre_scheduler','FreeRTOS_SGI_Handler')),'THERMAL single-core startup differs.')
        address=int(marker['st_value']);parts=[p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and
            not p['p_flags']&2 and p['p_vaddr']<=address and address+32<=p['p_vaddr']+p['p_filesz']]
        require(len(parts)==1,'THERMAL geometry must be immutable and file backed.')
        offset=address-parts[0]['p_vaddr']
        require(struct.unpack('<4Q',parts[0].data()[offset:offset+32])==(0x4153373354485331,1,808,60),
                'THERMAL single-core execution geometry differs.')
    else:require(marker is None,'Single-core THERMAL image requires its execution profile.')
    return profile

def session_check(raw):
    require(len(raw)==808,'Thermal session must be808bytes.')
    require(board.session_check(raw[:592])==2,'Thermal requires USB0/provider selection2.')
    require(struct.unpack_from('<3Q',raw,592)==(0x4153373354484131,1,808),'Invalid thermal session.')
    owner,authority=struct.unpack_from('<2Q',raw,616)
    require(owner and authority and owner not in struct.unpack_from('<2Q',raw,456),'Invalid thermal owner.')
    require(all(any(raw[o:o+32]) for o in (632,664,696,728)),'Missing thermal physical evidence.')
    apb,hz,configure,alarm,critical,reserved=struct.unpack_from('<2I4B',raw,760)
    parent=apb>>24;rate={0:24000000,3:600000000}.get(parent,0);div=(apb&31)+1
    require(not apb&~0x0300001f and rate and rate%div==0 and hz==rate//div and configure in (0,1) and
        alarm==critical==31 and not reserved and raw[802:808]==bytes(6),'Unsupported thermal clock/control profile.')
    for i in range(5):
        hot,hysteresis,critical=struct.unpack_from('<3H',raw,772+6*i)
        require(0<critical<=hot<hysteresis<=4095,'Invalid thermal thresholds.')

def request_bytes(address,identity,raw,epoch,generation,lease):
    request,contract=board.request_bytes(address,identity,epoch,generation,lease,raw)
    request=struct.pack('<Q',REQUEST_MAGIC)+request[8:]
    contract=bytearray(contract);struct.pack_into('<Q',contract,184,808)
    return request,bytes(contract)

def build_launcher(out,clang,lld):
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text()
    (out/'base-supervised-launch.c').write_text(text)
    text=board.adapt_launcher(text,{'0x4153373353554c33':'0x4153373354484c31','REQUEST[1]==3':'REQUEST[1]==2','CONTRACT->version==3':'CONTRACT->version==2',
        'CONTRACT->reserved[0] && !CONTRACT->reserved[1]':'CONTRACT->reserved[0]==A733_BOARD_SESSION_ADDRESS && CONTRACT->reserved[1]==sizeof(struct thermal_session)',
        '20*A733_SMP_COUNTER_HZ':'60*A733_SMP_COUNTER_HZ'})
    (out/'thermal-launch-base.c').write_text(text)
    for name,path in [('launch.c',ROOT/'apps/thermal/launch.c'),('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h'),('thermal_session.h',ROOT/'apps/thermal/thermal_session.h'),('a733_board_application.h',ROOT/'include/a733_board_application.h')]:shutil.copy2(path,out/name)
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-Wall','-Wextra','-Werror','-DSUNSTONE_BOARD_SUPERVISED=1',f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC}ULL','-I'+str(out)]
    commands=[[clang,*flags,'-c',out/name,'-o',out/(name+'.o')] for name in ('launch.c','launch.S')]
    commands += [[lld,'-T',out/'launch.ld',out/'launch.c.o',out/'launch.S.o','-o',out/'launch.elf'],['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',out/'launch.elf',out/'launch.bin']]
    for command in commands:
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (out/'launcher-build.log').open('a') as f:f.write(r.stdout+r.stderr)
        require(r.returncode==0,'THERMAL launcher build failed; inspect retained log.')
    base.save_json(out/'launcher-commands.json',[[str(x) for x in c] for c in commands])
    require(0<(out/'launch.bin').stat().st_size<=0xc00,'Launcher exceeds SRAM.')

def validate_admission(record,image_hash,template_hash,raw,cache=None,frozen=False,execution_profile="thermal-supervised-v1"):
    # Existing DDR/platform/manual-recovery checks stay exact and byte-snapshotted.
    require(record.get('schema')=='a733-thermal-supervised-entry-v1','Wrong THERMAL admission schema.')
    common={**record,'schema':'a733-board-supervised-entry-v2'}
    cache=board.validate_admission(common,image_hash,template_hash,cache,frozen,
        'board-single-core-supervised-v1' if execution_profile=='thermal-single-core-supervised-v1' else 'board-supervised-v2')
    for key,offset in [('initial',632),('electrical',664),('clocks',696),('supervision',728)]:
        item=record[key];path=Path(item['path']);require(path.is_absolute(),'Evidence path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Missing frozen THERMAL evidence.');cache[str(path)]=path.read_bytes()
        data=cache[str(path)];require(hashlib.sha256(data).digest()==raw[offset:offset+32] and hashlib.sha256(data).hexdigest()==item['sha256'],'THERMAL evidence differs.')
        fact=json.loads(data)
        require(fact.get('schema')=='a733-thermal-'+key+'-v1' and fact.get('identity')==record['identity'] and fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and fact.get('reviewed') is True,'THERMAL authority is not bound to this image/target.')
        required={'initial':['exclusive_ths_and_irq71','remote_writers_excluded','no_inherited_thermal_service','security_access_authorized','gic_route_owned','boot_lineage_reviewed'],
            'electrical':['avcc_adc_digital_supplies_stable','sid_shadows_readable','device_mappings_verified','no_physical_aliases'],
            'clocks':['stable_apb1_ref_osc','no_clock_reset_writers','gpadc_consumers_excluded' if raw[768] else 'stable_enabled_gpadc24m'],
            'supervision':['whole_board_power_removal','permanent_retention','no_reuse','one_attempt']}[key]
        require(all(fact.get(k) is True for k in required),'Required THERMAL physical fact absent.')
    return cache

def prepare(image,manifest,admission,template,output,clang,lld):
    snapshots={name:Path(path).read_bytes() for name,path in [('image.elf',image),('image-input-manifest.json',manifest),('admission.json',admission),('session-template.bin',template)]}
    raw=bytearray(snapshots['session-template.bin']);session_check(raw)
    ih=hashlib.sha256(snapshots['image.elf']).hexdigest();th=hashlib.sha256(raw).hexdigest()
    mf=json.loads(snapshots['image-input-manifest.json'])
    execution_profile=image_manifest_check(mf,snapshots['image.elf'])
    require(struct.unpack_from('<I',raw,532)[0]==(2 if execution_profile=='thermal-single-core-supervised-v1' else 1),'Thermal USB takeover mode differs from execution profile.')
    record=json.loads(snapshots['admission.json']);cache=validate_admission(record,ih,th,raw,execution_profile=execution_profile)
    epoch,generation,lease=(secrets.randbits(64) or 1 for _ in range(3))
    struct.pack_into('<3Q',raw,24,epoch,generation,lease)
    out=Path(output);out.mkdir(parents=True,exist_ok=False)
    for name,data in snapshots.items():(out/name).write_bytes(data)
    segments,address,identity=base.image_parts(out/'image.elf')
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
    p=dict(schema=SCHEMA,physical_access=False,execution_profile=execution_profile,image_sha256=ih,build_id=identity.hex(),epoch=epoch,generation=generation,lease=lease,reservation=record['reservation'],session='session.bin',profile=None,manifest='image-input-manifest.json',image='image.elf',segments=uploads,entry_evidence=evidence,backup_sizes={name:n for name,a,n in base.REGIONS},files={x.name:base.digest(x) for x in out.iterdir() if x.is_file()})
    base.save_json(out/'prepared.json',p);return validate_bundle(out)

def validate_bundle(bundle):
    out=Path(bundle);p=json.loads((out/'prepared.json').read_text());require(p.get('schema')==SCHEMA and p.get('physical_access') is False,'Wrong THERMAL bundle.')
    required={'image.elf','image-input-manifest.json','admission.json','session-template.bin','session.bin','request.bin','contract.bin','launch.bin','result-zero.bin','report-zero.bin'}
    require(required<=set(p['files']) and all(Path(item['name']).name==item['name'] and item['name'] in p['files'] for item in p['entry_evidence'].values()),'Unsealed THERMAL authority or transport input.')
    for name,h in p['files'].items():require(Path(name).name==name and base.digest(out/name)==h,'THERMAL bundle input changed.')
    require(base.digest(out/'image.elf')==p['image_sha256'],'THERMAL image changed.')
    manifest=json.loads((out/'image-input-manifest.json').read_text())
    execution_profile=image_manifest_check(manifest,(out/'image.elf').read_bytes())
    require(p.get('execution_profile','thermal-supervised-v1')==execution_profile,'Thermal execution profile differs.')
    sizes={name:(out/name).stat().st_size for name in p['files']}
    require(p.get('backup_sizes')=={name:n for name,a,n in base.REGIONS} and 0<sizes.get('launch.bin',0)<=0xc00,'THERMAL protected SRAM geometry changed.')
    require((out/'result-zero.bin').read_bytes()==bytes(16384) and (out/'report-zero.bin').read_bytes()==bytes(128),'THERMAL initial report backing changed.')
    raw=(out/'session.bin').read_bytes();session_check(raw)
    require(struct.unpack_from('<I',raw,532)[0]==(2 if execution_profile=='thermal-single-core-supervised-v1' else 1),'Thermal USB takeover mode differs from execution profile.')
    template=(out/'session-template.bin').read_bytes();cache={path:(out/item['name']).read_bytes() for path,item in p['entry_evidence'].items()}
    validate_admission(json.loads((out/'admission.json').read_text()),p['image_sha256'],hashlib.sha256(template).hexdigest(),template,cache,True,execution_profile)
    segments,address,identity=base.image_parts(out/'image.elf');require(identity.hex()==p['build_id'],'THERMAL build identity changed.')
    expected_raw=bytearray(template);struct.pack_into('<3Q',expected_raw,24,p['epoch'],p['generation'],p['lease']);expected_raw[48:80]=identity
    require(raw==expected_raw,'THERMAL session differs from admitted derivation.')
    expected=[]
    for i,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{i}.bin';require((out/name).read_bytes()==segment['data'],'THERMAL ELF bytes changed.');expected.append({'name':name,'address':segment['address']})
    expected.append({'name':'session.bin','address':0x41f00000});require(p['segments']==expected and all(item['name'] in sizes for item in expected),'THERMAL upload spans changed or unsealed.')
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,p['epoch'],p['generation'],p['lease'])):require((out/name).read_bytes()==data,'THERMAL launch contract changed.')
    p['_sizes']={name:(out/name).stat().st_size for name in p['files']};return p

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','admission','template','output','clang','lld'):parser.add_argument('--'+name,type=Path,required=True)
    a=parser.parse_args();prepare(a.image,a.manifest,a.admission,a.template,a.output,a.clang,a.lld)
