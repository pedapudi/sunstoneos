#!/usr/bin/env python3
"""Offline preparation of a supervised reusable STORAGE_READ application session."""
import hashlib
import importlib.util
import json
from pathlib import Path
import secrets
import shutil
import struct
import subprocess
from types import SimpleNamespace
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
board=module('storage_read_board_prepare',ROOT/'apps/board/prepare.py')
base=board.base
require=base.require
SCHEMA='a733-storage-read-supervised-bundle-v2'
CONTRACT_MAGIC=0x4153373352444331
REQUEST_MAGIC=0x4153373352444c31
SESSION_MAGIC=0x4153373352444131
SESSION_BYTES=880
SESSION_DEFINE=[]
PROFILES={'sunstone-a733-storage-read':'storage-read-supervised-v2',
          'sunstone-a733-storage-read-single-core':'storage-read-single-core-supervised-v2'}
SINGLE_CORE_TARGET='sunstone-a733-storage-read-single-core'
SINGLE_CORE_BYTES=880
AUTHORITY_PREFIX='a733-storage-read-'
ENTRY_SCHEMA='a733-storage-read-supervised-entry-v2'
DMA_MEMORY_FACTS=('private_dma_buffer','private_uncached_descriptors',
                  'exclusive_cache_line_ownership','no_dirty_aliases')
REQUIRED_FACTS={
 'initial':[],
 'memory':['private_pio_buffer','identity_cacheable_inner_shareable','no_aliases','permanent_reservation'],
 'supplies':['dcdc1_dc1sw2_3300000_cldo5_1800000','source_qualified_settling','pf_domain_3300','no_voltage_writers'],
 'topology':['sd0_pf0_6_route','inserted_removable_sd','no_card_substitution','pf6_active_low_detect','source_qualified_pulls_drives','no_alternate_pad_writers','complete_pin_authority'],
 'supervision':['whole_board_power_removal','permanent_resource_retention','no_buffer_reuse_after_uncertainty','one_attempt','no_watchdog_arm','no_reset_retry']}

def inherited_mux(raw):
    functions=list(raw[792:813:3])
    return 1 if all(f in (0,15) for f in functions) else 2 if functions==[3,3,15,3,15,3,15] else 0

def session_check(raw):
    require(len(raw)==880,'SD0 read session must be 880 bytes.')
    require(board.session_check(raw[:592],usb_reference_modes=(1,2))==2,'SD0 read requires USB0/provider selection 2.')
    require(struct.unpack_from('<3Q',raw,592)==(0x4153373352444131,2,880),'Invalid storage_read session extension.')
    owner,authority=struct.unpack_from('<2Q',raw,616)
    require(owner and authority and owner not in struct.unpack_from('<2Q',raw,456),'Invalid storage_read owner.')
    require(all(any(raw[o:o+32]) for o in (632,664,696,728,760)),'Missing SD0 physical authority.')
    require(raw[834:840]==bytes(6),'Reserved SD0 fields must be zero.')
    profile=struct.unpack_from('<I',raw,860)[0]
    require(profile in (0,1,2,3),'Unsupported SD0 bus profile.')
    first,blocks,reserved=struct.unpack_from('<QII',raw,864)
    require(1<=blocks<=(64 if profile in (2,3) else 8) and not reserved and first<=0xffffffff and blocks-1<=0xffffffff-first,'Invalid native application range.')
    identify,read,voltage=struct.unpack_from('<QQI',raw,840)
    require(0<identify<=10000000 and 0<read<=5000000 and voltage and not voltage&~0x00300000,'Invalid bounded SD0 profile.')
    require(inherited_mux(raw),'Unsupported inherited SD0 pin route.')
    for i in range(7):
        ef,ep,ed=raw[792+3*i:795+3*i];df,dp,dd=raw[813+3*i:816+3*i]
        require(ep<=2 and ed<=3 and df==(2 if (i<6 if profile else 1<=i<=3) else 0) and dp<=2 and dd<=3 and (i==2 or dp==1),'Invalid selected SD0 PF route.')

