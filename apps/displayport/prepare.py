#!/usr/bin/env python3
"""Prepare one retained supervised 720p60 DisplayPort application session."""
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
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
board=module('displayport_board_prepare',ROOT/'apps/board/prepare.py')
base=board.base
require=base.require
SCHEMA='a733-displayport-supervised-bundle-v1'
CONTRACT_MAGIC=0x4153373344504331
REQUEST_MAGIC=0x4153373344504c31


def session_check(raw):
    require(len(raw)==952,'DisplayPort session must contain 952 bytes.')
    require(board.session_check(raw[:592])==2,'DisplayPort requires PMIC and USB0 workload 2.')
    for at in (564,567):
        function,pull,drive=raw[at:at+3]
        require(function==2 and pull<=2 and drive<=3,'DisplayPort PMIC PL0/PL1 require valid inherited function-2 configurations.')
    fabric=struct.unpack_from('<3I',raw,488)
    require(fabric[1]&0x80000000 and fabric[2]&0x80000000,
        'DisplayPort USB0 inherited fabric clocks require their enable bits.')
    require(raw[80:112]==raw[280:312] and any(raw[80:112]),'Board qualification/recovery identity differs.')
    require(struct.unpack_from('<3Q',raw,592)==(0x4153373344504131,1,952),'DisplayPort extension differs.')
    owner,authority,client=struct.unpack_from('<3Q',raw,616)
    require(owner and authority and client<16 and owner not in struct.unpack_from('<2Q',raw,456),'Invalid DisplayPort owner.')
    require(all(any(raw[offset:offset+32]) for offset in range(640,832,32)),'Missing DisplayPort evidence digest.')
    parameters_check(raw)


def parameters_check(raw):
    gates0,gates1,oscillator,reference,video1,serdes,rtc,apb=struct.unpack_from('<8I',raw,832)
    typec_owner,qualification,recovery=struct.unpack_from('<3Q',raw,864)
    owner=struct.unpack_from('<Q',raw,616)[0];pmic,usb=struct.unpack_from('<2Q',raw,456)
    identity,latch_owner,known,value=struct.unpack_from('<2Q2I',raw,888)
    low,high,ma,threshold,device,reserved=struct.unpack_from('<6I',raw,912)
    require(typec_owner and qualification and recovery and len({owner,typec_owner,pmic,usb})==4,
        'DisplayPort and TYPEC24 owners must be distinct from PMIC/USB0.')
    require((gates0,gates1)==(0x10007,0x10007) and identity and latch_owner and known==0x3fff and
        not value&~0x3fff and 4750<=low<=high<=5250 and ma>=500 and 0<threshold<=800 and
        0x80<=device<=0x82 and not reserved and 10000000<=apb<=400000000 and not apb%1000000,
        'DisplayPort inherited clock, latch or source profile differs.')
    for at in range(936,948,3):
        function,pull,drive=raw[at:at+3]
        require(function in (0,15) and pull<=2 and drive<=3,'Invalid TYPEC24 initial pin configuration.')
    require(not any(raw[948:952]),'DisplayPort reserved fields must be zero.')
    clocks_check(oscillator,reference,video1,serdes,rtc,apb)


def clocks_check(oscillator,reference,video1,serdes,rtc,apb):
    n=((reference>>8)&255)+1;m=((reference>>16)&127)+1
    require((oscillator>>14)&3==2 and oscillator&0x10000002==0x10000002 and
        reference&0xf8000000==0xf8000000 and 80<=n<=116 and 26000000*n==24000000*m,
        'DisplayPort requires the admitted 26 MHz oscillator and normalized 24 MHz reference.')
    n=((video1>>8)&255)+1;m=((video1>>20)&7)+1
    require(video1&0xfc000002==0xfc000000 and 53<=n<=105 and 24000000*n==1188000000*m and
        serdes&0x8700001f==0x80000000 and rtc&16,
        'DisplayPort VIDEO1 or SERDES inherited clock profile differs.')


