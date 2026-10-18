#!/usr/bin/env python3
"""Freeze a reviewed native I2S session and sixty-second SRAM launch offline."""
import argparse
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import types
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
supervisor=module('i2s_prepare_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
protocol=module('i2s_protocol',ROOT/'apps/i2s/observe.py')
require,digest,save_json=supervisor.require,supervisor.digest,supervisor.save_json
SCHEMA='a733-i2s-supervised-bundle-v1'
SESSION_ADDRESS=0x41f00000
CONTRACT_MAGIC=0x4153373341554331
REQUEST_MAGIC=0x4153373341554c31
CONDITIONS={'dma0-cpu-and-remote-users-excluded','fifo-producers-quiescent-and-prior-dma-drained',
    'dma-route-and-irq-ownership','audio-parent-and-shared-clock-ownership','i2s0-pins-bank-voltage-and-device-wiring',
    'device-mappings-and-cache-alias-exclusion','pmic-usb-inherited-power-and-fixed-sink',
    'manual-whole-board-power-removal','retain-all-resources-until-power-removal','stop-after-first-failure'}
def sha(data):return hashlib.sha256(data).hexdigest()

def session_layout(elf):
    """Use actual linked C layout; no image field is added for packaging."""
    def resolve(d):
        while d.tag in ('DW_TAG_typedef','DW_TAG_const_type','DW_TAG_volatile_type','DW_TAG_restrict_type'):
            d=d.get_DIE_from_attribute('DW_AT_type')
        return d
    def size(d):
        d=resolve(d)
        if 'DW_AT_byte_size' in d.attributes:return int(d.attributes['DW_AT_byte_size'].value)
        require(d.tag=='DW_TAG_array_type','Unsupported native session type')
        n=1
        for c in d.iter_children():
            if c.tag=='DW_TAG_subrange_type':
                n*=int(c.attributes['DW_AT_count'].value) if 'DW_AT_count' in c.attributes else int(c.attributes['DW_AT_upper_bound'].value)+1
        return n*size(d.get_DIE_from_attribute('DW_AT_type'))
    def walk(d,prefix='',base=0):
        fields={};d=resolve(d)
        for c in d.iter_children():
            if c.tag!='DW_TAG_member':continue
            name=c.attributes['DW_AT_name'].value.decode();t=resolve(c.get_DIE_from_attribute('DW_AT_type'))
            require(t.tag!='DW_TAG_pointer_type','I2S launch session must contain only values')
            offset=base+int(c.attributes['DW_AT_data_member_location'].value);key=prefix+name
            fields[key]={'offset':offset,'size':size(t)}
            if t.tag=='DW_TAG_structure_type':fields.update(walk(t,key+'.',offset))
            if t.tag=='DW_TAG_array_type':require(resolve(t.get_DIE_from_attribute('DW_AT_type')).tag!='DW_TAG_pointer_type','Pointer array in native session')
        return fields
    require(elf.has_dwarf_info(),'Native I2S DWARF layout evidence required')
    layouts=[]
    for cu in elf.get_dwarf_info().iter_CUs():
        for d in cu.iter_DIEs():
            if d.tag=='DW_TAG_structure_type' and 'DW_AT_name' in d.attributes and d.attributes['DW_AT_name'].value==b'i2s_session' and 'DW_AT_byte_size' in d.attributes:
                value={'size':size(d),'fields':walk(d)}
                if value not in layouts:layouts.append(value)
    require(len(layouts)==1 and 208<layouts[0]['size']<=4096,'Missing or conflicting native I2S session layout')
    return layouts[0]

def image_data(raw):
    elf=ELFFile(io.BytesIO(raw))
    require(elf.elfclass==64 and elf.little_endian and elf['e_type']=='ET_EXEC' and elf['e_machine']=='EM_AARCH64' and elf['e_entry']==0x41000000,'Native I2S ELF required')
    parts=[]
    for s in elf.iter_segments():
        if s['p_type']!='PT_LOAD':continue
        a,m,n,o,flags=(int(s[k]) for k in ('p_vaddr','p_memsz','p_filesz','p_offset','p_flags'))
        require(a==s['p_paddr'] and 0x41000000<=a<a+m<=0x42200000 and 0<=n<=m and 0<=o<=o+n<=len(raw) and flags&4 and not flags&~7 and flags&3!=3,'I2S segment outside compact DDR ownership')
        require(not flags&1 or a+m<=0x41200000,'Executable I2S data outside code prefix')
        require(not n or a+n<=SESSION_ADDRESS,'I2S initialized bytes overlap session or monitor')
        require(all(a+m<=p['address'] or p['address']+p['memory_bytes']<=a for p in parts),'Overlapping I2S ELF allocations')
        parts.append({'address':a,'memory_bytes':m,'flags':flags,'data':raw[o:o+n]})
    table=elf.get_section_by_name('.symtab');require(table is not None,'Native I2S symbol table required')
    symbols={s.name:s for s in table.iter_symbols()}
    for name,address in [('__a733_smp_result',0x42000000),('__a733_smp_contract',0x42004000),('__a733_smp_contract_end',0x42005000),('__storage_session_start',SESSION_ADDRESS),('__storage_session_end',SESSION_ADDRESS+4096)]:
        require(name in symbols and symbols[name]['st_value']==address,'I2S linker reservation differs: '+name)
    first,last=(int(symbols[n]['st_value']) for n in ('__device_pool_start','__device_pool_end'))
    require(0x41200000<=first<last<=SESSION_ADDRESS and last-first==2097152 and not (first|last)&4095,'I2S DMA pool ownership differs')
    def read(name,n):
        require(name in symbols and symbols[name]['st_size']>=n,'Missing I2S immutable symbol: '+name)
        a=int(symbols[name]['st_value'])
        for p in parts:
            if p['address']<=a and a+n<=p['address']+len(p['data']):
                require(not p['flags']&2,'Writable I2S immutable input')
                return a,p['data'][a-p['address']:a-p['address']+n]
        raise ValueError('I2S immutable input lacks file backing')
    address,identity=read('a733_smp_build_id',32);_,pcm=read('i2s_recorded_pcm',16384)
    require(sha(pcm)==read('i2s_recorded_pcm_digest',32)[1].hex(),'I2S recorded PCM digest differs')
    require(0x41000000<=address<=0x411fffe0 and {'i2s_client_step','i2s_session_valid','i2s_output'}<=set(symbols),'Wrong native application identity')
    return parts,address,identity,session_layout(elf),pcm

def field(raw,layout,name):
    f=layout['fields'][name];return raw[f['offset']:f['offset']+f['size']]
def number(raw,layout,name):return int.from_bytes(field(raw,layout,name),'little')
def session_profile(raw,layout,identity,pcm):
    require(len(raw)==layout['size'] and struct.unpack_from('<3Q',raw)==(0x4153373341555331,1,len(raw)),'Invalid native I2S session')
    values={k:number(raw,layout,k) for k in ['epoch','generation','lease','direction','audio_cold','shared.identity','shared.pmic.owner','shared.pmic.identity','shared.pmic.startup_deadline','shared.usb.owner','shared.usb.handoff','shared.usb.deadline_us','dma.allowed.owner','dma.allowed.handoff','dma.allowed.deadline_us','dma.controller','dma.evidence','dma.established','dma.owned_channels','dma.service_channels']}
    epoch,generation,lease=(values[k] for k in ('epoch','generation','lease'))
    require(all((epoch,generation,lease)) and values['direction']<=1 and values['audio_cold']<=1,'I2S session identities/direction differ')
    require(field(raw,layout,'image')==identity and field(raw,layout,'pcm')==hashlib.sha256(pcm).digest(),'I2S image/recorded PCM binding differs')
    require(all(any(field(raw,layout,k)) for k in ('evidence','memory','recovery')),'I2S prerequisite digests missing')
    require(values['shared.identity']==values['shared.pmic.identity']==values['shared.usb.handoff']==generation and
        values['dma.allowed.handoff']==values['dma.evidence']==lease,'I2S shared/DMA handoff identity differs')
    require(not any(values[k] for k in ('shared.pmic.startup_deadline','shared.usb.deadline_us','dma.allowed.deadline_us')),'Uploaded I2S provider deadlines are forbidden')
    owners=[values[k] for k in ('shared.pmic.owner','shared.usb.owner','dma.allowed.owner')]
    require(all(owners) and len(set(owners))==3,'Conflicting I2S owners')
    require(values['dma.established']==0x5f and values['dma.owned_channels']==0xffff and values['dma.service_channels']==1 and values['dma.controller']==0x04601000,'I2S supervised DMA ownership differs')
    require(field(raw,layout,'dma.allowed.fabric')==field(raw,layout,'shared.usb.fabric') and field(raw,layout,'dma.allowed.rate_hz')==field(raw,layout,'shared.usb.rate_hz'),'I2S DMA/USB fabric profiles differ')
    return {k:values[k] for k in ('epoch','generation','lease','direction','audio_cold')}

def validate_admission(admission_raw,raw,layout,identity,pcm,image_hash,cache=None,frozen=False):
    record=json.loads(admission_raw);profile=session_profile(raw,layout,identity,pcm)
    require(record.get('schema')=='a733-i2s-supervised-entry-v1' and record.get('image_sha256')==image_hash and
        field(raw,layout,'evidence')==hashlib.sha256(admission_raw).digest() and all(record.get(k)==v for k,v in profile.items()),'I2S session does not bind reviewed image/entry')
    require(record.get('shared_sha256')==sha(field(raw,layout,'shared')) and record.get('dma_sha256')==sha(field(raw,layout,'dma')) and
        record.get('inherited_sha256')==sha(field(raw,layout,'inherited')),'I2S typed configuration differs from reviewed facts')
    cache={} if cache is None else cache
    def evidence(ref):
        require(isinstance(ref,dict) and set(ref)=={'path','sha256'},'Invalid I2S evidence reference')
        p=Path(ref['path']);require(p.is_absolute(),'I2S evidence path must be absolute')
        if str(p) not in cache:require(not frozen,'Missing frozen I2S entry evidence');cache[str(p)]=p.read_bytes()
        data=cache[str(p)];require(sha(data)==ref['sha256'],'I2S entry evidence changed');return data
    target,port=record.get('identity'),record.get('port');require(target and port,'Explicit I2S target/port required')
    ddr_raw=evidence(record['ddr']);ddr=json.loads(ddr_raw)
    require(field(raw,layout,'memory')==hashlib.sha256(ddr_raw).digest() and ddr.get('schema')=='a733-ddr-reservation-v1' and
        ddr.get('identity')==target and ddr.get('port')==port and ddr.get('image_sha256')==image_hash and
        ddr.get('ranges')==[[0x41000000,0x42200000]] and all(ddr.get(k) is True for k in ('exclusive','published_cache_disabled','remote_writers_excluded')) and ddr.get('evidence'),'Actual I2S compact DDR/alias ownership required')
    for ref in ddr['evidence']:evidence(ref)
    platform=json.loads(evidence(record['platform']));original=supervisor.observer.validate_platform
    validate=types.FunctionType(original.__code__,{**original.__globals__,'artifact':evidence})
    validate(platform,{'image_sha256':image_hash},target,port)
    facts_raw=evidence(record['recovery']);facts=json.loads(facts_raw)
    require(field(raw,layout,'recovery')==hashlib.sha256(facts_raw).digest() and facts.get('schema')=='a733-i2s-initial-entry-v1' and
        facts.get('identity')==target and facts.get('port')==port and facts.get('image_sha256')==image_hash and
        all(facts.get(k)==v for k,v in profile.items()) and facts.get('shared_sha256')==record['shared_sha256'] and
        facts.get('dma_sha256')==record['dma_sha256'] and facts.get('inherited_sha256')==record['inherited_sha256'] and
        set(facts.get('conditions',{}))==CONDITIONS and facts.get('one_attempt') is True and
        facts.get('whole_board_power_removal') is True and facts.get('no_resource_reuse_before_power_removal') is True,
        'Actual DMA prior drain, audio/electrical/ownership and retained supervision evidence required')
    for ref in facts['conditions'].values():evidence(ref)
    return cache

def request_bytes(address,identity,raw,profile):
    e,g,l=(profile[k] for k in ('epoch','generation','lease'));h=hashlib.sha256(raw).digest()
    request=struct.pack('<8Q',REQUEST_MAGIC,3,128,e,g,g^((1<<64)-1),l,address)+identity+h
    contract=struct.pack('<6Q',0,3,224,e,g,g^((1<<64)-1))+identity
    contract+=struct.pack('<14Q',0,0,0,0,l,0,0,0,0,0,0,0,SESSION_ADDRESS,len(raw))+h
    return request,contract+bytes(4096-len(contract))

def build_launcher(output,session_size,clang,lld,objcopy):
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text()
    for old,new in [('0x4153373353554c33','0x4153373341554c31'),('CONTRACT->reserved[0] && !CONTRACT->reserved[1]',f'CONTRACT->reserved[0]==0x41f00000 && CONTRACT->reserved[1]=={session_size}'),('20*A733_SMP_COUNTER_HZ','60*A733_SMP_COUNTER_HZ')]:
        require(old in text,'Maintained supervised launcher changed');text=text.replace(old,new)
    (output/'launch.c').write_text(text)
    for name,p in [('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h')]: (output/name).write_bytes(p.read_bytes())
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-Wall','-Wextra','-Werror',f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC}ULL','-I'+str(output)]
    commands=[[str(clang),*flags,'-MMD','-MF',str(output/(target+'.d')),'-c',str(output/name),'-o',str(output/target)] for name,target in [('launch.S','entry.o'),('launch.c','launch.o')]]
    commands.extend([[str(lld),'-T',str(output/'launch.ld'),str(output/'entry.o'),str(output/'launch.o'),'-o',str(output/'launch.elf')],[str(objcopy),'-O','binary',str(output/'launch.elf'),str(output/'launch.bin')]])
    save_json(output/'commands.json',commands)
    for command in commands:
        r=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        with (output/'build.log').open('a') as f:f.write(r.stdout)
        require(r.returncode==0,'I2S SRAM launcher build failed; inspect build.log')
    require(0<(output/'launch.bin').stat().st_size<=0xc00,'I2S launcher exceeds protected SRAM')

