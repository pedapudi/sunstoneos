#!/usr/bin/env python3
"""Offline preparation of one supervised caller-supplied deinterlace application session."""
import hashlib
import importlib.util
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
board=module('deinterlace_board_prepare',ROOT/'apps/board/prepare.py')
base=board.base
require=base.require
SCHEMA='a733-deinterlace-supervised-bundle-v1'
CONTRACT_MAGIC=0x4153373344494331
REQUEST_MAGIC=0x4153373344494c31

def session_check(raw):
    require(len(raw)==888,'deinterlace session must be888bytes.')
    require(board.session_check(raw[:592])==2,'deinterlace requires USB0/provider selection2.')
    require(raw[80:112]==raw[280:312] and any(raw[80:112]),
        'deinterlace board qualification must bind the same recovery evidence.')
    require(struct.unpack_from('<3Q',raw,592)==(0x4153373344494131,1,888),'Invalid deinterlace session extension.')
    owner,authority,client=struct.unpack_from('<3Q',raw,616)
    require(owner and authority and client<16 and owner not in struct.unpack_from('<2Q',raw,456),'Invalid deinterlace owner.')
    require(all(any(raw[o:o+32]) for o in (640,672,704,736,768)) and not any(raw[800:832]),'Missing deinterlace physical or fixture evidence.')
    module,hz,g0,g1,uv,rail=struct.unpack_from('<6I',raw,832)
    require((module,hz)==(1,300000000) and
        (g0,g1,uv,rail)==(0x10007,0x10007,800000,1),'Unsupported inherited deinterlace clock/rail state.')
    geometry_check(struct.unpack_from('<8I',raw,856))

def geometry_check(geometry):
    require(len(geometry)==8 and tuple(geometry[:5])==(640,480,640,640,0x3231564e) and
        geometry[5] in (1,2,3) and tuple(geometry[6:])==(1,0),
        'Deinterlace requires fixed top-first NV12 WEAVE, first-operation motion or fixed-strength TNR.')
    return 460800 if geometry[5]==1 else 921600

def embedded_reference(image,session):
    """Bind immutable native geometry before preparation or runtime upload."""
    session_check(session)
    _image_parts(image,mode=struct.unpack_from('<I',session,876)[0])
    return None

def upload_bytes(pixels,session):
    session_check(session)
    require(len(pixels)==geometry_check(struct.unpack_from('<8I',session,856)),
        'deinterlace source size must include both fixed NV12 planes.')
    require(hashlib.sha256(pixels).digest()==session[768:800],'deinterlace source digest differs.')
    epoch,generation=struct.unpack_from('<2Q',session,24)
    require(epoch and generation,'deinterlace upload requires a live session identity.')
    return struct.pack('<4I2Q',0x55494453,1,len(pixels),0,epoch,generation)+pixels

def request_bytes(address,identity,raw,epoch,generation,lease):
    session_check(raw)
    reservation=struct.unpack_from('<Q',raw,200)[0]
    require(len(identity)==32 and 0x41000000<=address<=0x411fffe0 and
        all(type(x) is int and 0<x<2**64 for x in (epoch,generation,lease,reservation)),
        'Invalid retained deinterlace identity.')
    digest=hashlib.sha256(raw).digest()
    request=struct.pack('<8Q',REQUEST_MAGIC,2,128,epoch,generation,generation^((1<<64)-1),lease,address)+identity+digest
    contract=struct.pack('<6Q',0,2,288,epoch,generation,generation^((1<<64)-1))+identity
    contract+=struct.pack('<18Q',0,0,0,0,lease,0,0,0,0,0,0,0,1,reservation,0x48000000,0x78000000,0x78000000,0x79000000)
    contract+=digest+raw[80:112]
    require(len(contract)==288,'Extended deinterlace contract ABI differs.')
    return request,contract+bytes(4096-len(contract))

