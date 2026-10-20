#!/usr/bin/env python3
"""Offline preparation of one supervised caller-supplied G2D application session."""
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
board=module('g2d_board_prepare',ROOT/'apps/board/prepare.py')
base=board.base
require=base.require
SCHEMA='a733-g2d-supervised-bundle-v1'
CONTRACT_MAGIC=0x4153373347324331
REQUEST_MAGIC=0x4153373347324c31

def session_check(raw):
    require(len(raw)==904,'G2D session must be904bytes.')
    require(board.session_check(raw[:592])==2,'G2D requires USB0/provider selection2.')
    require(raw[80:112]==raw[280:312] and any(raw[80:112]),
        'G2D board qualification must bind the same recovery evidence.')
    require(struct.unpack_from('<3Q',raw,592)==(0x4153373347324131,1,904),'Invalid G2D session extension.')
    owner,authority,client=struct.unpack_from('<3Q',raw,616)
    require(owner and authority and client<16 and owner not in struct.unpack_from('<2Q',raw,456),'Invalid G2D owner.')
    require(all(any(raw[o:o+32]) for o in (640,672,704,736,768)) and not any(raw[800:832]),'Missing G2D physical or fixture evidence.')
    module,hz,g0,g1,uv,rail=struct.unpack_from('<6I',raw,832)
    require((module,hz)==(0x01000003,100000000) and
        (g0,g1,uv,rail)==(0x10007,0x10007,800000,1),'Unsupported inherited G2D clock/rail state.')
    geometry_check(struct.unpack_from('<12I',raw,856))

def geometry_check(geometry):
    require(len(geometry)==12 and all(type(value) is int and 0<=value<=0xffffffff for value in geometry),
        'G2D geometry requires twelve unsigned32-bit words.')
    width,height,stride,format_,operation,dx,dy,w,h,sx,sy,color=geometry
    require(0<width<=1280 and 0<height<=720 and width*4<=stride<=5120 and not stride%4 and
        stride*height<=3686400 and format_==0 and w>0 and h>0 and
        dx<width and dy<height and w<=width-dx and h<=height-dy,'Invalid G2D destination geometry.')
    require((operation==1 and sx==sy==0) or
        (operation==2 and color==0 and sx<width and sy<height and w<=width-sx and h<=height-sy),
        'Invalid G2D source rectangle or operation.')
    return stride*height

def embedded_reference(image,session):
    """Bind immutable native geometry before preparation or runtime upload."""
    session_check(session)
    _image_parts(image)
    return None

def upload_bytes(pixels,session):
    session_check(session)
    require(len(pixels)==geometry_check(struct.unpack_from('<12I',session,856)),
        'G2D source size must equal stride times height, including padding.')
    require(hashlib.sha256(pixels).digest()==session[768:800],'G2D source digest differs.')
    epoch,generation=struct.unpack_from('<2Q',session,24)
    require(epoch and generation,'G2D upload requires a live session identity.')
    return struct.pack('<4I2Q',0x55444753,1,len(pixels),0,epoch,generation)+pixels

def request_bytes(address,identity,raw,epoch,generation,lease):
    session_check(raw)
    reservation=struct.unpack_from('<Q',raw,200)[0]
    require(len(identity)==32 and 0x41000000<=address<=0x411fffe0 and
        all(type(x) is int and 0<x<2**64 for x in (epoch,generation,lease,reservation)),
        'Invalid retained G2D identity.')
    digest=hashlib.sha256(raw).digest()
    request=struct.pack('<8Q',REQUEST_MAGIC,2,128,epoch,generation,generation^((1<<64)-1),lease,address)+identity+digest
    contract=struct.pack('<6Q',0,2,288,epoch,generation,generation^((1<<64)-1))+identity
    contract+=struct.pack('<18Q',0,0,0,0,lease,0,0,0,0,0,0,0,1,reservation,0x48000000,0x78000000,0x78000000,0x79000000)
    contract+=digest+raw[80:112]
    require(len(contract)==288,'Extended G2D contract ABI differs.')
    return request,contract+bytes(4096-len(contract))

