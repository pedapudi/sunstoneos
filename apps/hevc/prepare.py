#!/usr/bin/env python3
"""HEVC preparation source; allocation/table admission awaits a frozen native proof.

Production preparation refuses until that check is integrated. No actual HEVC
image or placeholder launcher was built for this source-only handoff.
"""
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
board=module('hevc_decoder_board_prepare',ROOT/'apps/board/prepare.py')
base=board.base
require=base.require
SCHEMA='a733-hevc-supervised-bundle-v1'
CONTRACT_MAGIC=0x4153373348354331
REQUEST_MAGIC=0x4153373348354c31

def session_check(raw):
    require(len(raw)==856,'HEVC decoder session must be856bytes.')
    require(board.session_check(raw[:592])==2,'HEVC decoder requires USB0/provider selection2.')
    require(struct.unpack_from('<3Q',raw,592)==(0x4153373348354131,1,856),'Invalid HEVC decoder session extension.')
    owner,authority,client=struct.unpack_from('<3Q',raw,616)
    require(owner and authority and client<16 and owner not in struct.unpack_from('<2Q',raw,456),'Invalid HEVC decoder owner.')
    require(all(any(raw[o:o+32]) for o in (640,672,704,736,768)),
        'Missing HEVC physical or input evidence.')
    first,first_bytes,second,second_bytes=struct.unpack_from('<4Q',raw,800)
    require(first==0 and 0<first_bytes<=262144 and second==first_bytes and
        0<second_bytes<=262144 and second+second_bytes<=524288,
        'HEVC requires two contiguous nonempty bounded input ranges.')
    module,hz,g0,g1,uv,rail=struct.unpack_from('<6I',raw,832)
    require(not module&~0x0700001f and (module>>24)<=1 and 0<hz<=720000000 and
        (g0,g1,uv,rail)==(0x10007,0x10007,800000,1),'Unsupported inherited HEVC decoder clock/rail state.')

def embedded_reference(image,session):
    """Check the same immutable ELF geometry used by preparation and preload."""
    session_check(session)
    _image_parts(image)
    return None

def upload_bytes(pixels,reference,session):
    session_check(session)
    reference=reference or b''
    require(0<len(pixels)<=524288 and not reference,'Decoder requires a bounded HEVC clip and no runtime oracle.')
    require(hashlib.sha256(pixels).digest()==session[768:800],'HEVC input digest differs.')
    require(sum(struct.unpack_from('<Q',session,offset)[0] for offset in (808,824))==len(pixels),
        'HEVC ranges do not cover the exact clip.')
    epoch,generation=struct.unpack_from('<2Q',session,24)
    require(epoch and generation,'HEVC upload requires a live session identity.')
    return struct.pack('<4I2Q',0x55363235,1,len(pixels),2,epoch,generation)+pixels

def request_bytes(address,identity,raw,epoch,generation,lease):
    session_check(raw)
    reservation=struct.unpack_from('<Q',raw,200)[0]
    require(len(identity)==32 and 0x41000000<=address<=0x411fffe0 and
        all(type(x) is int and 0<x<2**64 for x in (epoch,generation,lease,reservation)),
        'Invalid retained decoder identity.')
    digest=hashlib.sha256(raw).digest()
    request=struct.pack('<8Q',REQUEST_MAGIC,2,128,epoch,generation,generation^((1<<64)-1),lease,address)+identity+digest
    contract=struct.pack('<6Q',0,2,288,epoch,generation,generation^((1<<64)-1))+identity
    contract+=struct.pack('<18Q',0,0,0,0,lease,0,0,0,0,0,0,0,1,reservation,0x48000000,0x78000000,0x78000000,0x79000000)
    contract+=digest+raw[80:112]
    require(len(contract)==288,'Extended decoder contract ABI differs.')
    return request,contract+bytes(4096-len(contract))

