#!/usr/bin/env python3
"""Prepare immutable supervised GPU RAM inputs offline; never communicate with a device."""
import argparse
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
board=module('gpu_board_preparation',ROOT/'apps/board/prepare.py')
base=board.base
require,digest,save_json=board.require,board.digest,board.save_json
SCHEMA='a733-gpu-supervised-bundle-v2'
CONTRACT_MAGIC=0x4153373347534332
REQUEST_MAGIC=0x4153373347534c32
RANGES=[[0x41000000,0x42200000],[0x48000000,0x79000000]]
GPU_CONDITIONS={'initial-power-removal-and-gpu-drain','gpu-reset-clocks-off-no-requests',
    'gpu-pll-400mhz-parent','gpu-dcdc4-800mv-settled','shared-ccu-pck-pmic-ownership',
    'remote-gpu-masters-excluded','identity-device-gpu-gic-mappings'}

def validate_admission(record,image_hash,template_hash,cache=None,frozen=False):
    require(record.get('schema')=='a733-gpu-supervised-entry-v2','GPU-specific admission required.')
    common={**record,'schema':'a733-board-supervised-entry-v2'}
    cache=board.validate_admission(common,image_hash,template_hash,cache,frozen)
    def evidence(item):
        require(isinstance(item,dict) and set(item)=={'path','sha256'},'Invalid GPU evidence reference.')
        name=item['path'];require(Path(name).is_absolute(),'GPU evidence must use an absolute path.')
        if name not in cache:
            require(not frozen,'Frozen GPU evidence missing.');cache[name]=Path(name).read_bytes()
        require(hashlib.sha256(cache[name]).hexdigest()==item['sha256'],'GPU evidence bytes changed.')
        return cache[name]
    ddr=json.loads(evidence(record['gpu_ddr']))
    require(ddr.get('schema')=='a733-ddr-reservation-v1' and ddr.get('identity')==record['identity'] and
        ddr.get('port')==record['port'] and ddr.get('image_sha256')==image_hash and
        ddr.get('exclusive') is True and ddr.get('published_cache_disabled') is True and
        ddr.get('remote_writers_excluded') is True and ddr.get('ranges')==[[0x48000000,0x79000000]] and ddr.get('evidence'),
        'Actual large GPU DDR ownership evidence required.')
    for ref in ddr['evidence']:evidence(ref)
    gpu=json.loads(evidence(record['gpu']))
    require(gpu.get('schema')=='a733-gpu-initial-entry-v1' and gpu.get('identity')==record['identity'] and
        gpu.get('port')==record['port'] and gpu.get('image_sha256')==image_hash and
        gpu.get('reservation')==record['reservation'] and gpu.get('cold_master') and
        gpu.get('entry_conditions_established') is True and gpu.get('no_gpu_activity_since_initial_isolation') is True and
        set(gpu.get('conditions',{}))==GPU_CONDITIONS,
        'Actual power, shared ownership, mappings and initial GPU isolation evidence required.')
    for ref in gpu['conditions'].values():evidence(ref)
    require(not record.get('payloads') and record.get('radio_ddr') is None,'Fixed GPU application accepts no unrelated assets.')
    return cache

def session_check(raw):
    require(len(raw)==592 and struct.unpack_from('<3Q',raw)==(0x4153373342535332,2,592),'Distinct supervised session required.')
    require(raw[112:184]==bytes(72) and struct.unpack_from('<I',raw,400)[0]==2,
        'Supervised GPU session must contain no watchdog or uploaded software time claims.')
    owners=struct.unpack_from('<2Q',raw,456)
    require(owners[0] and owners[1] and owners[0]!=owners[1] and all(x not in (9,10,11) for x in owners),
        'GPU session owners conflict.')

