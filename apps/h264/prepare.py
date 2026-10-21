#!/usr/bin/env python3
"""Offline preparation of one supervised bounded H.264 application session."""
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
board=module('h264_board_prepare',ROOT/'apps/board/prepare.py')
base=board.base
require=base.require
SCHEMA='a733-h264-supervised-bundle-v3'
VGA_SCHEMA='a733-h264-supervised-bundle-v4'
FULLHD_SCHEMA='a733-h264-supervised-bundle-v5'
CONTRACT_MAGIC=0x4153373348324331
REQUEST_MAGIC=0x4153373348324c31

def session_check(raw):
    require(len(raw)==856,'H.264 session must be856bytes.')
    require(board.session_check(raw[:592])==2,'H.264 requires USB0/provider selection2.')
    magic,version,size=struct.unpack_from('<3Q',raw,592)
    require((magic,size)==(0x4153373348324131,856) and version in (1,2,3,4) and
        (version==1 or not any(raw[800:832])),'Invalid H.264 session extension.')
    owner,authority,client=struct.unpack_from('<3Q',raw,616)
    require(owner and authority and client<16 and owner not in struct.unpack_from('<2Q',raw,456),'Invalid H.264 owner.')
    require(all(any(raw[o:o+32]) for o in (640,672,704,736,768)),'Missing H.264 physical or fixture evidence.')
    module,hz,g0,g1,uv,rail=struct.unpack_from('<6I',raw,832)
    require(not module&~0x0700001f and (module>>24)<=1 and 0<hz<=720000000 and
        (g0,g1,uv,rail)==(0x10007,0x10007,800000,1),'Unsupported inherited H.264 clock/rail state.')

def upload_bytes(session,stream,reference=None):
    """Produce one bounded upload from immutable per-session inputs."""
    session_check(session)
    if struct.unpack_from('<Q',session,600)[0] in (2,3,4):
        require(reference is None,'VGA oracle belongs to individual manifest descriptors.')
        return stream_module().manifest_upload(session,stream)
    require(0<len(stream)<=8192 and hashlib.sha256(stream).digest()==session[768:800],
            'AnnexB upload does not match the sealed session digest/limit.')
    oracle=any(session[800:832])
    require(oracle==(reference is not None),'Qualification oracle mode differs.')
    if oracle:
        require(len(reference) in (2304,4608) and hashlib.sha256(reference).digest()==session[800:832],
                'Independent pixel oracle differs from the sealed session.')
    return struct.pack('<4I2Q',0x55363248,2,len(stream),len(reference) if oracle else 0,
        *struct.unpack_from('<2Q',session,24))+stream+(reference or b'')

def stream_module():
    spec=importlib.util.spec_from_file_location('h264_vga_stream',ROOT/'apps/h264/stream.py')
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def fullhd_module():return module('h264_fullhd_prepare',ROOT/'apps/h264/fullhd.py')
def image_parts(path,version):return fullhd_module().image_parts(path,high=version==4) if version>=3 else base.image_parts(path)
def bundle_schema(version):return 'a733-h264-supervised-bundle-v6' if version==4 else FULLHD_SCHEMA if version==3 else VGA_SCHEMA if version==2 else SCHEMA
def profile_name(version):return stream_module().profile(version)['name'] if version in (2,3,4) else None
def execution_profile(version):return 'h264-high-supervised-v1' if version==4 else 'h264-fullhd-supervised-v1' if version==3 else 'h264-supervised-v1'

def request_bytes(address,identity,raw,epoch,generation,lease):
    if struct.unpack_from('<Q',raw,600)[0]>=3:return fullhd_module().request_bytes(address,identity,raw,epoch,generation,lease)
    request,contract=board.request_bytes(address,identity,epoch,generation,lease,raw)
    request=struct.pack('<Q',REQUEST_MAGIC)+request[8:]
    contract=bytearray(contract);struct.pack_into('<Q',contract,184,856)
    return request,bytes(contract)

