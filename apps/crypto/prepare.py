#!/usr/bin/env python3
"""Offline preparation of one supervised fixed CRYPTO application session."""
import hashlib
import io
from elftools.elf.elffile import ELFFile
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
board=module('crypto_board_prepare',ROOT/'apps/board/prepare.py')
vectors=module('crypto_vectors',ROOT/'apps/crypto/vectors.py')
operation=module('crypto_operation',ROOT/'apps/crypto/request.py')
base=board.base
require=base.require
SCHEMA='a733-crypto-supervised-bundle-v1'
CONTRACT_MAGIC=0x4153373343524331
REQUEST_MAGIC=0x4153373343524c31

PROFILES={'sunstone-a733-crypto':'crypto-supervised-v1',
          'sunstone-a733-crypto-single-core':'crypto-single-core-supervised-v1'}
def image_manifest_check(manifest,raw):
    target=manifest.get('target');profile=PROFILES.get(target)
    require(profile and manifest.get('execution_profile')==profile and
            manifest.get('final_elf')==hashlib.sha256(raw).hexdigest(),'Tested CRYPTO image manifest required.')
    elf=ELFFile(io.BytesIO(raw));symbols={v.name:v for v in elf.get_section_by_name('.symtab').iter_symbols()}
    marker=symbols.get('crypto_single_core_geometry')
    if target=='sunstone-a733-crypto-single-core':
        require(marker is not None and marker['st_size']==32 and
                symbols.get('sd_single_core_entry') and symbols['sd_single_core_entry']['st_value']==elf['e_entry'] and
                not any(n in symbols for n in ('a733_cpu_start_init','a733_cpu_primary_continue',
                    'a733_smp_pre_scheduler','FreeRTOS_SGI_Handler')),'CRYPTO single-core startup differs.')
        address=int(marker['st_value']);parts=[p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and
            not p['p_flags']&2 and p['p_vaddr']<=address and address+32<=p['p_vaddr']+p['p_filesz']]
        require(len(parts)==1,'CRYPTO geometry must be immutable and file backed.')
        offset=address-parts[0]['p_vaddr']
        require(struct.unpack('<4Q',parts[0].data()[offset:offset+32])==(0x4153373343525331,1,824,60),
                'CRYPTO single-core execution geometry differs.')
    else:require(marker is None,'Single-core CRYPTO image requires its execution profile.')
    return profile

def session_check(raw):
    require(len(raw)==824,'Crypto session must be824bytes.')
    require(board.session_check(raw[:592],usb_reference_modes=(1,2))==2,'Crypto requires USB0/provider selection2.')
    require(struct.unpack_from('<Q',raw,592)[0]==0x4153373343524131 and struct.unpack_from('<Q',raw,600)[0] in (1,2) and struct.unpack_from('<Q',raw,608)[0]==824,'Invalid crypto session extension.')
    owner,authority=struct.unpack_from('<2Q',raw,616)
    require(owner and authority and owner not in struct.unpack_from('<2Q',raw,456),'Invalid crypto owner.')
    require(all(any(raw[o:o+32]) for o in (632,664,696,728)),'Missing crypto physical evidence.')
    if struct.unpack_from('<Q',raw,600)[0]==2:
        require(any(raw[760:792]),'Missing authenticated crypto request');return
    require(raw[760:792]==hashlib.sha256(vectors.INPUTS).digest() and raw[792:824]==hashlib.sha256(vectors.REFERENCE).digest(),'Crypto vectors differ.')

def embedded_reference(image,session):
    """Require exact fixed diagnostic bytes in immutable ELF sections."""
    import io
    from elftools.elf.elffile import ELFFile
    session_check(session)
    if struct.unpack_from('<Q',session,600)[0]==2:return None
    elf=ELFFile(io.BytesIO(image));table=elf.get_section_by_name('.symtab')
    require(table is not None,'Crypto image has no vector symbols.')
    symbols={s.name:(int(s['st_value']),int(s['st_size'])) for s in table.iter_symbols()}
    for name,expected in [('crypto_inputs',vectors.INPUTS),('crypto_reference',vectors.REFERENCE)]:
        require(name in symbols,'Missing crypto vector: '+name)
        first,size=symbols[name];require(size==len(expected),'Wrong crypto vector extent.')
        found=False
        for part in elf.iter_segments():
            if part['p_type']=='PT_LOAD' and not int(part['p_flags'])&2 and int(part['p_vaddr'])<=first and first+size<=int(part['p_vaddr'])+int(part['p_filesz']):
                start=first-int(part['p_vaddr']);require(part.data()[start:start+size]==expected,'Embedded crypto vector differs.');found=True;break
        require(found,'Crypto vector is not immutable/file-backed.')
    return vectors.REFERENCE