def build_launcher(out,clang,lld):
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text()
    (out/'base-supervised-launch.c').write_text(text)
    require('REQUEST[1]==3' in text and 'CONTRACT->version==3' in text,'Unexpected supervised launcher version guards.')
    for anchor in ('0x4153373353554c33','REQUEST[1]==3','CONTRACT->version==3',
                   'CONTRACT->reserved[0] && !CONTRACT->reserved[1]','CONTRACT->bytes==sizeof(*CONTRACT)',
                   'CONTRACT->reserved[2+i]'):
        require(text.count(anchor)==1,'Shared G2D launcher adaptation anchor changed: '+anchor)
    text=text.replace('0x4153373353554c33','0x4153373347324c31').replace('REQUEST[1]==3','REQUEST[1]==2').replace('CONTRACT->version==3','CONTRACT->version==2')
    text=text.replace('CONTRACT->reserved[0] && !CONTRACT->reserved[1]','CONTRACT->reserved[0]==1 && CONTRACT->reserved[1] && CONTRACT->reserved[2]==0x48000000 && CONTRACT->reserved[3]==0x78000000 && CONTRACT->reserved[4]==0x78000000 && CONTRACT->reserved[5]==0x79000000')
    text=text.replace('CONTRACT->bytes==sizeof(*CONTRACT)','CONTRACT->bytes==288').replace('CONTRACT->reserved[2+i]','((volatile uint64_t *)((uintptr_t)CONTRACT+224))[i]')
    # The shared20-second native lifetime remains exact for G2D.
    (out/'g2d-launch-base.c').write_text(text)
    for name,path in [('launch.c',ROOT/'apps/g2d/launch.c'),('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h'),('a733_g2d_application.h',ROOT/'include/a733_g2d_application.h'),('a733_board_application.h',ROOT/'include/a733_board_application.h')]:shutil.copy2(path,out/name)
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-MD','-Wall','-Wextra','-Werror','-DSUNSTONE_BOARD_SUPERVISED=1','-DA733_BOARD_SUPERVISED_SECONDS=20',f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC}ULL','-I'+str(out)]
    commands=[[clang,*flags,'-c',out/name,'-o',out/(name+'.o')] for name in ('launch.c','launch.S')]
    commands += [[lld,'-T',out/'launch.ld',out/'launch.c.o',out/'launch.S.o','-o',out/'launch.elf'],['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',out/'launch.elf',out/'launch.bin']]
    for command in commands:
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (out/'launcher-build.log').open('a') as f:f.write(r.stdout+r.stderr)
        require(r.returncode==0,'G2D launcher build failed; inspect retained log.')
    base.save_json(out/'launcher-commands.json',[[str(x) for x in c] for c in commands])
    require(0<(out/'launch.bin').stat().st_size<=0xc00,'Launcher exceeds SRAM.')

def physical_authority(record):
    require(record.get('physical_authority') is True and record.get('modeled') is False,
        'Modeled G2D evidence cannot authorize physical execution.')


def allocation_check(proof,raw,table):
    """Check actual provider rows and every IOMMU descriptor, including holes."""
    rounded=(geometry_check(struct.unpack_from('<12I',raw,856))+4095)&~4095
    require(proof.get('rounded')==rounded and proof.get('table_bytes')==32768,
        'G2D allocation geometry differs.')
    groups=proof.get('allocations',{})
    require(set(groups)=={'boot_pool','uncached_pool'} and
        len(groups['boot_pool'])==2 and len(groups['uncached_pool'])==4,'Incomplete G2D allocations.')
    extents=[]
    for pool,low,high,sizes,state in [('boot_pool',0x48000000,0x78000000,[rounded]*2,1),
            ('uncached_pool',0x78000000,0x79000000,[32768,rounded+8192,rounded+8192,12288],2)]:
        for slot,(row,size) in enumerate(zip(groups[pool],sizes)):
            address=row.get('physical');alignment=16384 if pool=='uncached_pool' and slot==0 else 4096
            require(type(address) is int and row.get('slot')==slot and row.get('cpu')==address and
                row.get('bytes')==size and row.get('state')==state and low<=address<=high-size and
                not address%alignment,'G2D provider extent differs.')
            require(all(address>=a+n or a>=address+size for a,n in extents),'G2D physical extents alias.')
            extents.append((address,size))
    nc=groups['uncached_pool'];pa=nc[0]['physical']
    spans=[dict(iova=iova,physical=nc[i+1]['physical']+4096,bytes=size,access=access)
        for i,(iova,size,access) in enumerate([(0x10000000,rounded,1),(0x10400000,rounded,3),(0x10800000,4096,1)])]
    require(proof.get('spans')==spans and proof.get('table_physical')==pa and
        proof.get('source_physical')==spans[0]['physical'] and
        proof.get('destination_physical')==spans[1]['physical'] and
        proof.get('rcq_physical')==spans[2]['physical'] and proof.get('rcq_bytes')==4096,
        'G2D guarded mapping differs.')
    words=[0]*8192
    def address(value):return (value&0xfffff000)|((value>>24)&0x300)
    windows=[]
    for span in spans:
        for window in range(span['iova']>>22,((span['iova']+span['bytes']-1)>>22)+1):
            if window not in windows:windows.append(window)
    require(len(windows)<=4,'G2D mapping exceeds table capacity.')
    for slot,window in enumerate(windows):words[window]=address(pa+16384+slot*4096)|1
    for span in spans:
        for offset in range(0,span['bytes'],4096):
            va=span['iova']+offset;slot=windows.index(va>>22)
            words[4096+slot*1024+((va>>12)&1023)]=address(span['physical']+offset)|2|4|(8 if span['access']&2 else 0)
    require(table==struct.pack('<8192I',*words) and
        hashlib.sha256(table).hexdigest()==proof.get('table_sha256'),
        'G2D page-table bytes, permissions or unmapped guards differ.')