def request_bytes(address,identity,epoch,generation,lease,session,reservation,entry_digest):
    require(len(identity)==32 and len(entry_digest)==32 and any(entry_digest) and
        0x41000000<=address<=0x411fffe0 and all(type(x) is int and 0<x<2**64 for x in (epoch,generation,lease,reservation)),
        'Invalid supervised GPU launch identity.')
    request=struct.pack('<8Q',REQUEST_MAGIC,2,128,epoch,generation,generation^((1<<64)-1),lease,address)+identity+entry_digest
    contract=struct.pack('<6Q',0,2,288,epoch,generation,generation^((1<<64)-1))+identity
    contract+=struct.pack('<18Q',0,0,0,0,lease,0,0,0,0,0,0,0,1,reservation,0x48000000,0x78000000,0x78000000,0x79000000)
    contract+=hashlib.sha256(session).digest()+entry_digest
    require(len(contract)==288,'GPU contract ABI differs.')
    return request,contract+bytes(4096-len(contract))

def image_parts(path):
    from elftools.elf.elffile import ELFFile
    with Path(path).open('rb') as f:
        elf=ELFFile(f);require(elf['e_machine']=='EM_AARCH64' and elf['e_entry']==0x41000000,'Native GPU ELF required.')
        symbols={s.name:(s['st_value'],s['st_size']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
        parts=[]
        for s in elf.iter_segments():
            if s['p_type']!='PT_LOAD':continue
            a,n=s['p_paddr'],s['p_memsz'];data=s.data()
            require(a==s['p_vaddr'] and n>0 and all(a>=p['address']+p['memory_bytes'] or p['address']>=a+n for p in parts),'Invalid GPU ELF allocations.')
            require((0x41000000<=a<a+n<=0x42200000 and (not data or a+len(data)<=0x42000000)) or
                (not data and (a,a+n) in [(0x48000000,0x78000000),(0x78000000,0x79000000)]),'GPU ELF span outside fixed reservations.')
            require(not s['p_flags']&1 or a+n<=0x41200000,'GPU executable bytes outside image prefix.')
            parts.append({'address':a,'memory_bytes':n,'data':data})
        for name,value in {'__device_pool_start':0x48000000,'__device_pool_end':0x78000000,'__uncached_pool_start':0x78000000,'__uncached_pool_end':0x79000000,'__a733_smp_contract':0x42004000,'__a733_smp_contract_end':0x42005000}.items():
            require(symbols.get(name,(0,))[0]==value,'GPU linker ownership differs.')
        address,size=symbols['a733_smp_build_id'];require(size==32,'Invalid GPU build identity.')
        for p in parts:
            if p['address']<=address and address+32<=p['address']+len(p['data']):return parts,address,p['data'][address-p['address']:address-p['address']+32]
        raise ValueError('GPU build identity has no image bytes.')

def image_manifest_check(manifest,image,session=None):
    """Bind the execution profile to immutable native startup, not a mode label."""
    import io
    from elftools.elf.elffile import ELFFile
    profiles={'sunstone-a733-gpu-render':'gpu-supervised-v2',
              'sunstone-a733-gpu-compute-single-core':'gpu-single-core-supervised-v2'}
    target=manifest.get('target')
    require(target in profiles and manifest.get('execution_profile')==profiles[target] and
            manifest.get('final_elf')==hashlib.sha256(image).hexdigest(),'Supervised GPU manifest required.')
    if session is not None:
        session_check(session)
        mode=struct.unpack_from('<I',session,532)[0]
        require(mode in (1,2) and (mode==1 or target=='sunstone-a733-gpu-compute-single-core'),
                'ROM USB0 takeover requires the single-core GPU image.')
    elf=ELFFile(io.BytesIO(image));table=elf.get_section_by_name('.symtab')
    require(table is not None,'GPU image symbols absent.')
    symbols={s.name:(int(s['st_value']),int(s['st_size'])) for s in table.iter_symbols()}
    marker=symbols.get('gpu_single_core_geometry')
    if target=='sunstone-a733-gpu-render':
        require(marker is None,'Single-core GPU image requires its own execution profile.')
        return
    require(manifest.get('workload')=='fixed-arithmetic-repeat' and marker is not None and marker[1]==32 and
            symbols.get('sd_single_core_entry',(0,))[0]==int(elf.header['e_entry']) and
            not any(symbols.get(name,(0,))[0] for name in
                ('a733_cpu_start_init','a733_cpu_primary_continue','a733_smp_pre_scheduler','FreeRTOS_SGI_Handler','monitor_reset')),
            'Single-core GPU startup differs.')
    first,size=marker
    parts=[p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and not int(p['p_flags'])&2 and
           int(p['p_vaddr'])<=first and first+size<=int(p['p_vaddr'])+int(p['p_filesz'])]
    require(len(parts)==1,'Single-core GPU geometry must be immutable/file-backed.')
    offset=first-int(parts[0]['p_vaddr'])
    require(parts[0].data()[offset:offset+size]==struct.pack('<4Q',0x4153373347505331,1,592,60),
            'Single-core GPU geometry differs.')

def build_launcher(output,clang,lld):
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text()
    changes={'0x4153373353554c33':'0x4153373347534c32','REQUEST[1]==3':'REQUEST[1]==2',
        'CONTRACT->version==3':'CONTRACT->version==2','CONTRACT->bytes==sizeof(*CONTRACT)':'CONTRACT->bytes==288',
        'CONTRACT->reserved[0] && !CONTRACT->reserved[1]':'CONTRACT->reserved[0]==1 && CONTRACT->reserved[1] && CONTRACT->reserved[2]==0x48000000 && CONTRACT->reserved[3]==0x78000000 && CONTRACT->reserved[4]==0x78000000 && CONTRACT->reserved[5]==0x79000000',
        'CONTRACT->reserved[2+i]':'((volatile uint64_t *)((uintptr_t)CONTRACT+256))[i]',
        '20*A733_SMP_COUNTER_HZ':'60*A733_SMP_COUNTER_HZ'}
    for old,new in changes.items():require(old in text,'Supervised launcher source changed.');text=text.replace(old,new)
    (output/'gpu-launch-base.c').write_text(text)
    for target,source in [('launch.c',ROOT/'apps/gpu-render/launch.c'),('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h'),('a733_board_application.h',ROOT/'include/a733_board_application.h')]:shutil.copy2(source,output/target)
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-Wall','-Wextra','-Werror','-DA733_SMP_CONTRACT_MAGIC=0x4153373347534332ULL','-I'+str(output)]
    commands=[]
    for name,target in [('launch.S','entry.o'),('launch.c','launch.o')]:commands.append([str(clang),*flags,'-MMD','-MF',str(output/(target+'.d')),'-c',str(output/name),'-o',str(output/target)])
    commands += [[str(lld),'-T',str(output/'launch.ld'),str(output/'entry.o'),str(output/'launch.o'),'-o',str(output/'launch.elf')],['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',str(output/'launch.elf'),str(output/'launch.bin')]]
    save_json(output/'commands.json',commands)
    for command in commands:
        result=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        with (output/'build.log').open('a') as f:f.write(result.stdout)
        require(result.returncode==0,'GPU launcher build failed; inspect build.log.')
    require(0<(output/'launch.bin').stat().st_size<=0xc00,'GPU launcher exceeds reserved SRAM.')

def prepare(image,manifest,admission,session_template,output,clang,lld):
    output=Path(output);image_data=Path(image).read_bytes();manifest_data=Path(manifest).read_bytes()
    admission_data=Path(admission).read_bytes();template=Path(session_template).read_bytes()
    image_hash=hashlib.sha256(image_data).hexdigest();m=json.loads(manifest_data);record=json.loads(admission_data)
    image_manifest_check(m,image_data,template)
    cache=validate_admission(record,image_hash,hashlib.sha256(template).hexdigest());session_check(template)
    gpu=json.loads(cache[record['gpu']['path']]);entry=bytes.fromhex(record['gpu']['sha256'])
    require(template[80:112]==entry and struct.unpack_from('<Q',template,192)[0]==gpu['cold_master'],'Session does not bind actual GPU entry evidence.')
    output.mkdir(parents=True,exist_ok=False)
    for name,data in [('image.elf',image_data),('image-input-manifest.json',manifest_data),('admission.json',admission_data),('session-template.bin',template)]:(output/name).write_bytes(data)
    parts,address,identity=image_parts(output/'image.elf');raw=bytearray(template)
    epoch,generation,lease=(secrets.randbits(64) or 1 for _ in range(3))
    struct.pack_into('<3Q',raw,24,epoch,generation,lease);raw[48:80]=identity
    build_launcher(output,clang,lld);uploads=[]
    for i,p in enumerate(parts):
        if p['data']:
            name=f'segment-{i}.bin';(output/name).write_bytes(p['data']);uploads.append({'name':name,'address':p['address']})
    (output/'session.bin').write_bytes(raw);uploads.append({'name':'session.bin','address':0x41f00000})
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,epoch,generation,lease,raw,record['reservation'],entry)):(output/name).write_bytes(data)
    (output/'result-zero.bin').write_bytes(bytes(16384));(output/'report-zero.bin').write_bytes(bytes(128))
    frozen={}
    for i,(name,data) in enumerate(cache.items()):
        target=f'entry-evidence-{i}.bin';(output/target).write_bytes(data);frozen[name]={'name':target,'sha256':hashlib.sha256(data).hexdigest()}
    p={'schema':SCHEMA,'image_sha256':image_hash,'launcher_sha256':digest(output/'launch.bin'),'build_id':identity.hex(),
        'epoch':epoch,'generation':generation,'lease':lease,'reservation':record['reservation'],'segments':uploads,
        'reservations':[{k:v for k,v in x.items() if k!='data'} for x in parts], 'entry_evidence':frozen,
        'physical_access':False,'retention':'whole-board-power-removal','files':{p.name:digest(p) for p in output.iterdir() if p.is_file()}}
    save_json(output/'prepared.json',p);return validate_bundle(output)