def request_bytes(address,identity,raw,epoch,generation,lease):
    request,contract=board.request_bytes(address,identity,epoch,generation,lease,raw)
    request=struct.pack('<Q',REQUEST_MAGIC)+request[8:]
    contract=bytearray(contract);struct.pack_into('<Q',contract,184,824)
    return request,bytes(contract)

def build_launcher(out,clang,lld):
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text()
    (out/'base-supervised-launch.c').write_text(text)
    text=text.replace('0x4153373353554c33','0x4153373343524c31').replace('REQUEST[1]==3','REQUEST[1]==2').replace('CONTRACT->version==3','CONTRACT->version==2')
    text=text.replace('CONTRACT->reserved[0] && !CONTRACT->reserved[1]','CONTRACT->reserved[0]==A733_BOARD_SESSION_ADDRESS && CONTRACT->reserved[1]==sizeof(struct crypto_session)')
    text=text.replace('20*A733_SMP_COUNTER_HZ','60*A733_SMP_COUNTER_HZ')
    (out/'crypto-launch-base.c').write_text(text)
    for name,path in [('launch.c',ROOT/'apps/crypto/launch.c'),('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h'),('crypto_session.h',ROOT/'apps/crypto/crypto_session.h'),('a733_board_application.h',ROOT/'include/a733_board_application.h')]:shutil.copy2(path,out/name)
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-Wall','-Wextra','-Werror','-DSUNSTONE_BOARD_SUPERVISED=1',f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC}ULL','-I'+str(out)]
    commands=[[clang,*flags,'-c',out/name,'-o',out/(name+'.o')] for name in ('launch.c','launch.S')]
    commands += [[lld,'-T',out/'launch.ld',out/'launch.c.o',out/'launch.S.o','-o',out/'launch.elf'],['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',out/'launch.elf',out/'launch.bin']]
    for command in commands:
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (out/'launcher-build.log').open('a') as f:f.write(r.stdout+r.stderr)
        require(r.returncode==0,'CRYPTO launcher build failed; inspect retained log.')
    base.save_json(out/'launcher-commands.json',[[str(x) for x in c] for c in commands])
    require(0<(out/'launch.bin').stat().st_size<=0xc00,'Launcher exceeds SRAM.')

def validate_admission(record,image_hash,template_hash,raw,cache=None,frozen=False,execution_profile="crypto-supervised-v1"):
    # Existing DDR/platform/manual-recovery checks stay exact and byte-snapshotted.
    require(record.get('schema')=='a733-crypto-supervised-entry-v1','Wrong CRYPTO admission schema.')
    common={**record,'schema':'a733-board-supervised-entry-v2'}
    cache=board.validate_admission(common,image_hash,template_hash,cache,frozen,
        "board-single-core-supervised-v1" if execution_profile=="crypto-single-core-supervised-v1" else "board-supervised-v2")
    for key,offset in [('initial',632),('memory',664),('clocks',696),('supervision',728)]:
        item=record[key];path=Path(item['path']);require(path.is_absolute(),'Evidence path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Missing frozen CRYPTO evidence.');cache[str(path)]=path.read_bytes()
        data=cache[str(path)];require(hashlib.sha256(data).digest()==raw[offset:offset+32] and hashlib.sha256(data).hexdigest()==item['sha256'],'CRYPTO evidence differs.')
        fact=json.loads(data)
        require(fact.get('schema')=='a733-crypto-'+key+'-v1' and fact.get('identity')==record['identity'] and fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and fact.get('reviewed') is True,'CRYPTO authority is not bound to this image/target.')
        required={'initial':['all_secure_nonsecure_ce_clients_excluded','remote_firmware_excluded','prior_ce_dma_independently_resolved','secure_bank_read_authorized','irq200_201_both_worlds_owned_masked','boot_lineage_reviewed'],
            'memory':['private_cacheable_page','no_physical_aliases','ce_physical_ram_reachable','exclusive_cache_lines','permanent_page_reservation'],
            'clocks':['observed_ce400_peri0','stable_ref_peri0_fabric','no_clock_reset_writers','ce_local_gates_resets_ready'],
            'supervision':['whole_board_power_removal','permanent_dma_retention','no_reuse','one_attempt']}[key]
        require(all(fact.get(k) is True for k in required),'Required CRYPTO physical fact absent.')
    return cache

