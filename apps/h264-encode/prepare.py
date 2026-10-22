#!/usr/bin/env python3
"""Offline preparation of one supervised caller-supplied H264 encoder application session."""
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
board=module('h264_encoder_board_prepare',ROOT/'apps/board/prepare.py')
base=board.base
require=base.require
SCHEMA='a733-h264-encode-supervised-bundle-v1'
CONTRACT_MAGIC=0x4153373348454331
REQUEST_MAGIC=0x4153373348454c31

def session_check(raw):
    require(len(raw)==856,'H264 encoder session must be856bytes.')
    require(board.session_check(raw[:592])==2,'H264 encoder requires USB0/provider selection2.')
    require(struct.unpack_from('<3Q',raw,592)==(0x4153373348454131,1,856),'Invalid H264 encoder session extension.')
    owner,authority,client=struct.unpack_from('<3Q',raw,616)
    require(owner and authority and client<16 and owner not in struct.unpack_from('<2Q',raw,456),'Invalid H264 encoder owner.')
    require(all(any(raw[o:o+32]) for o in (640,672,704,736,768)) and not any(raw[800:832]),'Missing H264 encoder physical or fixture evidence.')
    module,hz,g0,g1,uv,rail=struct.unpack_from('<6I',raw,832)
    require(not module&~0x0700001f and (module>>24)<=1 and 0<hz<=720000000 and
        (g0,g1,uv,rail)==(0x10007,0x10007,800000,1),'Unsupported inherited H264 encoder clock/rail state.')

def embedded_reference(image,session):
    """Bind immutable runtime capacities; runtime input is uploaded separately."""
    import io
    from elftools.elf.elffile import ELFFile
    session_check(session)
    elf=ELFFile(io.BytesIO(image));table=elf.get_section_by_name('.symtab')
    require(table is not None,'H264 encoder image has no runtime geometry.')
    found=[s for s in table.iter_symbols() if s.name=='h264_encode_geometry']
    require(len(found)==1,'Missing unique immutable H264 encoder runtime geometry.')
    first,size=int(found[0]['st_value']),int(found[0]['st_size'])
    expected=struct.pack('<8Q',0x4153373348454131,1,856,288,0x41f00000,60,921600,1048576)
    require(size==len(expected),'H264 encoder geometry size differs.')
    parts=[p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and not int(p['p_flags'])&2 and
           int(p['p_vaddr'])<=first and first+size<=int(p['p_vaddr'])+int(p['p_filesz'])]
    require(len(parts)==1,'H264 encoder runtime geometry must be immutable/file-backed.')
    offset=first-int(parts[0]['p_vaddr']);require(parts[0].data()[offset:offset+size]==expected,'H264 encoder runtime capacities differ.')
    return None

def upload_bytes(pixels,reference,session,frames=2):
    session_check(session)
    reference=reference or b''
    require(len(pixels)==921600 and not reference,'Encoder requires VGA NV12 and no reference oracle.')
    require(hashlib.sha256(pixels).digest()==session[768:800],'H264 encoder input digest differs.')
    require((hashlib.sha256(reference).digest() if reference else bytes(32))==session[800:832],
            'H264 encoder oracle mode/digest differs.')
    epoch,generation=struct.unpack_from('<2Q',session,24)
    require(epoch and generation,'H264 encoder upload requires a live session identity.')
    require(1<=frames<=30,'H264 encoder sequences hold 1 to 30 pictures.')
    return struct.pack('<4I2Q',0x55454853,1,len(pixels),frames,epoch,generation)+pixels+reference

