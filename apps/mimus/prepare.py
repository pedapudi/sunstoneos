#!/usr/bin/env python3
"""Freeze a reviewed native mimus session and 180-second SRAM launch offline."""
import argparse
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import types
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
supervisor=module('mimus_prepare_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
require,digest,save_json=supervisor.require,supervisor.digest,supervisor.save_json
SCHEMA='a733-mimus-supervised-bundle-v1'
SESSION_ADDRESS=0x41f00000
CONTRACT_MAGIC=0x415337334d494333
REQUEST_MAGIC=0x415337334d494c33
CONDITIONS={'cached-eight-core-entry-and-shared-coherence','device-mappings-and-cache-alias-exclusion',
 'pmic-usb-inherited-power-and-fixed-sink','model-assets-and-inference-arena-exclusive',
 'manual-whole-board-power-removal','retain-all-resources-until-power-removal','stop-after-first-failure'}

def sha(data):return hashlib.sha256(data).hexdigest()

def image_data(raw):
    elf=ELFFile(io.BytesIO(raw))
    require(elf.elfclass==64 and elf.little_endian and elf['e_type']=='ET_EXEC' and elf['e_machine']=='EM_AARCH64' and elf['e_entry']==0x41000000,'Native mimus ELF required')
    parts=[]
    for s in elf.iter_segments():
        if s['p_type']!='PT_LOAD':continue
        a,m,n,o,flags=(int(s[k]) for k in ('p_vaddr','p_memsz','p_filesz','p_offset','p_flags'))
        require(a==s['p_paddr'] and 0x41000000<=a<a+m<=0x42200000 and 0<=n<=m and 0<=o<=o+n<=len(raw) and flags&4 and not flags&~7 and flags&3!=3,'mimus segment outside compact DDR ownership')
        require(not flags&1 or a+m<=0x41200000,'Executable mimus data outside code prefix')
        require(not n or a+n<=SESSION_ADDRESS,'mimus initialized bytes overlap session or monitor')
        require(all(a+m<=p['address'] or p['address']+p['memory_bytes']<=a for p in parts),'Overlapping mimus ELF allocations')
        parts.append({'address':a,'memory_bytes':m,'flags':flags,'data':raw[o:o+n]})
    table=elf.get_section_by_name('.symtab');require(table is not None,'Native mimus symbol table required')
    symbols={s.name:s for s in table.iter_symbols()}
    for name,address in [('__a733_smp_result',0x42000000),('__a733_smp_contract',0x42004000),('__a733_smp_contract_end',0x42005000),('__storage_session_start',SESSION_ADDRESS),('__storage_session_end',SESSION_ADDRESS+4096)]:
        require(name in symbols and symbols[name]['st_value']==address,'mimus linker reservation differs: '+name)
    def read(name,n):
        require(name in symbols and symbols[name]['st_size']>=n,'Missing mimus immutable symbol: '+name)
        a=int(symbols[name]['st_value'])
        for p in parts:
            if p['address']<=a and a+n<=p['address']+len(p['data']):
                require(not p['flags']&2,'Writable mimus immutable input')
                return a,p['data'][a-p['address']:a-p['address']+n]
        raise ValueError('mimus immutable input lacks file backing')
    address,identity=read('a733_smp_build_id',32)
    require(0x41000000<=address<=0x411fffe0 and {'mimus_application_start','mimus_native_checkpoint','a733_smp_diagnostic_start'}<=set(symbols),'Wrong native application identity')
    return parts,address,identity,{'size':704,'wire':'a733-mimus-supervised-2'}

def session_profile(raw,layout,identity):
    require(len(raw)==704 and struct.unpack_from('<3Q',raw)==(0x415337334d494d32,2,704),'Invalid supervised mimus wire session')
    require(raw[656:688]==identity,'Mimus immutable build identity differs')
    epoch,generation,lease=struct.unpack_from('<3Q',raw,24)
    require(all((epoch,generation,lease)) and all(any(raw[a:b]) for a,b in ((48,80),(80,112),(184,216),(216,248),(424,456))),'Missing mimus identity/evidence')
    require(not any(raw[112:160]+raw[256:416]+raw[456:488]+raw[504:512]+raw[552:556]+raw[560:632]+raw[638:656]+raw[688:704]),'Uploaded mimus watchdog/deadline or storage authority forbidden')
    version,paced,extent=struct.unpack_from('<3Q',raw,160)
    topology=struct.unpack_from('<Q',raw,416)[0];owners=struct.unpack_from('<2Q',raw,512)
    require(version==2 and paced in (0,1) and 128<=extent<=33554432 and not any(raw[248:256]) and topology and all(owners) and owners[0]!=owners[1],'Mimus application/provider profile differs')
    return dict(epoch=epoch,generation=generation,lease=lease)

def configuration_sha256(raw):
    value=bytearray(raw);value[80:112]=bytes(32);return sha(value)

def validate_admission(admission_raw,raw,layout,identity,image_hash,cache=None,frozen=False):
    record=json.loads(admission_raw);profile=session_profile(raw,layout,identity)
    require(record.get('schema')=='a733-mimus-supervised-entry-v1' and record.get('image_sha256')==image_hash and
      raw[48:80]==bytes.fromhex(image_hash) and raw[80:112]==hashlib.sha256(admission_raw).digest() and
      record.get('configuration_sha256')==configuration_sha256(raw) and all(record.get(k)==v for k,v in profile.items()),'Mimus session does not bind reviewed image/entry')
    cache={} if cache is None else cache
    def evidence(ref):
        require(isinstance(ref,dict) and set(ref)=={'path','sha256'},'Invalid mimus evidence reference')
        p=Path(ref['path']);require(p.is_absolute(),'Mimus evidence path must be absolute')
        if str(p) not in cache:require(not frozen,'Missing frozen mimus entry evidence');cache[str(p)]=p.read_bytes()
        data=cache[str(p)];require(sha(data)==ref['sha256'],'Mimus entry evidence changed');return data
    target,port=record.get('identity'),record.get('port');require(target and port,'Explicit mimus target/port required')
    ddr_raw=evidence(record['ddr']);ddr=json.loads(ddr_raw)
    require(raw[424:456]==hashlib.sha256(ddr_raw).digest() and ddr.get('schema')=='a733-ddr-reservation-v1' and
      ddr.get('identity')==target and ddr.get('port')==port and ddr.get('image_sha256')==image_hash and
      ddr.get('ranges')==[[0x41000000,0x42200000],[0x43000000,0x45000000],[0x46000000,0x47000000]] and
      all(ddr.get(k) is True for k in ('exclusive','published_cache_disabled','remote_writers_excluded')) and ddr.get('evidence'),'Actual mimus image/assets/arena DDR ownership required')
    for ref in ddr['evidence']:evidence(ref)
    platform=json.loads(evidence(record['platform']));original=supervisor.observer.validate_platform
    types.FunctionType(original.__code__,{**original.__globals__,'artifact':evidence})(platform,{'image_sha256':image_hash},target,port)
    require(set(record.get('conditions',{}))==CONDITIONS and record.get('one_attempt') is True and
      record.get('whole_board_power_removal') is True and record.get('no_resource_reuse_before_power_removal') is True,'Mimus retained supervision evidence required')
    for ref in record['conditions'].values():evidence(ref)
    reference=json.loads(evidence(record['recognition_reference']))
    require(reference.get('exit')==0 and reference.get('bundle_sha256')==raw[216:248].hex() and bool(reference.get('paced',False))==bool(struct.unpack_from('<Q',raw,168)[0]) and reference.get('stdout'),'Pinned successful reference for exact mimus assets/mode required')
    return cache

def validate_assets(raw,session):
    require(len(raw)==struct.unpack_from('<Q',session,176)[0] and sha(raw)==session[216:248].hex() and raw[:8]==b'SUNMIM01','Mimus authenticated assets differ')
    offset=128
    for size in struct.unpack_from('<3Q',raw,8):
        require(size and offset+size<=len(raw),'Mimus asset extent differs');offset=(offset+size+63)&~63
        require(offset<=len(raw),'Mimus asset padding differs')

def request_bytes(address,identity,raw,profile):
    e,g,l=(profile[k] for k in ('epoch','generation','lease'));h=hashlib.sha256(raw).digest()
    request=struct.pack('<8Q',REQUEST_MAGIC,3,128,e,g,g^((1<<64)-1),l,address)+identity+h
    contract=struct.pack('<6Q',0,3,224,e,g,g^((1<<64)-1))+identity
    contract+=struct.pack('<14Q',0,0,0,0,l,0,0,0,0,0,0,0,SESSION_ADDRESS,len(raw))+h
    return request,contract+bytes(4096-len(contract))

def build_launcher(output,session_size,clang,lld,objcopy):
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text()
    for old,new in [('0x4153373353554c33','0x415337334d494c33'),('CONTRACT->reserved[0] && !CONTRACT->reserved[1]',f'CONTRACT->reserved[0]==0x41f00000 && CONTRACT->reserved[1]=={session_size}'),('20*A733_SMP_COUNTER_HZ','180*A733_SMP_COUNTER_HZ')]:
        require(old in text,'Maintained supervised launcher changed');text=text.replace(old,new)
    (output/'launch.c').write_text(text)
    for name,p in [('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h')]: (output/name).write_bytes(p.read_bytes())
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-Wall','-Wextra','-Werror',f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC}ULL','-I'+str(output)]
    commands=[[str(clang),*flags,'-MMD','-MF',str(output/(target+'.d')),'-c',str(output/name),'-o',str(output/target)] for name,target in [('launch.S','entry.o'),('launch.c','launch.o')]]
    commands.extend([[str(lld),'-T',str(output/'launch.ld'),str(output/'entry.o'),str(output/'launch.o'),'-o',str(output/'launch.elf')],[str(objcopy),'-O','binary',str(output/'launch.elf'),str(output/'launch.bin')]])
    save_json(output/'commands.json',commands)
    for command in commands:
        r=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        with (output/'build.log').open('a') as f:f.write(r.stdout)
        require(r.returncode==0,'mimus SRAM launcher build failed; inspect build.log')
    require(0<(output/'launch.bin').stat().st_size<=0xc00,'mimus launcher exceeds protected SRAM')

