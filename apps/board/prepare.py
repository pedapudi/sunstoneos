#!/usr/bin/env python3
"""Offline protected input preparation for the supervised complete board image."""
import io
import types
from elftools.elf.elffile import ELFFile
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
base=module('board_smp_base',ROOT/'tools/check-a733-smp.py')
supervisor=module('board_supervised_base',ROOT/'tools/check-a733-smp-supervised.py')
require,digest,save_json=base.require,base.digest,base.save_json
SESSION_MAGIC=0x4153373342535332
CONTRACT_MAGIC=0x4153373342534332
REQUEST_MAGIC=0x4153373342534c32
SCHEMA='a733-board-supervised-bundle-v2'
artifact=supervisor.observer.artifact
PROFILES={'sunstone-a733-board':'board-supervised-v2',
          'sunstone-a733-board-single-core':'board-single-core-supervised-v1'}
def image_manifest_check(manifest,raw):
    target=manifest.get('target');profile=PROFILES.get(target)
    require(profile and manifest.get('execution_profile')==profile and
            manifest.get('final_elf')==hashlib.sha256(raw).hexdigest(),'Tested Board image manifest required.')
    elf=ELFFile(io.BytesIO(raw));symbols={v.name:v for v in elf.get_section_by_name('.symtab').iter_symbols()}
    marker=symbols.get('board_single_core_geometry')
    if target=='sunstone-a733-board-single-core':
        require(marker is not None and marker['st_size']==32 and
                symbols.get('sd_single_core_entry') and symbols['sd_single_core_entry']['st_value']==elf['e_entry'] and
                not any(n in symbols for n in ('a733_cpu_start_init','a733_cpu_primary_continue',
                    'a733_smp_pre_scheduler','FreeRTOS_SGI_Handler')),'Board single-core startup differs.')
        address=int(marker['st_value']);parts=[p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and
            not p['p_flags']&2 and p['p_vaddr']<=address and address+32<=p['p_vaddr']+p['p_filesz']]
        require(len(parts)==1,'Board geometry must be immutable and file backed.')
        offset=address-parts[0]['p_vaddr']
        require(struct.unpack('<4Q',parts[0].data()[offset:offset+32])==(0x4153373342534331,1,592,60),
                'Board single-core execution geometry differs.')
    else:require(marker is None,'Single-core Board image requires its execution profile.')
    return profile

def usb3_hid_image_check(raw):
    # A caller-supplied manifest cannot add capabilities to an older image.
    elf=ELFFile(io.BytesIO(raw));table=elf.get_section_by_name('.symtab')
    matches=[v for v in table.iter_symbols() if v.name=='board_usb3_hid_capability'] if table else []
    require(len(matches)==1 and matches[0]['st_size']==32,'Native SuperSpeed keyboard capability is absent.')
    address=int(matches[0]['st_value']);parts=[p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and
        not p['p_flags']&2 and p['p_vaddr']<=address and address+32<=p['p_vaddr']+p['p_filesz']]
    require(len(parts)==1,'SuperSpeed keyboard capability must be immutable and file backed.')
    offset=address-parts[0]['p_vaddr']
    require(struct.unpack('<4Q',parts[0].data()[offset:offset+32])==(0x4153373353484431,1,8,64),
            'Native SuperSpeed keyboard capability differs.')

def single_core_platform(value,prepared,identity,port):
    require(value.get('schema')=='a733-board-single-core-platform-v1' and value.get('identity')==identity and
            value.get('port')==port and value.get('image_sha256')==prepared['image_sha256'],
            'Board single-core platform admission differs.')
    required={'primary-cache-transition-and-secondary-off','cold-cache-and-dirty-alias-exclusion',
              'dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded',
              'physical-code-publication','usb-clock-power-reset-ownership','usb-device-mapping',
              'physical-counter-continuity','rom-usb0-pio-takeover',
              'usb0-irq188-189-190-disabled-inactive-clear','usb-supplies-stable-and-writers-excluded'}
    require(set(value.get('conditions',{}))==required,'Board single-core platform conditions differ.')
    for item in value['conditions'].values():artifact(item)
    require(value.get('entry_conditions_established') is True and value.get('physical_coherence_result')=='under-test',
            'Board platform record must establish actual entry conditions.')