def request_bytes(address,identity,raw,epoch,generation,lease):
    session_check(raw)
    reservation=struct.unpack_from('<Q',raw,200)[0]
    require(len(identity)==32 and 0x41000000<=address<=0x411fffe0 and
        all(type(x) is int and 0<x<2**64 for x in (epoch,generation,lease,reservation)),
        'Invalid retained encoder identity.')
    digest=hashlib.sha256(raw).digest()
    request=struct.pack('<8Q',REQUEST_MAGIC,2,128,epoch,generation,generation^((1<<64)-1),lease,address)+identity+digest
    contract=struct.pack('<6Q',0,2,288,epoch,generation,generation^((1<<64)-1))+identity
    contract+=struct.pack('<18Q',0,0,0,0,lease,0,0,0,0,0,0,0,1,reservation,0x48000000,0x78000000,0x78000000,0x79000000)
    contract+=digest+raw[80:112]
    require(len(contract)==288,'Extended encoder contract ABI differs.')
    return request,contract+bytes(4096-len(contract))

def build_launcher(out,clang,lld):
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text()
    (out/'base-supervised-launch.c').write_text(text)
    text=board.adapt_launcher(text,{'0x4153373353554c33':'0x4153373348454c31','REQUEST[1]==3':'REQUEST[1]==2','CONTRACT->version==3':'CONTRACT->version==2',
        'CONTRACT->reserved[0] && !CONTRACT->reserved[1]':'CONTRACT->reserved[0]==1 && CONTRACT->reserved[1] && CONTRACT->reserved[2]==0x48000000 && CONTRACT->reserved[3]==0x78000000 && CONTRACT->reserved[4]==0x78000000 && CONTRACT->reserved[5]==0x79000000',
        'CONTRACT->bytes==sizeof(*CONTRACT)':'CONTRACT->bytes==288','CONTRACT->reserved[2+i]':'((volatile uint64_t *)((uintptr_t)CONTRACT+224))[i]',
        '20*A733_SMP_COUNTER_HZ':'60*A733_SMP_COUNTER_HZ'})
    (out/'h264-encode-launch-base.c').write_text(text)
    for name,path in [('launch.c',ROOT/'apps/h264-encode/launch.c'),('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h'),('a733_h264_encode_application.h',ROOT/'include/a733_h264_encode_application.h'),('a733_board_application.h',ROOT/'include/a733_board_application.h')]:shutil.copy2(path,out/name)
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-Wall','-Wextra','-Werror','-DSUNSTONE_BOARD_SUPERVISED=1',f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC}ULL','-I'+str(out)]
    commands=[[clang,*flags,'-c',out/name,'-o',out/(name+'.o')] for name in ('launch.c','launch.S')]
    commands += [[lld,'-T',out/'launch.ld',out/'launch.c.o',out/'launch.S.o','-o',out/'launch.elf'],['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',out/'launch.elf',out/'launch.bin']]
    for command in commands:
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (out/'launcher-build.log').open('a') as f:f.write(r.stdout+r.stderr)
        require(r.returncode==0,'H264 encoder launcher build failed; inspect retained log.')
    base.save_json(out/'launcher-commands.json',[[str(x) for x in c] for c in commands])
    require(0<(out/'launch.bin').stat().st_size<=0xc00,'Launcher exceeds SRAM.')