def request_bytes(address,identity,raw,epoch,generation,lease):
    session_check(raw)
    reservation=struct.unpack_from('<Q',raw,200)[0]
    require(len(identity)==32 and 0x41000000<=address<=0x411fffe0 and
        all(type(x) is int and 0<x<2**64 for x in (epoch,generation,lease,reservation)),
        'Invalid retained DisplayPort identity.')
    digest=hashlib.sha256(raw).digest()
    request=struct.pack('<8Q',REQUEST_MAGIC,2,128,epoch,generation,generation^((1<<64)-1),lease,address)+identity+digest
    contract=struct.pack('<6Q',0,2,288,epoch,generation,generation^((1<<64)-1))+identity
    contract+=struct.pack('<18Q',0,0,0,0,lease,0,0,0,0,0,0,0,1,reservation,0x48000000,0x78000000,0x78000000,0x79000000)
    contract+=digest+raw[80:112]
    require(len(contract)==288,'Extended DisplayPort contract ABI differs.')
    return request,contract+bytes(4096-len(contract))

def launcher_base_source(text):
    require('REQUEST[1]==3' in text and 'CONTRACT->version==3' in text,'Unexpected supervised launcher version guards.')
    for anchor in ('0x4153373353554c33','REQUEST[1]==3','CONTRACT->version==3',
                   'CONTRACT->reserved[0] && !CONTRACT->reserved[1]','CONTRACT->bytes==sizeof(*CONTRACT)',
                   'CONTRACT->reserved[2+i]'):
        require(text.count(anchor)==1,'Shared DisplayPort launcher adaptation anchor changed: '+anchor)
    text=text.replace('0x4153373353554c33','0x4153373344504c31').replace('REQUEST[1]==3','REQUEST[1]==2').replace('CONTRACT->version==3','CONTRACT->version==2')
    text=text.replace('CONTRACT->reserved[0] && !CONTRACT->reserved[1]','CONTRACT->reserved[0]==1 && CONTRACT->reserved[1] && CONTRACT->reserved[2]==0x48000000 && CONTRACT->reserved[3]==0x78000000 && CONTRACT->reserved[4]==0x78000000 && CONTRACT->reserved[5]==0x79000000')
    text=text.replace('CONTRACT->bytes==sizeof(*CONTRACT)','CONTRACT->bytes==288').replace('CONTRACT->reserved[2+i]','((volatile uint64_t *)((uintptr_t)CONTRACT+224))[i]')
    # The shared20-second native lifetime remains exact for DisplayPort.
    return text


def build_launcher(out,clang,lld):
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text()
    (out/'base-supervised-launch.c').write_text(text)
    text=launcher_base_source(text)
    (out/'displayport-launch-base.c').write_text(text)
    for name,path in [('launch.c',ROOT/'apps/displayport/launch.c'),('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h'),('a733_displayport_application.h',ROOT/'include/a733_displayport_application.h'),('a733_board_application.h',ROOT/'include/a733_board_application.h'),('a733_pins.h',ROOT/'include/a733_pins.h')]:shutil.copy2(path,out/name)
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Oz','-g','-MD','-Wall','-Wextra','-Werror','-DSUNSTONE_BOARD_SUPERVISED=1','-DA733_BOARD_SUPERVISED_SECONDS=20',f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC}ULL','-I'+str(out)]
    commands=[[clang,*flags,'-c',out/name,'-o',out/(name+'.o')] for name in ('launch.c','launch.S')]
    commands += [[lld,'-T',out/'launch.ld',out/'launch.c.o',out/'launch.S.o','-o',out/'launch.elf'],['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',out/'launch.elf',out/'launch.bin']]
    for command in commands:
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (out/'launcher-build.log').open('a') as f:f.write(r.stdout+r.stderr)
        require(r.returncode==0,'DisplayPort launcher build failed; inspect retained log.')
    base.save_json(out/'launcher-commands.json',[[str(x) for x in c] for c in commands])
    require(0<(out/'launch.bin').stat().st_size<=0xc00,'Launcher exceeds SRAM.')

def physical_authority(record):
    model_flags=[record[key] for key in ('modeled','model') if key in record]
    require(record.get('physical_authority') is True and model_flags and all(flag is False for flag in model_flags),
        'Modeled DisplayPort evidence cannot authorize physical execution.')