def request_bytes(address,identity,epoch,generation,lease,session):
    require(len(identity)==32 and all(type(x) is int and 0<x<2**64 for x in (epoch,generation,lease)) and
        0x41000000<=address<=0x411fffe0,'Invalid supervised launch identity.')
    h=hashlib.sha256(session).digest()
    request=struct.pack('<8Q',REQUEST_MAGIC,2,128,epoch,generation,generation^((1<<64)-1),lease,address)+identity+h
    contract=struct.pack('<6Q',0,2,224,epoch,generation,generation^((1<<64)-1))+identity
    contract+=struct.pack('<14Q',0,0,0,0,lease,0,0,0,0,0,0,0,0x41f00000,592)+h
    return request,contract+bytes(4096-len(contract))
def session_check(raw,usb_reference_modes=(1,2)):
    require(len(raw)==592 and struct.unpack_from('<3Q',raw)==(SESSION_MAGIC,2,592),'Distinct supervised session required.')
    require(raw[112:144]==bytes(32) and raw[144:184]==bytes(40),'Supervised session must contain no timer/deadline claims.')
    require(struct.unpack_from('<I',raw,532)[0] in usb_reference_modes,'Unsupported USB0 reference/takeover mode.')
    workload=struct.unpack_from('<I',raw,400)[0];require(workload in (0,1,2,3,4,6,8) and (workload not in (4,6,8) or struct.unpack_from('<Q',raw)[0]==0x4153373342535332),'Unsupported fixed workload.')
    return workload

def profile_spans(data,session,payload_spans=None):
    require(len(data)==1872 and struct.unpack_from('<QII',data) in ((0x3152464f52545353,4,1872),(0x3152464f52545353,6,1872)),
        'Unsupported fixed radio profile geometry.')
    mode=struct.unpack_from('<H',data,1338)[0];version=struct.unpack_from('<I',data,8)[0]
    require(mode==(1 if version==6 else 0) and (not mode or any(data[1328:1332])),
        'Invalid admitted TCP client/listener mode or peer IPv4.')
    policy,dns,reserved,address_timeout,lease_cap=struct.unpack_from('<B4s3sII',data,1856)
    require(policy in (0,1) and reserved==bytes(3) and
        ((policy==0 and address_timeout==lease_cap==0) or
         (policy==1 and 0<address_timeout<=(1<<63)-1 and 0<lease_cap<=(1<<63)-1)),
        'Invalid fixed/DHCP radio address policy.')
    require(struct.unpack_from('<QQ',session,352)==(0x43000000,len(data)) and
        hashlib.sha256(data).digest()==session[368:400],'Profile address/length/digest differs from session.')
    require(all(struct.unpack_from('<Q',data,offset)[0]==0 for offset in (192,200,208,344,360,1232,1288,1320)),
        'Uploaded radio handoffs/software deadlines must be zero.')
    addresses=struct.unpack_from('<4Q',data,432);lengths=struct.unpack_from('<4Q',data,464)
    spans=[]
    for start,length in zip(addresses,lengths):
        require(0x43010000<=start<0x43100000 and 0<length<=0x43100000-start,'Invalid retained firmware span.')
        require(all(start>=begin+size or begin>=start+length for begin,size in spans),'Overlapping profile firmware spans.')
        spans.append((start,length))
    if payload_spans is not None:require(list(payload_spans)==spans,'Exactly the four authenticated profile spans must be uploaded.')
    return spans

