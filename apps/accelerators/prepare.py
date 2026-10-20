#!/usr/bin/env python3
"""Freeze an explicitly admitted combined accelerator session without device access."""
import argparse
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile

ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
npu=module('accelerator_npu_preparation',ROOT/'apps/npu-convolution/prepare-supervised.py')
supervisor,protocol=npu.supervisor,npu.protocol
require,digest,save_json,sha=npu.require,npu.digest,npu.save_json,npu.sha
SCHEMA='a733-accelerators-supervised-bundle-v1'
SESSION_MAGIC=0x4153373341434131
CONTRACT_MAGIC=0x4153373341434331
SESSION_ADDRESS=0x41f00000
PROFILE='accelerators-single-core-supervised-v1'
# This supported ABI is checked against the selected image's DWARF, not inferred
# from the enclosing structure's size. Stripped or changed images are refused.
NPU_LAYOUT=(608,224,296,520,88,112,160,192)
GPU_CONDITIONS={
 'initial':{'initial-power-removal-and-gpu-drain','gpu-reset-clocks-off-no-requests',
     'remote-gpu-masters-excluded','identity-device-gpu-gic-mappings','irq-ownership'},
 'memory':{'firmware-and-workload-reservations','all-gpu-address-paths-confined',
     'no-physical-aliases','cache-publication-and-output-acquisition'},
 'electrical':{'gpu-pll-400mhz-parent','gpu-dcdc4-800mv-settled',
     'shared-ccu-pck-pmic-ownership','npu-gpu-concurrent-clock-power-exclusion'},
 'supervision':{'manual-whole-board-power-removal','retain-all-resources-until-power-removal',
     'stop-after-first-failure','combined-accelerator-lifetime'}}