def validate_admission(record,image_hash,template_hash,raw,cache=None,frozen=False):
    # Existing DDR/platform/manual-recovery checks stay exact and byte-snapshotted.
    require(record.get('schema')=='a733-h264-encode-supervised-entry-v1','Wrong H264 encoder admission schema.')
    common={**record,'schema':'a733-board-supervised-entry-v2'}
    cache=board.validate_admission(common,image_hash,template_hash,cache,frozen)
    for key,offset in [('initial',640),('memory',672),('electrical',704),('supervision',736)]:
        item=record[key];path=Path(item['path']);require(path.is_absolute(),'Evidence path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Missing frozen H264 encoder evidence.');cache[str(path)]=path.read_bytes()
        data=cache[str(path)];require(hashlib.sha256(data).digest()==raw[offset:offset+32] and hashlib.sha256(data).hexdigest()==item['sha256'],'H264 encoder evidence differs.')
        fact=json.loads(data)
        require(fact.get('schema')=='a733-h264-encode-'+key+'-v1' and fact.get('identity')==record['identity'] and fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and fact.get('reviewed') is True,'H264 encoder authority is not bound to this image/target.')
        required={'initial':['all_masters_excluded','prior_transactions_drained','prior_walks_drained','encoder_inherited_powered_reset_deasserted','skipped_masters_physically_quiescent','remote_secure_writers_excluded','irq80_owned_masked','iommu_irqs_owned_masked'],
            'memory':['private_nc_arenas','no_physical_aliases','iommu_banks_disabled','iommu_clocks_stable','encoder_device_mapping'],
            'electrical':['dcdc2_shared_800mv','ve_operating_point_source_reviewed','stable_ve_pll'],
            'supervision':['whole_board_power_removal','permanent_dma_retention','no_reuse_after_fault','one_attempt']}[key]
        require(all(fact.get(k) is True for k in required),'Required H264 encoder physical fact absent.')
    item=record['h264_encode_ddr'];path=Path(item['path']);require(path.is_absolute(),'Encoder DDR evidence path must be absolute.')
    if str(path) not in cache:
        require(not frozen,'Frozen encoder DDR evidence absent.');cache[str(path)]=path.read_bytes()
    data=cache[str(path)];require(hashlib.sha256(data).hexdigest()==item['sha256'],'Encoder DDR evidence changed.')
    fact=json.loads(data)
    require(fact.get('schema')=='a733-ddr-reservation-v1' and fact.get('identity')==record['identity'] and
        fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and
        fact.get('exclusive') is True and fact.get('published_cache_disabled') is True and
        fact.get('remote_writers_excluded') is True and fact.get('ranges')==[[0x48000000,0x79000000]] and fact.get('evidence'),
        'Extended DRAM reservation/exclusion evidence required.')
    for item in fact['evidence']:
        path=Path(item['path']);require(path.is_absolute(),'Encoder DDR support path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Frozen encoder DDR support absent.');cache[str(path)]=path.read_bytes()
        require(hashlib.sha256(cache[str(path)]).hexdigest()==item['sha256'],'Encoder DDR support changed.')
    memory=json.loads(cache[record['memory']['path']])
    expected=[[0x10001000,462848,'r'],[0x10074000,462848,'r'],[0x10400000,4194304,'rw'],
        [0x10801000,491520,'rw'],[0x1087b000,491520,'rw'],[0x108f5000,12288,'rw'],
        [0x108fa000,12288,'rw'],[0x108ff000,28672,'rw']]
    require(record['reservation']==struct.unpack_from('<Q',raw,200)[0] and
        memory.get('dram_reservation_id')==record['reservation'] and memory.get('iova_spans')==expected and
        memory.get('encoder_masters')==[3] and memory.get('outer_guard_pages_unmapped') is True and
        memory.get('scratch_internal_guards_mapped') is True and memory.get('source_extents_reviewed') is True and
        memory.get('wb_range')==[0x48000000,0x78000000] and memory.get('nc_range')==[0x78000000,0x79000000],
        'Encoder memory/reference/guard authority differs.')
    return cache

