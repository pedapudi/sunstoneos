#!/usr/bin/env python3
"""Offline preparation of one supervised fixed RTC application session."""
import io
import types
from elftools.elf.elffile import ELFFile
import datetime
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
board=module('rtc_board_prepare',ROOT/'apps/board/prepare.py')
base=board.base
require=base.require
SCHEMA='a733-rtc-supervised-bundle-v1'
CONTRACT_MAGIC=0x4153373352544331
REQUEST_MAGIC=0x4153373352544c31

artifact=board.supervisor.observer.artifact
PROFILES={'sunstone-a733-rtc':'rtc-supervised-v1',
          'sunstone-a733-rtc-single-core':'rtc-single-core-supervised-v1'}
def image_manifest_check(manifest,raw):
    target=manifest.get('target');profile=PROFILES.get(target)
    require(profile and manifest.get('execution_profile')==profile and
            manifest.get('final_elf')==hashlib.sha256(raw).hexdigest(),'Tested RTC image manifest required.')
    elf=ELFFile(io.BytesIO(raw));symbols={v.name:v for v in elf.get_section_by_name('.symtab').iter_symbols()}
    marker=symbols.get('rtc_single_core_geometry')
    if target=='sunstone-a733-rtc-single-core':
        require(marker is not None and marker['st_size']==32 and
                symbols.get('sd_single_core_entry')['st_value']==elf['e_entry'] and
                not any(n in symbols for n in ('a733_cpu_start_init','a733_cpu_primary_continue',
                    'a733_smp_pre_scheduler','FreeRTOS_SGI_Handler')),'RTC single-core startup differs.')
        address=int(marker['st_value']);parts=[p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and
            not p['p_flags']&2 and p['p_vaddr']<=address and address+32<=p['p_vaddr']+p['p_filesz']]
        require(len(parts)==1,'RTC geometry must be immutable and file backed.')
        offset=address-parts[0]['p_vaddr']
        require(struct.unpack('<4Q',parts[0].data()[offset:offset+32])==(0x4153373352545331,1,784,60),
                'RTC single-core execution geometry differs.')
    else:require(marker is None,'Single-core RTC image requires its execution profile.')
    return profile

def single_core_platform(value,prepared,identity,port):
    require(value.get('schema')=='a733-rtc-single-core-platform-v1' and value.get('identity')==identity and
            value.get('port')==port and value.get('image_sha256')==prepared['image_sha256'],
            'RTC single-core platform admission differs.')
    required={'primary-cache-transition-and-secondary-off','cold-cache-and-dirty-alias-exclusion',
              'dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded',
              'physical-code-publication','usb-clock-power-reset-ownership','usb-device-mapping',
              'physical-counter-continuity','rom-usb0-pio-takeover',
              'usb0-irq188-189-190-disabled-inactive-clear','usb-supplies-stable-and-writers-excluded'}
    require(set(value.get('conditions',{}))==required,'RTC single-core platform conditions differ.')
    for item in value['conditions'].values():artifact(item)
    require(value.get('entry_conditions_established') is True and value.get('physical_coherence_result')=='under-test',
            'RTC platform record must establish actual entry conditions.')

