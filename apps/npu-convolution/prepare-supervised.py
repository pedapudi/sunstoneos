#!/usr/bin/env python3
"""Package native supervised NPU inputs without device access."""
import argparse
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[2]

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
supervisor=module('npu_prepare_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
protocol=module('npu_prepare_protocol',ROOT/'apps/npu-convolution/observe.py')
require,digest,save_json=supervisor.require,supervisor.digest,supervisor.save_json
SESSION_MAGIC=0x415337334e504131
CONTRACT_MAGIC=0x415337334e504331
SCHEMA='a733-npu-supervised-bundle-v1'
SESSION_ADDRESS=0x41f00000
PROFILE_FIELDS=('npu_evidence','command_admission','usb_evidence','pmic_evidence','pin_evidence',
    'npu_owner','pmic_owner','usb_owner','bridge_owner')
# Session versions: 3 for the single-core profiles, 2 for the all-core one.
SINGLE_CORE_VERSION,ALL_CORE_VERSION=3,2
def sha(data):return hashlib.sha256(data).hexdigest()


def selected_model(raw):
    elf=ELFFile(io.BytesIO(raw));table=elf.get_section_by_name('.symtab')
    require(table is not None,'Native symbol table required.')
    matches=[v for v in table.iter_symbols() if v.name=='a733_npu_application_model']
    require(len(matches)==1,'Unique immutable model required.')
    symbol=matches[0];section=elf.get_section(symbol['st_shndx'])
    require(not section['sh_flags']&1,'Model is writable.')
    offset=symbol['st_value']-section['sh_addr'];data=section.data()[offset:offset+symbol['st_size']]
    selected=next((v for v in protocol.CATALOGUE if v['model']['bytes']==len(data) and v['model']['sha256']==sha(data)),None)
    require(selected is not None,'Model is absent from reviewed catalogue.')
    return selected


def image_data(raw):
    """Validate the compact image before admitting any load or session upload."""
    elf=ELFFile(io.BytesIO(raw))
    require(elf.elfclass==64 and elf.little_endian and elf['e_machine']=='EM_AARCH64' and
        elf['e_type']=='ET_EXEC' and elf['e_entry']==0x41000000,'Invalid native image header.')
    parts=[]
    for p in elf.iter_segments():
        if p['p_type']!='PT_LOAD':continue
        a,m,n,o,flags=(int(p[k]) for k in ('p_vaddr','p_memsz','p_filesz','p_offset','p_flags'))
        require(a==p['p_paddr'] and 0x41000000<=a<a+m<=0x42200000 and 0<=n<=m and
            0<=o<=o+n<=len(raw) and not flags&~7 and flags&4 and flags&3!=3,'Invalid compact DDR segment.')
        require(not flags&1 or a+m<=0x41200000,'Executable segment exceeds immutable code.')
        require(not n or a+n<=0x42000000,'Upload overlaps retained monitor/contract.')
        require(a+m<=SESSION_ADDRESS or a>=SESSION_ADDRESS+4096,'Image overlaps immutable session page.')
        require(all(a+m<=q['address'] or q['address']+q['memory_bytes']<=a for q in parts),'Overlapping image allocations.')
        parts.append({'address':a,'memory_bytes':m,'flags':flags,'data':raw[o:o+n]})
    require(any(p['address']==0x41000000 and p['flags']&1 and p['data'] for p in parts),'Missing executable entry.')
    table=elf.get_section_by_name('.symtab');require(table is not None,'Native symbol table required.')
    symbols={s.name:s for s in table.iter_symbols()}
    def read(name,n):
        require(name in symbols and symbols[name]['st_size']>=n,'Missing immutable symbol: '+name)
        a=int(symbols[name]['st_value'])
        for p in parts:
            if p['address']<=a and a+n<=p['address']+len(p['data']):
                require(not p['flags']&2,'Immutable symbol is writable: '+name)
                return a,p['data'][a-p['address']:a-p['address']+n]
        raise ValueError('Symbol has no initialized bytes: '+name)
    for name,a in [('__a733_smp_result',0x42000000),('__a733_smp_contract',0x42004000),('__a733_smp_contract_end',0x42005000)]:
        require(name in symbols and int(symbols[name]['st_value'])==a,'Compact linker reservation differs.')
    address,build=read('a733_smp_build_id',32)
    require(0x41000000<=address<=0x411fffe0,'Build identity outside immutable code.')
    _,layout_raw=read('a733_npu_session_layout',48);layout=struct.unpack('<6Q',layout_raw)
    size,boot,boot_bytes,npu,pmic,pins=layout
    require(224==boot and 72<=boot_bytes<4096 and boot+boot_bytes==size and size<=4096 and not size%8 and
        72==npu and npu+24<=pmic<pmic+24<=pins<boot_bytes,'Invalid exported native session layout.')
    hashes=[]
    selected=selected_model(raw)
    for name,(n,expected) in protocol.selected_artifacts({'model_name':selected['name']}).items():
        _,data=read('a733_npu_application_'+name,n);require(sha(data)==expected,'Fixed NPU artifact differs: '+name)
        hashes.append(bytes.fromhex(expected))
    require(read('a733_npu_application_artifact_hashes',96)[1]==b''.join(hashes),'Embedded artifact identity table differs.')
    return parts,address,build,layout


def image_manifest_check(manifest,raw,session=None):
    profiles={'a733-npu-convolution-supervised':'npu-supervised-v1',
              'a733-npu-convolution-single-core':'npu-single-core-supervised-v1'}
    elf=ELFFile(io.BytesIO(raw));symbols={v.name:v for v in elf.get_section_by_name('.symtab').iter_symbols()}
    target='a733-npu-convolution-single-core' if 'npu_single_core_geometry' in symbols else 'a733-npu-convolution-supervised'
    profile=profiles[target]
    runtime=symbols.get('npu_runtime_geometry')
    if runtime:
        require(target=='a733-npu-convolution-single-core','Runtime requires single-core image.')
        section=elf.get_section(runtime['st_shndx']);offset=runtime['st_value']-section['sh_addr']
        require(not section['sh_flags']&1,'Runtime geometry is writable.')
        geometry=section.data()[offset:offset+runtime['st_size']]
        if runtime['st_size']==48:
            model=selected_model(raw)
            require(struct.unpack('<6Q',geometry)==(0x415337334e504c32,2,64,512,model['input']['bytes'],model['reference']['bytes']),
                'Large runtime geometry differs.')
            require(model.get('family') in ('sh-nt','nn-pool'),'Large runtime model family differs.')
            profile='npu-runtime-large-single-core-supervised-v1'
        else:
            require(runtime['st_size']==32 and struct.unpack('<4Q',geometry)==(0x415337334e505232,1,64,512),
                'Runtime geometry differs.')
            require(selected_model(raw).get('family','nn')=='nn','Small runtime model family differs.')
            profile='npu-runtime-single-core-supervised-v1'
    elf=ELFFile(io.BytesIO(raw));symbols={v.name:v for v in elf.get_section_by_name('.symtab').iter_symbols()}
    marker=symbols.get('npu_single_core_geometry')
    if target=='a733-npu-convolution-single-core':
        require(marker is not None and marker['st_size']==32 and
            ('sd_single_core_entry' in symbols and symbols['sd_single_core_entry']['st_value'])==elf['e_entry'] and
            not any(name in symbols for name in ('a733_cpu_start_init','a733_cpu_primary_continue',
                'a733_smp_pre_scheduler','FreeRTOS_SGI_Handler','a733_observe_svc')),
            'Single-core NPU startup differs.')
        address=int(marker['st_value']);parts=[v for v in elf.iter_segments() if v['p_type']=='PT_LOAD' and
            not v['p_flags']&2 and v['p_vaddr']<=address and address+32<=v['p_vaddr']+v['p_filesz']]
        require(len(parts)==1,'NPU geometry must be immutable and file backed.')
        offset=address-parts[0]['p_vaddr'];geometry=struct.unpack('<4Q',parts[0].data()[offset:offset+32])
        require(geometry==(0x415337334e505331,1,image_data(raw)[3][0],20),'NPU execution geometry differs.')
    else:require(marker is None,'Single-core NPU image requires its execution profile.')
    if session is not None:
        require(struct.unpack_from('<Q',session,8)[0]==(SINGLE_CORE_VERSION if marker is not None else ALL_CORE_VERSION),
            'Session execution version differs from native image.')
    return profile


def session_profile(raw,layout):
    size,boot,boot_bytes,npu,pmic,pins=layout
    version=struct.unpack_from("<Q",raw,8)[0]
    require(len(raw)==size and struct.unpack_from('<3Q',raw)==(SESSION_MAGIC,version,size) and
        version in (SINGLE_CORE_VERSION,ALL_CORE_VERSION),'Distinct native NPU session required.')
    epoch,generation,lease=struct.unpack_from('<3Q',raw,24);owner,whole=struct.unpack_from('<2Q',raw,208)
    require(all((epoch,generation,lease,owner,whole)),'Session identities must be nonzero.')
    p=dict(zip(PROFILE_FIELDS,struct.unpack_from('<9Q',raw,boot)))
    require(all(p.values()) and len({p[k] for k in ('npu_owner','pmic_owner','usb_owner','bridge_owner')})==4,
        'Supervised NPU profile differs.')
    require(struct.unpack_from('<3Q',raw,boot+pmic)==(p['pmic_owner'],p['pmic_evidence'],0),'Uploaded PMIC deadline differs.')
    return {'epoch':epoch,'generation':generation,'lease':lease,**p,'supervisor':owner,'whole_platform':whole}


def validate_admission(admission_raw,raw,build,layout,image_hash,cache=None,frozen=False):
    session_profile(raw,layout)
    require(raw[48:80]==build,'Session belongs to another native build.')
    require(all(any(raw[o:o+32]) for o in (80,112,144,176)),
        'Native session identity fields must be nonzero.')
    boot,npu=layout[1],layout[3]
    words=struct.unpack_from('<6I',raw,boot+npu)
    masks=(0x0300001f,0x8700001f,0x8700001f)
    parents=((24,0,0,600,0,0,0,0),(24,0,800,600,480,0,0,0),(24,600,0,480,400,0,0,0))
    for i,word in enumerate(words[:3]):
        mhz=parents[i][(word>>24)&7];divisor=(word&31)+1
        require(not word&~masks[i] and (not i or word&(1<<31)) and mhz and
            not (mhz*1000000)%divisor and words[i+3]==mhz*1000000//divisor,
            'Unsupported NPU fabric clock configuration.')
    return {}


def request_bytes(address,build,raw,profile):
    session_hash=hashlib.sha256(raw).digest();epoch,generation,lease=(profile[k] for k in ('epoch','generation','lease'))
    request=struct.pack('<8Q',0x4153373353554c33,3,128,epoch,generation,generation^((1<<64)-1),lease,address)+build+session_hash
    contract=struct.pack('<6Q',0,3,224,epoch,generation,generation^((1<<64)-1))+build
    contract+=struct.pack('<14Q',0,0,0,0,lease,0,0,0,0,0,0,0,profile['supervisor'],0)+session_hash
    return request,contract+bytes(4096-len(contract))


def build_launcher(output,clang,lld,objcopy,*,contract_magic=CONTRACT_MAGIC,
                   contract_version=3,session_bytes=0):
    require((contract_magic==CONTRACT_MAGIC and contract_version==3 and session_bytes==0) or
        (contract_magic==0x4153373341434331 and contract_version==1 and
         0<session_bytes<=65536 and session_bytes%8==0),'Unsupported retained launcher profile.')
    for name,path in [('launch.c',ROOT/'platform/a733-smp-supervised/launch.c'),('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),
        ('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h')]:
        (output/name).write_bytes(path.read_bytes())
    commands=[]
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin',
        '-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-Wall','-Wextra','-Werror',
        f'-DA733_SMP_CONTRACT_MAGIC={contract_magic}ULL',
        f'-DA733_SUPERVISED_CONTRACT_VERSION={contract_version}',
        f'-DA733_SUPERVISED_SESSION_BYTES={session_bytes}','-I'+str(output)]
    def run(command):
        command=list(map(str,command));commands.append(command);result=subprocess.run(command,capture_output=True,text=True)
        with (output/'build.log').open('a') as f:f.write(result.stdout+result.stderr)
        save_json(output/'commands.json',commands);require(result.returncode==0,'SRAM launcher build failed.')
    for name in ('launch.c','launch.S'):run([clang,*flags,'-MD','-MF',output/(name+'.d'),'-c',output/name,'-o',output/(name+'.o')])
    run([lld,'-T',output/'launch.ld',output/'launch.c.o',output/'launch.S.o','-o',output/'launch.elf'])
    run([objcopy,'-O','binary',output/'launch.elf',output/'launch.bin'])
    require(0<(output/'launch.bin').stat().st_size<=0xc00,'Launcher exceeds SRAM reservation.')


output_directory=module('accelerator_host_evidence',ROOT/'tools/host_evidence.py').unique_directory

def prepare(image,manifest,session,admission,output,clang,lld,objcopy):
    image_raw=Path(image).read_bytes();manifest_raw=Path(manifest).read_bytes() if manifest else b'{}';raw=Path(session).read_bytes();admission_raw=Path(admission).read_bytes() if admission else b'{}'
    parts,address,build,layout=image_data(image_raw);profile=session_profile(raw,layout)
    manifest_record=json.loads(manifest_raw)
    execution_profile=image_manifest_check(manifest_record,image_raw,raw)
    cache=validate_admission(admission_raw,raw,build,layout,sha(image_raw));output=output_directory(output)
    build_launcher(output,clang,lld,objcopy)
    fixed={'image.elf':image_raw,'image-input-manifest.json':manifest_raw,'admission.json':admission_raw,'session.bin':raw}
    uploads=[]
    for i,p in enumerate(parts):
        if p['data']:
            name=f'segment-{i}.bin';fixed[name]=p['data'];uploads.append({'name':name,'address':p['address']})
    uploads.append({'name':'session.bin','address':SESSION_ADDRESS})
    evidence={}
    for i,(path,data) in enumerate(cache.items()):
        name=f'entry-evidence-{i}.bin';fixed[name]=data;evidence[path]={'name':name,'sha256':sha(data)}
    request,contract=request_bytes(address,build,raw,profile)
    fixed.update({'request.bin':request,'contract.bin':contract,'result-zero.bin':bytes(16384),'report-zero.bin':bytes(128)})
    for name,data in fixed.items():(output/name).write_bytes(data)
    p={'schema':SCHEMA,'physical_access':False,'image_sha256':sha(image_raw),'build_id':build.hex(),'model_name':selected_model(image_raw)['name'],'execution_profile':execution_profile,**profile,
        'session_layout':list(layout),'segments':uploads,'entry_evidence':evidence,'launcher_sha256':digest(output/'launch.bin'),
        'files':{name:digest(output/name) for name in {'launch.bin', 'contract.bin', 'session.bin', 'request.bin', 'report-zero.bin', 'image.elf', 'result-zero.bin'}|{v['name'] for v in uploads}}}
    save_json(output/'prepared.json',p);return validate_bundle(output)


def validate_bundle(bundle):
    bundle=Path(bundle);metadata=(bundle/'prepared.json').read_bytes();p=json.loads(metadata);require(p.get('schema')==SCHEMA and p.get('physical_access') is False,'Unsupported NPU bundle.')
    runtime={'report-zero.bin', 'result-zero.bin', 'contract.bin', 'launch.bin', 'session.bin', 'request.bin', 'image.elf'}|{v['name'] for v in p['segments']}
    p['files']={name:p['files'][name] for name in runtime};p['entry_evidence']={}
    data={}
    for name,expected in p['files'].items():
        require(Path(name).name==name,'Invalid prepared filename.');data[name]=(bundle/name).read_bytes()
        require(sha(data[name])==expected,'Prepared input changed: '+name)
    parts,address,build,layout=image_data(data['image.elf']);raw=data['session.bin'];profile=session_profile(raw,layout)
    require(p.get('model_name','conv8')==selected_model(data['image.elf'])['name'],'Selected model differs from image.')
    require(p['image_sha256']==sha(data['image.elf']) and p['build_id']==build.hex() and p['session_layout']==list(layout) and
        all(p.get(k)==v for k,v in profile.items()),'Prepared native/session metadata differs.')
    execution_profile=image_manifest_check({},data['image.elf'],raw)
    require(p.get('execution_profile',execution_profile)==execution_profile,'Prepared execution profile differs.')
    cache={}
    validate_admission(b'{}',raw,build,layout,p['image_sha256'],cache,True)
    expected=[]
    for i,part in enumerate(parts):
        if part['data']:
            name=f'segment-{i}.bin';require(data[name]==part['data'],'ELF upload differs.');expected.append({'name':name,'address':part['address']})
    expected.append({'name':'session.bin','address':SESSION_ADDRESS});require(p['segments']==expected,'Upload list differs.')
    for name,value in zip(('request.bin','contract.bin'),request_bytes(address,build,raw,profile)):require(data[name]==value,'Launch identity differs.')
    require(data['result-zero.bin']==bytes(16384) and data['report-zero.bin']==bytes(128) and
        0<len(data['launch.bin'])<=0xc00 and sha(data['launch.bin'])==p['launcher_sha256'],'Launcher/result reservation differs.')
    p['_sizes']={name:len(value) for name,value in data.items()};p['_frozen']=data;p['_prepared_sha256']=sha(metadata);return p

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','session','output','clang','lld','objcopy'):parser.add_argument('--'+name,type=Path,required=True)
    for name in ('manifest','admission'):parser.add_argument('--'+name,type=Path)
    a=parser.parse_args();prepare(a.image,a.manifest,a.session,a.admission,a.output,a.clang,a.lld,a.objcopy)