def validate_admission(record,image_hash,template_hash,raw,cache=None,frozen=False):
    require(record.get('schema')=='a733-displayport-supervised-entry-v1','Wrong DisplayPort admission schema.')
    physical_authority(record)
    common={**record,'schema':'a733-board-supervised-entry-v2'}
    cache=board.validate_admission(common,image_hash,template_hash,cache,frozen)
    def evidence(item):
        require(isinstance(item,dict) and set(item)=={'path','sha256'},'Invalid DisplayPort evidence reference.')
        path=Path(item['path']);require(path.is_absolute(),'DisplayPort evidence path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Missing frozen DisplayPort evidence.');cache[str(path)]=path.read_bytes()
        data=cache[str(path)];require(hashlib.sha256(data).hexdigest()==item['sha256'],'DisplayPort evidence changed.')
        return data
    names=('isolation','memory','analog','power','serdes','supervision')
    fields={
        'isolation':('all_masters_excluded','prior_transactions_drained','prior_walks_drained','remote_secure_writers_excluded','display_irqs_owned_masked','iommu_irqs_owned_masked'),
        'memory':('private_nc_arena','no_physical_aliases','iommu_banks_disabled','iommu_clocks_stable','display_device_mapping'),
        'analog':('dp_analog_qualified','source_apb_independently_qualified'),
        'power':('typec24_source_limits_qualified','dp_supplies_stable'),
        'serdes':('comb0_exclusive_from_usb3','four_lane_dp_route_qualified','source_clocks_stable','source_apb_independently_qualified'),
        'supervision':('whole_board_power_removal','permanent_dma_retention','no_reuse_after_fault','one_attempt')}
    for index,name in enumerate(names):
        data=evidence(record[name]);offset=640+index*32
        require(hashlib.sha256(data).digest()==raw[offset:offset+32],'DisplayPort fact digest differs from session.')
        fact=json.loads(data);physical_authority(fact)
        require(fact.get('schema')=='a733-displayport-'+name+'-v1' and fact.get('identity')==record['identity'] and
            fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and fact.get('reviewed') is True and
            all(fact.get(key) is True for key in fields[name]),'DisplayPort physical facts do not cover the admitted image/target.')
        if name in ('analog','serdes'):
            require(fact.get('qualified_apb_hz')==struct.unpack_from('<I',raw,860)[0] and fact.get('apb_evidence'),
                'DisplayPort AUX APB frequency requires independent physical evidence.')
            for ref in fact['apb_evidence']:evidence(ref)
    ddr=json.loads(evidence(record['displayport_ddr']));physical_authority(ddr)
    require(ddr.get('schema')=='a733-ddr-reservation-v1' and ddr.get('identity')==record['identity'] and
        ddr.get('port')==record['port'] and ddr.get('image_sha256')==image_hash and
        ddr.get('exclusive') is True and ddr.get('published_cache_disabled') is True and
        ddr.get('remote_writers_excluded') is True and ddr.get('ranges')==[[0x48000000,0x79000000]] and ddr.get('evidence'),
        'Extended DisplayPort DRAM exclusion evidence differs.')
    for ref in ddr['evidence']:evidence(ref)
    proof=json.loads(evidence(record['allocation']));physical_authority(proof)
    require(proof.get('schema')=='a733-displayport-allocation-v1' and
        proof.get('image_sha256')==image_hash and proof.get('session_sha256')==template_hash,
        'DisplayPort allocation proof covers different image/session bytes.')
    allocation_check(proof,raw,evidence(record['page_table']),evidence(record['allocation_bytes']))
    return cache


def pattern_bytes():
    colors=(0xffff,0xffe0,0x07ff,0x07e0,0xf81f,0xf800,0x001f,0)
    row=b''.join(struct.pack('<H',color)*160 for color in colors)
    pixels=bytearray()
    for y in range(720):
        current=bytearray(row)
        if y<8 or y>=712:current[:]=bytes(2560)
        else:
            current[:16]=bytes(16);current[2544:]=bytes(16)
            current[y*2:y*2+2]=bytes(2)
            current[(1279-y)*2:(1279-y)*2+2]=bytes(2)
        pixels.extend(current)
    return bytes(pixels)