def session_check(raw):
    require(len(raw) in (768,784),'RTC session must be768 or784bytes.')
    require(board.session_check(raw[:592],usb_reference_modes=(1,2))==2,'RTC requires USB0/provider selection2.')
    version=struct.unpack_from('<Q',raw,600)[0]
    require((len(raw)==768 and version==1) or (len(raw)==784 and version in (2,3,4)),'RTC version/extent differs.')
    require(struct.unpack_from('<3Q',raw,592)==(0x4153373352544131,version,len(raw)),'Invalid RTC session extension.')
    if version==4:require(struct.unpack_from('<I',raw,772)[0]<65535,'Service seed needs a distinct second valid day.')
    if version>=2:
        flags,day,hms,reserved=struct.unpack_from('<4I',raw,768)
        require(((version==2 and 0<flags<4) or (version in (3,4) and flags in (4,5))) and not reserved,'Explicit RTC mode flags required.')
        require((flags&6 and 1<=day<=65535 and not hms&~0x1f3f3f and (hms>>16)<24 and ((hms>>8)&63)<60 and (hms&63)<60) or
                (not flags&6 and not day and not hms),'Invalid RTC calendar target.')
    owner,authority=struct.unpack_from('<2Q',raw,616)
    require(owner and authority and owner not in struct.unpack_from('<2Q',raw,456),'Invalid RTC owner.')
    require(all(any(raw[o:o+32]) for o in (632,664,696,728)),'Missing RTC physical evidence.')
    delay,reserved=struct.unpack_from('<2I',raw,760)
    require(1<=delay<=10 and not reserved,'RTC alarm delay must be1..10seconds.')

def calendar_target(year,month,date,hour,minute,second):
    target=datetime.datetime(year,month,date,hour,minute,second)
    day=(target.date()-datetime.date(1970,1,1)).days+1
    require(1<=day<=65535,'Calendar exceeds the TypeA16-bit day range.')
    return day,(hour<<16)|(minute<<8)|second

def setting_session(adopted,flags,day,hms,clock_digest,service=False):
    """Produce an explicit initialization template; authority is reviewed separately."""
    session_check(adopted)
    require(len(adopted)==768 and len(clock_digest)==32,'An adoption base and exact clock evidence digest are required.')
    raw=bytearray(adopted)+struct.pack('<4I',flags,day,hms,0)
    struct.pack_into('<2Q',raw,600,4 if service else 3 if flags&4 else 2,len(raw));raw[664:696]=clock_digest
    session_check(raw);return bytes(raw)

def request_bytes(address,identity,raw,epoch,generation,lease):
    request,contract=board.request_bytes(address,identity,epoch,generation,lease,raw)
    request=struct.pack('<Q',REQUEST_MAGIC)+request[8:]
    contract=bytearray(contract);struct.pack_into('<Q',contract,184,len(raw))
    return request,bytes(contract)

def build_launcher(out,clang,lld):
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text()
    (out/'base-supervised-launch.c').write_text(text)
    text=board.adapt_launcher(text,{'0x4153373353554c33':'0x4153373352544c31','REQUEST[1]==3':'REQUEST[1]==2','CONTRACT->version==3':'CONTRACT->version==2',
        'CONTRACT->reserved[0] && !CONTRACT->reserved[1]':'CONTRACT->reserved[0]==A733_BOARD_SESSION_ADDRESS && (CONTRACT->reserved[1]==sizeof(struct a733_rtc_session) || CONTRACT->reserved[1]==sizeof(struct a733_rtc_session_set))',
        '20*A733_SMP_COUNTER_HZ':'60*A733_SMP_COUNTER_HZ','CONTRACT->startup_deadline=start+A733_SMP_COUNTER_HZ;':'CONTRACT->startup_deadline=start+6*A733_SMP_COUNTER_HZ;'})
    (out/'rtc-launch-base.c').write_text(text)
    for name,path in [('launch.c',ROOT/'apps/rtc/launch.c'),('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h'),('a733_rtc_application.h',ROOT/'include/a733_rtc_application.h'),('a733_board_application.h',ROOT/'include/a733_board_application.h'),('a733_rtc_initialize.h',ROOT/'include/a733_rtc_initialize.h'),('a733_rtc.h',ROOT/'include/a733_rtc.h')]:shutil.copy2(path,out/name)
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-Wall','-Wextra','-Werror','-DSUNSTONE_BOARD_SUPERVISED=1','-DA733_BOARD_STARTUP_SECONDS=6',f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC}ULL','-I'+str(out)]
    commands=[[clang,*flags,'-c',out/name,'-o',out/(name+'.o')] for name in ('launch.c','launch.S')]
    commands += [[lld,'-T',out/'launch.ld',out/'launch.c.o',out/'launch.S.o','-o',out/'launch.elf'],['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',out/'launch.elf',out/'launch.bin']]
    base.save_json(out/'launcher-commands.json',[[str(x) for x in c] for c in commands])
    for command in commands:
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (out/'launcher-build.log').open('a') as f:f.write(r.stdout+r.stderr)
        require(r.returncode==0,'RTC launcher build failed; inspect retained log.')
    require(0<(out/'launch.bin').stat().st_size<=0xc00,'Launcher exceeds SRAM.')