def prepare(image,manifest,admission,template,output,clang,lld,pixels,reference=None):
    snapshots={name:Path(path).read_bytes() for name,path in [('image.elf',image),('image-input-manifest.json',manifest),('admission.json',admission),('session-template.bin',template)]}
    snapshots['input.nv12']=Path(pixels).read_bytes()
    snapshots['reference.yu12']=Path(reference).read_bytes() if reference else b''
    raw=bytearray(snapshots['session-template.bin']);session_check(raw)
    upload_bytes(snapshots['input.nv12'],snapshots['reference.yu12'],raw)
    embedded_reference(snapshots['image.elf'],raw)
    ih=hashlib.sha256(snapshots['image.elf']).hexdigest();th=hashlib.sha256(raw).hexdigest()
    mf=json.loads(snapshots['image-input-manifest.json'])
    require(mf.get('target')=='sunstone-a733-h264-encode' and mf.get('execution_profile')=='h264-encode-supervised-v1' and mf.get('final_elf')==ih,'Tested H264 encoder image manifest required.')
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
    (out/'upload.bin').write_bytes(upload_bytes(snapshots['input.nv12'],snapshots['reference.yu12'],raw))
    (out/'session.bin').write_bytes(raw);uploads.append({'name':'session.bin','address':0x41f00000})
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,epoch,generation,lease)):(out/name).write_bytes(data)
    for name,count in [('result-zero.bin',16384),('report-zero.bin',128)]:(out/name).write_bytes(bytes(count))
    evidence={}
    for i,(path,data) in enumerate(cache.items()):
        name=f'evidence-{i}.bin';(out/name).write_bytes(data);evidence[path]={'name':name,'sha256':hashlib.sha256(data).hexdigest()}
    p=dict(schema=SCHEMA,physical_access=False,image_sha256=ih,build_id=identity.hex(),epoch=epoch,generation=generation,lease=lease,reservation=record['reservation'],session='session.bin',profile=None,manifest='image-input-manifest.json',image='image.elf',segments=uploads,entry_evidence=evidence,backup_sizes={name:n for name,a,n in base.REGIONS},files={x.name:base.digest(x) for x in out.iterdir() if x.is_file()})
    base.save_json(out/'prepared.json',p);return validate_bundle(out)

def validate_bundle(bundle):
    out=Path(bundle);p=json.loads((out/'prepared.json').read_text());require(p.get('schema')==SCHEMA and p.get('physical_access') is False,'Wrong H264 encoder bundle.')
    required={'image.elf','image-input-manifest.json','admission.json','session-template.bin','session.bin','request.bin','contract.bin','launch.bin','result-zero.bin','report-zero.bin','input.nv12','reference.yu12','upload.bin'}
    require(required<=set(p['files']) and all(Path(item['name']).name==item['name'] and item['name'] in p['files'] for item in p['entry_evidence'].values()),'Unsealed H264 encoder authority or transport input.')
    for name,h in p['files'].items():require(Path(name).name==name and base.digest(out/name)==h,'H264 encoder bundle input changed.')
    require(base.digest(out/'image.elf')==p['image_sha256'],'H264 encoder image changed.')
    manifest=json.loads((out/'image-input-manifest.json').read_text())
    require(manifest.get('target')=='sunstone-a733-h264-encode' and manifest.get('execution_profile')=='h264-encode-supervised-v1' and manifest.get('final_elf')==p['image_sha256'],'H264 encoder image manifest differs.')
    sizes={name:(out/name).stat().st_size for name in p['files']}
    require(p.get('backup_sizes')=={name:n for name,a,n in base.REGIONS} and 0<sizes.get('launch.bin',0)<=0xc00,'H264 encoder protected SRAM geometry changed.')
    require((out/'result-zero.bin').read_bytes()==bytes(16384) and (out/'report-zero.bin').read_bytes()==bytes(128),'H264 encoder initial report backing changed.')
    raw=(out/'session.bin').read_bytes();session_check(raw)
    require((out/'upload.bin').read_bytes()==upload_bytes((out/'input.nv12').read_bytes(),(out/'reference.yu12').read_bytes(),raw),'H264 encoder upload bytes differ.')
    template=(out/'session-template.bin').read_bytes();cache={path:(out/item['name']).read_bytes() for path,item in p['entry_evidence'].items()}
    validate_admission(json.loads((out/'admission.json').read_text()),p['image_sha256'],hashlib.sha256(template).hexdigest(),template,cache,True)
    segments,address,identity=image_parts(out/'image.elf');require(identity.hex()==p['build_id'],'H264 encoder build identity changed.')
    expected_raw=bytearray(template);struct.pack_into('<3Q',expected_raw,24,p['epoch'],p['generation'],p['lease']);expected_raw[48:80]=identity
    require(raw==expected_raw,'H264 encoder session differs from admitted derivation.')
    expected=[]
    for i,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{i}.bin';require((out/name).read_bytes()==segment['data'],'H264 encoder ELF bytes changed.');expected.append({'name':name,'address':segment['address']})
    expected.append({'name':'session.bin','address':0x41f00000});require(p['segments']==expected and all(item['name'] in sizes for item in expected),'H264 encoder upload spans changed or unsealed.')
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,p['epoch'],p['generation'],p['lease'])):require((out/name).read_bytes()==data,'H264 encoder launch contract changed.')
    embedded_reference((out/'image.elf').read_bytes(),raw)
    p['_reference']=(out/'reference.yu12').read_bytes() or None
    p['_upload']=(out/'upload.bin').read_bytes()
    p['_sizes']={name:(out/name).stat().st_size for name in p['files']};return p


