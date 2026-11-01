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
    # Hardware state is checked by native startup and the live supervisor.
    # Legacy evidence records are optional metadata, never launch authority.
    require(not record.get('payloads') and record.get('radio_ddr') is None,
        'Fixed GPU application accepts no unrelated assets.')
    return {}

def session_check(raw):
    if len(raw)==632:
        magic,version,size,width,height=struct.unpack_from('<5Q',raw,592)
        require(magic==0x4153373347545831 and version in (1,2,3) and size==632 and 0<width<=1920 and 0<height<=1080,
                'Invalid GPU texture session envelope.')
        raw=raw[:592]
    if len(raw)==8944:
        require(struct.unpack_from('<4Q',raw,592)==(0x4153373347505031,1,8944,8288),
                'Invalid GPU program envelope.')
        require(hashlib.sha256(raw[656:]).digest()==raw[624:656],'GPU program digest differs.')
        raw=raw[:592]
    require(len(raw)==592 and struct.unpack_from('<3Q',raw)==(0x4153373342535332,2,592),'Distinct supervised session required.')
    require(raw[112:184]==bytes(72) and struct.unpack_from('<I',raw,400)[0]==2,
        'Supervised GPU session must contain no watchdog or uploaded software time claims.')
    require(all(any(raw[o:o+32]) for o in (80,216,248,280)) and
        all(struct.unpack_from('<4Q',raw,184)), 'Native GPU session identities must be nonzero.')
    require(struct.unpack_from('<2I',raw,512)==(3300000,3300000) and
        all(struct.unpack_from('<3I',raw,500)) and struct.unpack_from('<I',raw,532)[0] in (1,2),
        'Unsupported GPU USB supply or fabric configuration.')
    require(not any(raw[352:400]+raw[404:456]+raw[472:488]+raw[528:532]+raw[536:564]+raw[570:588]),
        'GPU session contains unrelated device state.')
    owners=struct.unpack_from('<2Q',raw,456)
    require(owners[0] and owners[1] and owners[0]!=owners[1] and all(x not in (9,10,11) for x in owners),
        'GPU session owners conflict.')

def program_admission(record,cache,raw):
    if len(raw)!=8944:return
    package=raw[656:]
    require(hashlib.sha256(package).digest()==raw[624:656],
        'GPU program package differs from the native session digest.')

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

def native_manifest(image):
    """Select the capture protocol from native capability symbols."""
    import io
    from elftools.elf.elffile import ELFFile
    elf=ELFFile(io.BytesIO(image));table=elf.get_section_by_name('.symtab')
    require(table is not None,'GPU image symbols absent.')
    names={s.name for s in table.iter_symbols()}
    single='gpu_single_core_geometry' in names
    texture='gpu_runtime_texture_marker' in names
    draw='gpu_runtime_draw_marker' in names
    workload=('copied-indexed-texture-draw' if texture else 'copied-triangle-draw' if draw else
        'trusted-compiled-program' if 'gpu_program_application_marker' in names else
        'shared-usb-pending' if 'gpu_transport_only_marker' in names else
        'fixed-arithmetic-repeat' if single else 'fixed-clear')
    return dict(target=('sunstone-a733-gpu-texture-single-core' if texture else
        'sunstone-a733-gpu-draw-single-core' if draw else
        'sunstone-a733-gpu-compute-single-core' if single else 'sunstone-a733-gpu-render'),
        execution_profile='gpu-single-core-supervised-v2' if single else 'gpu-supervised-v2',
        workload=workload,runtime_input='gpu_runtime_input_marker' in names)