def prepare(image,manifest,session,admission,assets,output,clang,lld,objcopy):
    image_raw=Path(image).read_bytes();manifest_raw=Path(manifest).read_bytes();raw=Path(session).read_bytes();admission_raw=Path(admission).read_bytes()
    parts,address,identity,layout=image_data(image_raw);profile=session_profile(raw,layout,identity);m=json.loads(manifest_raw)
    require(m.get('status')=='PASS' and m.get('physical') is False and m.get('image_sha256')==sha(image_raw) and m.get('build_id')==identity.hex(),'Exact final native mimus handoff manifest required')
    asset_raw=Path(assets).read_bytes();validate_assets(asset_raw,raw)
    cache=validate_admission(admission_raw,raw,layout,identity,sha(image_raw));output=Path(output);output.mkdir(parents=True,exist_ok=False)
    build_launcher(output,len(raw),clang,lld,objcopy)
    data={'image.elf':image_raw,'image-input-manifest.json':manifest_raw,'session.bin':raw,'admission.json':admission_raw,'assets.bin':asset_raw};uploads=[]
    for i,p in enumerate(parts):
        if p['data']:
            name=f'segment-{i}.bin';data[name]=p['data'];uploads.append({'name':name,'address':p['address']})
    uploads.extend([{'name':'session.bin','address':SESSION_ADDRESS},{'name':'assets.bin','address':0x43000000}]);frozen={}
    for i,(path,value) in enumerate(cache.items()):
        name=f'entry-evidence-{i}.bin';data[name]=value;frozen[path]={'name':name,'sha256':sha(value)}
    request,contract=request_bytes(address,identity,raw,profile);data.update({'request.bin':request,'contract.bin':contract,'result-zero.bin':bytes(16384),'report-zero.bin':bytes(128)})
    for name,value in data.items():(output/name).write_bytes(value)
    p={'schema':SCHEMA,'physical_access':False,'image_sha256':sha(image_raw),'build_id':identity.hex(),**profile,'session_layout':layout,
        'session_sha256':sha(raw),'retention':'whole-board-power-removal','segments':uploads,
        'reservations':[{k:v for k,v in x.items() if k!='data'} for x in parts],'entry_evidence':frozen,'launcher_sha256':digest(output/'launch.bin'),
        'files':{p.name:digest(p) for p in output.iterdir() if p.is_file()}}
    save_json(output/'prepared.json',p);return validate_bundle(output)