def image_parts(path,high=False):
    # Same fixed large-profile ELF geometry as apps/gpu-render/prepare.py.
    with Path(path).open('rb') as f:
        elf=ELFFile(f);require(elf['e_machine']=='EM_AARCH64' and elf['e_entry']==0x41000000,'Native extended-memory encoder ELF required.')
        symbols={s.name:(s['st_value'],s['st_size']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
        parts=[]
        for s in elf.iter_segments():
            if s['p_type']!='PT_LOAD':continue
            a,n=s['p_paddr'],s['p_memsz'];data=s.data()
            require(a==s['p_vaddr'] and n>0 and len(data)<=n and not (s['p_flags']&3==3) and
                all(a>=p['address']+p['memory_bytes'] or p['address']>=a+n for p in parts),'Invalid full-HD ELF allocations.')
            require((0x41000000<=a<a+n<=0x42200000 and (not data or a+len(data)<=0x42000000)) or
                (not data and (a,a+n) in [(0x48000000,0x78000000),(0x78000000,0x79000000)]),'Extended encoder ELF span outside reservations.')
            require(not s['p_flags']&1 or a+n<=0x41200000,'Extended encoder code outside image prefix.')
            parts.append({'address':a,'memory_bytes':n,'data':data,'flags':int(s['p_flags'])})
        for name,value in {'__device_pool_start':0x48000000,'__device_pool_end':0x78000000,'__uncached_pool_start':0x78000000,'__uncached_pool_end':0x79000000,'__a733_smp_contract':0x42004000,'__a733_smp_contract_end':0x42005000}.items():
            require(symbols.get(name,(0,))[0]==value,'Extended encoder linker ownership differs.')
        require('__bss_end' in symbols and symbols['__bss_end'][0]<=0x41f00000,'Extended encoder session overlaps BSS.')
        def immutable(name,size):
            address,n=symbols.get(name,(0,0));require(n==size,'Extended encoder immutable marker absent: '+name)
            match=[p for p in parts if not p['flags']&2 and p['address']<=address and address+size<=p['address']+len(p['data'])]
            require(len(match)==1,'Extended encoder marker is not immutable/file-backed.')
            p=match[0];return address,p['data'][address-p['address']:address-p['address']+size]
        _,geometry=immutable('h264_encode_geometry',64)
        require(struct.unpack('<8Q',geometry)==(0x4153373348454131,1,856,288,0x41f00000,60,921600,1048576),'H264 encode compiled geometry differs.')
        address,identity=immutable('a733_smp_build_id',32)
        for p in parts:p.pop('flags')
        return parts,address,identity

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','admission','template','output','clang','lld','input'):parser.add_argument('--'+name,type=Path,required=True)
    a=parser.parse_args();prepare(a.image,a.manifest,a.admission,a.template,a.output,a.clang,a.lld,a.input)
