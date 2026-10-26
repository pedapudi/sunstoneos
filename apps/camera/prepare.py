#!/usr/bin/env python3
"""Freeze a reviewed native camera session and sixty-second SRAM launch offline."""
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
supervisor=module('camera_prepare_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
protocol=module('camera_protocol',ROOT/'apps/camera/observe.py')
require,digest,save_json=supervisor.require,supervisor.digest,supervisor.save_json
SCHEMA='a733-camera-supervised-bundle-v1'
SESSION_ADDRESS=0x41f00000
CONTRACT_MAGIC=0x4153373343414333
REQUEST_MAGIC=0x4153373343414c33
CONDITIONS={'camera-module-supplies-reset-and-bank-voltage',
    'camera-all-ten-iommu-masters-and-remote-firmware-excluded',
    'camera-prior-transactions-and-pagewalks-resolved',
    'camera-both-iommu-banks-disabled-and-all-master-domains-accessible',
    'camera-media-apb1-mclk-parents-and-common-register-ownership',
    'camera-analog-trim-receiver-timing-and-prefetch-guards-qualified',
    'camera-sensor-and-all-producers-quiescent',
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
            require(t.tag!='DW_TAG_pointer_type','camera launch session must contain only values')
            offset=base+int(c.attributes['DW_AT_data_member_location'].value);key=prefix+name
            fields[key]={'offset':offset,'size':size(t)}
            if t.tag=='DW_TAG_structure_type':fields.update(walk(t,key+'.',offset))
            if t.tag=='DW_TAG_array_type':require(resolve(t.get_DIE_from_attribute('DW_AT_type')).tag!='DW_TAG_pointer_type','Pointer array in native session')
        return fields
    require(elf.has_dwarf_info(),'Native camera DWARF layout evidence required')
    layouts=[]
    for cu in elf.get_dwarf_info().iter_CUs():
        for d in cu.iter_DIEs():
            if d.tag=='DW_TAG_structure_type' and 'DW_AT_name' in d.attributes and d.attributes['DW_AT_name'].value==b'camera_session' and 'DW_AT_byte_size' in d.attributes:
                value={'size':size(d),'fields':walk(d)}
                if value not in layouts:layouts.append(value)
    require(len(layouts)==1 and 208<layouts[0]['size']<=4096,'Missing or conflicting native camera session layout')
    return layouts[0]

def image_data(raw):
    elf=ELFFile(io.BytesIO(raw))
    require(elf.elfclass==64 and elf.little_endian and elf['e_type']=='ET_EXEC' and elf['e_machine']=='EM_AARCH64' and elf['e_entry']==0x41000000,'Native camera ELF required')
    parts=[]
    for s in elf.iter_segments():
        if s['p_type']!='PT_LOAD':continue
        a,m,n,o,flags=(int(s[k]) for k in ('p_vaddr','p_memsz','p_filesz','p_offset','p_flags'))
        require(a==s['p_paddr'] and (0x41000000<=a<a+m<=0x42200000 or (a==0x48000000 and m==0x2000000 and n==0 and flags==6)) and 0<=n<=m and 0<=o<=o+n<=len(raw) and flags&4 and not flags&~7 and flags&3!=3,'camera segment outside compact DDR ownership')
        require(not flags&1 or a+m<=0x41200000,'Executable camera data outside code prefix')
        require(not n or a+n<=SESSION_ADDRESS,'camera initialized bytes overlap session or monitor')
        require(all(a+m<=p['address'] or p['address']+p['memory_bytes']<=a for p in parts),'Overlapping camera ELF allocations')
        parts.append({'address':a,'memory_bytes':m,'flags':flags,'data':raw[o:o+n]})
    table=elf.get_section_by_name('.symtab');require(table is not None,'Native camera symbol table required')
    symbols={s.name:s for s in table.iter_symbols()}
    for name,address in [('__a733_smp_result',0x42000000),('__a733_smp_contract',0x42004000),('__a733_smp_contract_end',0x42005000),('__storage_session_start',SESSION_ADDRESS),('__storage_session_end',SESSION_ADDRESS+4096)]:
        require(name in symbols and symbols[name]['st_value']==address,'camera linker reservation differs: '+name)
    first,last=(int(symbols[n]['st_value']) for n in ('__device_pool_start','__device_pool_end'))
    require(all(n in symbols for n in ('__uncached_pool_start','__uncached_pool_end')) and
        symbols['__uncached_pool_start']['st_value']==symbols['__uncached_pool_end']['st_value'],
        'Camera image must not reserve an uncached pool')
    def read(name,n):
        require(name in symbols and symbols[name]['st_size']>=n,'Missing camera immutable symbol: '+name)
        a=int(symbols[name]['st_value'])
        for p in parts:
            if p['address']<=a and a+n<=p['address']+len(p['data']):
                require(not p['flags']&2,'Writable camera immutable input')
                return a,p['data'][a-p['address']:a-p['address']+n]
        raise ValueError('camera immutable input lacks file backing')
    address,identity=read('a733_smp_build_id',32)
    modes=[(160,120),(176,144),(320,240),(640,480),(720,480),(720,576),(1024,768),(1280,720),(1920,1080),(2592,1944)]
    def page(n):return (n+4095)&~4095
    profile={'size':3,'width':640,'height':480,'output':614400,'legacy':True,'stream':None,
             'pool_start':first,'pool_end':last}
    if 'a733_camera_image_profile' in symbols:
        _,value=read('a733_camera_image_profile',72)
        keys=('magic','version','size','width','height','input_format','output_format','stream',
              'plane','slot','table','source','output','bytes','pool_start','pool_end')
        profile=dict(zip(keys,struct.unpack('<14I2Q',value)))
        k=profile['size'];require(0<=k<len(modes),'Unsupported camera size enum')
        w,h=modes[k];plane=w*h;slot=page(plane)+4096;table=36864 if k==9 else 32768
        require(profile['stream'] in (0,1),'Unsupported camera operation')
        source=table+8192+(4 if profile['stream'] else 2)*slot
        expected=(0x43414d50,1,k,w,h,0,0x3631564e,profile['stream'],plane,slot,table,source,2*plane,72,first,last)
        require(tuple(profile.values())==expected,'Camera immutable geometry differs')
        profile['legacy']=False
    large=not profile['legacy'] and profile['source']+page(profile['output'])>2097152
    if large:
        require((first,last)==(0x48000000,0x4a000000) and any(p['address']==first and p['memory_bytes']==last-first and p['flags']==6 and not p['data'] for p in parts),'Camera external reservation differs')
    else:
        require(0x41200000<=first<last<=SESSION_ADDRESS and last-first==2097152 and not (first|last)&4095 and all(p['address']<0x48000000 for p in parts),'camera compact DMA pool ownership differs')
    profile['ddr_ranges']=[[0x41000000,0x42200000]]+([[first,last]] if large else [])

    def executable(symbol):
        section=symbol['st_shndx'];start=int(symbol['st_value']);size=int(symbol['st_size'])
        return (symbol['st_info']['type']=='STT_FUNC' and isinstance(section,int) and
            0<section<elf.num_sections() and elf.get_section(section)['sh_flags']&4 and size>0 and
            any(p['flags']&1 and p['address']<=start and start+size<=p['address']+len(p['data']) for p in parts))
    functions={name for name,symbol in symbols.items() if executable(symbol)}
    native={'a733_camera_native_supervised_init','a733_camera_native_supervised_stream_init'}
    if not profile['legacy']:
        selected='a733_camera_native_supervised_stream_init' if profile['stream'] else 'a733_camera_native_supervised_init'
        require(selected in functions,'Camera profile differs from executable operation')
    require(0x41000000<=address<=0x411fffe0 and
        {'camera_capture_step','camera_session_valid','camera_output'}<=functions and
        native.intersection(functions),'Wrong native application identity')
    layout=session_layout(elf);layout['image_profile']=profile
    return parts,address,identity,layout

def field(raw,layout,name):
    f=layout['fields'][name];return raw[f['offset']:f['offset']+f['size']]
def number(raw,layout,name):return int.from_bytes(field(raw,layout,name),'little')
def session_profile(raw,layout,identity):
    require(len(raw)==layout['size'] and struct.unpack_from('<3Q',raw)==(0x4153373343415332,2,len(raw)),'Invalid supervised camera session')
    keys=['epoch','generation','lease','pck_client','shared.identity','shared.pmic.owner','shared.pmic.identity',
          'shared.pmic.startup_deadline','shared.usb.owner','shared.usb.handoff','shared.usb.deadline_us',
          'camera.version','camera.bytes','camera.owner','camera.epoch','camera.generation','camera.lease',
          'camera.deadline_us','camera.proof','camera.origin','camera.excluded_masters','camera.powered_masters',
          'camera.raw_trim','camera.parameter_guard_bytes','camera.plane_guard_bytes','camera.apb1','camera.apb1_hz',
          'camera.settle','camera.deskew','camera.reset_bank','camera.reset_pin','camera.test_pattern','camera.pwdn_only',
          'camera.discard_frames',
          'media.owner','media.handoff','media.deadline_us','media.groups','media.camera_mclk_mask']
    values={k:number(raw,layout,k) for k in keys}
    epoch,generation,lease=(values[k] for k in ('epoch','generation','lease'))
    require(all((epoch,generation,lease)) and values['pck_client']<16,'Camera launch identities differ')
    require(field(raw,layout,'image')==identity and field(raw,layout,'camera.build')==identity,'Camera image binding differs')
    require(all(any(field(raw,layout,k)) for k in ('evidence','memory','recovery')) and
        field(raw,layout,'camera.evidence')==field(raw,layout,'evidence'),'Camera evidence binding missing')
    zero=['deadline_us','minimum_reload_us','maximum_recovery_us','guard_us','reset_by_us','reload_before_ticks',
          'reload_after_ticks','rtc_control','rtc_divider','rtc_source','shared.pmic.startup_deadline',
          'shared.usb.deadline_us','camera.deadline_us','media.deadline_us']
    require(not any(number(raw,layout,k) for k in zero),'Uploaded camera deadlines/watchdog state forbidden')
    require(values['shared.identity']==values['shared.pmic.identity']==values['shared.usb.handoff']==generation and
        values['camera.epoch']==epoch and values['camera.generation']==generation and values['camera.lease']==lease and
        values['media.handoff']==lease,'Camera shared/native identity differs')
    owners=[values[k] for k in ('shared.pmic.owner','shared.usb.owner','camera.owner')]
    require(all(owners) and len(set(owners))==3 and values['media.owner']==owners[2],'Conflicting camera owners')
    require(values['camera.version']==2 and values['camera.bytes']==layout['fields']['camera']['size'] and
        values['camera.proof']==0x6ff and values['camera.origin'] in (1,2) and
        values['camera.excluded_masters']==values['camera.powered_masters']==0xfcf and
        values['camera.raw_trim']==0xb00 and values['camera.parameter_guard_bytes']==values['camera.plane_guard_bytes']==4096 and
        1<=values['camera.settle']<=255 and 1<=values['camera.deskew']<=31,'Camera native admission profile differs')
    # 0 is the ordinary image, 1 the OV5640 color bars; a PWDN-only module names no reset GPIO.
    require(values['camera.test_pattern'] in (0,1) and values['camera.pwdn_only'] in (0,1),'Camera test pattern or module profile differs')
    # Images captured and discarded before the readable one, each a full sensor stop/start.
    require(values['camera.discard_frames']<=15,'Camera discard count exceeds the native limit')
    mclk=values['media.camera_mclk_mask']
    require(mclk in (2,4),'Exactly one camera connector must be selected')
    connector=0 if mclk==2 else 1
    control_pins=(5,6,7,8) if connector==0 else (9,10,14,15)
    bank,pin=values['camera.reset_bank'],values['camera.reset_pin'];inherited=field(raw,layout,'camera.inherited')
    if values['camera.pwdn_only']:
        require(bank==pin==0 and inherited[14*3:15*3]==bytes(3),'PWDN-only camera module names reset wiring')
        require(inherited[11*3]==1,'Camera inherited powerdown GPIO must be asserted')
    else:
        require(1<=bank<=10 and pin<(0,11,17,24,16,7,15,20,17,28,26)[bank] and not(bank==4 and pin in control_pins) and not(bank==10 and 10*connector<=pin<10*(connector+1)),'Camera reset wiring conflicts')
        require(inherited[11*3]==inherited[14*3]==1,'Camera inherited reset/powerdown GPIO must be asserted')
    rails=struct.unpack('<3I',field(raw,layout,'camera.rails'));uv=struct.unpack('<3I',field(raw,layout,'camera.programmed_uv'))
    require(len(set(rails))==3 and set(rails)<={0,1,3,7,16,19,20,24,38} and all(uv),'Unsupported camera supply profile')
    apb=values['camera.apb1'];parent=600000000 if apb>>24==3 else 24000000;div=(apb&31)+1
    require(not apb&~0x0300001f and apb>>24 in (0,3) and parent%div==0 and values['camera.apb1_hz']==parent//div,'Camera APB1 profile differs')
    require(values['media.groups']==9 and mclk in (2,4) and
        field(raw,layout,'media.fabric')==field(raw,layout,'shared.usb.fabric') and
        field(raw,layout,'media.rate_hz')==field(raw,layout,'shared.usb.rate_hz'),'Camera media/USB fabric profile differs')
    return {k:values[k] for k in ('epoch','generation','lease')}

def camera_configuration_sha256(raw,layout):
    # The record repeats its admission digest. Exclude only that self-reference
    # from the reviewed configuration hash; session SHA256 still covers it.
    value=bytearray(field(raw,layout,'camera'))
    entry=layout['fields']['camera.evidence'];base=layout['fields']['camera']['offset']
    value[entry['offset']-base:entry['offset']-base+entry['size']]=bytes(entry['size'])
    return sha(value)

def validate_admission(admission_raw,raw,layout,identity,image_hash,cache=None,frozen=False):
    record=json.loads(admission_raw);profile=session_profile(raw,layout,identity)
    require(record.get('schema')=='a733-camera-supervised-entry-v1' and record.get('image_sha256')==image_hash and
        field(raw,layout,'evidence')==hashlib.sha256(admission_raw).digest() and all(record.get(k)==v for k,v in profile.items()),'camera session does not bind reviewed image/entry')
    require(record.get('shared_sha256')==sha(field(raw,layout,'shared')) and record.get('camera_sha256')==camera_configuration_sha256(raw,layout) and
        record.get('media_sha256')==sha(field(raw,layout,'media')),'camera typed configuration differs from reviewed facts')
    cache={} if cache is None else cache
    def evidence(ref):
        require(isinstance(ref,dict) and set(ref)=={'path','sha256'},'Invalid camera evidence reference')
        p=Path(ref['path']);require(p.is_absolute(),'camera evidence path must be absolute')
        if str(p) not in cache:require(not frozen,'Missing frozen camera entry evidence');cache[str(p)]=p.read_bytes()
        data=cache[str(p)];require(sha(data)==ref['sha256'],'camera entry evidence changed');return data
    target,port=record.get('identity'),record.get('port');require(target and port,'Explicit camera target/port required')
    ddr_raw=evidence(record['ddr']);ddr=json.loads(ddr_raw)
    require(field(raw,layout,'memory')==hashlib.sha256(ddr_raw).digest() and ddr.get('schema')=='a733-ddr-reservation-v1' and
        ddr.get('identity')==target and ddr.get('port')==port and ddr.get('image_sha256')==image_hash and
        ddr.get('ranges')==layout.get('image_profile',{}).get('ddr_ranges',[[0x41000000,0x42200000]]) and all(ddr.get(k) is True for k in ('exclusive','published_cache_disabled','remote_writers_excluded')) and ddr.get('evidence'),'Actual camera compact DDR/alias ownership required')
    for ref in ddr['evidence']:evidence(ref)
    platform=json.loads(evidence(record['platform']));original=supervisor.observer.validate_platform
    validate=types.FunctionType(original.__code__,{**original.__globals__,'artifact':evidence})
    validate(platform,{'image_sha256':image_hash},target,port)
    facts_raw=evidence(record['recovery']);facts=json.loads(facts_raw)
    require(field(raw,layout,'recovery')==hashlib.sha256(facts_raw).digest() and facts.get('schema')=='a733-camera-initial-entry-v1' and
        facts.get('identity')==target and facts.get('port')==port and facts.get('image_sha256')==image_hash and
        all(facts.get(k)==v for k,v in profile.items()) and facts.get('shared_sha256')==record['shared_sha256'] and
        facts.get('camera_sha256')==record['camera_sha256'] and facts.get('media_sha256')==record['media_sha256'] and
        set(facts.get('conditions',{}))==CONDITIONS and facts.get('one_attempt') is True and
        facts.get('whole_board_power_removal') is True and facts.get('no_resource_reuse_before_power_removal') is True,
        'Actual camera reset/prior drain/guards/electrical/ownership and retained supervision evidence required')
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
    for old,new in [('0x4153373353554c33','0x4153373343414c33'),('CONTRACT->reserved[0] && !CONTRACT->reserved[1]',f'CONTRACT->reserved[0]==0x41f00000 && CONTRACT->reserved[1]=={session_size}'),('20*A733_SMP_COUNTER_HZ','60*A733_SMP_COUNTER_HZ')]:
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
        require(r.returncode==0,'camera SRAM launcher build failed; inspect build.log')
    require(0<(output/'launch.bin').stat().st_size<=0xc00,'camera launcher exceeds protected SRAM')

def prepare(image,manifest,session,admission,output,clang,lld,objcopy):
    image_raw=Path(image).read_bytes();manifest_raw=Path(manifest).read_bytes();raw=Path(session).read_bytes();admission_raw=Path(admission).read_bytes()
    parts,address,identity,layout=image_data(image_raw);profile=session_profile(raw,layout,identity);m=json.loads(manifest_raw)
    require(m.get('status')=='PASS' and m.get('physical') is False and m.get('image_sha256')==sha(image_raw) and m.get('build_id')==identity.hex(),'Exact final native camera handoff manifest required')
    cache=validate_admission(admission_raw,raw,layout,identity,sha(image_raw));output=Path(output);output.mkdir(parents=True,exist_ok=False)
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
        'session_sha256':sha(raw),'retention':'whole-board-power-removal','segments':uploads,
        'reservations':[{k:v for k,v in x.items() if k!='data'} for x in parts],'entry_evidence':frozen,'launcher_sha256':digest(output/'launch.bin'),
        'files':{p.name:digest(p) for p in output.iterdir() if p.is_file()}}
    save_json(output/'prepared.json',p);return validate_bundle(output)

def validate_bundle(bundle):
    bundle=Path(bundle);metadata=(bundle/'prepared.json').read_bytes();p=json.loads(metadata)
    require(p.get('schema')==SCHEMA and p.get('physical_access') is False and p.get('retention')=='whole-board-power-removal','Unsupported camera bundle')
    data={}
    for name,h in p['files'].items():
        require(Path(name).name==name,'Invalid camera bundle filename');data[name]=(bundle/name).read_bytes();require(sha(data[name])==h,'camera bundle bytes changed')
    parts,address,identity,layout=image_data(data['image.elf']);raw=data['session.bin'];profile=session_profile(raw,layout,identity)
    require(p['session_layout']==layout and p['build_id']==identity.hex() and p['image_sha256']==sha(data['image.elf']) and
        p['session_sha256']==sha(raw) and all(p.get(k)==v for k,v in profile.items()),'camera prepared layout/session identity differs')
    m=json.loads(data['image-input-manifest.json']);require(m.get('status')=='PASS' and m.get('physical') is False and m.get('image_sha256')==p['image_sha256'] and m.get('build_id')==p['build_id'],'camera native handoff manifest differs')
    cache={path:data[item['name']] for path,item in p['entry_evidence'].items()};require(all(sha(cache[path])==item['sha256'] for path,item in p['entry_evidence'].items()),'Frozen camera evidence changed')
    validate_admission(data['admission.json'],raw,layout,identity,p['image_sha256'],cache,True);expected=[]
    for i,x in enumerate(parts):
        if x['data']:
            name=f'segment-{i}.bin';require(data[name]==x['data'],'camera upload differs from ELF');expected.append({'name':name,'address':x['address']})
    expected.append({'name':'session.bin','address':SESSION_ADDRESS});require(p['segments']==expected and p['reservations']==[{k:v for k,v in x.items() if k!='data'} for x in parts],'camera upload/reservation list differs')
    for name,value in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,profile)):require(data[name]==value,'camera launcher identity differs')
    require(data['result-zero.bin']==bytes(16384) and data['report-zero.bin']==bytes(128) and 0<len(data['launch.bin'])<=0xc00 and sha(data['launch.bin'])==p['launcher_sha256'],'camera protected launch/result extent differs')
    p['_sizes']={k:len(v) for k,v in data.items()};p['_frozen']=data;p['_prepared_sha256']=sha(metadata);return p
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','session','admission','output','clang','lld','objcopy'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();prepare(a.image,a.manifest,a.session,a.admission,a.output,a.clang,a.lld,a.objcopy)