def validate_admission(record,image_hash,template_hash,raw,cache=None,frozen=False):
    # Existing DDR/platform/manual-recovery checks stay exact and byte-snapshotted.
    require(record.get('schema')=='a733-g2d-supervised-entry-v1','Wrong G2D admission schema.')
    physical_authority(record)
    common={**record,'schema':'a733-board-supervised-entry-v2'}
    cache=board.validate_admission(common,image_hash,template_hash,cache,frozen)
    for key,offset in [('initial',640),('memory',672),('electrical',704),('supervision',736)]:
        item=record[key];path=Path(item['path']);require(path.is_absolute(),'Evidence path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Missing frozen G2D evidence.');cache[str(path)]=path.read_bytes()
        data=cache[str(path)];require(hashlib.sha256(data).digest()==raw[offset:offset+32] and hashlib.sha256(data).hexdigest()==item['sha256'],'G2D evidence differs.')
        fact=json.loads(data)
        physical_authority(fact)
        require(fact.get('schema')=='a733-g2d-'+key+'-v1' and fact.get('identity')==record['identity'] and fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and fact.get('reviewed') is True,'G2D authority is not bound to this image/target.')
        required={'initial':['all_masters_excluded','prior_transactions_drained','prior_walks_drained','g2d_inherited_powered_reset_deasserted','skipped_masters_physically_quiescent','remote_secure_writers_excluded','g2d_irq_owned_masked','iommu_irqs_owned_masked'],
            'memory':['private_nc_arenas','no_physical_aliases','iommu_banks_disabled','iommu_clocks_stable','g2d_device_mapping'],
            'electrical':['dcdc2_shared_800mv','g2d_operating_point_source_reviewed','stable_g2d_parent'],
            'supervision':['whole_board_power_removal','permanent_dma_retention','no_reuse_after_fault','one_attempt']}[key]
        require(all(fact.get(k) is True for k in required),'Required G2D physical fact absent.')
    item=record['g2d_ddr'];path=Path(item['path']);require(path.is_absolute(),'G2D DDR evidence path must be absolute.')
    if str(path) not in cache:
        require(not frozen,'Frozen G2D DDR evidence absent.');cache[str(path)]=path.read_bytes()
    data=cache[str(path)];require(hashlib.sha256(data).hexdigest()==item['sha256'],'G2D DDR evidence changed.')
    fact=json.loads(data)
    physical_authority(fact)
    require(fact.get('schema')=='a733-ddr-reservation-v1' and fact.get('identity')==record['identity'] and
        fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and
        fact.get('exclusive') is True and fact.get('published_cache_disabled') is True and
        fact.get('remote_writers_excluded') is True and fact.get('ranges')==[[0x48000000,0x79000000]] and fact.get('evidence'),
        'Extended DRAM reservation/exclusion evidence required.')
    for item in fact['evidence']:
        path=Path(item['path']);require(path.is_absolute(),'G2D DDR support path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Frozen G2D DDR support absent.');cache[str(path)]=path.read_bytes()
        require(hashlib.sha256(cache[str(path)]).hexdigest()==item['sha256'],'G2D DDR support changed.')
    def evidence(item):
        require(isinstance(item,dict) and set(item)=={'path','sha256'},'Invalid G2D allocation reference.')
        path=Path(item['path']);require(path.is_absolute(),'G2D allocation path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Frozen G2D allocation evidence absent.');cache[str(path)]=path.read_bytes()
        data=cache[str(path)];require(hashlib.sha256(data).hexdigest()==item['sha256'],'G2D allocation evidence changed.')
        return data
    proof=json.loads(evidence(record['allocation']))
    physical_authority(proof)
    require(proof.get('image')==image_hash and proof.get('session')==template_hash,
        'G2D allocation proof covers different image/session bytes.')
    allocation_check(proof,raw,evidence(record['page_table']))
    return cache