def build_launcher(output,clang,lld):
    shutil.copy2(ROOT/'apps/board/launch.c',output/'launch.c')
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text()
    (output/'base-supervised-launch.c').write_text(text)
    text=text.replace('0x4153373353554c33','0x4153373342534c32').replace('REQUEST[1]==3','REQUEST[1]==2').replace('CONTRACT->version==3','CONTRACT->version==2')
    text=text.replace('CONTRACT->reserved[0] && !CONTRACT->reserved[1]','CONTRACT->reserved[0]==A733_BOARD_SESSION_ADDRESS && CONTRACT->reserved[1]==sizeof(struct a733_board_session)')
    text=text.replace('20*A733_SMP_COUNTER_HZ','A733_BOARD_SUPERVISED_SECONDS*A733_SMP_COUNTER_HZ')
    (output/'board-launch-base.c').write_text(text)
    for name,source in [('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),
                        ('boot.h',ROOT/'platform/a733-smp-cached/boot.h'),('a733_board_application.h',ROOT/'include/a733_board_application.h')]:shutil.copy2(source,output/name)
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-Wall','-Wextra','-Werror',
        '-DSUNSTONE_BOARD_SUPERVISED=1','-DA733_SMP_CONTRACT_MAGIC=0x4153373342534332ULL','-I'+str(output)]
    commands=[]
    for name,target in [('launch.S','entry.o'),('launch.c','launch.o')]:commands.append([clang,*flags,'-c',output/name,'-o',output/target])
    commands.extend([[lld,'-T',output/'launch.ld',output/'entry.o',output/'launch.o','-o',output/'launch.elf'],
        ['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',output/'launch.elf',output/'launch.bin']])
    for command in commands:
        result=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (output/'build.log').open('a') as f:f.write(result.stdout+result.stderr)
        require(result.returncode==0,'Board launcher build failed; inspect build.log.')
    save_json(output/'commands.json',[[str(x) for x in c] for c in commands])
    require(0<(output/'launch.bin').stat().st_size<=0xc00,'Board launcher exceeds reserved SRAM.')

def validate_admission(record,image_hash,template_hash,cache=None,frozen=False,execution_profile="board-supervised-v2"):
    # Snapshot each evidence byte string once; parse/hash use that same value.
    cache={} if cache is None else cache
    def evidence(item):
        require(isinstance(item,dict) and set(item)=={'path','sha256'},'Invalid evidence reference.')
        path=Path(item['path']);require(path.is_absolute(),'Evidence path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Evidence missing from frozen bundle.');cache[str(path)]=path.read_bytes()
        data=cache[str(path)];require(hashlib.sha256(data).hexdigest()==item['sha256'],'Evidence changed.');return data
    require(record.get('schema')=='a733-board-supervised-entry-v2' and record.get('session_template_sha256')==template_hash and
        record.get('image_sha256')==image_hash and type(record.get('reservation')) is int and 0<record['reservation']<2**64,
        'Board session template admission differs.')
    identity,port=record.get('identity'),record.get('port');require(identity and port,'Fresh target identity required.')
    def memory(item,ranges):
        ddr=json.loads(evidence(item))
        require(ddr.get('schema')=='a733-ddr-reservation-v1' and ddr.get('identity')==identity and ddr.get('port')==port and
            ddr.get('image_sha256')==image_hash and ddr.get('exclusive') is True and ddr.get('published_cache_disabled') is True and
            ddr.get('remote_writers_excluded') is True and ddr.get('ranges')==ranges and ddr.get('evidence'),
            'Actual DDR initialization/reservation does not cover the selected image.')
        for ref in ddr['evidence']:evidence(ref)
    memory(record['ddr'],[[0x41000000,0x42200000]])
    if record.get('radio_ddr') is not None:memory(record['radio_ddr'],[[0x43000000,0x43100000]])
    platform=json.loads(evidence(record['platform']))
    # Reuse the existing exact platform schema, substituting only byte-snapshot
    # artifact validation; its callback never opens returned paths itself.
    import types
    original=single_core_platform if execution_profile=='board-single-core-supervised-v1' else supervisor.observer.validate_platform
    validate=types.FunctionType(original.__code__,{**original.__globals__,'artifact':evidence})
    validate(platform,{'image_sha256':image_hash},identity,port)
    recovery=record.get('recovery',{})
    require(recovery.get('mode')=='manual-usb-reconnect' and all(recovery.get(k) is True for k in
        ['demonstrated_fel','explicit_reconnect_before_resumption','preserve_microsd','one_attempt']) and recovery.get('evidence'),
        'Explicit one-attempt manual recovery record required.')
    for ref in recovery['evidence']:evidence(ref)
    require(record.get('whole_board_power_removal') is True and record.get('no_resource_reuse_before_power_removal') is True,
        'Explicit complete-power-removal/retention plan required.')
    if record.get('superspeed') is not None:
        ss=json.loads(evidence(record['superspeed']))
        require(ss.get('schema')=='a733-usb3-gen1-physical-entry-v1' and ss.get('identity')==identity and ss.get('port')==port and
            ss.get('image_sha256')==image_hash and ss.get('session_template_sha256')==template_hash and
            ss.get('physical_access_performed') is True and type(ss.get('qualified_source_ma')) is int and ss['qualified_source_ma']>=900,
            'Actual SuperSpeed physical/source admission differs.')
        device_class=ss.get('device_class','msc')
        require(device_class in ('msc','hid-keyboard'),'Unsupported fixed SuperSpeed device class.')
        required={'typec24-comb0-fitted-route','dcdc2-comb0-core-stable-800mv','cldo5-comb0-analog-stable-1800mv',
            'comb0-rtc-reference-and-common-clocks-stable','comb0-dp-remote-writers-excluded',
            ('reviewed-passive-gen1-cable-and-hid-keyboard' if device_class=='hid-keyboard' else 'reviewed-passive-gen1-cable-and-bot-device'),'source-switch-discharge-and-reverse-blocking-qualified'}
        require(set(ss.get('conditions',{}))==required and all(v is True for v in ss['conditions'].values()) and ss.get('evidence'),
            'SuperSpeed route/power/exclusion evidence missing.')
        for ref in ss['evidence']:evidence(ref)
    return cache

