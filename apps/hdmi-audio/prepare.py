#!/usr/bin/env python3
"""Prepare one retained HDMI audio session from exact PCM and reviewed records."""
import argparse
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import struct
import subprocess
import types
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('hdmi_audio_base',ROOT/'apps/hdmi/prepare.py')
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
spec=importlib.util.spec_from_file_location('hdmi_audio_dma_security',ROOT/'tools/dma-nonsecure-access.py')
security_authority=importlib.util.module_from_spec(spec);spec.loader.exec_module(security_authority)
spec=importlib.util.spec_from_file_location('hdmi_audio_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
supervisor=importlib.util.module_from_spec(spec);spec.loader.exec_module(supervisor)
SCHEMA='a733-hdmi-audio-supervised-bundle-v1'
GEOMETRY=(1,656,440,216,0x41f10000,192000,48000,2,16,32)
USB_GEOMETRY=(2,816,440,216,0x41f10000,192000,48000,2,16,32,656,160)
USB_FACTS={
 'initial':('cold_usb0_usb1_reset_gates_held','remote_usb_phy_consumers_excluded',
    'prior_usb_dma_drained','external_usb_dma_writers_excluded',
    'usb_irqs_188_189_190_owned_quiet','no_concurrent_rom_firmware_access'),
 'electrical':('stable_shared_fabric','dedicated_sink_no_vbus_drive'),
 'supervision':('manual_whole_board_power_removal','retain_full_20s_lifetime','one_attempt','no_reuse_after_fault')}
MEMORY_FACTS=('full_pool_and_pcm_window_exclusive','no_physical_aliases',
    'coherent_cache_visibility','remote_dma_writers_excluded','all_other_dma0_producers_quiet',
    'prior_dma_transactions_drained','irq_owned_masked','immutable_buffer_descriptor_ownership',
    'retain_until_whole_board_power_removal')
ELECTRICAL_FACTS=('hdmi_analog_profile_reviewed','rails_settled','shared_clock_ownership',
    'i2s3_route_owned','sink_lpcm_stereo_48khz_16bit')


def require(condition,message):
    if not condition:raise ValueError(message)


def sha(data):return hashlib.sha256(data).hexdigest()


def physical_authority(record):
    require(record.get('physical_authority') is True and record.get('modeled') is False,
        'Modeled HDMI audio evidence cannot authorize physical execution.')


def image_metadata(image,session_size=None):
    parts,address,identity,size=base.image_data(image)
    require(size in (656,816) and (session_size is None or size==session_size) and
        base.framebuffer_metadata(image)==(0,0),
        'HDMI audio requires the matching offline or copied-USB pattern image.')
    # Audio adopts the same complete NC reservation, without DE/framebuffer use.
    base.geometry(parts,True)
    with io.BytesIO(image) as stream:
        elf=ELFFile(stream)
        require(elf['e_machine']=='EM_AARCH64' and elf['e_entry']==0x41000000,
            'Native A733 HDMI audio ELF required.')
        symbols=list(elf.get_section_by_name('.symtab').iter_symbols())
        def marker(name,size,writable=False):
            matches=[s for s in symbols if s.name==name and s['st_info']['type']=='STT_OBJECT']
            require(len(matches)==1 and matches[0]['st_size']==size,'Ambiguous HDMI audio marker: '+name)
            s=matches[0];start=int(s['st_value'])
            spans=[p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and
                int(p['p_vaddr'])<=start and start+size<=int(p['p_vaddr'])+int(p['p_memsz'])]
            require(len(spans)==1 and bool(spans[0]['p_flags']&2)==writable,
                'HDMI audio marker has wrong memory ownership: '+name)
            p=spans[0];offset=start-int(p['p_vaddr'])
            require(writable or offset+size<=len(p.data()),'HDMI audio geometry is not file backed.')
            return start,bytes(p.data()[offset:offset+size])
        build_address,build_identity=marker('a733_smp_build_id',32)
        require(build_address==address and build_identity==identity,'HDMI build identity differs.')
        _,geometry=marker('a733_hdmi_audio_geometry',80 if size==656 else 96)
        require(struct.unpack('<10Q' if size==656 else '<12Q',geometry)==(GEOMETRY if size==656 else USB_GEOMETRY),'Linked HDMI audio geometry differs.')
        pool_symbols={name:[int(s['st_value']) for s in symbols if s.name==name] for name in
            ('__uncached_pool_start','__uncached_pool_end')}
        require(pool_symbols=={'__uncached_pool_start':[0x41c00000],'__uncached_pool_end':[0x41e00000]},
            'HDMI audio uncached pool differs from native memory ownership.')
        result,_=marker('hdmi_audio_result',160,True)
        ready,_=marker('hdmi_audio_result_ready',4,True)
        require(not result%64 and not ready%64 and
            (result+160<=ready or ready+4<=result),'HDMI audio result publication storage aliases.')
    return parts,address,identity,dict(result_address=result,ready_address=ready)


def session_check(raw,pcm):
    require(len(raw) in (656,816),'HDMI audio session must be656 or816 bytes.')
    magic,version,size,epoch,generation,lease=struct.unpack_from('<6Q',raw)
    supervisor,owner=struct.unpack_from('<2Q',raw,208)
    require((magic,version,size) in ((base.SESSION_MAGIC,1,656),(base.SESSION_MAGIC,2,816)) and size==len(raw) and
        all((epoch,generation,lease,supervisor,owner)),'HDMI audio session identity differs.')
    require(struct.unpack_from('<3I',raw,288)==(4,0,0),
        'HDMI audio requires the limited-range720p60 hardware pattern.')
    authority,frames=struct.unpack_from('<2Q',raw,536)
    require(authority and 1<=frames<=48000 and len(pcm)==frames*4 and
        hashlib.sha256(pcm).digest()==raw[440:472],'HDMI audio PCM identity or frame extent differs.')
    require(any(raw[472:504]) and any(raw[504:536]),'HDMI audio memory/electrical records absent.')
    controller,evidence,established,owned,service=struct.unpack_from('<2QI2H',raw,632)
    require(controller==0x04601000 and evidence and established==0x5f and service==1 and
        owned==0xffff and evidence==struct.unpack_from('<Q',raw,608)[0] and
        struct.unpack_from('<I',raw,584)[0]&1 and struct.unpack_from('<Q',raw,616)[0]==0,
        'HDMI audio requires the immutable supervised DMA0 handoff.')
    fields=dict(epoch=epoch,generation=generation,lease=lease,supervisor=supervisor,
        authority=authority,frames=frames,source_bytes=len(pcm))
    if len(raw)==816:
        usb_owner,handoff=struct.unpack_from('<2Q',raw,656)
        require(usb_owner and handoff and usb_owner not in
            (owner,struct.unpack_from('<Q',raw,304)[0],authority,struct.unpack_from('<Q',raw,600)[0]),
            'HDMI audio USB owner aliases another retained owner.')
        require(all(any(raw[offset:offset+32]) for offset in (672,704,736)) and
            struct.unpack_from('<2I',raw,768)==(1,0) and
            struct.unpack_from('<4I',raw,776)==(3300000,3300000,1200000,900000),
            'HDMI audio requires cold USB reference1, exact rails and three evidence digests.')
        fields.update(usb_owner=usb_owner,usb_handoff=handoff)
    return fields


def session_bytes(pattern,pcm,*,memory,electrical,authority,dma,admission,usb=None):
    require(len(pattern)==440 and len(dma)==104 and len(pcm)%4==0,
        'HDMI audio producer requires pattern440, DMA104 and complete stereo frames.')
    require(usb is None or len(usb)==160,'HDMI audio USB authority must be160 bytes.')
    raw=bytearray(pattern);struct.pack_into('<2Q',raw,8,1 if usb is None else 2,656 if usb is None else 816)
    raw[80:112]=hashlib.sha256(admission).digest()
    raw+=hashlib.sha256(pcm).digest()+hashlib.sha256(memory).digest()+hashlib.sha256(electrical).digest()
    raw+=struct.pack('<2Q',authority,len(pcm)//4)+dma
    if usb is not None:raw+=usb
    session_check(raw,pcm)
    return bytes(raw)


def sink_lpcm(edid):
    """Read the CTA Basic Audio flag or a stereo48kHz16-bit LPCM SAD."""
    require(len(edid)>=128 and not len(edid)%128 and edid[:8]==b'\0\xff\xff\xff\xff\xff\xff\0' and
        len(edid)==128*(1+edid[126]) and all(not sum(edid[n:n+128])%256 for n in range(0,len(edid),128)),
        'HDMI audio sink EDID is truncated or has an invalid checksum.')
    supported=False
    for first in range(128,len(edid),128):
        extension=edid[first:first+128]
        if extension[0]!=2 or extension[1]<3:continue
        end=extension[2]
        require(end==0 or 4<=end<=127,'Invalid CTA data block extent.')
        supported|=bool(extension[3]&0x40)
        cursor=4
        while end and cursor<end:
            tag=extension[cursor]>>5;count=extension[cursor]&31;cursor+=1
            require(cursor+count<=end,'Truncated CTA data block.')
            if tag==1:
                require(count%3==0,'Truncated CTA short audio descriptor.')
                for pos in range(cursor,cursor+count,3):
                    a,b,c=extension[pos:pos+3]
                    supported|=(a>>3==1 and (a&7)+1>=2 and bool(b&4) and bool(c&1))
            cursor+=count
    require(supported,'Sink does not advertise stereo48kHz16-bit LPCM.')
    return True


def validate_data(image,raw,admission,pcm,records):
    parts,address,identity,locations=image_metadata(image,session_size=len(raw))
    fields=session_check(raw,pcm);record=json.loads(admission);image_hash=sha(image)
    require(raw[48:80]==identity and raw[80:112]==hashlib.sha256(admission).digest(),
        'HDMI audio session does not bind this image/admission.')
    base.clock_profile(record,struct.unpack_from('<I',raw,300)[0])
    require(record.get('schema')=='a733-hdmi-supervised-admission-v1' and
        record.get('image_sha256')==image_hash and
        all(record.get(key)==fields[key] for key in ('epoch','generation','lease','supervisor')),
        'HDMI audio admission names another image/session.')
    physical_authority(record)
    require('dma_nonsecure_access' in record,'HDMI audio DMA nonsecure authority is missing.')
    reference=record['dma_nonsecure_access']
    require(isinstance(reference,dict) and set(reference)=={'path','sha256'} and
        isinstance(reference['path'],str) and Path(reference['path']).is_absolute(),
        'Invalid HDMI audio DMA security reference.')
    data=records['dma_nonsecure_access']
    require(sha(data)==reference['sha256'],'HDMI audio DMA security evidence changed.')
    security=json.loads(data)
    require(isinstance(security,dict) and isinstance(security.get('evidence'),list),
        'Invalid HDMI audio DMA security authority.')
    require(len({json.dumps(ref,sort_keys=True) for ref in security['evidence']})==len(security['evidence']),
        'Duplicate HDMI audio DMA security evidence references.')
    def evidence(ref):
        matches=[i for i,item in enumerate(security['evidence']) if item==ref]
        require(bool(matches),'Unsealed HDMI audio DMA security evidence.')
        key='dma_security_evidence_'+str(matches[0])
        require(key in records,'Missing frozen HDMI audio DMA security evidence.')
        return records[key]
    controller,dma_evidence,_,_,service=struct.unpack_from('<2QI2H',raw,632)
    expected={k:fields[k] for k in ('epoch','generation','lease')}
    expected.update(identity=record.get('identity'),port=record.get('port'),image_sha256=image_hash,
        controller=controller,service_channels=service,dma_evidence=dma_evidence,
        dma_handoff=struct.unpack_from('<Q',raw,608)[0],configuration_sha256=sha(raw[552:656]))
    security_authority.validate(data,expected,evidence,require)
    require(record.get('audio')=='48khz-stereo-s16le-retained-v1' and
        record.get('audio_authority')==fields['authority'] and record.get('pcm_sha256')==sha(pcm) and
        record.get('pcm_bytes')==len(pcm),'HDMI audio source/authority admission differs.')
    for key,offset in (('analog',112),('power',144),('supervision',176),
        ('audio_memory',472),('audio_electrical',504)):
        require(hashlib.sha256(records[key]).digest()==raw[offset:offset+32],
            key+' record differs from admitted session.')
    for kind,facts in (('memory',MEMORY_FACTS),('electrical',ELECTRICAL_FACTS)):
        fact=json.loads(records['audio_'+kind]);physical_authority(fact)
        require(fact.get('schema')=='a733-hdmi-audio-'+kind+'-v1' and
            fact.get('image_sha256')==image_hash and fact.get('authority')==fields['authority'] and
            all(fact.get(key)==fields[key] for key in ('epoch','generation','lease','supervisor')) and
            fact.get('reviewed') is True and all(fact.get(key) is True for key in facts),
            'HDMI audio typed '+kind+' authority is incomplete.')
        if kind=='memory':
            require(fact.get('dma_handoff_sha256')==sha(raw[552:656]) and
                fact.get('dma_pool_ranges')==[[0x41c00000,0x41e00000]] and
                fact.get('pcm_sha256')==sha(pcm) and fact.get('pcm_range')==[0x41f10000,0x41f10000+len(pcm)],
                'HDMI audio memory authority covers another handoff/PCM range.')
        else:
            require(fact.get('sink_edid_sha256')==sha(records['sink_edid']),
                'HDMI audio electrical authority covers another sink.')
    require(record['sink_edid']['sha256']==sha(records['sink_edid']), 'Sink EDID admission differs.')
    sink_lpcm(records['sink_edid'])
    if len(raw)==816:
        paths=json.loads(records['entry-index'])
        require(isinstance(paths,list) and len(set(paths))==len(paths),'Invalid HDMI audio entry index.')
        validate_entry(record,image_hash,{path:records['entry-evidence-'+str(i)] for i,path in enumerate(paths)},True)
        require(record.get('usb_capture')=='copied-retained-v1','Copied USB admission is missing.')
        for kind,offset in (('initial',672),('electrical',704),('supervision',736)):
            key='usb_'+kind
            require(hashlib.sha256(records[key]).digest()==raw[offset:offset+32],
                'HDMI audio USB '+kind+' record digest differs.')
            fact=json.loads(records[key]);physical_authority(fact)
            require(fact.get('schema')=='a733-hdmi-audio-usb-'+kind+'-v1' and
                fact.get('reviewed') is True and fact.get('image_sha256')==image_hash and
                all(fact.get(k)==record.get(k) and record.get(k) for k in ('identity','port')) and
                all(fact.get(k)==fields[k] for k in ('epoch','generation','lease','supervisor','usb_owner','usb_handoff')) and
                all(fact.get(k) is True for k in USB_FACTS[kind]),'HDMI audio USB '+kind+' authority is incomplete.')
            if kind=='electrical':
                require(fact.get('supply_uv')==list(struct.unpack_from('<4I',raw,776)) and
                    fact.get('fabric')==list(struct.unpack_from('<3I',raw,792)) and
                    fact.get('fabric_hz')==list(struct.unpack_from('<3I',raw,804)),
                    'HDMI audio USB electrical profile differs.')
    else:require('usb_capture' not in record,'Offline HDMI audio cannot authorize copied USB.')
    return parts,address,identity,fields,locations


def validate_entry(record,image_hash,cache=None,frozen=False):
    cache={} if cache is None else cache
    def evidence(ref):
        require(isinstance(ref,dict) and set(ref)=={'path','sha256'} and
            isinstance(ref['path'],str) and Path(ref['path']).is_absolute(),'Invalid HDMI audio entry evidence.')
        path=ref['path']
        if path not in cache:
            require(not frozen,'Missing frozen HDMI audio entry evidence.')
            cache[path]=Path(path).read_bytes()
        require(sha(cache[path])==ref['sha256'],'HDMI audio entry evidence changed.')
        # Shared admission opens only read_text() on resolved DDR/platform refs.
        return types.SimpleNamespace(read_text=lambda:cache[path].decode('utf-8'))
    platform_validate=types.FunctionType(supervisor.observer.validate_platform.__code__,
        {**supervisor.observer.validate_platform.__globals__,'artifact':evidence})
    observer=types.SimpleNamespace(validate_platform=platform_validate)
    validate=types.FunctionType(supervisor.validate_admission.__code__,
        {**supervisor.validate_admission.__globals__,'artifact':evidence,'observer':observer})
    validate({**record,'schema':'a733-smp-supervised-entry-v1'},image_hash)
    return cache


def validate(image,session,admission,pcm):
    image_data=image.read_bytes();raw=session.read_bytes();admission_data=admission.read_bytes();pcm_data=pcm.read_bytes()
    record=json.loads(admission_data);records={}
    for key in ('analog','power','supervision','audio_memory','audio_electrical'):
        records[key]=(admission.parent/record[key]).resolve().read_bytes()
    records['sink_edid']=(admission.parent/record['sink_edid']['path']).resolve().read_bytes()
    if len(raw)==816:
        for key in ('usb_initial','usb_electrical','usb_supervision'):
            records[key]=(admission.parent/record[key]).resolve().read_bytes()
    require('dma_nonsecure_access' in record,'HDMI audio DMA nonsecure authority is missing.')
    reference=record['dma_nonsecure_access']
    def load(ref):
        require(isinstance(ref,dict) and set(ref)=={'path','sha256'} and
            isinstance(ref['path'],str) and Path(ref['path']).is_absolute(),
            'Invalid HDMI audio DMA security reference.')
        data=Path(ref['path']).read_bytes()
        require(sha(data)==ref['sha256'],'HDMI audio DMA security evidence changed.')
        return data
    records['dma_nonsecure_access']=load(reference)
    security=json.loads(records['dma_nonsecure_access'])
    require(isinstance(security,dict) and isinstance(security.get('evidence'),list),
        'Invalid HDMI audio DMA security authority.')
    for i,ref in enumerate(security['evidence']):records['dma_security_evidence_'+str(i)]=load(ref)
    if len(raw)==816:
        entry=validate_entry(record,sha(image_data))
        records['entry-index']=json.dumps(list(entry)).encode()
        for i,data in enumerate(entry.values()):records['entry-evidence-'+str(i)]=data
    validated=validate_data(image_data,raw,admission_data,pcm_data,records)
    snapshots=dict(image_elf=image_data,session=raw,admission=admission_data,pcm=pcm_data,**records)
    return validated,snapshots


def prepare(image,session,admission,pcm,output,clang,lld,objcopy):
    (parts,address,identity,fields,locations),snapshots=validate(image,session,admission,pcm)
    out=output.resolve();out.mkdir(parents=True,exist_ok=False)
    for name,path in [('launch.c',ROOT/'platform/a733-smp-supervised/launch.c'),
        ('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),
        ('boot.h',ROOT/'platform/a733-smp-cached/boot.h')]:shutil.copy2(path,out/name)
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin',
        '-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-Wall','-Wextra','-Werror',
        f'-DA733_SMP_CONTRACT_MAGIC={base.CONTRACT_MAGIC}ULL','-I'+str(out)]
    commands=[[clang,*flags,'-MD','-MF',out/(name+'.d'),'-c',out/name,'-o',out/(name+'.o')] for name in ('launch.c','launch.S')]
    commands+=[[lld,'-T',out/'launch.ld',out/'launch.c.o',out/'launch.S.o','-o',out/'launch.elf'],
        [objcopy,'-O','binary',out/'launch.elf',out/'launch.bin']]
    for command in commands:
        result=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (out/'build.log').open('a') as stream:stream.write(result.stdout+result.stderr)
        result.check_returncode()
    (out/'commands.json').write_text(json.dumps([[str(x) for x in c] for c in commands],indent=2)+'\n')
    require(0<(out/'launch.bin').stat().st_size<=0xc00,'HDMI audio launcher exceeds protected SRAM.')
    payloads=[]
    for i,(start,size,data) in enumerate(parts):
        if data:
            name=f'segment-{i}.bin';(out/name).write_bytes(data);payloads.append(dict(name=name,address=start))
    request,contract=base.request_bytes(address,identity,snapshots['session'],fields['epoch'],fields['generation'],fields['lease'],fields['supervisor'])
    fixed={'session.bin':(snapshots['session']+bytes(4096-len(snapshots['session'])),0x41f00000),
        'input.pcm':(snapshots['pcm'],0x41f10000),'request.bin':(request,0x52c00),
        'contract.bin':(contract,0x42004000),'result-zero.bin':(bytes(16384),0x42000000),
        'report-zero.bin':(bytes(128),0x52d00),'launch.bin':((out/'launch.bin').read_bytes(),0x52000)}
    for name,(data,start) in fixed.items():(out/name).write_bytes(data);payloads.append(dict(name=name,address=start))
    for key,data in snapshots.items():
        name='source-session.bin' if key=='session' else key+'.bin'
        (out/name).write_bytes(data)
    if len(snapshots['session'])==816:
        (out/'image.elf').write_bytes(snapshots['image_elf'])
        (out/'admission.json').write_bytes(snapshots['admission'])
    authority_record=json.loads(snapshots['admission'])
    metadata=dict(schema=SCHEMA,physical_access=False,physical_authority=authority_record.get('physical_authority'),
        modeled=authority_record.get('modeled'),build_id=identity.hex(),
        image_sha256=sha(snapshots['image_elf']),session_sha256=sha(snapshots['session']),
        **fields,**locations,payloads=payloads,reservations=[dict(address=a,bytes=n) for a,n,_ in parts],
        files={p.name:sha(p.read_bytes()) for p in out.iterdir() if p.is_file()},
        tools={str(tool.absolute()):sha(tool.read_bytes()) for tool in (clang,lld,objcopy)})
    if len(snapshots['session'])==816:
        metadata['segments']=[p for p in payloads if p['name'].startswith('segment-') or p['name'] in ('session.bin','input.pcm')]
    (out/'prepared.json').write_text(json.dumps(metadata,indent=2)+'\n')
    return validate_bundle(out)


def validate_bundle(bundle):
    metadata=json.loads((bundle/'prepared.json').read_bytes())
    require(metadata.get('schema')==SCHEMA and metadata.get('physical_access') is False,'Wrong HDMI audio bundle.')
    frozen={}
    for name,expected in metadata['files'].items():
        require(Path(name).name==name,'Invalid HDMI audio bundle filename.')
        frozen[name]=(bundle/name).read_bytes();require(sha(frozen[name])==expected,'HDMI audio bundle changed: '+name)
    required={key+'.bin' for key in ('image_elf','admission','pcm','analog','power','supervision','audio_memory','audio_electrical','sink_edid','dma_nonsecure_access')}
    required.update(('source-session.bin','session.bin','input.pcm','request.bin','contract.bin',
        'result-zero.bin','report-zero.bin','launch.bin'))
    require(required<=set(frozen),'HDMI audio bundle omits admitted inputs.')
    records={key:frozen[key+'.bin'] for key in ('analog','power','supervision','audio_memory','audio_electrical','sink_edid')}
    records['dma_nonsecure_access']=frozen['dma_nonsecure_access.bin']
    security=json.loads(records['dma_nonsecure_access'])
    require(isinstance(security,dict) and isinstance(security.get('evidence'),list),
        'Invalid HDMI audio DMA security authority.')
    for i in range(len(security['evidence'])):
        key='dma_security_evidence_'+str(i)
        require(key+'.bin' in frozen,'Missing frozen HDMI audio DMA security evidence.')
        records[key]=frozen[key+'.bin']
    raw=frozen['source-session.bin'];session_size=len(raw)
    if session_size==816:
        for key in ('usb_initial','usb_electrical','usb_supervision'):
            require(key+'.bin' in frozen,'Missing frozen HDMI audio USB authority.')
            records[key]=frozen[key+'.bin']
        require(frozen.get('image.elf')==frozen['image_elf.bin'] and frozen.get('admission.json')==frozen['admission.bin'],
            'HDMI audio supervised aliases differ from admitted bytes.')
    if session_size==816:
        records['entry-index']=frozen['entry-index.bin']
        for i in range(len(json.loads(records['entry-index']))):
            records['entry-evidence-'+str(i)]=frozen['entry-evidence-'+str(i)+'.bin']
    parts,address,identity,fields,locations=validate_data(frozen['image_elf.bin'],raw,frozen['admission.bin'],frozen['pcm.bin'],records)
    require(frozen['session.bin']==raw+bytes(4096-session_size),'Invalid HDMI audio session page.')
    require(frozen['input.pcm']==frozen['pcm.bin'],'HDMI audio PCM upload differs.')
    require(metadata['build_id']==identity.hex() and metadata['image_sha256']==sha(frozen['image_elf.bin']) and
        metadata['session_sha256']==sha(raw) and
        all(metadata.get(k)==v for k,v in {**fields,**locations}.items()),'HDMI audio metadata changed.')
    request,contract=base.request_bytes(address,identity,raw,fields['epoch'],fields['generation'],fields['lease'],fields['supervisor'])
    require(frozen['request.bin']==request and frozen['contract.bin']==contract,'HDMI audio request/contract changed.')
    expected=[]
    for i,(start,size,data) in enumerate(parts):
        if data:
            name=f'segment-{i}.bin';require(frozen[name]==data,'HDMI audio image segment changed.');expected.append(dict(name=name,address=start))
    expected += [dict(name=name,address=start) for name,start in [('session.bin',0x41f00000),('input.pcm',0x41f10000),
        ('request.bin',0x52c00),('contract.bin',0x42004000),('result-zero.bin',0x42000000),('report-zero.bin',0x52d00),('launch.bin',0x52000)]]
    require(metadata['reservations']==[dict(address=a,bytes=n) for a,n,_ in parts] and
        metadata['payloads']==expected and frozen['result-zero.bin']==bytes(16384) and
        frozen['report-zero.bin']==bytes(128) and 0<len(frozen['launch.bin'])<=0xc00,'HDMI audio upload layout differs.')
    if session_size==816:
        require(metadata.get('segments')==[p for p in expected if p['name'].startswith('segment-') or p['name'] in ('session.bin','input.pcm')],
            'HDMI audio supervised upload segments differ.')
    authority_record=json.loads(frozen['admission.bin'])
    require(all(metadata.get(k)==authority_record.get(k) for k in ('physical_authority','modeled')),
        'HDMI audio bundle authority label differs.')
    metadata['_frozen']=frozen
    return metadata


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','session','admission','pcm','output','clang','lld','objcopy'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();prepare(**vars(args))