def validate_bundle(bundle):
    bundle=Path(bundle);metadata=(bundle/'prepared.json').read_bytes();p=json.loads(metadata)
    require(p.get('schema')==SCHEMA and p.get('physical_access') is False and p.get('retention')=='whole-board-power-removal','Unsupported mimus bundle')
    data={}
    for name,h in p['files'].items():
        require(Path(name).name==name,'Invalid mimus bundle filename');data[name]=(bundle/name).read_bytes();require(sha(data[name])==h,'mimus bundle bytes changed')
    parts,address,identity,layout=image_data(data['image.elf']);raw=data['session.bin'];profile=session_profile(raw,layout,identity)
    require(p['session_layout']==layout and p['build_id']==identity.hex() and p['image_sha256']==sha(data['image.elf']) and
        p['session_sha256']==sha(raw) and all(p.get(k)==v for k,v in profile.items()),'mimus prepared layout/session identity differs')
    m=json.loads(data['image-input-manifest.json']);require(m.get('status')=='PASS' and m.get('physical') is False and m.get('image_sha256')==p['image_sha256'] and m.get('build_id')==p['build_id'],'mimus native handoff manifest differs')
    cache={path:data[item['name']] for path,item in p['entry_evidence'].items()};require(all(sha(cache[path])==item['sha256'] for path,item in p['entry_evidence'].items()),'Frozen mimus evidence changed')
    validate_admission(data['admission.json'],raw,layout,identity,p['image_sha256'],cache,True);expected=[]
    for i,x in enumerate(parts):
        if x['data']:
            name=f'segment-{i}.bin';require(data[name]==x['data'],'mimus upload differs from ELF');expected.append({'name':name,'address':x['address']})
    validate_assets(data['assets.bin'],raw)
    expected.extend([{'name':'session.bin','address':SESSION_ADDRESS},{'name':'assets.bin','address':0x43000000}]);require(p['segments']==expected and p['reservations']==[{k:v for k,v in x.items() if k!='data'} for x in parts],'mimus upload/reservation list differs')
    for name,value in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,profile)):require(data[name]==value,'mimus launcher identity differs')
    require(data['result-zero.bin']==bytes(16384) and data['report-zero.bin']==bytes(128) and 0<len(data['launch.bin'])<=0xc00 and sha(data['launch.bin'])==p['launcher_sha256'],'mimus protected launch/result extent differs')
    p['_sizes']={k:len(v) for k,v in data.items()};p['_frozen']=data;p['_prepared_sha256']=sha(metadata);return p
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','session','admission','assets','output','clang','lld','objcopy'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();prepare(a.image,a.manifest,a.session,a.admission,a.assets,a.output,a.clang,a.lld,a.objcopy)