def prepare(image,manifest,admission,template,output,clang,lld,pixels,reference=None):
    snapshots={name:Path(path).read_bytes() for name,path in [('image.elf',image),('image-input-manifest.json',manifest),('admission.json',admission),('session-template.bin',template)]}
    snapshots['input.argb']=Path(pixels).read_bytes()
    require(reference is None,'G2D has no runtime reference upload.')
    raw=bytearray(snapshots['session-template.bin']);session_check(raw)
    upload_bytes(snapshots['input.argb'],raw)
    embedded_reference(snapshots['image.elf'],raw)
    ih=hashlib.sha256(snapshots['image.elf']).hexdigest();th=hashlib.sha256(raw).hexdigest()
    mf=json.loads(snapshots['image-input-manifest.json'])
    require(mf.get('target')=='sunstone-a733-g2d' and mf.get('execution_profile')=='g2d-supervised-v1' and mf.get('final_elf')==ih,'Tested G2D image manifest required.')
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
    (out/'upload.bin').write_bytes(upload_bytes(snapshots['input.argb'],raw))
    (out/'session.bin').write_bytes(raw);uploads.append({'name':'session.bin','address':0x41f00000})
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,epoch,generation,lease)):(out/name).write_bytes(data)
    for name,count in [('result-zero.bin',16384),('report-zero.bin',128)]:(out/name).write_bytes(bytes(count))
    evidence={}
    for i,(path,data) in enumerate(cache.items()):
        name=f'evidence-{i}.bin';(out/name).write_bytes(data);evidence[path]={'name':name,'sha256':hashlib.sha256(data).hexdigest()}
    p=dict(schema=SCHEMA,physical_access=False,image_sha256=ih,build_id=identity.hex(),epoch=epoch,generation=generation,lease=lease,reservation=record['reservation'],session='session.bin',profile=None,manifest='image-input-manifest.json',image='image.elf',segments=uploads,entry_evidence=evidence,backup_sizes={name:n for name,a,n in base.REGIONS},files={x.name:base.digest(x) for x in out.iterdir() if x.is_file()})
    base.save_json(out/'prepared.json',p);return validate_bundle(out)

def validate_bundle(bundle):
    out=Path(bundle);p=json.loads((out/'prepared.json').read_text());require(p.get('schema')==SCHEMA and p.get('physical_access') is False,'Wrong G2D bundle.')
    required={'image.elf','image-input-manifest.json','admission.json','session-template.bin','session.bin','request.bin','contract.bin','launch.bin','result-zero.bin','report-zero.bin','input.argb','upload.bin'}
    require(required<=set(p['files']) and all(Path(item['name']).name==item['name'] and item['name'] in p['files'] for item in p['entry_evidence'].values()),'Unsealed G2D authority or transport input.')
    for name,h in p['files'].items():require(Path(name).name==name and base.digest(out/name)==h,'G2D bundle input changed.')
    require(base.digest(out/'image.elf')==p['image_sha256'],'G2D image changed.')
    manifest=json.loads((out/'image-input-manifest.json').read_text())
    require(manifest.get('target')=='sunstone-a733-g2d' and manifest.get('execution_profile')=='g2d-supervised-v1' and manifest.get('final_elf')==p['image_sha256'],'G2D image manifest differs.')
    sizes={name:(out/name).stat().st_size for name in p['files']}
    require(p.get('backup_sizes')=={name:n for name,a,n in base.REGIONS} and 0<sizes.get('launch.bin',0)<=0xc00,'G2D protected SRAM geometry changed.')
    require((out/'result-zero.bin').read_bytes()==bytes(16384) and (out/'report-zero.bin').read_bytes()==bytes(128),'G2D initial report backing changed.')
    raw=(out/'session.bin').read_bytes();session_check(raw)
    require((out/'upload.bin').read_bytes()==upload_bytes((out/'input.argb').read_bytes(),raw),'G2D upload bytes differ.')
    template=(out/'session-template.bin').read_bytes();cache={path:(out/item['name']).read_bytes() for path,item in p['entry_evidence'].items()}
    validate_admission(json.loads((out/'admission.json').read_text()),p['image_sha256'],hashlib.sha256(template).hexdigest(),template,cache,True)
    segments,address,identity=image_parts(out/'image.elf');require(identity.hex()==p['build_id'],'G2D build identity changed.')
    expected_raw=bytearray(template);struct.pack_into('<3Q',expected_raw,24,p['epoch'],p['generation'],p['lease']);expected_raw[48:80]=identity
    require(raw==expected_raw,'G2D session differs from admitted derivation.')
    expected=[]
    for i,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{i}.bin';require((out/name).read_bytes()==segment['data'],'G2D ELF bytes changed.');expected.append({'name':name,'address':segment['address']})
    expected.append({'name':'session.bin','address':0x41f00000});require(p['segments']==expected and all(item['name'] in sizes for item in expected),'G2D upload spans changed or unsealed.')
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,p['epoch'],p['generation'],p['lease'])):require((out/name).read_bytes()==data,'G2D launch contract changed.')
    embedded_reference((out/'image.elf').read_bytes(),raw)
    p['_reference']=None
    p['_upload']=(out/'upload.bin').read_bytes()
    p['_sizes']={name:(out/name).stat().st_size for name in p['files']};return p