def prepare(image,manifest,session,admission,output,clang,lld,objcopy):
    image_raw=Path(image).read_bytes();manifest_raw=Path(manifest).read_bytes();raw=Path(session).read_bytes();admission_raw=Path(admission).read_bytes()
    parts,address,identity,layout,pcm=image_data(image_raw);profile=session_profile(raw,layout,identity,pcm);m=json.loads(manifest_raw)
    require(m.get('status')=='PASS' and m.get('physical') is False and m.get('image_sha256')==sha(image_raw) and m.get('build_id')==identity.hex(),'Exact final native I2S handoff manifest required')
    cache=validate_admission(admission_raw,raw,layout,identity,pcm,sha(image_raw));output=Path(output);output.mkdir(parents=True,exist_ok=False)
    build_launcher(output,len(raw),clang,lld,objcopy)
    data={'image.elf':image_raw,'image-input-manifest.json':manifest_raw,'session.bin':raw,'admission.json':admission_raw};uploads=[]
    for i,p in enumerate(parts):
        if p['data']:
            name=f'segment-{i}.bin';data[name]=p['data'];uploads.append({'name':name,'address':p['address']})
    uploads.append({'name':'session.bin','address':SESSION_ADDRESS});frozen={}
    for i,(path,value) in enumerate(cache.items()):
        name=f'entry-evidence-{i}.bin';data[name]=value;frozen[path]={'name':name,'sha256':sha(value)}
    request,contract=request_bytes(address,identity,raw,profile);data.update({'request.bin':request,'contract.bin':contract,'result-zero.bin':bytes(16384),'report-zero.bin':bytes(128)})
    for name,value in data.items():(output/name).write_bytes(value)
    p={'schema':SCHEMA,'physical_access':False,'image_sha256':sha(image_raw),'build_id':identity.hex(),**profile,'session_layout':layout,
        'session_sha256':sha(raw),'pcm_sha256':sha(pcm),'retention':'whole-board-power-removal','segments':uploads,
        'reservations':[{k:v for k,v in x.items() if k!='data'} for x in parts],'entry_evidence':frozen,'launcher_sha256':digest(output/'launch.bin'),
        'files':{p.name:digest(p) for p in output.iterdir() if p.is_file()}}
    save_json(output/'prepared.json',p);return validate_bundle(output)