def validate_admission(record,image_hash,template_hash,raw,cache=None,frozen=False,execution_profile="rtc-supervised-v1"):
    # Existing DDR/platform/manual-recovery checks stay exact and byte-snapshotted.
    require(record.get('schema')=='a733-rtc-supervised-entry-v1','Wrong RTC admission schema.')
    common={**record,'schema':'a733-board-supervised-entry-v2'}
    validate=board.validate_admission
    if execution_profile=='rtc-single-core-supervised-v1':
        observer=types.SimpleNamespace(validate_platform=single_core_platform)
        validate=types.FunctionType(validate.__code__,{**validate.__globals__,
            'supervisor':types.SimpleNamespace(observer=observer)},argdefs=validate.__defaults__)
    cache=validate(common,image_hash,template_hash,cache,frozen)
    for key,offset in [('initial',632),('clock',664),('electrical',696),('supervision',728)]:
        item=record[key];path=Path(item['path']);require(path.is_absolute(),'Evidence path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Missing frozen RTC evidence.');cache[str(path)]=path.read_bytes()
        data=cache[str(path)];require(hashlib.sha256(data).digest()==raw[offset:offset+32] and hashlib.sha256(data).hexdigest()==item['sha256'],'RTC evidence differs.')
        fact=json.loads(data)
        require(fact.get('schema')=='a733-rtc-'+key+'-v1' and fact.get('identity')==record['identity'] and fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and fact.get('reviewed') is True,'RTC authority is not bound to this image/target.')
        required={'initial':['exclusive_rtc_and_rccu_word','irq228_owned_disabled_inactive_unpending',
                    'remote_writers_excluded','other_cores_excluded','inherited_alarm_disabled_unpending'],
            'clock':['source_writers_excluded'] if len(raw)==784 else ['external_32768_stable','calendar_initialized','source_writers_excluded'],
            'electrical':['fitted_board_identified','vcc_rtc_power_stable'],
            'supervision':['whole_board_power_removal','permanent_ownership','no_reuse','one_attempt']}[key]
        require(all(fact.get(k) is True for k in required),'Required RTC physical fact absent.')
    if len(raw)==784:
        flags,day,hms,_=struct.unpack_from('<4I',raw,768)
        fact=json.loads(cache[str(Path(record['clock']['path']))])
        if not flags&1:require(fact.get('external_32768_stable') is True,'Calendar setting needs an admitted stable external source.')
        if not flags&6:require(fact.get('calendar_initialized') is True,'Source-only initialization needs an admitted calendar.')
        if flags&1:
            require(all(fact.get(k) is True for k in ('source_change_authorized','exclusive_osc32k_consumers','remote_source_writers_excluded','onboard_crystal_fitted')),
                    'RTC source change lacks exclusive inherited-consumer authority.')
        if flags&6:
            require(fact.get('calendar_write_authorized') is True and fact.get('requested_day')==day and fact.get('requested_hms')==hms,
                    'RTC calendar target lacks explicit authorization.')
        if struct.unpack_from('<Q',raw,600)[0]==4:
            require(fact.get('registered_native_calendar_clients_authorized') is True,'RTC native clients lack calendar-write authority.')
        if flags&4:
            require(all(fact.get(k) is True for k in ('exclusive_calendar_writer_lifetime','remote_calendar_writers_excluded')),
                    'Runtime calendar setting lacks retained exclusive writer authority.')
    return cache