def image_parts(path):
    return _image_parts(Path(path).read_bytes())

def _image_parts(data):
    import io
    elf=ELFFile(io.BytesIO(data));require(elf['e_machine']=='EM_AARCH64' and elf['e_entry']==0x41000000,'Native extended-memory G2D ELF required.')
    table=elf.get_section_by_name('.symtab');require(table is not None,'Native G2D symbol table required.')
    entries=list(table.iter_symbols());symbols={s.name:(s['st_value'],s['st_size']) for s in entries}
    parts=[]
    for s in elf.iter_segments():
        if s['p_type']!='PT_LOAD':continue
        a,n=s['p_paddr'],s['p_memsz'];data=s.data()
        require(a==s['p_vaddr'] and n>0 and len(data)<=n and not (s['p_flags']&3==3) and
            all(a>=p['address']+p['memory_bytes'] or p['address']>=a+n for p in parts),'Invalid G2D ELF allocations.')
        require((0x41000000<=a<a+n<=0x42200000 and (not data or a+len(data)<=0x42000000)) or
            (not data and (a,a+n) in [(0x48000000,0x78000000),(0x78000000,0x79000000)]),'Extended G2D ELF span outside reservations.')
        require(not s['p_flags']&1 or a+n<=0x41200000,'Extended G2D code outside image prefix.')
        parts.append({'address':a,'memory_bytes':n,'data':data,'flags':int(s['p_flags'])})
    for name,value in {'__device_pool_start':0x48000000,'__device_pool_end':0x78000000,'__uncached_pool_start':0x78000000,'__uncached_pool_end':0x79000000,'__a733_smp_contract':0x42004000,'__a733_smp_contract_end':0x42005000}.items():
        require(symbols.get(name,(0,))[0]==value,'Extended G2D linker ownership differs.')
    require(any(p['flags']&1 and p['address']<=0x41000000<p['address']+len(p['data']) for p in parts),
        'G2D entry lacks executable file-backed bytes.')
    require('__bss_end' in symbols and symbols['__bss_end'][0]<=0x41f00000,'Extended G2D session overlaps BSS.')
    def immutable(name,size):
        require(sum(s.name==name for s in entries)==1,'G2D immutable marker is not unique: '+name)
        address,n=symbols.get(name,(0,0));require(n==size,'Extended G2D immutable marker absent: '+name)
        match=[p for p in parts if not p['flags']&2 and p['address']<=address and address+size<=p['address']+len(p['data'])]
        require(len(match)==1,'Extended G2D marker is not immutable/file-backed.')
        p=match[0];return address,p['data'][address-p['address']:address-p['address']+size]
    _,geometry=immutable('g2d_geometry',88)
    require(struct.unpack('<11Q',geometry)==(0x4153373347324131,1,904,288,0x41f00000,20,3686400,3686400,192,1280,720),'G2D compiled geometry differs.')
    address,identity=immutable('a733_smp_build_id',32)
    for p in parts:p.pop('flags')
    return parts,address,identity

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','admission','template','output','clang','lld','input'):parser.add_argument('--'+name,type=Path,required=True)
    a=parser.parse_args();prepare(a.image,a.manifest,a.admission,a.template,a.output,a.clang,a.lld,a.input)