def build_launcher(out,clang,lld,fullhd=False,high=False):
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text()
    (out/'base-supervised-launch.c').write_text(text)
    text=board.adapt_launcher(text,{'0x4153373353554c33':'0x4153373348324c31','REQUEST[1]==3':'REQUEST[1]==2','CONTRACT->version==3':'CONTRACT->version==2',
        'CONTRACT->reserved[0] && !CONTRACT->reserved[1]':'CONTRACT->reserved[0]==A733_BOARD_SESSION_ADDRESS && CONTRACT->reserved[1]==sizeof(struct a733_h264_session)',
        '20*A733_SMP_COUNTER_HZ':'60*A733_SMP_COUNTER_HZ'})
    if fullhd:
        text=board.adapt_launcher(text,{'CONTRACT->reserved[0]==A733_BOARD_SESSION_ADDRESS && CONTRACT->reserved[1]==sizeof(struct a733_h264_session)':'CONTRACT->reserved[0]==1 && CONTRACT->reserved[1] && CONTRACT->reserved[2]==0x48000000 && CONTRACT->reserved[3]==0x78000000 && CONTRACT->reserved[4]==0x78000000 && CONTRACT->reserved[5]==0x79000000',
            'CONTRACT->bytes==sizeof(*CONTRACT)':'CONTRACT->bytes==288','CONTRACT->reserved[2+i]':'((volatile uint64_t *)((uintptr_t)CONTRACT+224))[i]'})
    (out/'h264-launch-base.c').write_text(text)
    for name,path in [('launch.c',ROOT/('apps/h264/fullhd-launch.c' if fullhd else 'apps/h264/launch.c')),('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h'),('a733_h264_application.h',ROOT/'include/a733_h264_application.h'),('a733_board_application.h',ROOT/'include/a733_board_application.h')]:shutil.copy2(path,out/name)
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-Wall','-Wextra','-Werror','-DSUNSTONE_BOARD_SUPERVISED=1',f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC}ULL','-I'+str(out)]
    if high:flags+=['-DSUNSTONE_H264_HIGH=1']
    commands=[[clang,*flags,'-c',out/name,'-o',out/(name+'.o')] for name in ('launch.c','launch.S')]
    commands += [[lld,'-T',out/'launch.ld',out/'launch.c.o',out/'launch.S.o','-o',out/'launch.elf'],['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',out/'launch.elf',out/'launch.bin']]
    base.save_json(out/'launcher-commands.json',[[str(x) for x in c] for c in commands])
    for command in commands:
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (out/'launcher-build.log').open('a') as f:f.write(r.stdout+r.stderr)
        require(r.returncode==0,'H.264 launcher build failed; inspect retained log.')
    require(0<(out/'launch.bin').stat().st_size<=0xc00,'Launcher exceeds SRAM.')

def validate_admission(record,image_hash,template_hash,raw,cache=None,frozen=False):
    # Existing DDR/platform/manual-recovery checks stay exact and byte-snapshotted.
    require(record.get('schema')==('a733-h264-supervised-entry-v2' if struct.unpack_from('<Q',raw,600)[0]>=3 else 'a733-h264-supervised-entry-v1'),'Wrong H.264 admission schema.')
    common={**record,'schema':'a733-board-supervised-entry-v2'}
    cache=board.validate_admission(common,image_hash,template_hash,cache,frozen)
    for key,offset in [('initial',640),('memory',672),('electrical',704),('supervision',736)]:
        item=record[key];path=Path(item['path']);require(path.is_absolute(),'Evidence path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Missing frozen H.264 evidence.');cache[str(path)]=path.read_bytes()
        data=cache[str(path)];require(hashlib.sha256(data).digest()==raw[offset:offset+32] and hashlib.sha256(data).hexdigest()==item['sha256'],'H.264 evidence differs.')
        fact=json.loads(data)
        require(fact.get('schema')=='a733-h264-'+key+'-v1' and fact.get('identity')==record['identity'] and fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and fact.get('reviewed') is True,'H.264 authority is not bound to this image/target.')
        required={'initial':['all_masters_excluded','prior_transactions_drained','prior_walks_drained','decoder_off_reset_held','skipped_masters_physically_quiescent','remote_secure_writers_excluded','irq81_owned_masked','iommu_irqs_owned_masked'],
            'memory':['private_nc_arenas','no_physical_aliases','iommu_banks_disabled','iommu_clocks_stable','immutable_rom_mapping'],
            'electrical':['dcdc2_shared_800mv','ve_operating_point_qualified','stable_ve_pll'],
            'supervision':['whole_board_power_removal','permanent_dma_retention',
                'no_reuse_after_fault' if struct.unpack_from('<Q',raw,600)[0] in (2,3,4) else 'no_reuse','one_attempt']}[key]
        require(all(fact.get(k) is True for k in required),'Required H.264 physical fact absent.')
        if key=='memory':
            expected_spans=([[0x10000000,524288,'r'],[0x10100000,614400,'rw'],
                [0x10200000,311296,'rw'],[0x10300000,0x22000,'rw'],[0x10400000,81920,'rw'],[0x10500000,0xc000,'rw']]
                if struct.unpack_from('<Q',raw,600)[0]==2 else [[0x10001000,8192,'r'],[0x10010000,8192,'rw'],
                [0x10014000,8192,'rw'],[0x10020000,0x21000,'rw'],[0x10068000,8192,'rw'],[0x10070000,0xc000,'rw']])
            if struct.unpack_from('<Q',raw,600)[0]==4:
                expected_spans=[[0x10000000,2097152,'r'],[0x10201000,8355840,'rw'],[0x109fa000,4177920,'rw'],[0x10df7000,217088,'rw'],[0x10e30000,1048576,'rw'],[0x10f34000,49152,'rw']]
            elif struct.unpack_from('<Q',raw,600)[0]==3:
                expected_spans=[[0x10000000,0x400000,'r'],[0x10401000,0x3fc000,'rw'],[0x10801000,0x1fe000,'rw'],[0x10a00000,0x35000,'rw'],[0x10a36000,0x80000,'rw'],[0x10abc000,0xc000,'rw']]
            if struct.unpack_from('<Q',raw,600)[0]>=3:
                require(record['reservation']==struct.unpack_from('<Q',raw,200)[0] and
                    fact.get('dram_reservation_id')==record['reservation'] and
                    fact.get('wb_range')==[0x48000000,0x78000000] and fact.get('nc_range')==[0x78000000,0x79000000],
                    'Full-HD memory reservation differs from boot authority.')
            require(fact.get('iova_spans')==expected_spans and fact.get('decoder_masters')==[6,7] and
                fact.get('guard_pages_unmapped') is True and fact.get('source_extents_reviewed') is True,
                'H.264 six-span, two-master, guarded memory authority differs.')
    if struct.unpack_from('<Q',raw,600)[0]>=3:
        item=record['h264_ddr'];path=Path(item['path']);require(path.is_absolute(),'Full-HD DDR evidence path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Frozen full-HD DDR evidence absent.');cache[str(path)]=path.read_bytes()
        data=cache[str(path)];require(hashlib.sha256(data).hexdigest()==item['sha256'],'Full-HD DDR evidence changed.')
        fact=json.loads(data)
        require(fact.get('schema')=='a733-ddr-reservation-v1' and fact.get('identity')==record['identity'] and
            fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and
            fact.get('exclusive') is True and fact.get('published_cache_disabled') is True and
            fact.get('remote_writers_excluded') is True and fact.get('ranges')==[[0x48000000,0x79000000]] and fact.get('evidence'),
            'Full-HD external DRAM reservation/exclusion evidence required.')
        for item in fact['evidence']:
            path=Path(item['path']);require(path.is_absolute(),'Full-HD DDR support path must be absolute.')
            if str(path) not in cache:
                require(not frozen,'Frozen full-HD DDR support absent.');cache[str(path)]=path.read_bytes()
            require(hashlib.sha256(cache[str(path)]).hexdigest()==item['sha256'],'Full-HD DDR support changed.')
    return cache