def superspeed_source_check(record,cache,session):
    workload=struct.unpack_from('<I',session,400)[0]
    if workload not in (6,8):return
    ref=record.get('superspeed')
    require(ref is not None,'Actual SuperSpeed physical admission required.')
    fact=json.loads(cache[ref['path']]);capacity=fact.get('qualified_source_ma')
    require(fact.get('device_class','msc')==('hid-keyboard' if workload==8 else 'msc'),'SuperSpeed class authority differs from session.')
    require(type(capacity) is int and 900<=struct.unpack_from('<I',session,552)[0]<=capacity,
            'Session source capacity exceeds actual SuperSpeed qualification.')

def prepare(image,manifest_path,admission_path,session_template,output,clang,lld,profile=None,payloads=()):
    image=Path(image);output=Path(output);manifest_path=Path(manifest_path);admission_path=Path(admission_path);session_template=Path(session_template)
    image_data=image.read_bytes();manifest_data=manifest_path.read_bytes();admission_data=admission_path.read_bytes()
    template_data=session_template.read_bytes();profile_data=Path(profile).read_bytes() if profile is not None else None
    payload_snapshots=[(dict(item),Path(item['path']).read_bytes()) for item in payloads]
    image_hash=hashlib.sha256(image_data).hexdigest();template_hash=hashlib.sha256(template_data).hexdigest()
    manifest=json.loads(manifest_data)
    execution_profile=image_manifest_check(manifest,image_data)
    admission=json.loads(admission_data)
    raw=bytearray(template_data);workload=session_check(raw,usb_reference_modes=(2,) if execution_profile=='board-single-core-supervised-v1' else (1,))
    require(workload!=4 or (execution_profile=='board-supervised-v2' and manifest.get('usb2_combined') is True),'Combined USB image capability required.')
    require(workload!=6 or (execution_profile=='board-supervised-v2' and manifest.get('usb3_gen1') is True),'SuperSpeed image capability required.')
    require(workload!=8 or (execution_profile=='board-supervised-v2' and manifest.get('usb3_gen1') is True and manifest.get('usb3_hid') is True),'SuperSpeed keyboard image capability required.')
    if workload==8:usb3_hid_image_check(image_data)
    evidence_cache=validate_admission(admission,image_hash,template_hash,execution_profile=execution_profile)
    superspeed_source_check(admission,evidence_cache,raw)
    require(not workload&1 or admission.get('radio_ddr') is not None,'Radio profile/firmware DDR reservation missing.')
    epoch,generation,lease=(secrets.randbits(64) or 1 for _ in range(3))
    struct.pack_into('<3Q',raw,24,epoch,generation,lease)
    output.mkdir(parents=True,exist_ok=False);build_launcher(output,clang,lld)
    for data,name in [(image_data,'image.elf'),(manifest_data,'image-input-manifest.json'),(admission_data,'admission.json'),(template_data,'session-template.bin')]:(output/name).write_bytes(data)
    segments,address,identity=base.image_parts(output/'image.elf')
    raw[48:80]=identity
    evidence_snapshots={}
    for index,(path,data) in enumerate(evidence_cache.items()):
        name=f'entry-evidence-{index}.bin';(output/name).write_bytes(data)
        evidence_snapshots[path]={'name':name,'sha256':hashlib.sha256(data).hexdigest()}
    uploads=[]
    for index,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{index}.bin';(output/name).write_bytes(segment['data']);uploads.append({'name':name,'address':segment['address']})
    (output/'session.bin').write_bytes(raw);uploads.append({'name':'session.bin','address':0x41f00000})
    profile_name=None
    if workload&1:
        require(profile is not None,'Admitted radio profile required.')
        data=profile_data
        spans=profile_spans(data,raw,[(int(item['address']),len(blob)) for item,blob in payload_snapshots])
        require(len(data)==struct.unpack_from('<Q',raw,360)[0] and hashlib.sha256(data).digest()==raw[368:400],'Radio profile differs from session.')
        require(struct.unpack_from('<Q',raw,352)[0]==0x43000000,'Invalid profile address.')
        (output/'profile.bin').write_bytes(data);uploads.append({'name':'profile.bin','address':0x43000000});profile_name='profile.bin'
    else:require(profile is None and not payloads,'Non-radio selection cannot upload radio assets.')
    ranges=[]
    for index,(item,data) in enumerate(payload_snapshots):
        start=int(item['address']);length=len(data)
        require(0x43010000<=start<0x43100000 and 0<length<=0x43100000-start,'Firmware span outside retained reservation.')
        require(all(start>=end or begin>=start+length for begin,end in ranges),'Overlapping firmware spans.')
        require(hashlib.sha256(data).hexdigest()==item['sha256'],'Firmware payload differs from admission.')
        ranges.append((start,start+length));name=f'firmware-{index}.bin';(output/name).write_bytes(data);uploads.append({'name':name,'address':start})
    require(list(payloads)==admission.get('payloads',[]),'Firmware payloads differ from reviewed admission.')
    request,contract=request_bytes(address,identity,epoch,generation,lease,raw)
    for name,data in [('request.bin',request),('contract.bin',contract),('result-zero.bin',bytes(16384)),('report-zero.bin',bytes(128))]:(output/name).write_bytes(data)
    result={'schema':SCHEMA,'execution_profile':execution_profile,'image_sha256':image_hash,'launcher_sha256':digest(output/'launch.bin'),'admission_sha256':hashlib.sha256(admission_data).hexdigest(),
        'reservation':admission['reservation'],'build_id':identity.hex(),'epoch':epoch,'generation':generation,'lease':lease,'workload':workload,
        'session':'session.bin','profile':profile_name,'manifest':'image-input-manifest.json','image':'image.elf','segments':uploads,
        'reservations':[{k:v for k,v in s.items() if k!='data'} for s in segments],
        'entry_evidence':evidence_snapshots,'backup_sizes':{name:length for name,address,length in base.REGIONS},'physical_access':False,
        'files':{p.name:digest(p) for p in output.iterdir() if p.is_file()}}
    save_json(output/'prepared.json',result);return validate_bundle(output)