def build_launcher(out,clang,lld):
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text()
    (out/'base-supervised-launch.c').write_text(text)
    require('REQUEST[1]==3' in text and 'CONTRACT->version==3' in text,'Unexpected supervised launcher version guards.')
    for anchor in ('0x4153373353554c33','REQUEST[1]==3','CONTRACT->version==3',
                   'CONTRACT->reserved[0] && !CONTRACT->reserved[1]','CONTRACT->bytes==sizeof(*CONTRACT)',
                   'CONTRACT->reserved[2+i]','20*A733_SMP_COUNTER_HZ'):
        require(text.count(anchor)==1,'Shared launcher adaptation anchor changed: '+anchor)
    text=text.replace('0x4153373353554c33','0x4153373348354c31').replace('REQUEST[1]==3','REQUEST[1]==2').replace('CONTRACT->version==3','CONTRACT->version==2')
    text=text.replace('CONTRACT->reserved[0] && !CONTRACT->reserved[1]','CONTRACT->reserved[0]==1 && CONTRACT->reserved[1] && CONTRACT->reserved[2]==0x48000000 && CONTRACT->reserved[3]==0x78000000 && CONTRACT->reserved[4]==0x78000000 && CONTRACT->reserved[5]==0x79000000')
    text=text.replace('CONTRACT->bytes==sizeof(*CONTRACT)','CONTRACT->bytes==288').replace('CONTRACT->reserved[2+i]','((volatile uint64_t *)((uintptr_t)CONTRACT+224))[i]')
    text=text.replace('20*A733_SMP_COUNTER_HZ','60*A733_SMP_COUNTER_HZ')
    (out/'hevc-launch-base.c').write_text(text)
    for name,path in [('launch.c',ROOT/'apps/hevc/launch.c'),('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h'),('a733_hevc_application.h',ROOT/'include/a733_hevc_application.h'),('a733_board_application.h',ROOT/'include/a733_board_application.h')]:shutil.copy2(path,out/name)
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-Wall','-Wextra','-Werror','-DSUNSTONE_BOARD_SUPERVISED=1',f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC}ULL','-I'+str(out)]
    commands=[[clang,*flags,'-c',out/name,'-o',out/(name+'.o')] for name in ('launch.c','launch.S')]
    commands += [[lld,'-T',out/'launch.ld',out/'launch.c.o',out/'launch.S.o','-o',out/'launch.elf'],['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',out/'launch.elf',out/'launch.bin']]
    for command in commands:
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (out/'launcher-build.log').open('a') as f:f.write(r.stdout+r.stderr)
        require(r.returncode==0,'HEVC decoder launcher build failed; inspect retained log.')
    base.save_json(out/'launcher-commands.json',[[str(x) for x in c] for c in commands])
    require(0<(out/'launch.bin').stat().st_size<=0xc00,'Launcher exceeds SRAM.')


def memory_spans():
    """Native decoder's complete guarded mappings, not physical allocation addresses."""
    return [[0x10001000,0x40000,'r'],[0x10043000,0x40000,'r'],
        [0x10085000,0x71000,'rw'],[0x100f8000,0x71000,'rw'],
        [0x1016b000,0x4000,'rw'],[0x10171000,0x4000,'rw'],
        [0x10177000,0xc7000,'rw'],[0x10240000,0x1000,'rw']]

