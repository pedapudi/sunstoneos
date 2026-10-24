#!/usr/bin/env python3
"""Offline preparation of one supervised NPU board session.

Follows apps/g2d/prepare.py: the board prefix and its admission come from
apps/board/prepare.py; this file adds the 808-byte NPU record of
apps/npu-board/npu-board-session.h, its evidence conditions, the extended
288-byte contract and the SRAM launcher. Tensors reach the image over USB0
at run time, so the bundle carries the selected model index and the input and
reference files for the capture tool; none of them is uploaded at launch.
"""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import secrets
import shutil
import struct
import subprocess
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
board=module('npu_board_board_prepare',ROOT/'apps/board/prepare.py')
base=board.base
require=base.require
SCHEMA='a733-npu-board-supervised-bundle-v1'
SESSION_MAGIC=0x415337334e424131
CONTRACT_MAGIC=0x415337334e424331
REQUEST_MAGIC=0x415337334e424c31
SESSION_BYTES=808
TARGET='sunstone-a733-npu-board'
PROFILE='npu-board-supervised-v1'
CATALOGUE=5
# Offsets in the record (npu-board-session.h); the board prefix ends at 592.
OWNER,COMMAND,SUPERVISOR,PCK,RESERVED,CATALOGUE_DIGEST=616,624,632,640,644,648
DIGESTS={'admission':680,'clock':712,'power':744,'supervision':776}
BOARD_OWNERS=456 # pmic, usb0, typec, usb2 owners: four u64
BOARD_FABRIC,BOARD_FABRIC_HZ,BOARD_PCK=488,500,540
# The conditions each evidence digest binds (docs/designs/npu-board.md). The
# admission record also references the extended DDR reservation record.
CONDITIONS={
    'admission':{'npu-startup-sram-confinement-drain-ownership','remote-writers-excluded','exclusive-cpus-memory-dma',
                 'pmic-bus-rail-pin-ownership','npu-irq97-disabled-inactive-clear'},
    'clock':{'supervised-cross-pll-disturbance-risk','exclusive-pll-npu-consumers-and-writers',
             'independent-dram-phy-sources','no-autonomous-parent-selector-writers'},
    'power':{'dcdc2-stable-800mv','authenticated-pmic-pin-inheritance'},
    'supervision':{'manual-whole-board-power-removal','retain-all-resources-until-power-removal','stop-after-first-failure'}}
FABRIC_MASKS=(0x0300001f,0x8700001f,0x8700001f)
PARENT_MHZ=((24,0,0,600,0,0,0,0),(24,0,800,600,480,0,0,0),(24,600,0,480,400,0,0,0))

