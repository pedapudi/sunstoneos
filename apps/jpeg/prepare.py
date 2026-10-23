#!/usr/bin/env python3
"""Offline preparation of one supervised caller-supplied JPEG application session."""
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
board=module('jpeg_board_prepare',ROOT/'apps/board/prepare.py')
fixtures=module('jpeg_fixture_profiles',ROOT/'apps/jpeg/fixture.py')
observe=module('jpeg_result_decoder',ROOT/'apps/jpeg/observe.py')
INPUT_CAPACITY,OUTPUT_CAPACITY=observe.INPUT_CAPACITY,observe.MAXIMUM
base=board.base
require=base.require
SCHEMA='a733-jpeg-supervised-bundle-v2'
CONTRACT_MAGIC=0x415337334a504332
REQUEST_MAGIC=0x415337334a504c32

def session_check(raw):
    require(len(raw)==856,'JPEG session must be856bytes.')
    require(board.session_check(raw[:592])==2,'JPEG requires USB0/provider selection2.')
    version=struct.unpack_from('<3Q',raw,592)
    require(version in ((0x415337334a504132,2,856),(0x415337334a504132,3,856)),'Invalid JPEG session extension.')
    require(version[1]==2 or not any(raw[800:832]),'A JPEG sequence session carries oracles per image.')
    owner,authority,client=struct.unpack_from('<3Q',raw,616)
    require(owner and authority and client<16 and owner not in struct.unpack_from('<2Q',raw,456),'Invalid JPEG owner.')
    require(all(any(raw[o:o+32]) for o in (640,672,704,736,768)),'Missing JPEG physical or fixture evidence.')
    module,hz,g0,g1,uv,rail=struct.unpack_from('<6I',raw,832)
    require(not module&~0x0700001f and (module>>24)<=1 and 0<hz<=720000000 and
        (g0,g1,uv,rail)==(0x10007,0x10007,800000,1),'Unsupported inherited JPEG clock/rail state.')

def embedded_reference(image,session):
    """Bind immutable runtime capacities; runtime input is uploaded separately."""
    import io
    from elftools.elf.elffile import ELFFile
    session_check(session)
    elf=ELFFile(io.BytesIO(image));table=elf.get_section_by_name('.symtab')
    require(table is not None,'JPEG image has no runtime geometry.')
    found=[s for s in table.iter_symbols() if s.name=='jpeg_runtime_geometry']
    require(len(found)==1,'Missing unique immutable JPEG runtime geometry.')
    first,size=int(found[0]['st_value']),int(found[0]['st_size'])
    expected=struct.pack('<6Q',0x415337334a504132,2,856,INPUT_CAPACITY,OUTPUT_CAPACITY,144)
    require(size==len(expected),'JPEG geometry size differs.')
    parts=[p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and not int(p['p_flags'])&2 and
           int(p['p_vaddr'])<=first and first+size<=int(p['p_vaddr'])+int(p['p_filesz'])]
    require(len(parts)==1,'JPEG runtime geometry must be immutable/file-backed.')
    offset=first-int(parts[0]['p_vaddr']);require(parts[0].data()[offset:offset+size]==expected,'JPEG runtime capacities differ.')
    return None

def upload_bytes(jpeg,reference,session):
    session_check(session)
    require(struct.unpack_from('<Q',session,600)[0]==2,'Single JPEG upload requires a version 2 session.')
    reference=reference or b''
    require(0<len(jpeg)<=INPUT_CAPACITY and len(reference)<=OUTPUT_CAPACITY,'JPEG upload exceeds admitted capacities.')
    require(hashlib.sha256(jpeg).digest()==session[768:800],'JPEG input digest differs.')
    require((hashlib.sha256(reference).digest() if reference else bytes(32))==session[800:832],
            'JPEG oracle mode/digest differs.')
    epoch,generation=struct.unpack_from('<2Q',session,24)
    require(epoch and generation,'JPEG upload requires a live session identity.')
    return struct.pack('<4I2Q',0x5547504a,1,len(jpeg),len(reference),epoch,generation)+jpeg+reference