def validate_admission(record,image_hash,template_hash,raw,cache=None,frozen=False):
    # Existing DDR/platform/manual-recovery checks stay exact and byte-snapshotted.
    require(record.get('schema')=='a733-hevc-supervised-entry-v1','Wrong HEVC decoder admission schema.')
    common={**record,'schema':'a733-board-supervised-entry-v2'}
    cache=board.validate_admission(common,image_hash,template_hash,cache,frozen)
    for key,offset in [('initial',640),('memory',672),('electrical',704),('supervision',736)]:
        item=record[key];path=Path(item['path']);require(path.is_absolute(),'Evidence path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Missing frozen HEVC decoder evidence.');cache[str(path)]=path.read_bytes()
        data=cache[str(path)];require(hashlib.sha256(data).digest()==raw[offset:offset+32] and hashlib.sha256(data).hexdigest()==item['sha256'],'HEVC decoder evidence differs.')
        fact=json.loads(data)
        require(fact.get('schema')=='a733-hevc-'+key+'-v1' and fact.get('identity')==record['identity'] and fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and fact.get('reviewed') is True,'HEVC decoder authority is not bound to this image/target.')
        required={'initial':['all_masters_excluded','prior_transactions_drained','prior_walks_drained','decoder_inherited_powered_reset_deasserted','skipped_masters_physically_quiescent','remote_secure_writers_excluded','irq80_owned_masked','iommu_irqs_owned_masked'],
            'memory':['private_nc_arenas','no_physical_aliases','iommu_banks_disabled','iommu_clocks_stable','decoder_device_mapping'],
            'electrical':['dcdc2_shared_800mv','ve_operating_point_source_reviewed','stable_ve_pll'],
            'supervision':['whole_board_power_removal','permanent_dma_retention','no_reuse_after_fault','one_attempt']}[key]
        require(all(fact.get(k) is True for k in required),'Required HEVC decoder physical fact absent.')
    item=record['hevc_ddr'];path=Path(item['path']);require(path.is_absolute(),'Decoder DDR evidence path must be absolute.')
    if str(path) not in cache:
        require(not frozen,'Frozen decoder DDR evidence absent.');cache[str(path)]=path.read_bytes()
    data=cache[str(path)];require(hashlib.sha256(data).hexdigest()==item['sha256'],'Decoder DDR evidence changed.')
    fact=json.loads(data)
    require(fact.get('schema')=='a733-ddr-reservation-v1' and fact.get('identity')==record['identity'] and
        fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and
        fact.get('exclusive') is True and fact.get('published_cache_disabled') is True and
        fact.get('remote_writers_excluded') is True and fact.get('ranges')==[[0x48000000,0x79000000]] and fact.get('evidence'),
        'Extended DRAM reservation/exclusion evidence required.')
    for item in fact['evidence']:
        path=Path(item['path']);require(path.is_absolute(),'Decoder DDR support path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Frozen decoder DDR support absent.');cache[str(path)]=path.read_bytes()
        require(hashlib.sha256(cache[str(path)]).hexdigest()==item['sha256'],'Decoder DDR support changed.')
    memory=json.loads(cache[record['memory']['path']])
    expected=memory_spans()
    require(record['reservation']==struct.unpack_from('<Q',raw,200)[0] and
        memory.get('dram_reservation_id')==record['reservation'] and memory.get('iova_spans')==expected and
        memory.get('decoder_masters')==[6,7] and memory.get('iommu_instance')==1 and memory.get('outer_guard_pages_unmapped') is True and
        memory.get('source_extents_reviewed') is True and
        memory.get('wb_range')==[0x48000000,0x78000000] and memory.get('nc_range')==[0x78000000,0x79000000],
        'HEVC decoder memory/guard authority differs.')
    # The native implementation and its allocation/table proof were not available
    # at this handoff. Keep production preparation closed rather than accepting
    # only IOVA labels and broad DRAM reservations as physical mapping proof.
    raise ValueError('HEVC physical allocation/page-table admission is pending native proof.')