def allocation_check(proof,raw,table,allocation):
    # This profile maps the complete post-table tail read-only. The first
    # 4 KiB and trailing redzones contain sentinels; they are mapped, not holes.
    row=proof.get('allocation',{});pa=row.get('physical')
    require(type(pa) is int and row.get('cpu')==pa and row.get('bytes')==2097152 and
        row.get('state')==2 and not pa%16384 and 0x78000000<=pa<=0x79000000-2097152,
        'DisplayPort actual NC allocation differs.')
    span=dict(iova=0x10000000,physical=pa+32768,bytes=2097152-32768,access=1)
    require(proof.get('spans')==[span] and proof.get('table_physical')==pa and proof.get('table_bytes')==32768 and
        proof.get('pixel_physical')==pa+36864 and proof.get('pixel_bytes')==1843200 and
        proof.get('scanned_masters')==0,'DisplayPort prelaunch mapping or unexecuted scan state differs.')
    words=[0]*8192;words[64]=((pa+16384)&0xfffff000)|1
    for offset in range(0,span['bytes'],4096):words[4096+offset//4096]=((pa+32768+offset)&0xfffff000)|6
    require(table==struct.pack('<8192I',*words) and hashlib.sha256(table).hexdigest()==proof.get('table_sha256'),
        'DisplayPort page-table descriptors, permissions or holes differ.')
    pixels=pattern_bytes()
    expected=table+bytes([0xa5])*4096+pixels+bytes([0xa5])*(2097152-36864-len(pixels))
    require(allocation==expected and hashlib.sha256(allocation).hexdigest()==proof.get('allocation_sha256') and
        hashlib.sha256(pixels).hexdigest()==proof.get('pixel_sha256'),
        'DisplayPort generated pixels, padding or sentinel redzones differ.')


def prepare(image,manifest,admission,template,output,clang,lld):
    snapshots={name:Path(path).read_bytes() for name,path in [('image.elf',image),('image-input-manifest.json',manifest),('admission.json',admission),('session-template.bin',template)]}
    raw=bytearray(snapshots['session-template.bin']);session_check(raw)
    ih=hashlib.sha256(snapshots['image.elf']).hexdigest();th=hashlib.sha256(raw).hexdigest()
    mf=json.loads(snapshots['image-input-manifest.json'])
    require(mf.get('target')=='sunstone-a733-displayport' and mf.get('execution_profile')=='displayport-supervised-v1' and mf.get('final_elf')==ih,'Tested DisplayPort image manifest required.')
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
    (out/'session.bin').write_bytes(raw);uploads.append({'name':'session.bin','address':0x41f00000})
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,epoch,generation,lease)):(out/name).write_bytes(data)
    for name,count in [('result-zero.bin',16384),('report-zero.bin',128)]:(out/name).write_bytes(bytes(count))
    evidence={}
    for i,(path,data) in enumerate(cache.items()):
        name=f'evidence-{i}.bin';(out/name).write_bytes(data);evidence[path]={'name':name,'sha256':hashlib.sha256(data).hexdigest()}
    p=dict(schema=SCHEMA,physical_access=False,image_sha256=ih,build_id=identity.hex(),epoch=epoch,generation=generation,lease=lease,reservation=record['reservation'],session='session.bin',profile=None,manifest='image-input-manifest.json',image='image.elf',segments=uploads,entry_evidence=evidence,backup_sizes={name:n for name,a,n in base.REGIONS},files={x.name:base.digest(x) for x in out.iterdir() if x.is_file()})
    base.save_json(out/'prepared.json',p);return validate_bundle(out)

def validate_bundle(bundle):
    out=Path(bundle);p=json.loads((out/'prepared.json').read_text());require(p.get('schema')==SCHEMA and p.get('physical_access') is False,'Wrong DisplayPort bundle.')
    required={'image.elf','image-input-manifest.json','admission.json','session-template.bin','session.bin','request.bin','contract.bin','launch.bin','result-zero.bin','report-zero.bin'}
    require(required<=set(p['files']) and all(Path(item['name']).name==item['name'] and item['name'] in p['files'] for item in p['entry_evidence'].values()),'Unsealed DisplayPort authority or transport input.')
    for name,h in p['files'].items():require(Path(name).name==name and base.digest(out/name)==h,'DisplayPort bundle input changed.')
    require(base.digest(out/'image.elf')==p['image_sha256'],'DisplayPort image changed.')
    manifest=json.loads((out/'image-input-manifest.json').read_text())
    require(manifest.get('target')=='sunstone-a733-displayport' and manifest.get('execution_profile')=='displayport-supervised-v1' and manifest.get('final_elf')==p['image_sha256'],'DisplayPort image manifest differs.')
    sizes={name:(out/name).stat().st_size for name in p['files']}
    require(p.get('backup_sizes')=={name:n for name,a,n in base.REGIONS} and 0<sizes.get('launch.bin',0)<=0xc00,'DisplayPort protected SRAM geometry changed.')
    require((out/'result-zero.bin').read_bytes()==bytes(16384) and (out/'report-zero.bin').read_bytes()==bytes(128),'DisplayPort initial report backing changed.')
    raw=(out/'session.bin').read_bytes();session_check(raw)
    template=(out/'session-template.bin').read_bytes();cache={path:(out/item['name']).read_bytes() for path,item in p['entry_evidence'].items()}
    validate_admission(json.loads((out/'admission.json').read_text()),p['image_sha256'],hashlib.sha256(template).hexdigest(),template,cache,True)
    segments,address,identity=image_parts(out/'image.elf');require(identity.hex()==p['build_id'],'DisplayPort build identity changed.')
    expected_raw=bytearray(template);struct.pack_into('<3Q',expected_raw,24,p['epoch'],p['generation'],p['lease']);expected_raw[48:80]=identity
    require(raw==expected_raw,'DisplayPort session differs from admitted derivation.')
    expected=[]
    for i,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{i}.bin';require((out/name).read_bytes()==segment['data'],'DisplayPort ELF bytes changed.');expected.append({'name':name,'address':segment['address']})
    expected.append({'name':'session.bin','address':0x41f00000});require(p['segments']==expected and all(item['name'] in sizes for item in expected),'DisplayPort upload spans changed or unsealed.')
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,p['epoch'],p['generation'],p['lease'])):require((out/name).read_bytes()==data,'DisplayPort launch contract changed.')
    p['_reference']=None
    p['_sizes']={name:(out/name).stat().st_size for name in p['files']};return p