def validate_bundle(bundle):
    bundle=Path(bundle);metadata=(bundle/'prepared.json').read_bytes();p=json.loads(metadata)
    require(p.get('schema')==SCHEMA and p.get('physical_access') is False and p.get('retention')=='whole-board-power-removal','Unsupported I2S bundle')
    data={}
    for name,h in p['files'].items():
        require(Path(name).name==name,'Invalid I2S bundle filename');data[name]=(bundle/name).read_bytes();require(sha(data[name])==h,'I2S bundle bytes changed')
    parts,address,identity,layout,pcm=image_data(data['image.elf']);raw=data['session.bin'];profile=session_profile(raw,layout,identity,pcm)
    require(p['session_layout']==layout and p['build_id']==identity.hex() and p['image_sha256']==sha(data['image.elf']) and
        p['session_sha256']==sha(raw) and p['pcm_sha256']==sha(pcm) and all(p.get(k)==v for k,v in profile.items()),'I2S prepared layout/session identity differs')
    m=json.loads(data['image-input-manifest.json']);require(m.get('status')=='PASS' and m.get('physical') is False and m.get('image_sha256')==p['image_sha256'] and m.get('build_id')==p['build_id'],'I2S native handoff manifest differs')
    cache={path:data[item['name']] for path,item in p['entry_evidence'].items()};require(all(sha(cache[path])==item['sha256'] for path,item in p['entry_evidence'].items()),'Frozen I2S evidence changed')
    validate_admission(data['admission.json'],raw,layout,identity,pcm,p['image_sha256'],cache,True);expected=[]
    for i,x in enumerate(parts):
        if x['data']:
            name=f'segment-{i}.bin';require(data[name]==x['data'],'I2S upload differs from ELF');expected.append({'name':name,'address':x['address']})
    expected.append({'name':'session.bin','address':SESSION_ADDRESS});require(p['segments']==expected and p['reservations']==[{k:v for k,v in x.items() if k!='data'} for x in parts],'I2S upload/reservation list differs')
    for name,value in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,profile)):require(data[name]==value,'I2S launcher identity differs')
    require(data['result-zero.bin']==bytes(16384) and data['report-zero.bin']==bytes(128) and 0<len(data['launch.bin'])<=0xc00 and sha(data['launch.bin'])==p['launcher_sha256'],'I2S protected launch/result extent differs')
    p['_sizes']={k:len(v) for k,v in data.items()};p['_frozen']=data;p['_prepared_sha256']=sha(metadata);return p
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','session','admission','output','clang','lld','objcopy'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();prepare(a.image,a.manifest,a.session,a.admission,a.output,a.clang,a.lld,a.objcopy)