def embedded_reference(image,session):
    """Bind actual immutable geometry and const build ID before behavioral use."""
    import io
    from elftools.elf.elffile import ELFFile
    session_check(session)
    elf=ELFFile(io.BytesIO(image));table=elf.get_section_by_name('.symtab')
    require(table is not None,'SD0 image has no symbols.')
    symbols={s.name:(int(s['st_value']),int(s['st_size'])) for s in table.iter_symbols()}
    sd=SESSION_MAGIC==0x4153373352444131
    expected=struct.pack('<5Q',SESSION_MAGIC,SESSION_BYTES,288,32768 if sd else 4096,2)
    markers=[('storage_read_geometry',expected)]
    if sd and struct.unpack_from('<I',session,860)[0]:
        markers.append(('storage_read_sd_modes',struct.pack('<4Q',0x4153373353444d31,2,15,400000000)))
    if sd:
        markers.append(('storage_read_dma_geometry',struct.pack('<6Q',0x4153373353444431,1,12,64,0x41c00000,528)))
    for name,body in markers:
        require(name in symbols,'Missing immutable SD0 geometry.')
        first,size=symbols[name];require(size==len(body),'Wrong SD0 geometry size.')
        parts=[p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and not int(p['p_flags'])&2 and int(p['p_vaddr'])<=first and first+size<=int(p['p_vaddr'])+int(p['p_filesz'])]
        require(len(parts)==1,'SD0 geometry must be immutable/file-backed.')
        off=first-int(parts[0]['p_vaddr']);require(parts[0].data()[off:off+size]==body,'SD0 geometry differs.')
    first,size=symbols.get('a733_smp_build_id',(0,0));require(size==32,'Const SD0 build ID absent.')
    parts=[p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and not int(p['p_flags'])&2 and int(p['p_vaddr'])<=first and first+size<=int(p['p_vaddr'])+int(p['p_filesz'])]
    require(len(parts)==1,'SD0 build ID must be immutable/file-backed.')
    off=first-int(parts[0]['p_vaddr']);require(parts[0].data()[off:off+32]==session[48:80],'Template and const SD0 build ID differ.')
    return None

def image_manifest_check(manifest,image,session=None):
    """Bind a supported execution profile to the actual immutable ELF startup."""
    import io
    from elftools.elf.elffile import ELFFile
    elf=ELFFile(io.BytesIO(image));table=elf.get_section_by_name('.symtab')
    require(table is not None,'SD0 image symbols absent.')
    symbols={s.name:(int(s['st_value']),int(s['st_size'])) for s in table.iter_symbols()}
    marker=symbols.get('storage_read_single_core_geometry')
    target=SINGLE_CORE_TARGET if marker is not None else 'sunstone-a733-storage-read'
    require(target in PROFILES,'Unsupported storage-read image startup.')
    if session is not None:
        require(struct.unpack_from('<I',session,532)[0]==1 or marker is not None,
                'ROM USB0 takeover requires the single-core SD0 image.')
    if target=='sunstone-a733-storage-read':
        require(marker is None,'Single-core SD0 image requires its own execution profile.')
        return
    require(marker is not None and marker[1]==32 and
            symbols.get('sd_single_core_entry',(0,))[0]==int(elf.header['e_entry']) and
            not any(symbols.get(name,(0,))[0] for name in
                ('a733_cpu_start_init','a733_cpu_primary_continue','a733_smp_pre_scheduler','FreeRTOS_SGI_Handler','monitor_reset')),
            'Single-core SD0 startup differs.')
    first,size=marker
    parts=[p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and not int(p['p_flags'])&2 and
           int(p['p_vaddr'])<=first and first+size<=int(p['p_vaddr'])+int(p['p_filesz'])]
    require(len(parts)==1,'Single-core SD0 geometry must be immutable/file-backed.')
    offset=first-int(parts[0]['p_vaddr'])
    require(parts[0].data()[offset:offset+size]==struct.pack('<4Q',0x4153373353444331,1,SINGLE_CORE_BYTES,60),
            'Single-core SD0 geometry differs.')