def build_launcher(out,clang,lld):
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text()
    (out/'base-supervised-launch.c').write_text(text)
    require('REQUEST[1]==3' in text and 'CONTRACT->version==3' in text,'Unexpected supervised launcher version guards.')
    for anchor in ('0x4153373353554c33','REQUEST[1]==3','CONTRACT->version==3',
                   'CONTRACT->reserved[0] && !CONTRACT->reserved[1]','CONTRACT->bytes==sizeof(*CONTRACT)',
                   'CONTRACT->reserved[2+i]'):
        require(text.count(anchor)==1,'Shared deinterlace launcher adaptation anchor changed: '+anchor)
    text=text.replace('0x4153373353554c33','0x4153373344494c31').replace('REQUEST[1]==3','REQUEST[1]==2').replace('CONTRACT->version==3','CONTRACT->version==2')
    text=text.replace('CONTRACT->reserved[0] && !CONTRACT->reserved[1]','CONTRACT->reserved[0]==1 && CONTRACT->reserved[1] && CONTRACT->reserved[2]==0x48000000 && CONTRACT->reserved[3]==0x78000000 && CONTRACT->reserved[4]==0x78000000 && CONTRACT->reserved[5]==0x79000000')
    text=text.replace('CONTRACT->bytes==sizeof(*CONTRACT)','CONTRACT->bytes==288').replace('CONTRACT->reserved[2+i]','((volatile uint64_t *)((uintptr_t)CONTRACT+224))[i]')
    # The shared20-second native lifetime remains exact for deinterlace.
    (out/'deinterlace-launch-base.c').write_text(text)
    for name,path in [('launch.c',ROOT/'apps/deinterlace/launch.c'),('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h'),('a733_deinterlace_application.h',ROOT/'include/a733_deinterlace_application.h'),('a733_board_application.h',ROOT/'include/a733_board_application.h')]:shutil.copy2(path,out/name)
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-MD','-Wall','-Wextra','-Werror','-DSUNSTONE_BOARD_SUPERVISED=1','-DA733_BOARD_SUPERVISED_SECONDS=20',f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC}ULL','-I'+str(out)]
    commands=[[clang,*flags,'-c',out/name,'-o',out/(name+'.o')] for name in ('launch.c','launch.S')]
    commands += [[lld,'-T',out/'launch.ld',out/'launch.c.o',out/'launch.S.o','-o',out/'launch.elf'],['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',out/'launch.elf',out/'launch.bin']]
    for command in commands:
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (out/'launcher-build.log').open('a') as f:f.write(r.stdout+r.stderr)
        require(r.returncode==0,'deinterlace launcher build failed; inspect retained log.')
    base.save_json(out/'launcher-commands.json',[[str(x) for x in c] for c in commands])
    require(0<(out/'launch.bin').stat().st_size<=0xc00,'Launcher exceeds SRAM.')

def physical_authority(record):
    require(record.get('physical_authority') is True and record.get('modeled') is False,
        'Modeled deinterlace evidence cannot authorize physical execution.')