def jpeg_size(data):
    """Width and height from the baseline SOF0 segment."""
    pos=2
    while pos+4<=len(data) and data[pos]==0xff:
        marker=data[pos+1];length=int.from_bytes(data[pos+2:pos+4],'big')
        if marker==0xc0:height,width=struct.unpack_from('>HH',data,pos+5);return width,height
        pos+=2+length
    raise ValueError('JPEG has no baseline frame header.')

def sequence_manifest(images,references):
    """Manifest of a version 3 session: per image its lengths, size and digests."""
    require(0<len(images)<=8 and len(references)==len(images),'A JPEG sequence holds one to eight images.')
    out=struct.pack('<4I',0x5347504a,1,len(images),0)
    for jpeg,reference in zip(images,references):
        reference=reference or b'';width,height=jpeg_size(jpeg)
        count=observe.geometry(width,height,(width+31)&~31,(height+31)&~31)
        require(0<len(jpeg)<=INPUT_CAPACITY and len(reference) in (0,count),'JPEG sequence entry exceeds admitted capacities.')
        out+=struct.pack('<4I',len(jpeg),len(reference),width,height)+hashlib.sha256(jpeg).digest()+(hashlib.sha256(reference).digest() if reference else bytes(32))
    return out

def sequence_upload(session,manifest):
    session_check(session)
    require(struct.unpack_from('<Q',session,600)[0]==3 and hashlib.sha256(manifest).digest()==session[768:800],'JPEG sequence manifest/session differs.')
    epoch,generation=struct.unpack_from('<2Q',session,24)
    return struct.pack('<4I2Q',0x5547504a,2,len(manifest),0,epoch,generation)+manifest

def image_upload(session,manifest,index,jpeg,reference=None):
    """Frame for image index; send it only after the previous result arrived."""
    reference=reference or b'';count=struct.unpack_from('<I',manifest,8)[0]
    require(0<=index<count,'JPEG sequence index exceeds manifest.')
    entry=manifest[16+80*index:96+80*index]
    require(struct.unpack_from('<2I',entry)==(len(jpeg),len(reference)) and entry[16:48]==hashlib.sha256(jpeg).digest() and
        entry[48:80]==(hashlib.sha256(reference).digest() if reference else bytes(32)),'JPEG image differs from its manifest entry.')
    epoch,generation=struct.unpack_from('<2Q',session,24)
    return struct.pack('<4I2Q',0x4947504a,index,len(jpeg),len(reference),epoch,generation)+jpeg+reference

def request_bytes(address,identity,raw,epoch,generation,lease):
    request,contract=board.request_bytes(address,identity,epoch,generation,lease,raw)
    request=struct.pack('<Q',REQUEST_MAGIC)+request[8:]
    contract=bytearray(contract);struct.pack_into('<Q',contract,184,856)
    return request,bytes(contract)

def build_launcher(out,clang,lld):
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text()
    (out/'base-supervised-launch.c').write_text(text)
    text=board.adapt_launcher(text,{'0x4153373353554c33':'0x415337334a504c32','REQUEST[1]==3':'REQUEST[1]==2','CONTRACT->version==3':'CONTRACT->version==2',
        'CONTRACT->reserved[0] && !CONTRACT->reserved[1]':'CONTRACT->reserved[0]==A733_BOARD_SESSION_ADDRESS && CONTRACT->reserved[1]==sizeof(struct a733_jpeg_session)',
        '20*A733_SMP_COUNTER_HZ':'60*A733_SMP_COUNTER_HZ'})
    (out/'jpeg-launch-base.c').write_text(text)
    for name,path in [('launch.c',ROOT/'apps/jpeg/launch.c'),('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h'),('a733_jpeg_application.h',ROOT/'include/a733_jpeg_application.h'),('a733_board_application.h',ROOT/'include/a733_board_application.h')]:shutil.copy2(path,out/name)
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-Wall','-Wextra','-Werror','-DSUNSTONE_BOARD_SUPERVISED=1',f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC}ULL','-I'+str(out)]
    commands=[[clang,*flags,'-c',out/name,'-o',out/(name+'.o')] for name in ('launch.c','launch.S')]
    commands += [[lld,'-T',out/'launch.ld',out/'launch.c.o',out/'launch.S.o','-o',out/'launch.elf'],['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',out/'launch.elf',out/'launch.bin']]
    for command in commands:
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (out/'launcher-build.log').open('a') as f:f.write(r.stdout+r.stderr)
        require(r.returncode==0,'JPEG launcher build failed; inspect retained log.')
    base.save_json(out/'launcher-commands.json',[[str(x) for x in c] for c in commands])
    require(0<(out/'launch.bin').stat().st_size<=0xc00,'Launcher exceeds SRAM.')