def fabric_check(raw):
    """Mirror a733_npu_platform_scopes over the board's fabric record."""
    fabric=struct.unpack_from('<3I',raw,BOARD_FABRIC);rates=struct.unpack_from('<3I',raw,BOARD_FABRIC_HZ)
    for i,(word,rate) in enumerate(zip(fabric,rates)):
        mhz=PARENT_MHZ[i][(word>>24)&7];divisor=(word&31)+1
        require(not word&~FABRIC_MASKS[i] and (not i or word&(1<<31)) and mhz and
            not (mhz*1000000)%divisor and rate==mhz*1000000//divisor,'Unsupported NPU fabric record.')

def session_check(raw):
    require(len(raw)==SESSION_BYTES,'The NPU board session must be 808 bytes.')
    require(board.session_check(raw[:592])==2,'The NPU board image requires USB0/provider selection 2.')
    require(raw[80:112]==raw[280:312] and any(raw[80:112]),
        'The board qualification must bind the same recovery evidence.')
    require(struct.unpack_from('<3Q',raw,592)==(SESSION_MAGIC,1,SESSION_BYTES),'Invalid NPU session extension.')
    owner,command,supervisor=struct.unpack_from('<3Q',raw,OWNER)
    pck,reserved=struct.unpack_from('<2I',raw,PCK)
    require(owner and command and supervisor and owner not in struct.unpack_from('<4Q',raw,BOARD_OWNERS),
        'Invalid NPU owner, command admission or supervisor.')
    require(pck<16 and pck!=struct.unpack_from('<I',raw,BOARD_PCK)[0] and not reserved,'Invalid NPU PCK client.')
    require(all(any(raw[o:o+32]) for o in DIGESTS.values()),'Missing NPU evidence digest.')
    fabric_check(raw)

def embedded_reference(image,session):
    """Bind the record to the image's geometry and linked catalogue."""
    session_check(session)
    require(session[CATALOGUE_DIGEST:CATALOGUE_DIGEST+32]==_image_parts(image)[3],
        'The session names another model catalogue than the image links.')
    return None

def request_bytes(address,identity,raw,epoch,generation,lease):
    session_check(raw)
    reservation=struct.unpack_from('<Q',raw,200)[0]
    require(len(identity)==32 and 0x41000000<=address<=0x411fffe0 and
        all(type(x) is int and 0<x<2**64 for x in (epoch,generation,lease,reservation)),
        'Invalid retained NPU board identity.')
    digest=hashlib.sha256(raw).digest()
    request=struct.pack('<8Q',REQUEST_MAGIC,2,128,epoch,generation,generation^((1<<64)-1),lease,address)+identity+digest
    contract=struct.pack('<6Q',0,2,288,epoch,generation,generation^((1<<64)-1))+identity
    contract+=struct.pack('<18Q',0,0,0,0,lease,0,0,0,0,0,0,0,1,reservation,0x48000000,0x78000000,0x78000000,0x79000000)
    contract+=digest+raw[80:112]
    require(len(contract)==288,'Extended NPU board contract ABI differs.')
    return request,contract+bytes(4096-len(contract))

def build_launcher(out,clang,lld,objcopy='/usr/lib/llvm-21/bin/llvm-objcopy'):
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text()
    (out/'base-supervised-launch.c').write_text(text)
    text=board.adapt_launcher(text,{'0x4153373353554c33':'0x415337334e424c31','REQUEST[1]==3':'REQUEST[1]==2',
        'CONTRACT->version==3':'CONTRACT->version==2',
        'CONTRACT->reserved[0] && !CONTRACT->reserved[1]':'CONTRACT->reserved[0]==1 && CONTRACT->reserved[1] && CONTRACT->reserved[2]==0x48000000 && CONTRACT->reserved[3]==0x78000000 && CONTRACT->reserved[4]==0x78000000 && CONTRACT->reserved[5]==0x79000000',
        'CONTRACT->bytes==sizeof(*CONTRACT)':'CONTRACT->bytes==288',
        'CONTRACT->reserved[2+i]':'((volatile uint64_t *)((uintptr_t)CONTRACT+224))[i]'},counts={})
    # The shared 20-second native lifetime stays exact for the NPU board image.
    (out/'npu-board-launch-base.c').write_text(text)
    for name,path in [('launch.c',ROOT/'apps/npu-board/launch.c'),('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),
                      ('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h'),
                      ('npu-board-session.h',ROOT/'apps/npu-board/npu-board-session.h'),
                      ('a733_board_application.h',ROOT/'include/a733_board_application.h')]:shutil.copy2(path,out/name)
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector',
        '-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-MD','-Wall','-Wextra','-Werror','-DSUNSTONE_BOARD_SUPERVISED=1',
        '-DA733_BOARD_SUPERVISED_SECONDS=20',f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC}ULL','-I'+str(out)]
    commands=[[clang,*flags,'-c',out/name,'-o',out/(name+'.o')] for name in ('launch.c','launch.S')]
    commands+=[[lld,'-T',out/'launch.ld',out/'launch.c.o',out/'launch.S.o','-o',out/'launch.elf'],
               [objcopy,'-O','binary',out/'launch.elf',out/'launch.bin']]
    for command in commands:
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (out/'launcher-build.log').open('a') as f:f.write(r.stdout+r.stderr)
        require(r.returncode==0,'NPU board launcher build failed; inspect the retained log.')
    base.save_json(out/'launcher-commands.json',[[str(x) for x in c] for c in commands])
    require(0<(out/'launch.bin').stat().st_size<=0xc00,'Launcher exceeds SRAM.')

def physical_authority(record):
    require(record.get('physical_authority') is True and record.get('modeled') is False,
        'Modeled NPU board evidence cannot authorize physical execution.')

def validate_admission(record,image_hash,template_hash,raw,cache=None,frozen=False):
    # The board's DDR, platform and manual-recovery checks stay exact.
    require(record.get('schema')=='a733-npu-board-supervised-entry-v1','Wrong NPU board admission schema.')
    physical_authority(record)
    common={**record,'schema':'a733-board-supervised-entry-v2'}
    cache=board.validate_admission(common,image_hash,template_hash,cache,frozen)
    def evidence(item):
        require(isinstance(item,dict) and set(item)=={'path','sha256'},'Invalid NPU board evidence reference.')
        path=Path(item['path']);require(path.is_absolute(),'NPU board evidence path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Frozen NPU board evidence absent.');cache[str(path)]=path.read_bytes()
        data=cache[str(path)];require(hashlib.sha256(data).hexdigest()==item['sha256'],'NPU board evidence changed.')
        return data
    ddr_ref=record['npu_ddr'];fact=json.loads(evidence(ddr_ref))
    physical_authority(fact)
    require(fact.get('schema')=='a733-ddr-reservation-v1' and fact.get('identity')==record['identity'] and
        fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and
        fact.get('exclusive') is True and fact.get('published_cache_disabled') is True and
        fact.get('remote_writers_excluded') is True and fact.get('ranges')==[[0x48000000,0x79000000]] and fact.get('evidence'),
        'Extended DRAM reservation and exclusion evidence required.')
    for item in fact['evidence']:evidence(item)
    for key,offset in DIGESTS.items():
        data=evidence(record[key]);require(hashlib.sha256(data).digest()==raw[offset:offset+32],'NPU board evidence digest differs: '+key)
        fact=json.loads(data)
        physical_authority(fact)
        require(fact.get('schema')=='a733-npu-board-'+key+'-v1' and fact.get('identity')==record['identity'] and
            fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and fact.get('reviewed') is True and
            set(fact.get('conditions',{}))==CONDITIONS[key],'NPU board '+key+' facts do not cover this entry.')
        for item in fact['conditions'].values():evidence(item)
        if key=='admission':require(fact.get('ddr_sha256')==ddr_ref['sha256'],'Admission covers another DDR reservation record.')
        if key=='power':require(fact.get('supply_uv')==800000,'Power record covers another DCDC2 operating point.')
        if key=='supervision':
            require(fact.get('supervisor')==struct.unpack_from('<Q',raw,SUPERVISOR)[0] and fact.get('one_attempt') is True,
                'One-attempt supervision identity differs.')
    return cache

def tensor_check(model,inputs,references):
    """The capture inputs and references against the catalogue entry."""
    catalogue=json.loads((ROOT/'tools/npu-session-models.json').read_text())['models']
    require(type(model) is int and 0<=model<len(catalogue),'Unknown catalogue model.')
    entry=catalogue[model]
    require(len(inputs)==len(entry['inputs']) and len(references)==len(entry['outputs']),'Tensor count differs from the catalogue.')
    for data,tensor in list(zip(inputs,entry['inputs']))+list(zip(references,entry['outputs'])):
        require(len(data)==tensor['reference']['bytes'],'Tensor size differs from the catalogue.')
    return entry

def prepare(image,manifest,admission,template,output,clang,lld,model,inputs,references):
    snapshots={name:Path(path).read_bytes() for name,path in [('image.elf',image),('image-input-manifest.json',manifest),('admission.json',admission),('session-template.bin',template)]}
    for i,path in enumerate(inputs):snapshots[f'input-{i}.bin']=Path(path).read_bytes()
    for i,path in enumerate(references):snapshots[f'reference-{i}.bin']=Path(path).read_bytes()
    tensor_check(model,[snapshots[f'input-{i}.bin'] for i in range(len(inputs))],[snapshots[f'reference-{i}.bin'] for i in range(len(references))])
    raw=bytearray(snapshots['session-template.bin']);session_check(raw)
    embedded_reference(snapshots['image.elf'],raw)
    ih=hashlib.sha256(snapshots['image.elf']).hexdigest();th=hashlib.sha256(raw).hexdigest()
    mf=json.loads(snapshots['image-input-manifest.json'])
    require(mf.get('target')==TARGET and mf.get('execution_profile')==PROFILE and mf.get('final_elf')==ih,'Tested NPU board image manifest required.')
    record=json.loads(snapshots['admission.json']);cache=validate_admission(record,ih,th,raw)
    epoch,generation,lease=(secrets.randbits(64) or 1 for _ in range(3))
    struct.pack_into('<3Q',raw,24,epoch,generation,lease)
    out=Path(output);out.mkdir(parents=True,exist_ok=False)
    for name,data in snapshots.items():(out/name).write_bytes(data)
    segments,address,identity,_=image_parts(out/'image.elf')
    # The runtime authenticates the linked build identity, not the ELF file hash.
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
    p=dict(schema=SCHEMA,physical_access=False,image_sha256=ih,build_id=identity.hex(),epoch=epoch,generation=generation,lease=lease,
        reservation=record['reservation'],session='session.bin',profile=None,manifest='image-input-manifest.json',image='image.elf',
        model=model,inputs=len(inputs),references=len(references),segments=uploads,entry_evidence=evidence,
        backup_sizes={name:n for name,a,n in base.REGIONS},files={x.name:base.digest(x) for x in out.iterdir() if x.is_file()})
    base.save_json(out/'prepared.json',p);return validate_bundle(out)

def validate_bundle(bundle):
    out=Path(bundle);p=json.loads((out/'prepared.json').read_text())
    require(p.get('schema')==SCHEMA and p.get('physical_access') is False,'Wrong NPU board bundle.')
    tensors={f'input-{i}.bin' for i in range(p['inputs'])}|{f'reference-{i}.bin' for i in range(p['references'])}
    required={'image.elf','image-input-manifest.json','admission.json','session-template.bin','session.bin','request.bin',
              'contract.bin','launch.bin','result-zero.bin','report-zero.bin'}|tensors
    require(required<=set(p['files']) and all(Path(item['name']).name==item['name'] and item['name'] in p['files']
        for item in p['entry_evidence'].values()),'Unsealed NPU board authority or capture input.')
    for name,h in p['files'].items():require(Path(name).name==name and base.digest(out/name)==h,'NPU board bundle input changed.')
    require(base.digest(out/'image.elf')==p['image_sha256'],'NPU board image changed.')
    manifest=json.loads((out/'image-input-manifest.json').read_text())
    require(manifest.get('target')==TARGET and manifest.get('execution_profile')==PROFILE and manifest.get('final_elf')==p['image_sha256'],
        'NPU board image manifest differs.')
    sizes={name:(out/name).stat().st_size for name in p['files']}
    require(p.get('backup_sizes')=={name:n for name,a,n in base.REGIONS} and 0<sizes.get('launch.bin',0)<=0xc00,'Protected SRAM geometry changed.')
    require((out/'result-zero.bin').read_bytes()==bytes(16384) and (out/'report-zero.bin').read_bytes()==bytes(128),'Initial report backing changed.')
    raw=(out/'session.bin').read_bytes();session_check(raw)
    tensor_check(p['model'],[(out/f'input-{i}.bin').read_bytes() for i in range(p['inputs'])],
        [(out/f'reference-{i}.bin').read_bytes() for i in range(p['references'])])
    template=(out/'session-template.bin').read_bytes();cache={path:(out/item['name']).read_bytes() for path,item in p['entry_evidence'].items()}
    validate_admission(json.loads((out/'admission.json').read_text()),p['image_sha256'],hashlib.sha256(template).hexdigest(),template,cache,True)
    segments,address,identity,_=image_parts(out/'image.elf');require(identity.hex()==p['build_id'],'NPU board build identity changed.')
    expected_raw=bytearray(template);struct.pack_into('<3Q',expected_raw,24,p['epoch'],p['generation'],p['lease']);expected_raw[48:80]=identity
    require(raw==expected_raw,'NPU board session differs from the admitted derivation.')
    expected=[]
    for i,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{i}.bin';require((out/name).read_bytes()==segment['data'],'NPU board ELF bytes changed.');expected.append({'name':name,'address':segment['address']})
    expected.append({'name':'session.bin','address':0x41f00000})
    require(p['segments']==expected and all(item['name'] in sizes for item in expected),'NPU board upload spans changed or unsealed.')
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,p['epoch'],p['generation'],p['lease'])):
        require((out/name).read_bytes()==data,'NPU board launch contract changed.')
    embedded_reference((out/'image.elf').read_bytes(),raw)
    p['_reference']=None
    p['_sizes']={name:(out/name).stat().st_size for name in p['files']};return p