def allocation_check(proof,raw,table):
    """Check actual provider rows and every IOMMU descriptor, including holes."""
    input_bytes=geometry_check(struct.unpack_from('<8I',raw,856))
    motion=input_bytes==921600;rounded=462848
    input_rounded=(input_bytes+4095)&~4095
    if motion:require(all(proof.get(key)==value for key,value in
        dict(input_bytes=input_bytes,input_rounded=input_rounded,md_bytes=76800,md_rounded=77824).items()),
        'Motion input/history allocation geometry differs.')
    require(proof.get('rounded')==rounded and proof.get('table_bytes')==32768,
        'deinterlace allocation geometry differs.')
    groups=proof.get('allocations',{})
    require(set(groups)=={'boot_pool','uncached_pool'} and
        len(groups['boot_pool'])==2 and len(groups['uncached_pool'])==(6 if motion else 3),'Incomplete deinterlace allocations.')
    extents=[]
    for pool,low,high,sizes,state in [('boot_pool',0x48000000,0x78000000,[input_rounded,rounded],1),
            ('uncached_pool',0x78000000,0x79000000,[32768,rounded+8192,rounded+8192]+([rounded+8192,86016,86016] if motion else []),2)]:
        for slot,(row,size) in enumerate(zip(groups[pool],sizes)):
            address=row.get('physical');alignment=16384 if pool=='uncached_pool' and slot==0 else 4096
            require(type(address) is int and row.get('slot')==slot and row.get('cpu')==address and
                row.get('bytes')==size and row.get('state')==state and low<=address<=high-size and
                not address%alignment,'deinterlace provider extent differs.')
            require(all(address>=a+n or a>=address+size for a,n in extents),'deinterlace physical extents alias.')
            extents.append((address,size))
    nc=groups['uncached_pool'];pa=nc[0]['physical']
    mapping=[(0x10000000,rounded,1),(0x10400000,rounded,3)]
    if motion:mapping += [(0x10800000,rounded,1),(0x10c00000,77824,1),(0x10c20000,77824,3)]
    spans=[dict(iova=iova,physical=nc[i+1]['physical']+4096,bytes=size,access=access)
        for i,(iova,size,access) in enumerate(mapping)]
    if motion:require(all(proof.get(key)==spans[index]['physical'] for key,index in
        [('current_physical',2),('flag_input_physical',3),('flag_output_physical',4)]),
        'Motion current/history physical identities differ.')
    require(proof.get('spans')==spans and proof.get('table_physical')==pa and
        proof.get('source_physical')==spans[0]['physical'] and
        proof.get('destination_physical')==spans[1]['physical'],
        'deinterlace guarded mapping differs.')
    words=[0]*8192
    def address(value):return (value&0xfffff000)|((value>>24)&0x300)
    windows=[]
    for span in spans:
        for window in range(span['iova']>>22,((span['iova']+span['bytes']-1)>>22)+1):
            if window not in windows:windows.append(window)
    require(len(windows)<=4,'deinterlace mapping exceeds table capacity.')
    for slot,window in enumerate(windows):words[window]=address(pa+16384+slot*4096)|1
    for span in spans:
        for offset in range(0,span['bytes'],4096):
            va=span['iova']+offset;slot=windows.index(va>>22)
            words[4096+slot*1024+((va>>12)&1023)]=address(span['physical']+offset)|2|4|(8 if span['access']&2 else 0)
    require(table==struct.pack('<8192I',*words) and
        hashlib.sha256(table).hexdigest()==proof.get('table_sha256'),
        'deinterlace page-table bytes, permissions or unmapped guards differ.')