def dwarf_layout(elf):
    require(elf.has_dwarf_info(),'Combined image must retain its ABI debug information.')
    wanted={
      'npu_session':(608,{'boot':224,'pll':520}),
      'a733_npu_application_boot':(296,{'npu':112,'pmic':160,'pins':192}),
      'a733_npu_pll_config':(88,{}),
      'accelerator_session':(9080,{'npu':24,'gpu_initial':632,'package_digest':760,'package':792})}
    found={}
    for cu in elf.get_dwarf_info().iter_CUs():
        for die in cu.iter_DIEs():
            name=die.attributes.get('DW_AT_name');size=die.attributes.get('DW_AT_byte_size')
            if die.tag!='DW_TAG_structure_type' or not name or not size:continue
            name=name.value.decode()
            if name not in wanted:continue
            actual={c.attributes['DW_AT_name'].value.decode():c.attributes['DW_AT_data_member_location'].value
                for c in die.iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_name' in c.attributes and 'DW_AT_data_member_location' in c.attributes}
            expected,members=wanted[name]
            require(size.value==expected and all(actual.get(k)==v for k,v in members.items()),'Native ABI drift: '+name)
            found[name]=True
    require(set(found)==set(wanted),'Combined image lacks complete supported ABI declarations.')

def image_data(raw):
    elf=ELFFile(io.BytesIO(raw))
    require(elf.elfclass==64 and elf.little_endian and elf['e_machine']=='EM_AARCH64' and
        elf['e_type']=='ET_EXEC' and elf['e_entry']==0x41000000,'Invalid combined native image.')
    parts=[]
    for p in elf.iter_segments():
        if p['p_type']!='PT_LOAD':continue
        a,m,n,o,flags=(int(p[k]) for k in ('p_vaddr','p_memsz','p_filesz','p_offset','p_flags'))
        require(a==p['p_paddr'] and 0x41000000<=a<a+m<=0x42200000 and 0<=n<=m and
            0<=o<=o+n<=len(raw) and flags&4 and not flags&~7 and flags&3!=3,'Invalid combined segment.')
        require(not flags&1 or a+m<=0x41200000,'Executable allocation exceeds immutable code.')
        require(not n or a+n<=0x42000000,'Initialized upload overlaps monitor.')
        require(a+m<=SESSION_ADDRESS or a>=SESSION_ADDRESS+65536,'Image overlaps complete session reservation.')
        require(all(a+m<=q['address'] or q['address']+q['memory_bytes']<=a for q in parts),'Image allocations overlap.')
        parts.append({'address':a,'memory_bytes':m,'flags':flags,'data':raw[o:o+n]})
    table=elf.get_section_by_name('.symtab');require(table is not None,'Native symbols required.')
    symbols={s.name:s for s in table.iter_symbols()}
    def read(name,size):
        require(name in symbols and symbols[name]['st_size']==size,'Missing/excess immutable symbol: '+name)
        a=int(symbols[name]['st_value'])
        for p in parts:
            if p['address']<=a and a+size<=p['address']+len(p['data']):
                require(not p['flags']&2,'Writable immutable symbol: '+name)
                return a,p['data'][a-p['address']:a-p['address']+size]
        raise ValueError('Unbacked immutable symbol: '+name)
    require(symbols.get('sd_single_core_entry') is not None and symbols['sd_single_core_entry']['st_value']==elf['e_entry'] and
        not any(k in symbols for k in ('a733_cpu_start_init','a733_cpu_primary_continue','a733_smp_pre_scheduler','FreeRTOS_SGI_Handler','a733_observe_svc')),'Combined startup differs.')
    for name,address in [('__a733_smp_result',0x42000000),('__a733_smp_contract',0x42004000),('__a733_smp_contract_end',0x42005000),
        ('__device_pool_start',0x41600000),('__device_pool_end',0x41a00000),
        ('__uncached_pool_start',0x41c00000),('__uncached_pool_end',0x41e00000)]:
        require(name in symbols and symbols[name]['st_value']==address,'Combined linker reservation differs.')
    address,build=read('a733_smp_build_id',32)
    require(0x41000000<=address<=0x411fffe0 and any(build),'Build identity outside executable prefix.')
    layout=struct.unpack('<8Q',read('accelerator_session_layout',64)[1])
    require(layout==(9080,24,608,632,760,792,8288,20),'Unsupported combined session layout.')
    require(read('accelerator_single_core_geometry',32)[1]==struct.pack('<4Q',SESSION_MAGIC,1,layout[0],20),'Combined execution geometry differs.')
    dwarf_layout(elf)
    model=npu.selected_model(raw);require(model.get('family')=='nn-pool','Complete NN pool model required.')
    hashes=[]
    for name,(size,expected) in protocol.selected_artifacts({'model_name':model['name']}).items():
        require(sha(read('a733_npu_application_'+name,size)[1])==expected,'Embedded NPU artifact differs.')
        hashes.append(bytes.fromhex(expected))
    require(read('a733_npu_application_artifact_hashes',96)[1]==b''.join(hashes),'NPU artifact table differs.')
    start,end=(int(symbols[k]['st_value']) for k in ('gpu_firmware','gpu_firmware_end'))
    firmware=next((p['data'][start-p['address']:end-p['address']] for p in parts if p['address']<=start<end<=p['address']+len(p['data'])),None)
    pin=json.loads((ROOT/'sources.lock.json').read_text())['gpu-firmware']
    require(firmware is not None and sha(firmware)==pin['sha256'],'Embedded GPU firmware differs from pinned provenance.')
    return parts,address,build,layout

def image_manifest_check(manifest,raw,session=None):
    _,_,build,layout=image_data(raw)
    require(manifest.get('target')=='sunstone-a733-accelerators' and manifest.get('execution_profile')==PROFILE and
        manifest.get('workload')=='interleaved-gpu-compute-complete-nn' and manifest.get('final_elf')==sha(raw) and
        manifest.get('build_id')==build.hex(),'Exact combined image manifest required.')
    if session is not None:session_profile(session,layout)
    return PROFILE

def session_profile(raw,layout):
    size,no,ns,go,ho,po,ps,_=layout
    require(len(raw)==size and struct.unpack_from('<3Q',raw)==(SESSION_MAGIC,1,size),'Combined session envelope differs.')
    inner=raw[no:no+ns];p=npu.session_profile(inner,NPU_LAYOUT)
    require(struct.unpack_from('<Q',inner,8)[0]==2 and all(p[k] not in (9,10) for k in ('npu_owner','pmic_owner','usb_owner','bridge_owner')),'Combined owners/version conflict.')
    require(all(any(raw[go+i:go+i+32]) for i in range(0,128,32)) and raw[ho:ho+32]==hashlib.sha256(raw[po:po+ps]).digest(),'Combined GPU evidence/package digest differs.')
    return p

def assemble_session(npu_session,package,admission_raw,layout):
    """Copy typed inputs and reviewed proof digests into the native envelope."""
    size,no,ns,go,ho,po,ps,_=layout;record=json.loads(admission_raw)
    require(len(npu_session)==ns and len(package)==ps,'Combined input extent differs.')
    raw=bytearray(size);struct.pack_into('<3Q',raw,0,SESSION_MAGIC,1,size)
    raw[no:no+ns]=npu_session
    for i,name in enumerate(GPU_CONDITIONS):
        ref=record['gpu_'+name]
        require(isinstance(ref,dict) and set(ref)=={'path','sha256'} and Path(ref['path']).is_absolute(),'GPU proof reference differs.')
        data=Path(ref['path']).read_bytes();require(sha(data)==ref['sha256'],'GPU proof changed during assembly.')
        raw[go+32*i:go+32*(i+1)]=hashlib.sha256(data).digest()
    raw[ho:ho+32]=hashlib.sha256(package).digest();raw[po:po+ps]=package
    session_profile(bytes(raw),layout);return bytes(raw)

def validate_admission(admission_raw,raw,build,layout,image_hash,cache=None,frozen=False):
    cache={} if cache is None else cache
    def evidence(ref):
        require(isinstance(ref,dict) and set(ref)=={'path','sha256'} and isinstance(ref['path'],str) and Path(ref['path']).is_absolute(),'Absolute hashed proof reference required.')
        name=ref['path']
        if name not in cache:
            require(not frozen,'Missing frozen combined proof.');cache[name]=Path(name).read_bytes()
        require(sha(cache[name])==ref['sha256'],'Combined proof changed.');return cache[name]
    record=json.loads(admission_raw);profile=session_profile(raw,layout)
    require(record.get('schema')=='a733-accelerators-supervised-entry-v1' and record.get('image_sha256')==image_hash and
        record.get('execution_profile')==PROFILE and record.get('combined_lifetime') is True,'Explicit combined physical admission required.')
    no,ns,go,po,ps=layout[1],layout[2],layout[3],layout[5],layout[6]
    nr=evidence(record['npu']);nrecord=json.loads(nr)
    npu.validate_admission(nr,raw[no:no+ns],build,NPU_LAYOUT,image_hash,cache,frozen)
    require(all(record.get(k)==nrecord.get(k) for k in ('identity','port','whole_platform','supervisor')) and
        record.get('identity') and record.get('port'),'Combined target/retention identity differs.')
    for i,(name,conditions) in enumerate(GPU_CONDITIONS.items()):
        data=evidence(record['gpu_'+name]);facts=json.loads(data)
        require(hashlib.sha256(data).digest()==raw[go+32*i:go+32*(i+1)],'GPU session proof digest differs: '+name)
        require(facts.get('schema')=='a733-accelerators-gpu-'+name+'-v1' and
            all(facts.get(k)==record[k] for k in ('identity','port','image_sha256','whole_platform','supervisor')) and
            facts.get('combined_lifetime') is True and set(facts.get('conditions',{}))==conditions,'GPU proof does not cover combined lifetime: '+name)
        for ref in facts['conditions'].values():evidence(ref)
        if name=='memory':
            require(facts.get('ranges')==[[0x41600000,0x41a00000],[0x41c00000,0x41e00000]],'Combined shared pool ranges differ.')
        if name=='supervision':require(facts.get('one_attempt') is True,'One-attempt retained recovery required.')
    package=raw[po:po+ps];program=json.loads(evidence(record['gpu_program']))
    require(program.get('schema')=='sunstone-gpu-trusted-program-v1' and program.get('source_reviewed') is True and
        program.get('package_sha256')==sha(package) and program.get('source_sha256')==package[80:112].hex() and
        program.get('compiler_sha256')==package[112:144].hex(),'GPU trusted source/compiler/package provenance differs.')
    require(sha(evidence(program['source']))==program['source_sha256'] and sha(evidence(program['compiler']))==program['compiler_sha256'],'GPU provenance bytes differ.')
    fw=evidence(record['gpu_firmware']);pin=json.loads((ROOT/'sources.lock.json').read_text())['gpu-firmware']
    require(sha(fw)==pin['sha256'],'GPU firmware provenance differs.')
    return cache

def request_bytes(address,build,raw,profile):
    request,_=npu.request_bytes(address,build,raw,profile)
    epoch,generation,lease=(profile[k] for k in ('epoch','generation','lease'))
    contract=struct.pack('<6Q',0,1,224,epoch,generation,generation^((1<<64)-1))+build
    contract+=struct.pack('<14Q',0,0,0,0,lease,0,0,0,0,0,0,0,profile['supervisor'],len(raw))+hashlib.sha256(raw).digest()
    require(len(contract)==224,'Combined contract differs.');return request,contract+bytes(4096-len(contract))

def prepare(image,manifest,session,admission,output,clang,lld,objcopy,*,package=None):
    image_raw=Path(image).read_bytes();manifest_raw=Path(manifest).read_bytes();raw=Path(session).read_bytes();admission_raw=Path(admission).read_bytes()
    parts,address,build,layout=image_data(image_raw)
    if package is not None:raw=assemble_session(raw,Path(package).read_bytes(),admission_raw,layout)
    profile=session_profile(raw,layout)
    image_manifest_check(json.loads(manifest_raw),image_raw,raw)
    cache=validate_admission(admission_raw,raw,build,layout,sha(image_raw))
    output=Path(output);output.mkdir(parents=True,exist_ok=False)
    npu.build_launcher(output,clang,lld,objcopy,contract_magic=CONTRACT_MAGIC,contract_version=1,session_bytes=len(raw))
    fixed={'image.elf':image_raw,'image-input-manifest.json':manifest_raw,'admission.json':admission_raw,'session.bin':raw};uploads=[]
    for i,p in enumerate(parts):
        if p['data']:
            name=f'segment-{i}.bin';fixed[name]=p['data'];uploads.append({'name':name,'address':p['address']})
    uploads.append({'name':'session.bin','address':SESSION_ADDRESS});evidence={}
    for i,(path,data) in enumerate(cache.items()):
        name=f'entry-evidence-{i}.bin';fixed[name]=data;evidence[path]={'name':name,'sha256':sha(data)}
    request,contract=request_bytes(address,build,raw,profile)
    fixed.update({'request.bin':request,'contract.bin':contract,'result-zero.bin':bytes(16384),'report-zero.bin':bytes(128)})
    for name,data in fixed.items():(output/name).write_bytes(data)
    p={'schema':SCHEMA,'physical_access':False,'image_sha256':sha(image_raw),'build_id':build.hex(),
       'model_name':npu.selected_model(image_raw)['name'],'execution_profile':PROFILE,**profile,
       'session_layout':list(layout),'segments':uploads,'entry_evidence':evidence,'launcher_sha256':digest(output/'launch.bin'),
       'files':{p.name:digest(p) for p in output.iterdir() if p.is_file()}}
    save_json(output/'prepared.json',p);return validate_bundle(output)

def validate_bundle(bundle):
    bundle=Path(bundle);metadata=(bundle/'prepared.json').read_bytes();p=json.loads(metadata)
    require(p.get('schema')==SCHEMA and p.get('physical_access') is False,'Unsupported combined bundle.')
    data={}
    for name,expected in p['files'].items():
        require(Path(name).name==name,'Invalid prepared filename.');data[name]=(bundle/name).read_bytes();require(sha(data[name])==expected,'Prepared bytes changed: '+name)
    parts,address,build,layout=image_data(data['image.elf']);raw=data['session.bin'];profile=session_profile(raw,layout)
    require(p['image_sha256']==sha(data['image.elf']) and p['build_id']==build.hex() and p['session_layout']==list(layout) and
        p['model_name']==npu.selected_model(data['image.elf'])['name'] and all(p.get(k)==v for k,v in profile.items()),'Combined metadata differs.')
    require(p['execution_profile']==image_manifest_check(json.loads(data['image-input-manifest.json']),data['image.elf'],raw),'Combined profile differs.')
    cache={path:data[item['name']] for path,item in p['entry_evidence'].items()}
    require(all(sha(cache[path])==item['sha256'] for path,item in p['entry_evidence'].items()),'Frozen proof differs.')
    validate_admission(data['admission.json'],raw,build,layout,p['image_sha256'],cache,True)
    expected=[]
    for i,part in enumerate(parts):
        if part['data']:
            name=f'segment-{i}.bin';require(data[name]==part['data'],'ELF upload differs.');expected.append({'name':name,'address':part['address']})
    expected.append({'name':'session.bin','address':SESSION_ADDRESS});require(p['segments']==expected,'Combined uploads differ.')
    for name,value in zip(('request.bin','contract.bin'),request_bytes(address,build,raw,profile)):require(data[name]==value,'Combined launch identity differs.')
    require(data['result-zero.bin']==bytes(16384) and data['report-zero.bin']==bytes(128) and 0<len(data['launch.bin'])<=0xc00 and sha(data['launch.bin'])==p['launcher_sha256'],'Launcher/result reservation differs.')
    p['_sizes']={k:len(v) for k,v in data.items()};p['_frozen']=data;p['_prepared_sha256']=sha(metadata);return p

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','admission','output','clang','lld','objcopy'):parser.add_argument('--'+name,type=Path,required=True)
    group=parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--session',type=Path,help='Complete reviewed combined session')
    group.add_argument('--npu-session',type=Path,help='Reviewed embedded NPU session to copy into a combined envelope')
    parser.add_argument('--package',type=Path,help='Reviewed GPU package; required with --npu-session')
    a=parser.parse_args()
    if bool(a.npu_session)!=bool(a.package):parser.error('--package and --npu-session must be supplied together')
    prepare(a.image,a.manifest,a.session or a.npu_session,a.admission,a.output,a.clang,a.lld,a.objcopy,package=a.package)