def validate_admission(record,image_hash,template_hash,raw,cache=None,frozen=False):
    # Existing DDR/platform/manual-recovery checks stay exact and byte-snapshotted.
    require(record.get('schema')=='a733-jpeg-supervised-entry-v1','Wrong JPEG admission schema.')
    common={**record,'schema':'a733-board-supervised-entry-v2'}
    cache=board.validate_admission(common,image_hash,template_hash,cache,frozen)
    for key,offset in [('initial',640),('memory',672),('electrical',704),('supervision',736)]:
        item=record[key];path=Path(item['path']);require(path.is_absolute(),'Evidence path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Missing frozen JPEG evidence.');cache[str(path)]=path.read_bytes()
        data=cache[str(path)];require(hashlib.sha256(data).digest()==raw[offset:offset+32] and hashlib.sha256(data).hexdigest()==item['sha256'],'JPEG evidence differs.')
        fact=json.loads(data)
        require(fact.get('schema')=='a733-jpeg-'+key+'-v1' and fact.get('identity')==record['identity'] and fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and fact.get('reviewed') is True,'JPEG authority is not bound to this image/target.')
        required={'initial':['all_masters_excluded','prior_transactions_drained','prior_walks_drained','decoder_off_reset_held','skipped_masters_physically_quiescent','remote_secure_writers_excluded','irq81_owned_masked','iommu_irqs_owned_masked'],
            'memory':['private_nc_arenas','no_physical_aliases','iommu_banks_disabled','iommu_clocks_stable','immutable_rom_mapping'],
            'electrical':['dcdc2_shared_800mv','ve_operating_point_qualified','stable_ve_pll'],
            # A sequence refills its retained buffers after each healthy image.
            'supervision':['whole_board_power_removal','permanent_dma_retention',
                'no_reuse_after_fault' if struct.unpack_from('<Q',raw,600)[0]==3 else 'no_reuse','one_attempt']}[key]
        require(all(fact.get(k) is True for k in required),'Required JPEG physical fact absent.')
    return cache

def prepare(image,manifest,admission,template,output,clang,lld,jpeg,reference=None):
    snapshots={name:Path(path).read_bytes() for name,path in [('image.elf',image),('image-input-manifest.json',manifest),('admission.json',admission),('session-template.bin',template)]}
    snapshots['input.jpg']=Path(jpeg).read_bytes()
    snapshots['reference.yu12']=Path(reference).read_bytes() if reference else b''
    raw=bytearray(snapshots['session-template.bin']);session_check(raw)
    upload_bytes(snapshots['input.jpg'],snapshots['reference.yu12'],raw)
    embedded_reference(snapshots['image.elf'],raw)
    ih=hashlib.sha256(snapshots['image.elf']).hexdigest();th=hashlib.sha256(raw).hexdigest()
    mf=json.loads(snapshots['image-input-manifest.json'])
    require(mf.get('target')=='sunstone-a733-jpeg' and mf.get('execution_profile')=='jpeg-runtime-supervised-v2' and mf.get('final_elf')==ih,'Tested JPEG image manifest required.')
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
    (out/'upload.bin').write_bytes(upload_bytes(snapshots['input.jpg'],snapshots['reference.yu12'],raw))
    (out/'session.bin').write_bytes(raw);uploads.append({'name':'session.bin','address':0x41f00000})
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,epoch,generation,lease)):(out/name).write_bytes(data)
    for name,count in [('result-zero.bin',16384),('report-zero.bin',128)]:(out/name).write_bytes(bytes(count))
    evidence={}
    for i,(path,data) in enumerate(cache.items()):
        name=f'evidence-{i}.bin';(out/name).write_bytes(data);evidence[path]={'name':name,'sha256':hashlib.sha256(data).hexdigest()}
    p=dict(schema=SCHEMA,physical_access=False,image_sha256=ih,build_id=identity.hex(),epoch=epoch,generation=generation,lease=lease,reservation=record['reservation'],session='session.bin',profile=None,manifest='image-input-manifest.json',image='image.elf',segments=uploads,entry_evidence=evidence,backup_sizes={name:n for name,a,n in base.REGIONS},files={x.name:base.digest(x) for x in out.iterdir() if x.is_file()})
    base.save_json(out/'prepared.json',p);return validate_bundle(out)