def prepare(image,manifest,admission,template,output,clang,lld,stream,reference=None):
    snapshots={name:Path(path).read_bytes() for name,path in [('image.elf',image),('image-input-manifest.json',manifest),('admission.json',admission),('session-template.bin',template)]}
    raw=bytearray(snapshots['session-template.bin']);session_check(raw);version=struct.unpack_from('<Q',raw,600)[0]
    stream_path=Path(stream);is_vga=struct.unpack_from('<Q',raw,600)[0] in (2,3,4)
    input_name='manifest.bin' if is_vga else 'input.h264'
    snapshots[input_name]=stream_path.read_bytes()
    if reference is not None:snapshots['oracle.yuv']=Path(reference).read_bytes()
    upload_bytes(raw,snapshots[input_name],snapshots.get('oracle.yuv'))
    if is_vga:
        frames=stream_module().validate_inputs(stream_path.parent,snapshots[input_name],version)
        for i,(data,oracle) in enumerate(frames):
            snapshots[f'frame-{i:03}.h264']=data
            if oracle is not None:snapshots[f'frame-{i:03}.yu12']=oracle
    ih=hashlib.sha256(snapshots['image.elf']).hexdigest();th=hashlib.sha256(raw).hexdigest()
    mf=json.loads(snapshots['image-input-manifest.json'])
    require(mf.get('target')=='sunstone-a733-h264' and mf.get('execution_profile')==execution_profile(version) and mf.get('final_elf')==ih,'Tested H.264 image manifest required.')
    record=json.loads(snapshots['admission.json']);cache=validate_admission(record,ih,th,raw)
    epoch,generation,lease=(secrets.randbits(64) or 1 for _ in range(3))
    struct.pack_into('<3Q',raw,24,epoch,generation,lease)
    out=Path(output);out.mkdir(parents=True,exist_ok=False)
    for name,data in snapshots.items():(out/name).write_bytes(data)
    segments,address,identity=image_parts(out/'image.elf',version)
    # Runtime authenticates linked build identity, not the ELF file hash.
    raw[48:80]=identity
    build_launcher(out,clang,lld,fullhd=True,high=version==4) if version>=3 else build_launcher(out,clang,lld)
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
    p=dict(schema=bundle_schema(version),physical_access=False,image_sha256=ih,build_id=identity.hex(),epoch=epoch,generation=generation,lease=lease,reservation=record['reservation'],session='session.bin',profile=profile_name(version),manifest='image-input-manifest.json',image='image.elf',segments=uploads,entry_evidence=evidence,backup_sizes={name:n for name,a,n in base.REGIONS},files={x.name:base.digest(x) for x in out.iterdir() if x.is_file()})
    base.save_json(out/'prepared.json',p);return validate_bundle(out)