def image_manifest_check(manifest,image,session=None):
    """Bind the execution profile to immutable native startup, not a mode label."""
    import io
    from elftools.elf.elffile import ELFFile
    profiles={'sunstone-a733-gpu-render':'gpu-supervised-v2',
              'sunstone-a733-gpu-compute-single-core':'gpu-single-core-supervised-v2',
              'sunstone-a733-gpu-draw-single-core':'gpu-single-core-supervised-v2',
              'sunstone-a733-gpu-texture-single-core':'gpu-single-core-supervised-v2'}
    manifest=native_manifest(image)
    target=manifest.get('target')
    require(target in profiles and manifest.get('execution_profile')==profiles[target],'Supervised GPU manifest required.')
    if session is not None:
        session_check(session)
        mode=struct.unpack_from('<I',session,532)[0]
        require(mode in (1,2) and (mode==1 or target in ('sunstone-a733-gpu-compute-single-core','sunstone-a733-gpu-draw-single-core','sunstone-a733-gpu-texture-single-core')),
                'ROM USB0 takeover requires the single-core GPU image.')
    elf=ELFFile(io.BytesIO(image));table=elf.get_section_by_name('.symtab')
    require(table is not None,'GPU image symbols absent.')
    symbols={s.name:(int(s['st_value']),int(s['st_size'])) for s in table.iter_symbols()}
    marker=symbols.get('gpu_single_core_geometry')
    if target=='sunstone-a733-gpu-render':
        require(marker is None and manifest.get('workload')=='fixed-clear',
                'Single-core GPU image requires its own execution profile.')
        return
    workload=manifest.get('workload')
    require(workload in ('fixed-arithmetic-repeat','shared-usb-pending','trusted-compiled-program','copied-triangle-draw','copied-indexed-texture-draw') and marker is not None and marker[1]==32 and
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
    transport=symbols.get('gpu_transport_only_marker')
    program=symbols.get('gpu_program_application_marker')
    runtime=symbols.get('gpu_runtime_input_marker')
    draw=symbols.get('gpu_runtime_draw_marker')
    require((target in ('sunstone-a733-gpu-draw-single-core','sunstone-a733-gpu-texture-single-core'))==(workload in ('copied-triangle-draw','copied-indexed-texture-draw'))==(draw is not None),
            'GPU copied-draw profile differs.')
    if draw is not None:
        first,size=draw
        parts=[p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and not int(p['p_flags'])&2 and
               int(p['p_vaddr'])<=first and first+size<=int(p['p_vaddr'])+int(p['p_filesz'])]
        require(size==16 and len(parts)==1,'GPU draw marker must be immutable/file-backed.')
        offset=first-int(parts[0]['p_vaddr'])
        require(parts[0].data()[offset:offset+size]==struct.pack('<2Q',0x4750554452415731,1),
                'GPU draw marker differs.')
    texture=symbols.get('gpu_runtime_texture_marker')
    require((target=='sunstone-a733-gpu-texture-single-core')==(workload=='copied-indexed-texture-draw')==(texture is not None),
            'GPU indexed texture profile differs.')
    if session is not None:
        require((len(session)==632)==(texture is not None),'GPU texture session/image mismatch.')
    if texture is not None:
        first,size=texture
        parts=[p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and not int(p['p_flags'])&2 and
               int(p['p_vaddr'])<=first and first+size<=int(p['p_vaddr'])+int(p['p_filesz'])]
        require(size==16 and len(parts)==1,'GPU texture marker must be immutable/file-backed.')
        offset=first-int(parts[0]['p_vaddr'])
        require(parts[0].data()[offset:offset+size]==struct.pack('<2Q',0x4750555458543031,2),
                'GPU texture marker differs.')
    version=struct.unpack_from('<Q',session,600)[0] if texture is not None and session is not None else 0
    if version>=2:
        # Session versions 2 and 3 select graphics profiles 3 and 4.
        name,magic=('gpu_depth_blend_marker',0x4750554450424c31) if version==2 else ('gpu_batch_marker',0x4750554241544331)
        marker=symbols.get(name)
        require(marker is not None and marker[1]==16,'GPU graphics profile capability absent.')
        first,size=marker
        parts=[p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and not int(p['p_flags'])&2 and
               int(p['p_vaddr'])<=first and first+size<=int(p['p_vaddr'])+int(p['p_filesz'])]
        require(len(parts)==1 and parts[0].data()[first-int(parts[0]['p_vaddr']):first-int(parts[0]['p_vaddr'])+size]==
                struct.pack('<2Q',magic,version+1),'GPU graphics profile marker differs.')
    require(manifest.get('runtime_input',False) is (runtime is not None), 'GPU runtime-input profile differs.')
    if runtime is not None:
        require(workload=='trusted-compiled-program' and runtime[1]==16,'GPU runtime input requires a program image.')
        first,size=runtime
        parts=[p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and not int(p['p_flags'])&2 and
               int(p['p_vaddr'])<=first and first+size<=int(p['p_vaddr'])+int(p['p_filesz'])]
        require(len(parts)==1,'GPU runtime-input marker must be immutable/file-backed.')
        offset=first-int(parts[0]['p_vaddr'])
        require(parts[0].data()[offset:offset+size]==struct.pack('<2Q',0x475055494e503032,2),
                'GPU runtime-input marker differs.')
    require((workload=='trusted-compiled-program')==(program is not None), 'GPU program profile differs.')
    if session is not None:
        require((len(session)==8944)==(workload=='trusted-compiled-program'), 'GPU session profile differs.')
    if workload!='shared-usb-pending':
        require(transport is None,'Transport-only image cannot claim GPU computation.')
    else:
        require(transport is not None and transport[1]==16,'Transport-only image marker absent.')
        first,size=transport
        parts=[p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and not int(p['p_flags'])&2 and
               int(p['p_vaddr'])<=first and first+size<=int(p['p_vaddr'])+int(p['p_filesz'])]
        require(len(parts)==1,'Transport-only marker must be immutable/file-backed.')
        offset=first-int(parts[0]['p_vaddr'])
        require(parts[0].data()[offset:offset+size]==struct.pack('<2Q',0x4750555553423031,1),
                'Transport-only marker differs.')

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

output_directory=module('accelerator_host_evidence',ROOT/'tools/host_evidence.py').new_directory

def prepare(image,manifest,admission,session_template,output,clang,lld):
    output=Path(output);image_data=Path(image).read_bytes();manifest_data=json.dumps(native_manifest(image_data)).encode()
    admission_data=Path(admission).read_bytes() if admission else b'{}';template=Path(session_template).read_bytes()
    image_hash=hashlib.sha256(image_data).hexdigest();m=json.loads(manifest_data);record=json.loads(admission_data)
    image_manifest_check(m,image_data,template)
    cache=validate_admission(record,image_hash,hashlib.sha256(template).hexdigest());session_check(template)
    program_admission(record,cache,template)
    entry=template[80:112];record['reservation']=struct.unpack_from('<Q',template,200)[0]
    require(any(entry) and struct.unpack_from('<Q',template,192)[0] and record['reservation'],
        'Native GPU session identities must be nonzero.')
    output=output_directory(output)
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
        'physical_access':False,'retention':'whole-board-power-removal','files':{name:digest(output/name) for name in {'launch.bin', 'contract.bin', 'session.bin', 'request.bin', 'report-zero.bin', 'image.elf', 'result-zero.bin'}|{v['name'] for v in uploads}}}
    save_json(output/'prepared.json',p);return validate_bundle(output)

