#!/usr/bin/env python3
"""Offline preparation of one supervised fixed ADC application session."""
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
board=module('adc_board_prepare',ROOT/'apps/board/prepare.py')
base=board.base
require=base.require
SCHEMA='a733-adc-supervised-bundle-v1'
CONTRACT_MAGIC=0x4153373341444331
REQUEST_MAGIC=0x4153373341444c31

FACTS={
 'initial':['board_selection_dividers_0_3_passive_only','no_external_drive_or_strap_change','gpadc_exclusive_handoff','no_inherited_fifo_dma_handlers','remote_secure_writers_excluded','irq70_owned_masked'],
 'electrical':['avcc_cldo3_safe_stable_qualified','internal_reference_qualified','actual_divider_impedance_verified','inputs_within_vcc_adc_range','device_mappings_security_verified'],
 'clocks':['stable_24mhz_osc_ref_fabric_apb1','gpadc24m_policy_authorized','all_parent_writers_honor_registry'],
 'supervision':['whole_board_power_removal','permanent_retention','no_reuse','one_attempt']}

LRADC_FACTS={
 'initial':['lradc0_r23_avcc_passive_only','no_external_drive_or_strap_change','lradc_exclusive_handoff','lradc_local_gate_off_reset_owned','no_inherited_controller_or_handlers','remote_secure_writers_excluded','irq69_owned_masked','whole_board_gic_authority'],
 'electrical':['avcc_cldo3_safe_stable_qualified','lradc_internal_reference_qualified','actual_r23_population_verified','device_mappings_security_verified'],
 'clocks':['stable_24mhz_osc_ref_fabric_apb1','stable_rtc32k_and_selected_upstream','all_parent_writers_honor_registry','all_legacy_rtc_source_calibration_oscillator_writers_excluded'],
 'supervision':FACTS['supervision']}

def session_check(raw):
    require(len(raw)==784,'ADC session must be784bytes.')
    require(board.session_check(raw[:592])==2,'ADC requires USB0/provider selection2.')
    magic,version,size=struct.unpack_from('<3Q',raw,592)
    require(magic==0x4153373341444131 and version in (1,2) and size==784,'Invalid ADC session.')
    owner,authority=struct.unpack_from('<2Q',raw,616)
    require(owner and authority and owner not in struct.unpack_from('<2Q',raw,456),'Invalid ADC ownership.')
    require(all(any(raw[o:o+32]) for o in (632,664,696,728)),'Missing ADC physical evidence.')
    apb,hz=struct.unpack_from('<2I',raw,760);parent=apb>>24;div=(apb&31)+1;source={0:24000000,3:600000000}.get(parent,0)
    require(not apb&~0x0300001f and source and source%div==0 and hz==source//div,'Invalid ADC APB1 profile.')
    uv,impedance=struct.unpack_from('<2I',raw,776)
    require((raw[768]==0 if version==2 else raw[768] in (0,1)) and raw[769]==13 and raw[770:776]==bytes(6) and uv==1800000 and 1<=impedance<=1000000,'Invalid nominal ADC supply/acquisition profile.')

def request_bytes(address,identity,raw,epoch,generation,lease):
    request,contract=board.request_bytes(address,identity,epoch,generation,lease,raw)
    request=struct.pack('<Q',REQUEST_MAGIC)+request[8:]
    contract=bytearray(contract);struct.pack_into('<Q',contract,184,784)
    return request,bytes(contract)

def build_launcher(out,clang,lld):
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text()
    (out/'base-supervised-launch.c').write_text(text)
    text=text.replace('0x4153373353554c33','0x4153373341444c31').replace('REQUEST[1]==3','REQUEST[1]==2').replace('CONTRACT->version==3','CONTRACT->version==2')
    text=text.replace('CONTRACT->reserved[0] && !CONTRACT->reserved[1]','CONTRACT->reserved[0]==A733_BOARD_SESSION_ADDRESS && CONTRACT->reserved[1]==sizeof(struct adc_session)')
    text=text.replace('20*A733_SMP_COUNTER_HZ','60*A733_SMP_COUNTER_HZ')
    (out/'adc-launch-base.c').write_text(text)
    for name,path in [('launch.c',ROOT/'apps/adc/launch.c'),('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h'),('adc_session.h',ROOT/'apps/adc/adc_session.h'),('a733_board_application.h',ROOT/'include/a733_board_application.h')]:shutil.copy2(path,out/name)
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-Wall','-Wextra','-Werror','-DSUNSTONE_BOARD_SUPERVISED=1',f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC}ULL','-I'+str(out)]
    commands=[[clang,*flags,'-c',out/name,'-o',out/(name+'.o')] for name in ('launch.c','launch.S')]
    commands += [[lld,'-T',out/'launch.ld',out/'launch.c.o',out/'launch.S.o','-o',out/'launch.elf'],['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',out/'launch.elf',out/'launch.bin']]
    for command in commands:
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (out/'launcher-build.log').open('a') as f:f.write(r.stdout+r.stderr)
        require(r.returncode==0,'ADC launcher build failed; inspect retained log.')
    base.save_json(out/'launcher-commands.json',[[str(x) for x in c] for c in commands])
    require(0<(out/'launch.bin').stat().st_size<=0xc00,'Launcher exceeds SRAM.')