def validate_bundle(bundle):
    bundle=Path(bundle);metadata=(bundle/'prepared.json').read_bytes();p=json.loads(metadata);require(p.get('schema')==SCHEMA and p.get('physical_access') is False,'Unsupported board bundle.')
    sizes={}
    for name,expected in p['files'].items():
        require(Path(name).name==name and digest(bundle/name)==expected,'Prepared board input changed.');sizes[name]=(bundle/name).stat().st_size
    require(digest(bundle/'image.elf')==p['image_sha256'] and digest(bundle/'launch.bin')==p['launcher_sha256'],'Image/launcher changed.')
    cache={path:(bundle/item['name']).read_bytes() for path,item in p['entry_evidence'].items()}
    require(all(hashlib.sha256(cache[path]).hexdigest()==item['sha256'] for path,item in p['entry_evidence'].items()),'Frozen evidence changed.')
    admitted=json.loads((bundle/'admission.json').read_text())
    manifest=json.loads((bundle/'image-input-manifest.json').read_text())
    execution_profile=image_manifest_check(manifest,(bundle/'image.elf').read_bytes())
    require(p.get('execution_profile','board-supervised-v2')==execution_profile,'Board execution profile changed.')
    validate_admission(admitted,p['image_sha256'],digest(bundle/'session-template.bin'),cache,True,execution_profile)
    require(p['workload']!=4 or (execution_profile=='board-supervised-v2' and manifest.get('usb2_combined') is True),'Combined USB image capability required.')
    require(p['workload'] not in (6,8) or admitted.get('superspeed') is not None,'Actual SuperSpeed physical admission required.')
    require(p['workload']!=6 or (execution_profile=='board-supervised-v2' and manifest.get('usb3_gen1') is True),'SuperSpeed image capability required.')
    require(not p['workload']&1 or admitted.get('radio_ddr') is not None,'Radio DDR reservation missing.')
    require(p['workload']!=8 or (execution_profile=='board-supervised-v2' and manifest.get('usb3_gen1') is True and manifest.get('usb3_hid') is True),'SuperSpeed keyboard image capability required.')
    raw=(bundle/'session.bin').read_bytes();require(session_check(raw,usb_reference_modes=(2,) if execution_profile=='board-single-core-supervised-v1' else (1,))==p['workload'],'Workload changed.')
    if p['workload']==8:usb3_hid_image_check((bundle/'image.elf').read_bytes())
    superspeed_source_check(admitted,cache,raw)
    derived=bytearray((bundle/'session-template.bin').read_bytes())
    struct.pack_into('<3Q',derived,24,p['epoch'],p['generation'],p['lease']);derived[48:80]=bytes.fromhex(p['build_id'])
    require(raw==derived,'Session differs from admitted template derivation.')
    require(struct.unpack_from('<3Q',raw,24)==(p['epoch'],p['generation'],p['lease']) and raw[48:80].hex()==p['build_id'],'Session identity changed.')
    segments,address,identity=base.image_parts(bundle/'image.elf')
    require(identity.hex()==p['build_id'],'Build identity changed.')
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,p['epoch'],p['generation'],p['lease'],raw)):
        require((bundle/name).read_bytes()==data,'Launch request/contract changed.')
    require(0<sizes['launch.bin']<=0xc00 and p['backup_sizes']=={name:length for name,address,length in base.REGIONS},'SRAM reservation changed.')
    expected=[]
    for index,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{index}.bin';require((bundle/name).read_bytes()==segment['data'],'ELF segment changed.')
            expected.append({'name':name,'address':segment['address']})
    expected.append({'name':'session.bin','address':0x41f00000})
    if p['workload']&1:
        require(p['profile']=='profile.bin' and digest(bundle/'profile.bin')==raw[368:400].hex(),'Radio profile changed.')
        require(sizes['profile.bin']==struct.unpack_from('<Q',raw,360)[0],'Radio profile size changed.')
        expected.append({'name':'profile.bin','address':0x43000000})
        spans=profile_spans((bundle/'profile.bin').read_bytes(),raw,[(int(item['address']),sizes.get(f'firmware-{i}.bin',0)) for i,item in enumerate(admitted.get('payloads',[]))])
    else:require(p['profile'] is None and not admitted.get('payloads',[]),'Unexpected radio profile/firmware.')
    admitted=json.loads((bundle/'admission.json').read_text())
    for index,item in enumerate(admitted.get('payloads',[])):
        name=f'firmware-{index}.bin';address=int(item['address'])
        require(digest(bundle/name)==item['sha256'] and 0x43010000<=address<0x43100000 and
            sizes[name]<=0x43100000-address,'Retained firmware span changed.')
        expected.append({'name':name,'address':address})
    require(p['segments']==expected and all(x['name'] in sizes for x in expected),'Unsealed or changed upload.')
    p['_sizes']=sizes;p['_prepared_sha256']=hashlib.sha256(metadata).hexdigest();return p
if __name__=='__main__':
    a=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','admission','session-template','output','clang','lld'):a.add_argument('--'+name,type=Path,required=True)
    a.add_argument('--profile',type=Path);a.add_argument('--payloads',type=Path)
    x=a.parse_args();prepare(x.image,x.manifest,x.admission,x.session_template,x.output,x.clang,x.lld,x.profile,json.loads(x.payloads.read_text()) if x.payloads else ())