def validate_bundle(bundle):
    bundle=Path(bundle);p=json.loads((bundle/'prepared.json').read_text())
    require(p.get('schema')==SCHEMA and p.get('physical_access') is False and p.get('retention')=='whole-board-power-removal','Unsupported GPU bundle.')
    runtime={'report-zero.bin', 'result-zero.bin', 'contract.bin', 'launch.bin', 'session.bin', 'request.bin', 'image.elf'}|{v['name'] for v in p['segments']}
    p['files']={name:p['files'][name] for name in runtime};p['entry_evidence']={}
    for name,h in p['files'].items():require(Path(name).name==name and digest(bundle/name)==h,'GPU bundle input changed.')
    require(digest(bundle/'image.elf')==p['image_sha256'] and digest(bundle/'launch.bin')==p['launcher_sha256'],'GPU executable changed.')
    cache={}
    validate_admission({},p['image_sha256'],None,cache,True)
    image_manifest_check({},(bundle/'image.elf').read_bytes(),(bundle/'session.bin').read_bytes())
    raw=(bundle/'session.bin').read_bytes();session_check(raw);program_admission({},cache,raw);parts,address,identity=image_parts(bundle/'image.elf')
    require(struct.unpack_from('<3Q',raw,24)==(p['epoch'],p['generation'],p['lease']) and
        raw[48:80]==identity and p['build_id']==identity.hex() and p['reservation']==struct.unpack_from('<Q',raw,200)[0],
        'GPU native session identity differs.')
    entry=raw[80:112]
    require(any(entry) and struct.unpack_from('<Q',raw,192)[0],'Native GPU session identities must be nonzero.')
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
    for name in ('image','session-template','output','clang','lld'):parser.add_argument('--'+name,type=Path,required=True)
    for name in ('manifest','admission'):parser.add_argument('--'+name,type=Path)
    a=parser.parse_args();prepare(a.image,a.manifest,a.admission,a.session_template,a.output,a.clang,a.lld)