def validate_bundle(bundle):
    bundle=Path(bundle);p=json.loads((bundle/'prepared.json').read_text())
    require(p.get('schema')==SCHEMA and p.get('physical_access') is False and p.get('retention')=='whole-board-power-removal','Unsupported GPU bundle.')
    for name,h in p['files'].items():require(Path(name).name==name and digest(bundle/name)==h,'GPU bundle input changed.')
    require(digest(bundle/'image.elf')==p['image_sha256'] and digest(bundle/'launch.bin')==p['launcher_sha256'],'GPU executable changed.')
    cache={name:(bundle/item['name']).read_bytes() for name,item in p['entry_evidence'].items()}
    require(all(hashlib.sha256(cache[name]).hexdigest()==item['sha256'] for name,item in p['entry_evidence'].items()),'Frozen GPU evidence changed.')
    record=json.loads((bundle/'admission.json').read_text());validate_admission(record,p['image_sha256'],digest(bundle/'session-template.bin'),cache,True)
    manifest=json.loads((bundle/'image-input-manifest.json').read_text())
    image_manifest_check(manifest,(bundle/'image.elf').read_bytes(),(bundle/'session.bin').read_bytes())
    raw=(bundle/'session.bin').read_bytes();session_check(raw);parts,address,identity=image_parts(bundle/'image.elf')
    derived=bytearray((bundle/'session-template.bin').read_bytes());struct.pack_into('<3Q',derived,24,p['epoch'],p['generation'],p['lease']);derived[48:80]=identity
    require(raw==derived and p['build_id']==identity.hex() and p['reservation']==record['reservation'],'GPU session derivation changed.')
    entry=bytes.fromhex(record['gpu']['sha256']);gpu=json.loads(cache[record['gpu']['path']])
    require(raw[80:112]==entry and struct.unpack_from('<Q',raw,192)[0]==gpu['cold_master'],'GPU initial entry binding changed.')
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,p['epoch'],p['generation'],p['lease'],raw,p['reservation'],entry)):
        require((bundle/name).read_bytes()==data,'GPU launch request/contract changed.')
    expected=[]
    for i,x in enumerate(parts):
        if x['data']:
            name=f'segment-{i}.bin';require((bundle/name).read_bytes()==x['data'],'GPU upload differs from ELF.');expected.append({'name':name,'address':x['address']})
    expected.append({'name':'session.bin','address':0x41f00000});require(p['segments']==expected and all(x['name'] in p['files'] for x in expected),'GPU upload set changed.')
    require(p['reservations']==[{k:v for k,v in x.items() if k!='data'} for x in parts], 'GPU reservation metadata changed.')
    return p

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','admission','session-template','output','clang','lld'):parser.add_argument('--'+name,type=Path,required=True)
    a=parser.parse_args();prepare(a.image,a.manifest,a.admission,a.session_template,a.output,a.clang,a.lld)