def validate_admission(record,image_hash,template_hash,raw,cache=None,frozen=False):
    # Existing DDR/platform/manual-recovery checks stay exact and byte-snapshotted.
    require(record.get('schema')=='a733-deinterlace-supervised-entry-v1','Wrong deinterlace admission schema.')
    physical_authority(record)
    common={**record,'schema':'a733-board-supervised-entry-v2'}
    cache=board.validate_admission(common,image_hash,template_hash,cache,frozen)
    for key,offset in [('initial',640),('memory',672),('electrical',704),('supervision',736)]:
        item=record[key];path=Path(item['path']);require(path.is_absolute(),'Evidence path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Missing frozen deinterlace evidence.');cache[str(path)]=path.read_bytes()
        data=cache[str(path)];require(hashlib.sha256(data).digest()==raw[offset:offset+32] and hashlib.sha256(data).hexdigest()==item['sha256'],'deinterlace evidence differs.')
        fact=json.loads(data)
        physical_authority(fact)
        require(fact.get('schema')=='a733-deinterlace-'+key+'-v1' and fact.get('identity')==record['identity'] and fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and fact.get('reviewed') is True,'deinterlace authority is not bound to this image/target.')
        required={'initial':['all_masters_excluded','prior_transactions_drained','prior_walks_drained',('di_inherited_powered_reset_deasserted' if struct.unpack_from('<I',raw,876)[0]==1 else
                'di_dedicated_reset_held_bus_and_module_gated'),'skipped_masters_physically_quiescent','remote_secure_writers_excluded','di_irq_owned_masked','iommu_irqs_owned_masked'],
            'memory':['private_nc_arenas','no_physical_aliases','iommu_banks_disabled','iommu_clocks_stable','di_device_mapping','dma_visibility_established'],
            'electrical':['dcdc2_shared_800mv','di_operating_point_source_reviewed','stable_di_parent'],
            'supervision':['whole_board_power_removal','permanent_dma_retention','no_reuse_after_fault','one_attempt']}[key]
        require(all(fact.get(k) is True for k in required),'Required deinterlace physical fact absent.')
    item=record['deinterlace_ddr'];path=Path(item['path']);require(path.is_absolute(),'deinterlace DDR evidence path must be absolute.')
    if str(path) not in cache:
        require(not frozen,'Frozen deinterlace DDR evidence absent.');cache[str(path)]=path.read_bytes()
    data=cache[str(path)];require(hashlib.sha256(data).hexdigest()==item['sha256'],'deinterlace DDR evidence changed.')
    fact=json.loads(data)
    physical_authority(fact)
    require(fact.get('schema')=='a733-ddr-reservation-v1' and fact.get('identity')==record['identity'] and
        fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and
        fact.get('exclusive') is True and fact.get('published_cache_disabled') is True and
        fact.get('remote_writers_excluded') is True and fact.get('ranges')==[[0x48000000,0x79000000]] and fact.get('evidence'),
        'Extended DRAM reservation/exclusion evidence required.')
    for item in fact['evidence']:
        path=Path(item['path']);require(path.is_absolute(),'deinterlace DDR support path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Frozen deinterlace DDR support absent.');cache[str(path)]=path.read_bytes()
        require(hashlib.sha256(cache[str(path)]).hexdigest()==item['sha256'],'deinterlace DDR support changed.')
    def evidence(item):
        require(isinstance(item,dict) and set(item)=={'path','sha256'},'Invalid deinterlace allocation reference.')
        path=Path(item['path']);require(path.is_absolute(),'deinterlace allocation path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Frozen deinterlace allocation evidence absent.');cache[str(path)]=path.read_bytes()
        data=cache[str(path)];require(hashlib.sha256(data).hexdigest()==item['sha256'],'deinterlace allocation evidence changed.')
        return data
    proof=json.loads(evidence(record['allocation']))
    physical_authority(proof)
    require(proof.get('image')==image_hash and proof.get('session')==template_hash,
        'deinterlace allocation proof covers different image/session bytes.')
    allocation_check(proof,raw,evidence(record['page_table']))
    return cache

def prepare(image,manifest,admission,template,output,clang,lld,pixels,reference=None):
    snapshots={name:Path(path).read_bytes() for name,path in [('image.elf',image),('image-input-manifest.json',manifest),('admission.json',admission),('session-template.bin',template)]}
    snapshots['input.nv12']=Path(pixels).read_bytes()
    require(reference is None,'deinterlace has no runtime reference upload.')
    raw=bytearray(snapshots['session-template.bin']);session_check(raw)
    upload_bytes(snapshots['input.nv12'],raw)
    embedded_reference(snapshots['image.elf'],raw)
    ih=hashlib.sha256(snapshots['image.elf']).hexdigest();th=hashlib.sha256(raw).hexdigest()
    mf=json.loads(snapshots['image-input-manifest.json'])
    require(mf.get('target')=='sunstone-a733-deinterlace' and mf.get('execution_profile')=='deinterlace-supervised-v1' and mf.get('final_elf')==ih,'Tested deinterlace image manifest required.')
    record=json.loads(snapshots['admission.json']);cache=validate_admission(record,ih,th,raw)
    epoch,generation,lease=(secrets.randbits(64) or 1 for _ in range(3))
    struct.pack_into('<3Q',raw,24,epoch,generation,lease)
    out=Path(output);out.mkdir(parents=True,exist_ok=False)
    for name,data in snapshots.items():(out/name).write_bytes(data)
    segments,address,identity=image_parts(out/'image.elf')
    # Runtime authenticates linked build identity, not the ELF file hash.
    raw[48:80]=identity
    build_launcher(out,clang,lld)
    uploads=[]
    for i,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{i}.bin';(out/name).write_bytes(segment['data']);uploads.append({'name':name,'address':segment['address']})
    (out/'upload.bin').write_bytes(upload_bytes(snapshots['input.nv12'],raw))
    (out/'session.bin').write_bytes(raw);uploads.append({'name':'session.bin','address':0x41f00000})
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,epoch,generation,lease)):(out/name).write_bytes(data)
    for name,count in [('result-zero.bin',16384),('report-zero.bin',128)]:(out/name).write_bytes(bytes(count))
    evidence={}
    for i,(path,data) in enumerate(cache.items()):
        name=f'evidence-{i}.bin';(out/name).write_bytes(data);evidence[path]={'name':name,'sha256':hashlib.sha256(data).hexdigest()}
    p=dict(schema=SCHEMA,physical_access=False,image_sha256=ih,build_id=identity.hex(),epoch=epoch,generation=generation,lease=lease,reservation=record['reservation'],session='session.bin',profile=None,manifest='image-input-manifest.json',image='image.elf',segments=uploads,entry_evidence=evidence,backup_sizes={name:n for name,a,n in base.REGIONS},files={x.name:base.digest(x) for x in out.iterdir() if x.is_file()})
    base.save_json(out/'prepared.json',p);return validate_bundle(out)