def prepare(image,manifest,admission,template,output,clang,lld,pixels,reference=None):
    snapshots={name:Path(path).read_bytes() for name,path in [('image.elf',image),('image-input-manifest.json',manifest),('admission.json',admission),('session-template.bin',template)]}
    snapshots['input.h265']=Path(pixels).read_bytes()
    require(reference is None,'HEVC numerical references remain outside runtime preparation.')
    raw=bytearray(snapshots['session-template.bin']);session_check(raw)
    upload_bytes(snapshots['input.h265'],None,raw)
    embedded_reference(snapshots['image.elf'],raw)
    ih=hashlib.sha256(snapshots['image.elf']).hexdigest();th=hashlib.sha256(raw).hexdigest()
    mf=json.loads(snapshots['image-input-manifest.json'])
    require(mf.get('target')=='sunstone-a733-hevc' and mf.get('execution_profile')=='hevc-supervised-v1' and mf.get('final_elf')==ih,'Tested HEVC decoder image manifest required.')
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
    (out/'upload.bin').write_bytes(upload_bytes(snapshots['input.h265'],None,raw))
    (out/'session.bin').write_bytes(raw);uploads.append({'name':'session.bin','address':0x41f00000})
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,epoch,generation,lease)):(out/name).write_bytes(data)
    for name,count in [('result-zero.bin',16384),('report-zero.bin',128)]:(out/name).write_bytes(bytes(count))
    evidence={}
    for i,(path,data) in enumerate(cache.items()):
        name=f'evidence-{i}.bin';(out/name).write_bytes(data);evidence[path]={'name':name,'sha256':hashlib.sha256(data).hexdigest()}
    p=dict(schema=SCHEMA,physical_access=False,image_sha256=ih,build_id=identity.hex(),epoch=epoch,generation=generation,lease=lease,reservation=record['reservation'],session='session.bin',profile=None,manifest='image-input-manifest.json',image='image.elf',segments=uploads,entry_evidence=evidence,backup_sizes={name:n for name,a,n in base.REGIONS},files={x.name:base.digest(x) for x in out.iterdir() if x.is_file()})
    base.save_json(out/'prepared.json',p);return validate_bundle(out)

def validate_bundle(bundle):
    out=Path(bundle);p=json.loads((out/'prepared.json').read_text());require(p.get('schema')==SCHEMA and p.get('physical_access') is False,'Wrong HEVC decoder bundle.')
    required={'image.elf','image-input-manifest.json','admission.json','session-template.bin','session.bin','request.bin','contract.bin','launch.bin','result-zero.bin','report-zero.bin','input.h265','upload.bin'}
    require(required<=set(p['files']) and all(Path(item['name']).name==item['name'] and item['name'] in p['files'] for item in p['entry_evidence'].values()),'Unsealed HEVC decoder authority or transport input.')
    for name,h in p['files'].items():require(Path(name).name==name and base.digest(out/name)==h,'HEVC decoder bundle input changed.')
    require(base.digest(out/'image.elf')==p['image_sha256'],'HEVC decoder image changed.')
    manifest=json.loads((out/'image-input-manifest.json').read_text())
    require(manifest.get('target')=='sunstone-a733-hevc' and manifest.get('execution_profile')=='hevc-supervised-v1' and manifest.get('final_elf')==p['image_sha256'],'HEVC decoder image manifest differs.')
    sizes={name:(out/name).stat().st_size for name in p['files']}
    require(p.get('backup_sizes')=={name:n for name,a,n in base.REGIONS} and 0<sizes.get('launch.bin',0)<=0xc00,'HEVC decoder protected SRAM geometry changed.')
    require((out/'result-zero.bin').read_bytes()==bytes(16384) and (out/'report-zero.bin').read_bytes()==bytes(128),'HEVC decoder initial report backing changed.')
    raw=(out/'session.bin').read_bytes();session_check(raw)
    require((out/'upload.bin').read_bytes()==upload_bytes((out/'input.h265').read_bytes(),None,raw),'HEVC decoder upload bytes differ.')
    template=(out/'session-template.bin').read_bytes();cache={path:(out/item['name']).read_bytes() for path,item in p['entry_evidence'].items()}
    validate_admission(json.loads((out/'admission.json').read_text()),p['image_sha256'],hashlib.sha256(template).hexdigest(),template,cache,True)
    segments,address,identity=image_parts(out/'image.elf');require(identity.hex()==p['build_id'],'HEVC decoder build identity changed.')
    expected_raw=bytearray(template);struct.pack_into('<3Q',expected_raw,24,p['epoch'],p['generation'],p['lease']);expected_raw[48:80]=identity
    require(raw==expected_raw,'HEVC decoder session differs from admitted derivation.')
    expected=[]
    for i,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{i}.bin';require((out/name).read_bytes()==segment['data'],'HEVC decoder ELF bytes changed.');expected.append({'name':name,'address':segment['address']})
    expected.append({'name':'session.bin','address':0x41f00000});require(p['segments']==expected and all(item['name'] in sizes for item in expected),'HEVC decoder upload spans changed or unsealed.')
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,p['epoch'],p['generation'],p['lease'])):require((out/name).read_bytes()==data,'HEVC decoder launch contract changed.')
    embedded_reference((out/'image.elf').read_bytes(),raw)
    p['_reference']=None
    p['_upload']=(out/'upload.bin').read_bytes()
    p['_sizes']={name:(out/name).stat().st_size for name in p['files']};return p