def prepare(image,manifest,admission,template,output,clang,lld,request=None):
    snapshots={name:Path(path).read_bytes() for name,path in [('image.elf',image),('image-input-manifest.json',manifest),('admission.json',admission),('session-template.bin',template)]}
    raw=bytearray(snapshots['session-template.bin']);session_check(raw)
    if struct.unpack_from('<Q',raw,600)[0]==2:
        require(request is not None,'Caller-supplied operation required');snapshots['operation.bin']=Path(request).read_bytes();operation.decode(snapshots['operation.bin'],raw)
    else:require(request is None,'Diagnostic session cannot accept a runtime operation')
    embedded_reference(snapshots['image.elf'],raw)
    ih=hashlib.sha256(snapshots['image.elf']).hexdigest();th=hashlib.sha256(raw).hexdigest()
    mf=json.loads(snapshots['image-input-manifest.json'])
    execution_profile=image_manifest_check(mf,snapshots['image.elf'])
    require(struct.unpack_from('<I',raw,532)[0]==(2 if execution_profile=='crypto-single-core-supervised-v1' else 1),'Crypto USB mode differs from execution profile.')
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
    out=Path(bundle);p=json.loads((out/'prepared.json').read_text());require(p.get('schema')==SCHEMA and p.get('physical_access') is False,'Wrong CRYPTO bundle.')
    required={'image.elf','image-input-manifest.json','admission.json','session-template.bin','session.bin','request.bin','contract.bin','launch.bin','result-zero.bin','report-zero.bin'}
    require(required<=set(p['files']) and all(Path(item['name']).name==item['name'] and item['name'] in p['files'] for item in p['entry_evidence'].values()),'Unsealed CRYPTO authority or transport input.')
    for name,h in p['files'].items():require(Path(name).name==name and base.digest(out/name)==h,'CRYPTO bundle input changed.')
    require(base.digest(out/'image.elf')==p['image_sha256'],'CRYPTO image changed.')
    manifest=json.loads((out/'image-input-manifest.json').read_text())
    execution_profile=image_manifest_check(manifest,(out/'image.elf').read_bytes())
    require(p.get('execution_profile','crypto-supervised-v1')==execution_profile,'Crypto execution profile differs.')
    sizes={name:(out/name).stat().st_size for name in p['files']}
    require(p.get('backup_sizes')=={name:n for name,a,n in base.REGIONS} and 0<sizes.get('launch.bin',0)<=0xc00,'CRYPTO protected SRAM geometry changed.')
    require((out/'result-zero.bin').read_bytes()==bytes(16384) and (out/'report-zero.bin').read_bytes()==bytes(128),'CRYPTO initial report backing changed.')
    raw=(out/'session.bin').read_bytes();session_check(raw)
    require(struct.unpack_from('<I',raw,532)[0]==(2 if execution_profile=='crypto-single-core-supervised-v1' else 1),'Crypto USB mode differs from execution profile.')
    template=(out/'session-template.bin').read_bytes();cache={path:(out/item['name']).read_bytes() for path,item in p['entry_evidence'].items()}
    validate_admission(json.loads((out/'admission.json').read_text()),p['image_sha256'],hashlib.sha256(template).hexdigest(),template,cache,True,execution_profile)
    segments,address,identity=base.image_parts(out/'image.elf');require(identity.hex()==p['build_id'],'CRYPTO build identity changed.')
    expected_raw=bytearray(template);struct.pack_into('<3Q',expected_raw,24,p['epoch'],p['generation'],p['lease']);expected_raw[48:80]=identity
    require(raw==expected_raw,'CRYPTO session differs from admitted derivation.')
    expected=[]
    for i,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{i}.bin';require((out/name).read_bytes()==segment['data'],'CRYPTO ELF bytes changed.');expected.append({'name':name,'address':segment['address']})
    expected.append({'name':'session.bin','address':0x41f00000});require(p['segments']==expected and all(item['name'] in sizes for item in expected),'CRYPTO upload spans changed or unsealed.')
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,p['epoch'],p['generation'],p['lease'])):require((out/name).read_bytes()==data,'CRYPTO launch contract changed.')
    p['_reference']=embedded_reference((out/'image.elf').read_bytes(),raw)
    if struct.unpack_from('<Q',raw,600)[0]==2:
        require('operation.bin' in p['files'],'Unsealed crypto operation');p['_operation']=(out/'operation.bin').read_bytes();operation.decode(p['_operation'],raw)
    else:require('operation.bin' not in p['files'],'Unexpected diagnostic operation')
    p['_sizes']={name:(out/name).stat().st_size for name in p['files']};return p

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','admission','template','output','clang','lld'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--request',type=Path)
    a=parser.parse_args();prepare(a.image,a.manifest,a.admission,a.template,a.output,a.clang,a.lld,a.request)
