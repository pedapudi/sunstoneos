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
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('hdmi_audio_base',ROOT/'apps/hdmi/prepare.py')
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
SCHEMA='a733-hdmi-audio-supervised-bundle-v1'
GEOMETRY=(1,656,440,216,0x41f10000,192000,48000,2,16,32)
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


def image_metadata(image):
    parts,address,identity,size=base.image_data(image)
    require(size==656 and base.framebuffer_metadata(image)==(0,0),
        'HDMI audio requires the656-byte pattern image, without framebuffer mode.')
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
        _,geometry=marker('a733_hdmi_audio_geometry',80)
        require(struct.unpack('<10Q',geometry)==GEOMETRY,'Linked HDMI audio geometry differs.')
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
    require(len(raw)==656,'HDMI audio session must be656 bytes.')
    magic,version,size,epoch,generation,lease=struct.unpack_from('<6Q',raw)
    supervisor,owner=struct.unpack_from('<2Q',raw,208)
    require((magic,version,size)==(base.SESSION_MAGIC,1,656) and
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
    return dict(epoch=epoch,generation=generation,lease=lease,supervisor=supervisor,
        authority=authority,frames=frames,source_bytes=len(pcm))


def session_bytes(pattern,pcm,*,memory,electrical,authority,dma,admission):
    require(len(pattern)==440 and len(dma)==104 and len(pcm)%4==0,
        'HDMI audio producer requires pattern440, DMA104 and complete stereo frames.')
    raw=bytearray(pattern);struct.pack_into('<Q',raw,16,656)
    raw[80:112]=hashlib.sha256(admission).digest()
    raw+=hashlib.sha256(pcm).digest()+hashlib.sha256(memory).digest()+hashlib.sha256(electrical).digest()
    raw+=struct.pack('<2Q',authority,len(pcm)//4)+dma
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
    parts,address,identity,locations=image_metadata(image)
    fields=session_check(raw,pcm);record=json.loads(admission);image_hash=sha(image)
    require(raw[48:80]==identity and raw[80:112]==hashlib.sha256(admission).digest(),
        'HDMI audio session does not bind this image/admission.')
    base.clock_profile(record,struct.unpack_from('<I',raw,300)[0])
    require(record.get('schema')=='a733-hdmi-supervised-admission-v1' and
        record.get('image_sha256')==image_hash and
        all(record.get(key)==fields[key] for key in ('epoch','generation','lease','supervisor')),
        'HDMI audio admission names another image/session.')
    physical_authority(record)
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
    return parts,address,identity,fields,locations


def validate(image,session,admission,pcm):
    image_data=image.read_bytes();raw=session.read_bytes();admission_data=admission.read_bytes();pcm_data=pcm.read_bytes()
    record=json.loads(admission_data);records={}
    for key in ('analog','power','supervision','audio_memory','audio_electrical'):
        records[key]=(admission.parent/record[key]).resolve().read_bytes()
    records['sink_edid']=(admission.parent/record['sink_edid']['path']).resolve().read_bytes()
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
    fixed={'session.bin':(snapshots['session']+bytes(4096-656),0x41f00000),
        'input.pcm':(snapshots['pcm'],0x41f10000),'request.bin':(request,0x52c00),
        'contract.bin':(contract,0x42004000),'result-zero.bin':(bytes(16384),0x42000000),
        'report-zero.bin':(bytes(128),0x52d00),'launch.bin':((out/'launch.bin').read_bytes(),0x52000)}
    for name,(data,start) in fixed.items():(out/name).write_bytes(data);payloads.append(dict(name=name,address=start))
    for key,data in snapshots.items():
        name='source-session.bin' if key=='session' else key+'.bin'
        (out/name).write_bytes(data)
    authority_record=json.loads(snapshots['admission'])
    metadata=dict(schema=SCHEMA,physical_access=False,physical_authority=authority_record.get('physical_authority'),
        modeled=authority_record.get('modeled'),build_id=identity.hex(),
        image_sha256=sha(snapshots['image_elf']),session_sha256=sha(snapshots['session']),
        **fields,**locations,payloads=payloads,reservations=[dict(address=a,bytes=n) for a,n,_ in parts],
        files={p.name:sha(p.read_bytes()) for p in out.iterdir() if p.is_file()},
        tools={str(tool.absolute()):sha(tool.read_bytes()) for tool in (clang,lld,objcopy)})
    (out/'prepared.json').write_text(json.dumps(metadata,indent=2)+'\n')
    return validate_bundle(out)


def validate_bundle(bundle):
    metadata=json.loads((bundle/'prepared.json').read_bytes())
    require(metadata.get('schema')==SCHEMA and metadata.get('physical_access') is False,'Wrong HDMI audio bundle.')
    frozen={}
    for name,expected in metadata['files'].items():
        require(Path(name).name==name,'Invalid HDMI audio bundle filename.')
        frozen[name]=(bundle/name).read_bytes();require(sha(frozen[name])==expected,'HDMI audio bundle changed: '+name)
    required={key+'.bin' for key in ('image_elf','admission','pcm','analog','power','supervision','audio_memory','audio_electrical','sink_edid')}
    required.update(('source-session.bin','session.bin','input.pcm','request.bin','contract.bin',
        'result-zero.bin','report-zero.bin','launch.bin'))
    require(required<=set(frozen),'HDMI audio bundle omits admitted inputs.')
    records={key:frozen[key+'.bin'] for key in ('analog','power','supervision','audio_memory','audio_electrical','sink_edid')}
    parts,address,identity,fields,locations=validate_data(frozen['image_elf.bin'],frozen['session.bin'][:656],frozen['admission.bin'],frozen['pcm.bin'],records)
    require(len(frozen['session.bin'])==4096 and not any(frozen['session.bin'][656:]) and frozen['session.bin'][:656]==frozen['source-session.bin'],'Invalid HDMI audio session page.')
    require(frozen['input.pcm']==frozen['pcm.bin'],'HDMI audio PCM upload differs.')
    require(metadata['build_id']==identity.hex() and metadata['image_sha256']==sha(frozen['image_elf.bin']) and
        metadata['session_sha256']==sha(frozen['session.bin'][:656]) and
        all(metadata.get(k)==v for k,v in {**fields,**locations}.items()),'HDMI audio metadata changed.')
    request,contract=base.request_bytes(address,identity,frozen['session.bin'][:656],fields['epoch'],fields['generation'],fields['lease'],fields['supervisor'])
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
    authority_record=json.loads(frozen['admission.bin'])
    require(all(metadata.get(k)==authority_record.get(k) for k in ('physical_authority','modeled')),
        'HDMI audio bundle authority label differs.')
    metadata['_frozen']=frozen
    return metadata


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','session','admission','pcm','output','clang','lld','objcopy'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();prepare(**vars(args))