def validate_admission(record,image_hash,template_hash,raw,cache=None,frozen=False):
    # Existing DDR/platform/manual-recovery checks stay exact and byte-snapshotted.
    require(record.get('schema')=='a733-adc-supervised-entry-v1','Wrong ADC admission schema.')
    common={**record,'schema':'a733-board-supervised-entry-v2'}
    cache=board.validate_admission(common,image_hash,template_hash,cache,frozen)
    version=struct.unpack_from('<Q',raw,600)[0]
    facts=LRADC_FACTS if version==2 else FACTS
    for key,offset in [('initial',632),('electrical',664),('clocks',696),('supervision',728)]:
        item=record[key];path=Path(item['path']);require(path.is_absolute(),'Evidence path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Missing frozen ADC evidence.');cache[str(path)]=path.read_bytes()
        data=cache[str(path)];require(hashlib.sha256(data).digest()==raw[offset:offset+32] and hashlib.sha256(data).hexdigest()==item['sha256'],'ADC evidence differs.')
        fact=json.loads(data)
        require(fact.get('schema')=='a733-adc-'+key+'-v1' and fact.get('identity')==record['identity'] and fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and fact.get('reviewed') is True,'ADC authority is not bound to image/target.')
        require(all(fact.get(k) is True for k in facts[key]),'Required ADC physical fact absent.')
        if key=='electrical':require((fact.get('cldo3_uv'),fact.get('cldo3_selector'),fact.get('source_impedance_ohms'))==(struct.unpack_from('<I',raw,776)[0],raw[769],struct.unpack_from('<I',raw,780)[0]),'ADC supply/impedance authority differs.')
        if key=='electrical' and version==2:
            # The schematic pull-up exceeds the documented conversion range.
            # Neither a generic reviewed flag nor a saturated sample grants an
            # electrical rating. Require the separate target-bound determination.
            reference=fact.get('lradc_idle_electrical_authority')
            require(isinstance(reference,dict) and set(reference)=={'path','sha256'},'Missing independent LRADC idle electrical authority.')
            path=Path(reference['path']);require(path.is_absolute(),'LRADC electrical authority path must be absolute.')
            if str(path) not in cache:
                require(not frozen,'Missing frozen LRADC electrical authority.');cache[str(path)]=path.read_bytes()
            data=cache[str(path)];require(hashlib.sha256(data).hexdigest()==reference['sha256'],'LRADC electrical determination differs.')
            determination=json.loads(data)
            require(determination.get('schema')=='a733-lradc-idle-electrical-authority-v1' and
                    determination.get('identity')==record['identity'] and determination.get('port')==record['port'] and
                    determination.get('image_sha256')==image_hash and determination.get('reviewed') is True and
                    determination.get('basis') in ('manufacturer-explicit-idle-overrange-rating','independent-board-electrical-determination') and
                    determination.get('nominal_pullup_uv')==1800000 and determination.get('documented_conversion_max_uv')==1350000 and
                    determination.get('safe_idle_overrange_at_admitted_avcc') is True and
                    determination.get('not_inferred_from_saturation_or_keymap') is True,
                    'LRADC idle overrange lacks independent electrical authority.')

        if key=='clocks':
            require(fact.get('configure_adc_clock')==raw[768] and fact.get('apb1')==struct.unpack_from('<I',raw,760)[0] and fact.get('apb1_hz')==struct.unpack_from('<I',raw,764)[0],'ADC parent policy authority differs.')
            require(version==2 or not raw[768] or fact.get('all_ths_adc_consumers_excluded') is True,'Cold GPADC parent setup requires every THS/ADC consumer excluded.')
    return cache