def prepare(image,manifest,admission,template,output,clang,lld):
    snapshots={name:Path(path).read_bytes() for name,path in [('image.elf',image),('image-input-manifest.json',manifest),('admission.json',admission),('session-template.bin',template)]}
    raw=bytearray(snapshots['session-template.bin']);session_check(raw)
    ih=hashlib.sha256(snapshots['image.elf']).hexdigest();th=hashlib.sha256(raw).hexdigest()
    mf=json.loads(snapshots['image-input-manifest.json'])
    execution_profile=image_manifest_check(mf,snapshots['image.elf'])
    require(struct.unpack_from('<I',raw,532)[0]==(2 if execution_profile=='rtc-single-core-supervised-v1' else 1),'RTC USB takeover mode differs from execution profile.')
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
    out=Path(bundle);p=json.loads((out/'prepared.json').read_text());require(p.get('schema')==SCHEMA and p.get('physical_access') is False,'Wrong RTC bundle.')
    required={'image.elf','image-input-manifest.json','admission.json','session-template.bin','session.bin','request.bin','contract.bin','launch.bin','result-zero.bin','report-zero.bin'}
    require(required<=set(p['files']) and all(Path(item['name']).name==item['name'] and item['name'] in p['files'] for item in p['entry_evidence'].values()),'Unsealed RTC authority or transport input.')
    for name,h in p['files'].items():require(Path(name).name==name and base.digest(out/name)==h,'RTC bundle input changed.')
    require(base.digest(out/'image.elf')==p['image_sha256'],'RTC image changed.')
    manifest=json.loads((out/'image-input-manifest.json').read_text())
    execution_profile=image_manifest_check(manifest,(out/'image.elf').read_bytes())
    require(p.get('execution_profile', 'rtc-supervised-v1')==execution_profile,'RTC execution profile differs.')
    sizes={name:(out/name).stat().st_size for name in p['files']}
    require(p.get('backup_sizes')=={name:n for name,a,n in base.REGIONS} and 0<sizes.get('launch.bin',0)<=0xc00,'RTC protected SRAM geometry changed.')
    require((out/'result-zero.bin').read_bytes()==bytes(16384) and (out/'report-zero.bin').read_bytes()==bytes(128),'RTC initial report backing changed.')
    raw=(out/'session.bin').read_bytes();session_check(raw)
    require(struct.unpack_from('<I',raw,532)[0]==(2 if execution_profile=='rtc-single-core-supervised-v1' else 1),'RTC USB takeover mode differs from execution profile.')
    template=(out/'session-template.bin').read_bytes();cache={path:(out/item['name']).read_bytes() for path,item in p['entry_evidence'].items()}
    validate_admission(json.loads((out/'admission.json').read_text()),p['image_sha256'],hashlib.sha256(template).hexdigest(),template,cache,True,execution_profile)
    segments,address,identity=base.image_parts(out/'image.elf');require(identity.hex()==p['build_id'],'RTC build identity changed.')
    expected_raw=bytearray(template);struct.pack_into('<3Q',expected_raw,24,p['epoch'],p['generation'],p['lease']);expected_raw[48:80]=identity
    require(raw==expected_raw,'RTC session differs from admitted derivation.')
    expected=[]
    for i,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{i}.bin';require((out/name).read_bytes()==segment['data'],'RTC ELF bytes changed.');expected.append({'name':name,'address':segment['address']})
    expected.append({'name':'session.bin','address':0x41f00000});require(p['segments']==expected and all(item['name'] in sizes for item in expected),'RTC upload spans changed or unsealed.')
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,p['epoch'],p['generation'],p['lease'])):require((out/name).read_bytes()==data,'RTC launch contract changed.')
    p['_sizes']={name:(out/name).stat().st_size for name in p['files']};return p

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','admission','template','output','clang','lld'):parser.add_argument('--'+name,type=Path,required=True)
    a=parser.parse_args();prepare(a.image,a.manifest,a.admission,a.template,a.output,a.clang,a.lld)