def image_parts(path,high=False):
    return _image_parts(Path(path).read_bytes())

def _image_parts(data):
    import io
    elf=ELFFile(io.BytesIO(data));require(elf['e_machine']=='EM_AARCH64' and elf['e_entry']==0x41000000,'Native extended-memory decoder ELF required.')
    table=elf.get_section_by_name('.symtab');require(table is not None,'Native HEVC symbol table required.')
    entries=list(table.iter_symbols());symbols={s.name:(s['st_value'],s['st_size']) for s in entries}
    parts=[]
    for s in elf.iter_segments():
        if s['p_type']!='PT_LOAD':continue
        a,n=s['p_paddr'],s['p_memsz'];data=s.data()
        require(a==s['p_vaddr'] and n>0 and len(data)<=n and not (s['p_flags']&3==3) and
            all(a>=p['address']+p['memory_bytes'] or p['address']>=a+n for p in parts),'Invalid full-HD ELF allocations.')
        require((0x41000000<=a<a+n<=0x42200000 and (not data or a+len(data)<=0x42000000)) or
            (not data and (a,a+n) in [(0x48000000,0x78000000),(0x78000000,0x79000000)]),'Extended decoder ELF span outside reservations.')
        require(not s['p_flags']&1 or a+n<=0x41200000,'Extended decoder code outside image prefix.')
        parts.append({'address':a,'memory_bytes':n,'data':data,'flags':int(s['p_flags'])})
    for name,value in {'__device_pool_start':0x48000000,'__device_pool_end':0x78000000,'__uncached_pool_start':0x78000000,'__uncached_pool_end':0x79000000,'__a733_smp_contract':0x42004000,'__a733_smp_contract_end':0x42005000}.items():
        require(symbols.get(name,(0,))[0]==value,'Extended decoder linker ownership differs.')
    require(any(p['flags']&1 and p['address']<=0x41000000<p['address']+len(p['data']) for p in parts),
        'HEVC entry lacks executable file-backed bytes.')
    require('__bss_end' in symbols and symbols['__bss_end'][0]<=0x41f00000,'Extended decoder session overlaps BSS.')
    def immutable(name,size):
        require(sum(s.name==name for s in entries)==1,'HEVC immutable marker is not unique: '+name)
        address,n=symbols.get(name,(0,0));require(n==size,'Extended decoder immutable marker absent: '+name)
        match=[p for p in parts if not p['flags']&2 and p['address']<=address and address+size<=p['address']+len(p['data'])]
        require(len(match)==1,'Extended decoder marker is not immutable/file-backed.')
        p=match[0];return address,p['data'][address-p['address']:address-p['address']+size]
    _,geometry=immutable('hevc_geometry',72)
    require(struct.unpack('<9Q',geometry)==(0x4153373348354131,1,856,288,0x41f00000,60,524288,460800,262144),'HEVC decode compiled geometry differs.')
    address,identity=immutable('a733_smp_build_id',32)
    for p in parts:p.pop('flags')
    return parts,address,identity

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','admission','template','output','clang','lld','input'):parser.add_argument('--'+name,type=Path,required=True)
    a=parser.parse_args();prepare(a.image,a.manifest,a.admission,a.template,a.output,a.clang,a.lld,a.input)