def validate_bundle(bundle):
    out=Path(bundle);p=json.loads((out/'prepared.json').read_text());require(p.get('schema')==SCHEMA and p.get('physical_access') is False,'Wrong deinterlace bundle.')
    required={'image.elf','image-input-manifest.json','admission.json','session-template.bin','session.bin','request.bin','contract.bin','launch.bin','result-zero.bin','report-zero.bin','input.nv12','upload.bin'}
    require(required<=set(p['files']) and all(Path(item['name']).name==item['name'] and item['name'] in p['files'] for item in p['entry_evidence'].values()),'Unsealed deinterlace authority or transport input.')
    for name,h in p['files'].items():require(Path(name).name==name and base.digest(out/name)==h,'deinterlace bundle input changed.')
    require(base.digest(out/'image.elf')==p['image_sha256'],'deinterlace image changed.')
    manifest=json.loads((out/'image-input-manifest.json').read_text())
    require(manifest.get('target')=='sunstone-a733-deinterlace' and manifest.get('execution_profile')=='deinterlace-supervised-v1' and manifest.get('final_elf')==p['image_sha256'],'deinterlace image manifest differs.')
    sizes={name:(out/name).stat().st_size for name in p['files']}
    require(p.get('backup_sizes')=={name:n for name,a,n in base.REGIONS} and 0<sizes.get('launch.bin',0)<=0xc00,'deinterlace protected SRAM geometry changed.')
    require((out/'result-zero.bin').read_bytes()==bytes(16384) and (out/'report-zero.bin').read_bytes()==bytes(128),'deinterlace initial report backing changed.')
    raw=(out/'session.bin').read_bytes();session_check(raw)
    require((out/'upload.bin').read_bytes()==upload_bytes((out/'input.nv12').read_bytes(),raw),'deinterlace upload bytes differ.')
    template=(out/'session-template.bin').read_bytes();cache={path:(out/item['name']).read_bytes() for path,item in p['entry_evidence'].items()}
    validate_admission(json.loads((out/'admission.json').read_text()),p['image_sha256'],hashlib.sha256(template).hexdigest(),template,cache,True)
    segments,address,identity=image_parts(out/'image.elf');require(identity.hex()==p['build_id'],'deinterlace build identity changed.')
    expected_raw=bytearray(template);struct.pack_into('<3Q',expected_raw,24,p['epoch'],p['generation'],p['lease']);expected_raw[48:80]=identity
    require(raw==expected_raw,'deinterlace session differs from admitted derivation.')
    expected=[]
    for i,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{i}.bin';require((out/name).read_bytes()==segment['data'],'deinterlace ELF bytes changed.');expected.append({'name':name,'address':segment['address']})
    expected.append({'name':'session.bin','address':0x41f00000});require(p['segments']==expected and all(item['name'] in sizes for item in expected),'deinterlace upload spans changed or unsealed.')
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,p['epoch'],p['generation'],p['lease'])):require((out/name).read_bytes()==data,'deinterlace launch contract changed.')
    embedded_reference((out/'image.elf').read_bytes(),raw)
    p['_reference']=None
    p['_upload']=(out/'upload.bin').read_bytes()
    p['_sizes']={name:(out/name).stat().st_size for name in p['files']};return p