def validate_bundle(bundle):
    out=Path(bundle);p=json.loads((out/'prepared.json').read_text());require(p.get('schema')==SCHEMA and p.get('physical_access') is False,'Wrong JPEG bundle.')
    required={'image.elf','image-input-manifest.json','admission.json','session-template.bin','session.bin','request.bin','contract.bin','launch.bin','result-zero.bin','report-zero.bin','input.jpg','reference.yu12','upload.bin'}
    require(required<=set(p['files']) and all(Path(item['name']).name==item['name'] and item['name'] in p['files'] for item in p['entry_evidence'].values()),'Unsealed JPEG authority or transport input.')
    for name,h in p['files'].items():require(Path(name).name==name and base.digest(out/name)==h,'JPEG bundle input changed.')
    require(base.digest(out/'image.elf')==p['image_sha256'],'JPEG image changed.')
    manifest=json.loads((out/'image-input-manifest.json').read_text())
    require(manifest.get('target')=='sunstone-a733-jpeg' and manifest.get('execution_profile')=='jpeg-runtime-supervised-v2' and manifest.get('final_elf')==p['image_sha256'],'JPEG image manifest differs.')
    sizes={name:(out/name).stat().st_size for name in p['files']}
    require(p.get('backup_sizes')=={name:n for name,a,n in base.REGIONS} and 0<sizes.get('launch.bin',0)<=0xc00,'JPEG protected SRAM geometry changed.')
    require((out/'result-zero.bin').read_bytes()==bytes(16384) and (out/'report-zero.bin').read_bytes()==bytes(128),'JPEG initial report backing changed.')
    raw=(out/'session.bin').read_bytes();session_check(raw)
    require((out/'upload.bin').read_bytes()==upload_bytes((out/'input.jpg').read_bytes(),(out/'reference.yu12').read_bytes(),raw),'JPEG upload bytes differ.')
    template=(out/'session-template.bin').read_bytes();cache={path:(out/item['name']).read_bytes() for path,item in p['entry_evidence'].items()}
    validate_admission(json.loads((out/'admission.json').read_text()),p['image_sha256'],hashlib.sha256(template).hexdigest(),template,cache,True)
    segments,address,identity=base.image_parts(out/'image.elf');require(identity.hex()==p['build_id'],'JPEG build identity changed.')
    expected_raw=bytearray(template);struct.pack_into('<3Q',expected_raw,24,p['epoch'],p['generation'],p['lease']);expected_raw[48:80]=identity
    require(raw==expected_raw,'JPEG session differs from admitted derivation.')
    expected=[]
    for i,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{i}.bin';require((out/name).read_bytes()==segment['data'],'JPEG ELF bytes changed.');expected.append({'name':name,'address':segment['address']})
    expected.append({'name':'session.bin','address':0x41f00000});require(p['segments']==expected and all(item['name'] in sizes for item in expected),'JPEG upload spans changed or unsealed.')
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,p['epoch'],p['generation'],p['lease'])):require((out/name).read_bytes()==data,'JPEG launch contract changed.')
    embedded_reference((out/'image.elf').read_bytes(),raw)
    p['_reference']=(out/'reference.yu12').read_bytes() or None
    p['_upload']=(out/'upload.bin').read_bytes()
    p['_sizes']={name:(out/name).stat().st_size for name in p['files']};return p

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','admission','template','output','clang','lld','jpeg'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--reference',type=Path)
    a=parser.parse_args();prepare(a.image,a.manifest,a.admission,a.template,a.output,a.clang,a.lld,a.jpeg,a.reference)