def request_bytes(address,identity,raw,epoch,generation,lease):
    request,contract=board.request_bytes(address,identity,epoch,generation,lease,raw)
    request=struct.pack('<Q',REQUEST_MAGIC)+request[8:]
    contract=bytearray(contract);struct.pack_into('<Q',contract,184,SESSION_BYTES)
    return request,bytes(contract)

def build_launcher(out,clang,lld):
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text()
    (out/'base-supervised-launch.c').write_text(text)
    text=board.adapt_launcher(text,{'0x4153373353554c33':hex(REQUEST_MAGIC),'REQUEST[1]==3':'REQUEST[1]==2','CONTRACT->version==3':'CONTRACT->version==2',
        'CONTRACT->reserved[0] && !CONTRACT->reserved[1]':'CONTRACT->reserved[0]==A733_BOARD_SESSION_ADDRESS && CONTRACT->reserved[1]==sizeof(struct storage_read_session)',
        '20*A733_SMP_COUNTER_HZ':'60*A733_SMP_COUNTER_HZ'})
    (out/'storage_read-launch-base.c').write_text(text)
    for name,path in [('launch.c',ROOT/'apps/storage-read/launch.c'),('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h'),('storage_read_session.h',ROOT/'apps/storage-read/storage_read_session.h'),('a733_board_application.h',ROOT/'include/a733_board_application.h'),('a733_pins.h',ROOT/'include/a733_pins.h')]:shutil.copy2(path,out/name)
    flags=SESSION_DEFINE+['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-Wall','-Wextra','-Werror','-DSUNSTONE_BOARD_SUPERVISED=1',f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC}ULL','-I'+str(out)]
    commands=[[clang,*flags,'-c',out/name,'-o',out/(name+'.o')] for name in ('launch.c','launch.S')]
    commands += [[lld,'-T',out/'launch.ld',out/'launch.c.o',out/'launch.S.o','-o',out/'launch.elf'],['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',out/'launch.elf',out/'launch.bin']]
    for command in commands:
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (out/'launcher-build.log').open('a') as f:f.write(r.stdout+r.stderr)
        require(r.returncode==0,'STORAGE_READ launcher build failed; inspect retained log.')
    base.save_json(out/'launcher-commands.json',[[str(x) for x in c] for c in commands])
    require(0<(out/'launch.bin').stat().st_size<=0xc00,'Launcher exceeds SRAM.')

def validate_admission(record,image_hash,template_hash,raw,cache=None,frozen=False):
    """Validate native session limits and the selected target directly."""
    session_check(raw)
    return board.validate_admission(record,image_hash,template_hash,cache,frozen)

def prepare(image,manifest,admission,template,output,clang,lld):
    snapshots={name:Path(path).read_bytes() for name,path in [('image.elf',image),('admission.json',admission),('session-template.bin',template)]}
    raw=bytearray(snapshots['session-template.bin']);session_check(raw)
    embedded_reference(snapshots['image.elf'],raw)
    ih=hashlib.sha256(snapshots['image.elf']).hexdigest();th=hashlib.sha256(raw).hexdigest()
    mf={}
    image_manifest_check(mf,snapshots['image.elf'],raw)
    record=json.loads(snapshots['admission.json']);cache=validate_admission(record,ih,th,raw)
    epoch,generation,lease=(secrets.randbits(64) or 1 for _ in range(3))
    struct.pack_into('<3Q',raw,24,epoch,generation,lease)
    helper=module('storage_output_directory',ROOT/'tools/host_evidence.py')
    out=helper.unique_directory(output)
    for name,data in snapshots.items():(out/name).write_bytes(data)
    segments,address,identity=base.image_parts(SimpleNamespace(read_bytes=lambda:snapshots['image.elf']))
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
    p=dict(schema=SCHEMA,physical_access=False,image_sha256=ih,build_id=identity.hex(),epoch=epoch,generation=generation,lease=lease,reservation=record['reservation'],session='session.bin',profile=None,manifest=None,image='image.elf',segments=uploads,entry_evidence=evidence,backup_sizes={name:n for name,a,n in base.REGIONS},files={x.name:base.digest(x) for x in out.iterdir() if x.is_file()})
    base.save_json(out/'prepared.json',p);return validate_bundle(out)

