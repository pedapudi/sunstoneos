#!/usr/bin/env python3
"""Offline preparation of one supervised caller-supplied ISP0 application session."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import secrets
import shutil
import struct
import subprocess
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
board=module('isp0_board_prepare',ROOT/'apps/board/prepare.py')
base=board.base
require=base.require
SCHEMA='a733-isp0-supervised-bundle-v1'
CONTRACT_MAGIC=0x4153373349504331
REQUEST_MAGIC=0x4153373349504c31

def session_check(raw):
    require(len(raw)==880,'ISP0 session must be880bytes.')
    require(board.session_check(raw[:592])==2,'ISP0 requires USB0/provider selection2.')
    require(raw[80:112]==raw[280:312] and any(raw[80:112]),
        'ISP0 board qualification must bind the same recovery evidence.')
    require(struct.unpack_from('<3Q',raw,592)==(0x4153373349504131,1,880),'Invalid ISP0 session extension.')
    owner,authority,client=struct.unpack_from('<3Q',raw,616)
    require(owner and authority and client<16 and owner not in struct.unpack_from('<2Q',raw,456),'Invalid ISP0 owner.')
    require(all(any(raw[o:o+32]) for o in (640,672,704,736,768)) and not any(raw[800:832]),'Missing ISP0 physical or fixture evidence.')
    g0,g1,uv,rail=struct.unpack_from('<4I',raw,832)
    require((g0,g1,uv,rail)==(0x10007,0x10007,800000,1),'Unsupported ISP0 IOMMU/rail state.')
    geometry_check(struct.unpack_from('<8I',raw,848))


def geometry_check(geometry):
    require(len(geometry)==8 and all(type(value) is int for value in geometry),'ISP0 geometry requires eight unsigned32-bit words.')
    require(tuple(geometry)==(640,480,1280,640,12,0,0,0),'ISP0 requires fixed BGGR12-to-NV16 geometry.')
    return 614400


def input_check(pixels):
    require(len(pixels)==614400,'ISP0 input must contain 614400 bytes.')
    require(all(sample[0]<=4095 for sample in struct.iter_unpack('<H',pixels)),
        'ISP0 BGGR12 samples must fit twelve bits in little-endian16 containers.')


def embedded_reference(image,session):
    """Bind immutable native geometry before preparation or runtime upload."""
    session_check(session)
    _image_parts(image)
    require(struct.unpack_from('<I',session,532)[0]==1,
        'Ordinary ISP0 SMP image requires cold USB reference mode1.')
    return None

def upload_bytes(pixels,session):
    session_check(session)
    input_check(pixels)
    require(len(pixels)==geometry_check(struct.unpack_from('<8I',session,848)),
        'ISP0 source size must equal stride times height, including padding.')
    require(hashlib.sha256(pixels).digest()==session[768:800],'ISP0 source digest differs.')
    epoch,generation=struct.unpack_from('<2Q',session,24)
    require(epoch and generation,'ISP0 upload requires a live session identity.')
    return struct.pack('<4I2Q',0x55495053,1,len(pixels),0,epoch,generation)+pixels

def request_bytes(address,identity,raw,epoch,generation,lease):
    session_check(raw)
    request,contract=board.request_bytes(address,identity,epoch,generation,lease,raw)
    request=struct.pack('<Q',REQUEST_MAGIC)+request[8:]
    contract=bytearray(contract);struct.pack_into('<Q',contract,184,880)
    return request,bytes(contract)


def build_launcher(out,clang,lld):
    for label,path in (('clang',clang),('lld',lld)):
        require(Path(path).is_file() and os.access(path,os.X_OK),
            'ISP0 launcher '+label+' must name an existing executable: '+str(path))
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text()
    (out/'base-supervised-launch.c').write_text(text)
    require('REQUEST[1]==3' in text and 'CONTRACT->version==3' in text,'Unexpected supervised launcher version guards.')
    for anchor in ('0x4153373353554c33','REQUEST[1]==3','CONTRACT->version==3',
                   'CONTRACT->reserved[0] && !CONTRACT->reserved[1]'):
        require(text.count(anchor)==1,'Shared ISP0 launcher adaptation anchor changed: '+anchor)
    text=text.replace('0x4153373353554c33','0x4153373349504c31').replace('REQUEST[1]==3','REQUEST[1]==2').replace('CONTRACT->version==3','CONTRACT->version==2')
    text=text.replace('CONTRACT->reserved[0] && !CONTRACT->reserved[1]',
        'CONTRACT->reserved[0]==A733_BOARD_SESSION_ADDRESS && CONTRACT->reserved[1]==sizeof(struct a733_isp0_session)')
    # The shared20-second native lifetime remains exact for ISP0.
    (out/'isp0-launch-base.c').write_text(text)
    for name,path in [('launch.c',ROOT/'apps/isp0/launch.c'),('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h'),('a733_isp0_application.h',ROOT/'include/a733_isp0_application.h'),('a733_board_application.h',ROOT/'include/a733_board_application.h')]:shutil.copy2(path,out/name)
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-MD','-Wall','-Wextra','-Werror','-DSUNSTONE_BOARD_SUPERVISED=1','-DA733_BOARD_SUPERVISED_SECONDS=20',f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC}ULL','-I'+str(out)]
    commands=[[clang,*flags,'-c',out/name,'-o',out/(name+'.o')] for name in ('launch.c','launch.S')]
    commands += [[lld,'-T',out/'launch.ld',out/'launch.c.o',out/'launch.S.o','-o',out/'launch.elf'],['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',out/'launch.elf',out/'launch.bin']]
    base.save_json(out/'launcher-commands.json',[[str(x) for x in c] for c in commands])
    for command in commands:
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (out/'launcher-build.log').open('a') as f:f.write(r.stdout+r.stderr)
        require(r.returncode==0,'ISP0 launcher build failed; inspect retained log.')
    require(0<(out/'launch.bin').stat().st_size<=0xc00,'Launcher exceeds SRAM.')

def physical_authority(record):
    require(record.get('physical_authority') is True and record.get('modeled') is False,
        'Modeled ISP0 evidence cannot authorize physical execution.')


def allocation_check(proof,raw,table,image):
    """Bind actual provider extents to linked pools and reconstruct all descriptors."""
    import io
    session_check(raw);_image_parts(image)
    symbols={s.name:int(s['st_value']) for s in ELFFile(io.BytesIO(image)).get_section_by_name('.symtab').iter_symbols()}
    wb_low,wb_high=(symbols[k] for k in ('__device_pool_start','__device_pool_end'))
    groups=proof.get('allocations',{})
    require(set(groups)=={'boot_pool','uncached_pool'} and
        len(groups['boot_pool'])==2 and len(groups['uncached_pool'])==8 and proof.get('table_bytes')==32768,
        'Incomplete ISP0 native allocations.')
    # Table, input+64-byte prefetch, Y, UV, load, save, save-load and VIPP.
    rounded=[32768,618496,307200,307200,32768,28672,16384,4096]
    extents=[]
    for pool,low,high,sizes,state in [('boot_pool',wb_low,wb_high,[614400]*2,1),
            ('uncached_pool',0x41c00000,0x41e00000,[n+(8192 if i else 0) for i,n in enumerate(rounded)],2)]:
        for slot,(row,size) in enumerate(zip(groups[pool],sizes)):
            address=row.get('physical');alignment=16384 if pool=='uncached_pool' and slot==0 else 4096
            require(type(address) is int and row.get('slot')==slot and row.get('cpu')==address and
                row.get('bytes')==size and row.get('state')==state and low<=address<=high-size and
                not address%alignment,'ISP0 native provider extent differs.')
            require(all(address>=a+n or a>=address+size for a,n in extents),'ISP0 native physical extents alias.')
            extents.append((address,size))
    nc=groups['uncached_pool'];pa=nc[0]['physical']
    iovas=(0x100000,0x300000,0x380000,0x200000,0x210000,0x220000,0x230000)
    access=(1,3,3,1,3,3,1)
    spans=[dict(iova=iova,physical=nc[i+1]['physical']+4096,bytes=rounded[i+1],access=permission)
        for i,(iova,permission) in enumerate(zip(iovas,access))]
    require(proof.get('spans')==spans and proof.get('table_physical')==pa,'ISP0 guarded mapping differs.')
    words=[0]*8192
    def descriptor_address(value):return (value&0xfffff000)|((value>>24)&0x300)
    windows=[]
    for span in spans:
        for window in range(span['iova']>>22,((span['iova']+span['bytes']-1)>>22)+1):
            if window not in windows:windows.append(window)
    require(len(windows)<=4,'ISP0 table capacity exceeded.')
    for slot,window in enumerate(windows):words[window]=descriptor_address(pa+16384+slot*4096)|1
    for span in spans:
        for offset in range(0,span['bytes'],4096):
            va=span['iova']+offset;slot=windows.index(va>>22)
            words[4096+slot*1024+((va>>12)&1023)]=descriptor_address(span['physical']+offset)|6|(8 if span['access']&2 else 0)
    require(table==struct.pack('<8192I',*words) and hashlib.sha256(table).hexdigest()==proof.get('table_sha256'),
        'ISP0 descriptors, permissions or unmapped guard holes differ.')


def validate_admission(record,image_hash,template_hash,raw,cache=None,frozen=False,image=None):
    # Existing DDR/platform/manual-recovery checks stay exact and byte-snapshotted.
    require(record.get('schema')=='a733-isp0-supervised-entry-v1','Wrong ISP0 admission schema.')
    physical_authority(record)
    common={**record,'schema':'a733-board-supervised-entry-v2'}
    cache=board.validate_admission(common,image_hash,template_hash,cache,frozen)
    for key,offset in [('initial',640),('memory',672),('electrical',704),('supervision',736)]:
        item=record[key];path=Path(item['path']);require(path.is_absolute(),'Evidence path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Missing frozen ISP0 evidence.');cache[str(path)]=path.read_bytes()
        data=cache[str(path)];require(hashlib.sha256(data).digest()==raw[offset:offset+32] and hashlib.sha256(data).hexdigest()==item['sha256'],'ISP0 evidence differs.')
        fact=json.loads(data)
        physical_authority(fact)
        require(fact.get('schema')=='a733-isp0-'+key+'-v1' and fact.get('identity')==record['identity'] and fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and fact.get('reviewed') is True,'ISP0 authority is not bound to this image/target.')
        required={'initial':['all_masters_excluded','prior_transactions_drained','prior_walks_drained','isp0_inherited_powered_reset_deasserted','skipped_masters_physically_quiescent','remote_secure_writers_excluded','isp0_vipp_dma_irqs_owned_masked','iommu_irqs_owned_masked'],
            'memory':['private_nc_arenas','no_physical_aliases','iommu_banks_disabled','iommu_clocks_stable','isp0_master1_read_master2_write_mapping'],
            'electrical':['dcdc2_shared_800mv','isp0_operating_point_source_reviewed','stable_isp0_parent'],
            'supervision':['whole_board_power_removal','permanent_dma_retention','no_reuse_after_fault','one_attempt']}[key]
        require(all(fact.get(k) is True for k in required),'Required ISP0 physical fact absent.')
    def evidence(item):
        require(isinstance(item,dict) and set(item)=={'path','sha256'},'Invalid ISP0 allocation reference.')
        path=Path(item['path']);require(path.is_absolute(),'ISP0 allocation path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Frozen ISP0 allocation evidence absent.');cache[str(path)]=path.read_bytes()
        data=cache[str(path)];require(hashlib.sha256(data).hexdigest()==item['sha256'],'ISP0 allocation evidence changed.')
        return data
    proof=json.loads(evidence(record['allocation']))
    physical_authority(proof)
    require(proof.get('image')==image_hash and proof.get('session')==template_hash,
        'ISP0 allocation proof covers different image/session bytes.')
    require(image is not None and hashlib.sha256(image).hexdigest()==image_hash,'ISP0 allocation requires its actual linked image.')
    allocation_check(proof,raw,evidence(record['page_table']),image)
    return cache

def prepare(image,manifest,admission,template,output,clang,lld,pixels,reference=None):
    snapshots={name:Path(path).read_bytes() for name,path in [('image.elf',image),('image-input-manifest.json',manifest),('admission.json',admission),('session-template.bin',template)]}
    snapshots['input.bggr12']=Path(pixels).read_bytes()
    require(reference is None,'ISP0 has no runtime reference upload.')
    raw=bytearray(snapshots['session-template.bin']);session_check(raw)
    upload_bytes(snapshots['input.bggr12'],raw)
    embedded_reference(snapshots['image.elf'],raw)
    ih=hashlib.sha256(snapshots['image.elf']).hexdigest();th=hashlib.sha256(raw).hexdigest()
    mf=json.loads(snapshots['image-input-manifest.json'])
    require(mf.get('target')=='sunstone-a733-isp0' and mf.get('execution_profile')=='isp0-supervised-v1' and mf.get('final_elf')==ih,'Tested ISP0 image manifest required.')
    record=json.loads(snapshots['admission.json']);cache=validate_admission(record,ih,th,raw,image=snapshots['image.elf'])
    epoch,generation,lease=(secrets.randbits(64) or 1 for _ in range(3))
    struct.pack_into('<3Q',raw,24,epoch,generation,lease)
    out=Path(output);out.mkdir(parents=True,exist_ok=False)
    for name,data in snapshots.items():(out/name).write_bytes(data)
    segments,address,identity=image_parts(out/'image.elf')
    # Runtime authenticates linked build identity, not the ELF file hash.
    raw[48:80]=identity
    build_launcher(out,clang,lld)
    return finish_bundle(out,raw,cache,record)


def finish_bundle(out,raw,cache,record):
    """Finish sealed metadata after an already built, retained launcher."""
    out=Path(out);session_check(raw)
    ih=base.digest(out/'image.elf')
    segments,address,identity=image_parts(out/'image.elf')
    require(raw[48:80]==identity,'ISP0 runtime build identity differs.')
    epoch,generation,lease=struct.unpack_from('<3Q',raw,24)
    uploads=[]
    for i,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{i}.bin';(out/name).write_bytes(segment['data']);uploads.append({'name':name,'address':segment['address']})
    (out/'upload.bin').write_bytes(upload_bytes((out/'input.bggr12').read_bytes(),raw))
    (out/'session.bin').write_bytes(raw);uploads.append({'name':'session.bin','address':0x41f00000})
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,epoch,generation,lease)):(out/name).write_bytes(data)
    for name,count in [('result-zero.bin',16384),('report-zero.bin',128)]:(out/name).write_bytes(bytes(count))
    evidence={}
    for i,(path,data) in enumerate(cache.items()):
        name=f'evidence-{i}.bin';(out/name).write_bytes(data);evidence[path]={'name':name,'sha256':hashlib.sha256(data).hexdigest()}
    p=dict(schema=SCHEMA,physical_access=False,image_sha256=ih,build_id=identity.hex(),epoch=epoch,generation=generation,lease=lease,reservation=record['reservation'],session='session.bin',profile=None,manifest='image-input-manifest.json',image='image.elf',segments=uploads,entry_evidence=evidence,backup_sizes={name:n for name,a,n in base.REGIONS},files={x.name:base.digest(x) for x in out.iterdir() if x.is_file()})
    base.save_json(out/'prepared.json',p);return validate_bundle(out)

def validate_bundle(bundle):
    out=Path(bundle);p=json.loads((out/'prepared.json').read_text());require(p.get('schema')==SCHEMA and p.get('physical_access') is False,'Wrong ISP0 bundle.')
    required={'image.elf','image-input-manifest.json','admission.json','session-template.bin','session.bin','request.bin','contract.bin','launch.bin','result-zero.bin','report-zero.bin','input.bggr12','upload.bin'}
    require(required<=set(p['files']) and all(Path(item['name']).name==item['name'] and item['name'] in p['files'] for item in p['entry_evidence'].values()),'Unsealed ISP0 authority or transport input.')
    for name,h in p['files'].items():require(Path(name).name==name and base.digest(out/name)==h,'ISP0 bundle input changed.')
    require(base.digest(out/'image.elf')==p['image_sha256'],'ISP0 image changed.')
    manifest=json.loads((out/'image-input-manifest.json').read_text())
    require(manifest.get('target')=='sunstone-a733-isp0' and manifest.get('execution_profile')=='isp0-supervised-v1' and manifest.get('final_elf')==p['image_sha256'],'ISP0 image manifest differs.')
    sizes={name:(out/name).stat().st_size for name in p['files']}
    require(p.get('backup_sizes')=={name:n for name,a,n in base.REGIONS} and 0<sizes.get('launch.bin',0)<=0xc00,'ISP0 protected SRAM geometry changed.')
    require((out/'result-zero.bin').read_bytes()==bytes(16384) and (out/'report-zero.bin').read_bytes()==bytes(128),'ISP0 initial report backing changed.')
    raw=(out/'session.bin').read_bytes();session_check(raw)
    require((out/'upload.bin').read_bytes()==upload_bytes((out/'input.bggr12').read_bytes(),raw),'ISP0 upload bytes differ.')
    template=(out/'session-template.bin').read_bytes();cache={path:(out/item['name']).read_bytes() for path,item in p['entry_evidence'].items()}
    validate_admission(json.loads((out/'admission.json').read_text()),p['image_sha256'],hashlib.sha256(template).hexdigest(),template,cache,True,image=(out/'image.elf').read_bytes())
    segments,address,identity=image_parts(out/'image.elf');require(identity.hex()==p['build_id'],'ISP0 build identity changed.')
    expected_raw=bytearray(template);struct.pack_into('<3Q',expected_raw,24,p['epoch'],p['generation'],p['lease']);expected_raw[48:80]=identity
    require(raw==expected_raw,'ISP0 session differs from admitted derivation.')
    expected=[]
    for i,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{i}.bin';require((out/name).read_bytes()==segment['data'],'ISP0 ELF bytes changed.');expected.append({'name':name,'address':segment['address']})
    expected.append({'name':'session.bin','address':0x41f00000});require(p['segments']==expected and all(item['name'] in sizes for item in expected),'ISP0 upload spans changed or unsealed.')
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,p['epoch'],p['generation'],p['lease'])):require((out/name).read_bytes()==data,'ISP0 launch contract changed.')
    embedded_reference((out/'image.elf').read_bytes(),raw)
    p['_reference']=None
    p['_upload']=(out/'upload.bin').read_bytes()
    p['_sizes']={name:(out/name).stat().st_size for name in p['files']};return p


def image_parts(path):
    return _image_parts(Path(path).read_bytes())

def _image_parts(data):
    import io
    elf=ELFFile(io.BytesIO(data));require(elf['e_machine']=='EM_AARCH64' and elf['e_entry']==0x41000000,'Native ISP0 ELF required.')
    table=elf.get_section_by_name('.symtab');require(table is not None,'Native ISP0 symbol table required.')
    entries=list(table.iter_symbols())
    require(not any(s.name=='board_single_core_geometry' for s in entries),
        'ISP0 ordinary SMP profile excludes the single-core image capability.')
    symbols={s.name:(s['st_value'],s['st_size']) for s in entries}
    parts=[]
    for s in elf.iter_segments():
        if s['p_type']!='PT_LOAD':continue
        a,n=s['p_paddr'],s['p_memsz'];data=s.data()
        require(a==s['p_vaddr'] and n>0 and len(data)<=n and not (s['p_flags']&3==3) and
            all(a>=p['address']+p['memory_bytes'] or p['address']>=a+n for p in parts),'Invalid ISP0 ELF allocations.')
        require(0x41000000<=a<a+n<=0x42200000 and (not data or a+len(data)<=0x42000000),
            'ISP0 ELF span outside ordinary board reservations.')
        require(not s['p_flags']&1 or a+n<=0x41200000,'ISP0 code outside image prefix.')
        parts.append({'address':a,'memory_bytes':n,'data':data,'flags':int(s['p_flags'])})
    for name,value in {'__uncached_pool_start':0x41c00000,'__uncached_pool_end':0x41e00000,'__a733_smp_contract':0x42004000,'__a733_smp_contract_end':0x42005000}.items():
        require(symbols.get(name,(0,))[0]==value,'ISP0 linker ownership differs.')
    require('__device_pool_start' in symbols and '__device_pool_end' in symbols and
        symbols['__device_pool_end'][0]-symbols['__device_pool_start'][0]==2097152 and
        0x41200000<=symbols['__device_pool_start'][0]<symbols['__device_pool_end'][0]<=0x41c00000 and
        not symbols['__device_pool_start'][0]%4096,'ISP0 linked WB pool differs.')
    require(any(p['flags']&1 and p['address']<=0x41000000<p['address']+len(p['data']) for p in parts),
        'ISP0 entry lacks executable file-backed bytes.')
    require('__bss_end' in symbols and symbols['__bss_end'][0]<=0x41f00000,'ISP0 session overlaps BSS.')
    def immutable(name,size):
        require(sum(s.name==name for s in entries)==1,'ISP0 immutable marker is not unique: '+name)
        address,n=symbols.get(name,(0,0));require(n==size,'ISP0 immutable marker absent: '+name)
        match=[p for p in parts if not p['flags']&2 and p['address']<=address and address+size<=p['address']+len(p['data'])]
        require(len(match)==1,'ISP0 marker is not immutable/file-backed.')
        p=match[0];return address,p['data'][address-p['address']:address-p['address']+size]
    _,geometry=immutable('isp0_geometry',88)
    require(struct.unpack('<11Q',geometry)==(0x4153373349504131,1,880,224,0x41f00000,20,614400,614400,192,640,480),'ISP0 compiled geometry differs.')
    address,identity=immutable('a733_smp_build_id',32)
    for p in parts:p.pop('flags')
    return parts,address,identity

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','admission','template','output','clang','lld','input'):parser.add_argument('--'+name,type=Path,required=True)
    a=parser.parse_args();prepare(a.image,a.manifest,a.admission,a.template,a.output,a.clang,a.lld,a.input)