def prepare(image,manifest,admission,template,output,clang,lld):
    snapshots={name:Path(path).read_bytes() for name,path in [('image.elf',image),('image-input-manifest.json',manifest),('admission.json',admission),('session-template.bin',template)]}
    raw=bytearray(snapshots['session-template.bin']);session_check(raw)
    ih=hashlib.sha256(snapshots['image.elf']).hexdigest();th=hashlib.sha256(raw).hexdigest()
    mf=json.loads(snapshots['image-input-manifest.json'])
    require(mf.get('target')=='sunstone-a733-adc' and mf.get('execution_profile')=='adc-supervised-v1' and mf.get('final_elf')==ih,'Tested ADC image manifest required.')
    record=json.loads(snapshots['admission.json']);cache=validate_admission(record,ih,th,raw)
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
    p=dict(schema=SCHEMA,physical_access=False,image_sha256=ih,build_id=identity.hex(),epoch=epoch,generation=generation,lease=lease,reservation=record['reservation'],session='session.bin',profile=None,manifest='image-input-manifest.json',image='image.elf',segments=uploads,entry_evidence=evidence,backup_sizes={name:n for name,a,n in base.REGIONS},files={x.name:base.digest(x) for x in out.iterdir() if x.is_file()})
    base.save_json(out/'prepared.json',p);return validate_bundle(out)

def validate_bundle(bundle):
    out=Path(bundle);p=json.loads((out/'prepared.json').read_text());require(p.get('schema')==SCHEMA and p.get('physical_access') is False,'Wrong ADC bundle.')
    required={'image.elf','image-input-manifest.json','admission.json','session-template.bin','session.bin','request.bin','contract.bin','launch.bin','result-zero.bin','report-zero.bin'}
    require(required<=set(p['files']) and all(Path(item['name']).name==item['name'] and item['name'] in p['files'] for item in p['entry_evidence'].values()),'Unsealed ADC authority or transport input.')
    for name,h in p['files'].items():require(Path(name).name==name and base.digest(out/name)==h,'ADC bundle input changed.')
    require(base.digest(out/'image.elf')==p['image_sha256'],'ADC image changed.')
    manifest=json.loads((out/'image-input-manifest.json').read_text())
    require(manifest.get('target')=='sunstone-a733-adc' and manifest.get('execution_profile')=='adc-supervised-v1' and manifest.get('final_elf')==p['image_sha256'],'ADC image manifest differs.')
    sizes={name:(out/name).stat().st_size for name in p['files']}
    require(p.get('backup_sizes')=={name:n for name,a,n in base.REGIONS} and 0<sizes.get('launch.bin',0)<=0xc00,'ADC protected SRAM geometry changed.')
    require((out/'result-zero.bin').read_bytes()==bytes(16384) and (out/'report-zero.bin').read_bytes()==bytes(128),'ADC initial report backing changed.')
    raw=(out/'session.bin').read_bytes();session_check(raw)
    template=(out/'session-template.bin').read_bytes();cache={path:(out/item['name']).read_bytes() for path,item in p['entry_evidence'].items()}
    validate_admission(json.loads((out/'admission.json').read_text()),p['image_sha256'],hashlib.sha256(template).hexdigest(),template,cache,True)
    segments,address,identity=base.image_parts(out/'image.elf');require(identity.hex()==p['build_id'],'ADC build identity changed.')
    expected_raw=bytearray(template);struct.pack_into('<3Q',expected_raw,24,p['epoch'],p['generation'],p['lease']);expected_raw[48:80]=identity
    require(raw==expected_raw,'ADC session differs from admitted derivation.')
    expected=[]
    for i,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{i}.bin';require((out/name).read_bytes()==segment['data'],'ADC ELF bytes changed.');expected.append({'name':name,'address':segment['address']})
    expected.append({'name':'session.bin','address':0x41f00000});require(p['segments']==expected and all(item['name'] in sizes for item in expected),'ADC upload spans changed or unsealed.')
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,p['epoch'],p['generation'],p['lease'])):require((out/name).read_bytes()==data,'ADC launch contract changed.')
    p['_sizes']={name:(out/name).stat().st_size for name in p['files']};return p

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','admission','template','output','clang','lld'):parser.add_argument('--'+name,type=Path,required=True)
    a=parser.parse_args();prepare(a.image,a.manifest,a.admission,a.template,a.output,a.clang,a.lld)