def image_parts(path):
    return _image_parts(Path(path).read_bytes())

def _image_parts(data,mode=None):
    import io
    elf=ELFFile(io.BytesIO(data));require(elf['e_machine']=='EM_AARCH64' and elf['e_entry']==0x41000000,'Native extended-memory deinterlace ELF required.')
    table=elf.get_section_by_name('.symtab');require(table is not None,'Native deinterlace symbol table required.')
    entries=list(table.iter_symbols());symbols={s.name:(s['st_value'],s['st_size']) for s in entries}
    parts=[]
    for s in elf.iter_segments():
        if s['p_type']!='PT_LOAD':continue
        a,n=s['p_paddr'],s['p_memsz'];data=s.data()
        require(a==s['p_vaddr'] and n>0 and len(data)<=n and not (s['p_flags']&3==3) and
            all(a>=p['address']+p['memory_bytes'] or p['address']>=a+n for p in parts),'Invalid deinterlace ELF allocations.')
        require((0x41000000<=a<a+n<=0x42200000 and (not data or a+len(data)<=0x42000000)) or
            (not data and (a,a+n) in [(0x48000000,0x78000000),(0x78000000,0x79000000)]),'Extended deinterlace ELF span outside reservations.')
        require(not s['p_flags']&1 or a+n<=0x41200000,'Extended deinterlace code outside image prefix.')
        parts.append({'address':a,'memory_bytes':n,'data':data,'flags':int(s['p_flags'])})
    for name,value in {'__device_pool_start':0x48000000,'__device_pool_end':0x78000000,'__uncached_pool_start':0x78000000,'__uncached_pool_end':0x79000000,'__a733_smp_contract':0x42004000,'__a733_smp_contract_end':0x42005000}.items():
        require(symbols.get(name,(0,))[0]==value,'Extended deinterlace linker ownership differs.')
    require(any(p['flags']&1 and p['address']<=0x41000000<p['address']+len(p['data']) for p in parts),
        'deinterlace entry lacks executable file-backed bytes.')
    require('__bss_end' in symbols and symbols['__bss_end'][0]<=0x41f00000,'Extended deinterlace session overlaps BSS.')
    def immutable(name,size):
        require(sum(s.name==name for s in entries)==1,'deinterlace immutable marker is not unique: '+name)
        address,n=symbols.get(name,(0,0));require(n==size,'Extended deinterlace immutable marker absent: '+name)
        match=[p for p in parts if not p['flags']&2 and p['address']<=address and address+size<=p['address']+len(p['data'])]
        require(len(match)==1,'Extended deinterlace marker is not immutable/file-backed.')
        p=match[0];return address,p['data'][address-p['address']:address-p['address']+size]
    _,geometry=immutable('deinterlace_geometry',88)
    compiled=struct.unpack('<11Q',geometry)
    require(compiled in [(0x4153373344494131,revision,888,288,0x41f00000,20,capacity,460800,192,640,480)
        for revision,capacity in ((1,460800),(2,921600),(3,921600))] and
        (mode not in (2,3) or compiled[1]>=mode),
        'Deinterlace compiled input capacity or geometry differs.')
    address,identity=immutable('a733_smp_build_id',32)
    for p in parts:p.pop('flags')
    return parts,address,identity

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','admission','template','output','clang','lld','input'):parser.add_argument('--'+name,type=Path,required=True)
    a=parser.parse_args();prepare(a.image,a.manifest,a.admission,a.template,a.output,a.clang,a.lld,a.input)