def validate_bundle(bundle):
    out=Path(bundle);metadata=(out/'prepared.json').read_bytes();p=json.loads(metadata);require(p.get('schema')==SCHEMA and p.get('physical_access') is False,'Wrong STORAGE_READ bundle.')
    required={'image.elf','admission.json','session-template.bin','session.bin','request.bin','contract.bin','launch.bin','result-zero.bin','report-zero.bin'}
    required.update(item['name'] for item in p['segments'])
    require(required<=set(p['files']),'Missing STORAGE_READ transport input.')
    data={}
    for name in required:
        h=p['files'][name]
        require(Path(name).name==name,'Invalid SD0 filename.');data[name]=(out/name).read_bytes()
        require(hashlib.sha256(data[name]).hexdigest()==h,'SD0 input changed.')
    require(hashlib.sha256(data['image.elf']).hexdigest()==p['image_sha256'],'STORAGE_READ image changed.')
    manifest={}
    image_manifest_check(manifest,data['image.elf'])
    sizes={name:len(value) for name,value in data.items()}
    require(p.get('backup_sizes')=={name:n for name,a,n in base.REGIONS} and 0<sizes.get('launch.bin',0)<=0xc00,'STORAGE_READ protected SRAM geometry changed.')
    require(data['result-zero.bin']==bytes(16384) and data['report-zero.bin']==bytes(128),'STORAGE_READ initial report backing changed.')
    raw=data['session.bin'];session_check(raw)
    image_manifest_check(manifest,data['image.elf'],raw)
    template=data['session-template.bin'];cache={};p['entry_evidence']={}
    validate_admission(json.loads(data['admission.json']),p['image_sha256'],hashlib.sha256(template).hexdigest(),template,cache,True)
    segments,address,identity=base.image_parts(SimpleNamespace(read_bytes=lambda:data['image.elf']));require(identity.hex()==p['build_id'],'STORAGE_READ build identity changed.')
    expected_raw=bytearray(template);struct.pack_into('<3Q',expected_raw,24,p['epoch'],p['generation'],p['lease']);expected_raw[48:80]=identity
    require(raw==expected_raw,'STORAGE_READ session differs from admitted derivation.')
    expected=[]
    for i,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{i}.bin';require(data[name]==segment['data'],'STORAGE_READ ELF bytes changed.');expected.append({'name':name,'address':segment['address']})
    expected.append({'name':'session.bin','address':0x41f00000});require(p['segments']==expected and all(item['name'] in sizes for item in expected),'STORAGE_READ upload spans changed.')
    for name,value in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,p['epoch'],p['generation'],p['lease'])):require(data[name]==value,'STORAGE_READ launch contract changed.')
    p['_reference']=embedded_reference(data['image.elf'],raw)
    p['_sizes']={name:len(value) for name,value in data.items()};p['_frozen']=data;p['_prepared_sha256']=hashlib.sha256(metadata).hexdigest();p['_bundle_directory']=str(out);return p

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','admission','template','output','clang','lld'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--manifest',type=Path,help=argparse.SUPPRESS)
    a=parser.parse_args();print(prepare(a.image,a.manifest,a.admission,a.template,a.output,a.clang,a.lld)['_bundle_directory'])