def image_parts(path):
    return _image_parts(Path(path).read_bytes())

def _image_parts(data):
    import io
    elf=ELFFile(io.BytesIO(data));require(elf['e_machine']=='EM_AARCH64' and elf['e_entry']==0x41000000,'Native extended-memory DisplayPort ELF required.')
    table=elf.get_section_by_name('.symtab');require(table is not None,'Native DisplayPort symbol table required.')
    entries=list(table.iter_symbols());symbols={s.name:(s['st_value'],s['st_size']) for s in entries}
    parts=[]
    for s in elf.iter_segments():
        if s['p_type']!='PT_LOAD':continue
        a,n=s['p_paddr'],s['p_memsz'];data=s.data()
        require(a==s['p_vaddr'] and n>0 and len(data)<=n and not (s['p_flags']&3==3) and
            all(a>=p['address']+p['memory_bytes'] or p['address']>=a+n for p in parts),'Invalid DisplayPort ELF allocations.')
        require((0x41000000<=a<a+n<=0x42200000 and (not data or a+len(data)<=0x42000000)) or
            (not data and (a,a+n) in [(0x48000000,0x78000000),(0x78000000,0x79000000)]),'Extended DisplayPort ELF span outside reservations.')
        require(not s['p_flags']&1 or a+n<=0x41200000,'Extended DisplayPort code outside image prefix.')
        parts.append({'address':a,'memory_bytes':n,'data':data,'flags':int(s['p_flags'])})
    for name,value in {'__device_pool_start':0x48000000,'__device_pool_end':0x78000000,'__uncached_pool_start':0x78000000,'__uncached_pool_end':0x79000000,'__a733_smp_contract':0x42004000,'__a733_smp_contract_end':0x42005000}.items():
        require(symbols.get(name,(0,))[0]==value,'Extended DisplayPort linker ownership differs.')
    require(any(p['flags']&1 and p['address']<=0x41000000<p['address']+len(p['data']) for p in parts),
        'DisplayPort entry lacks executable file-backed bytes.')
    require('__bss_end' in symbols and symbols['__bss_end'][0]<=0x41f00000,'Extended DisplayPort session overlaps BSS.')
    def immutable(name,size):
        found=[symbol for symbol in entries if symbol.name==name and symbol['st_info']['type']=='STT_OBJECT']
        require(len(found)==1,'DisplayPort immutable object marker is not unique: '+name)
        address,n=found[0]['st_value'],found[0]['st_size'];require(n==size,'Extended DisplayPort immutable marker absent: '+name)
        match=[p for p in parts if not p['flags']&2 and p['address']<=address and address+size<=p['address']+len(p['data'])]
        require(len(match)==1,'Extended DisplayPort marker is not immutable/file-backed.')
        p=match[0];return address,p['data'][address-p['address']:address-p['address']+size]
    _,geometry=immutable('displayport_geometry',104)
    require(struct.unpack('<13Q',geometry)==(0x4153373344504131,1,952,288,0x41f00000,20,1024,1280,720,2560,2097152,36864,0x10001000),'DisplayPort compiled geometry differs.')
    address,identity=immutable('a733_smp_build_id',32)
    for p in parts:p.pop('flags')
    return parts,address,identity

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','admission','template','output','clang','lld'):parser.add_argument('--'+name,type=Path,required=True)
    a=parser.parse_args();prepare(a.image,a.manifest,a.admission,a.template,a.output,a.clang,a.lld)