def validate_bundle(bundle):
    out=Path(bundle);p=json.loads((out/'prepared.json').read_text());require(p.get('schema') in (SCHEMA,VGA_SCHEMA,FULLHD_SCHEMA,'a733-h264-supervised-bundle-v6') and p.get('physical_access') is False,'Wrong H.264 bundle.')
    required={'image.elf','image-input-manifest.json','admission.json','session-template.bin','session.bin','request.bin','contract.bin','launch.bin','result-zero.bin','report-zero.bin'}
    require(required<=set(p['files']) and all(Path(item['name']).name==item['name'] and item['name'] in p['files'] for item in p['entry_evidence'].values()),'Unsealed H.264 authority or transport input.')
    for name,h in p['files'].items():require(Path(name).name==name and base.digest(out/name)==h,'H.264 bundle input changed.')
    require(base.digest(out/'image.elf')==p['image_sha256'],'H.264 image changed.')
    raw=(out/'session.bin').read_bytes();session_check(raw);version=struct.unpack_from('<Q',raw,600)[0]
    manifest=json.loads((out/'image-input-manifest.json').read_text())
    require(manifest.get('target')=='sunstone-a733-h264' and manifest.get('execution_profile')==execution_profile(version) and manifest.get('final_elf')==p['image_sha256'],'H.264 image manifest differs.')
    sizes={name:(out/name).stat().st_size for name in p['files']}
    require(p.get('backup_sizes')=={name:n for name,a,n in base.REGIONS} and 0<sizes.get('launch.bin',0)<=0xc00,'H.264 protected SRAM geometry changed.')
    require((out/'result-zero.bin').read_bytes()==bytes(16384) and (out/'report-zero.bin').read_bytes()==bytes(128),'H.264 initial report backing changed.')
    raw=(out/'session.bin').read_bytes();session_check(raw)
    is_vga=struct.unpack_from('<Q',raw,600)[0] in (2,3,4);input_name='manifest.bin' if is_vga else 'input.h264'
    require(p['schema']==bundle_schema(version) and p.get('profile')==profile_name(version),'H.264 bundle profile differs.')
    require(input_name in p['files'],'Unsealed H.264 input.')
    reference=(out/'oracle.yuv').read_bytes() if 'oracle.yuv' in p['files'] else None
    upload=upload_bytes(raw,(out/input_name).read_bytes(),reference)
    frames=stream_module().validate_inputs(out,(out/input_name).read_bytes(),version) if is_vga else None
    if is_vga:
        for i,(_,oracle) in enumerate(frames):
            require(f'frame-{i:03}.h264' in p['files'] and (oracle is None or f'frame-{i:03}.yu12' in p['files']),'Unsealed VGA frame input.')
    template=(out/'session-template.bin').read_bytes();cache={path:(out/item['name']).read_bytes() for path,item in p['entry_evidence'].items()}
    validate_admission(json.loads((out/'admission.json').read_text()),p['image_sha256'],hashlib.sha256(template).hexdigest(),template,cache,True)
    segments,address,identity=image_parts(out/'image.elf',version);require(identity.hex()==p['build_id'],'H.264 build identity changed.')
    expected_raw=bytearray(template);struct.pack_into('<3Q',expected_raw,24,p['epoch'],p['generation'],p['lease']);expected_raw[48:80]=identity
    require(raw==expected_raw,'H.264 session differs from admitted derivation.')
    expected=[]
    for i,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{i}.bin';require((out/name).read_bytes()==segment['data'],'H.264 ELF bytes changed.');expected.append({'name':name,'address':segment['address']})
    expected.append({'name':'session.bin','address':0x41f00000});require(p['segments']==expected and all(item['name'] in sizes for item in expected),'H.264 upload spans changed or unsealed.')
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,p['epoch'],p['generation'],p['lease'])):require((out/name).read_bytes()==data,'H.264 launch contract changed.')
    p['_upload']=upload;p['_reference']=reference
    if is_vga:p['_stream_frames']=frames;p['_stream_manifest']=(out/input_name).read_bytes()
    p['_sizes']={name:(out/name).stat().st_size for name in p['files']};return p

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','admission','template','output','clang','lld'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--input',type=Path,required=True)
    parser.add_argument('--reference',type=Path,help='Optional independent qualification oracle')
    a=parser.parse_args();prepare(a.image,a.manifest,a.admission,a.template,a.output,a.clang,a.lld,a.input,a.reference)