def image_parts(path):
    return _image_parts(Path(path).read_bytes())

def _image_parts(data):
    elf=ELFFile(io.BytesIO(data));require(elf['e_machine']=='EM_AARCH64' and elf['e_entry']==0x41000000,'Native extended-memory NPU board ELF required.')
    table=elf.get_section_by_name('.symtab');require(table is not None,'Native NPU board symbol table required.')
    entries=list(table.iter_symbols());symbols={s.name:(s['st_value'],s['st_size']) for s in entries}
    parts=[]
    for s in elf.iter_segments():
        if s['p_type']!='PT_LOAD':continue
        a,n=s['p_paddr'],s['p_memsz'];segment=s.data()
        require(a==s['p_vaddr'] and n>0 and len(segment)<=n and not (s['p_flags']&3==3) and
            all(a>=p['address']+p['memory_bytes'] or p['address']>=a+n for p in parts),'Invalid NPU board ELF allocations.')
        require((0x41000000<=a<a+n<=0x42200000 and (not segment or a+len(segment)<=0x42000000)) or
            (not segment and (a,a+n) in [(0x48000000,0x78000000),(0x78000000,0x79000000)]),'NPU board ELF span outside reservations.')
        require(not s['p_flags']&1 or a+n<=0x41200000,'NPU board code outside the image prefix.')
        parts.append({'address':a,'memory_bytes':n,'data':segment,'flags':int(s['p_flags'])})
    for name,value in {'__device_pool_start':0x48000000,'__device_pool_end':0x78000000,'__uncached_pool_start':0x78000000,
                       '__uncached_pool_end':0x79000000,'__a733_smp_contract':0x42004000,'__a733_smp_contract_end':0x42005000}.items():
        require(symbols.get(name,(0,))[0]==value,'Extended NPU board linker ownership differs.')
    require(any(p['flags']&1 and p['address']<=0x41000000<p['address']+len(p['data']) for p in parts),
        'NPU board entry lacks executable file-backed bytes.')
    require('__bss_end' in symbols and symbols['__bss_end'][0]<=0x41f00000,'NPU board session overlaps BSS.')
    def immutable(name,size):
        require(sum(s.name==name for s in entries)==1,'NPU board immutable marker is not unique: '+name)
        address,n=symbols.get(name,(0,0));require(n==size,'NPU board immutable marker absent: '+name)
        match=[p for p in parts if not p['flags']&2 and p['address']<=address and address+size<=p['address']+len(p['data'])]
        require(len(match)==1,'NPU board marker is not immutable and file-backed.')
        p=match[0];return address,p['data'][address-p['address']:address-p['address']+size]
    _,geometry=immutable('npu_board_geometry',48)
    require(struct.unpack('<6Q',geometry)==(SESSION_MAGIC,1,SESSION_BYTES,288,0x41f00000,20),'NPU board compiled geometry differs.')
    _,catalogue=immutable('npu_board_catalogue_digest',32)
    address,identity=immutable('a733_smp_build_id',32)
    for p in parts:p.pop('flags')
    return parts,address,identity,catalogue

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','admission','template','output','clang','lld'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--model',type=int,required=True,help='catalogue index (tools/npu-session-models.json order)')
    parser.add_argument('--input',type=Path,action='append',required=True)
    parser.add_argument('--reference',type=Path,action='append',required=True)
    a=parser.parse_args();prepare(a.image,a.manifest,a.admission,a.template,a.output,a.clang,a.lld,a.model,a.input,a.reference)
