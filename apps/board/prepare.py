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
    elf=ELFFile(io.BytesIO(raw));symbols={v.name:v for v in elf.get_section_by_name('.symtab').iter_symbols()}
    marker=symbols.get('board_single_core_geometry')
    target='sunstone-a733-board-single-core' if marker is not None else 'sunstone-a733-board';profile=PROFILES[target]
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

def usb3_topology_image_check(raw):
    elf=ELFFile(io.BytesIO(raw));table=elf.get_section_by_name('.symtab')
    matches=[v for v in table.iter_symbols() if v.name=='board_usb3_topology_capability'] if table else []
    require(len(matches)==1 and matches[0]['st_size']==32,'Native USB topology capability is absent.')
    address=int(matches[0]['st_value']);parts=[p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and
        not p['p_flags']&2 and p['p_vaddr']<=address and address+32<=p['p_vaddr']+p['p_filesz']]
    require(len(parts)==1,'USB topology capability must be immutable and file backed.')
    offset=address-parts[0]['p_vaddr']
    require(struct.unpack('<4Q',parts[0].data()[offset:offset+32])==(0x4153373353545031,1,0x1540,7),
            'Native USB topology capability differs.')


def usb_audio_image_check(raw):
    elf=ELFFile(io.BytesIO(raw));table=elf.get_section_by_name('.symtab')
    matches=[v for v in table.iter_symbols() if v.name=='board_usb_audio_capability' and v['st_info']['type']=='STT_OBJECT'] if table else []
    require(len(matches)==1 and matches[0]['st_size']==32,'Native USB audio capability is absent.')
    address=int(matches[0]['st_value']);parts=[p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and
        not p['p_flags']&2 and p['p_vaddr']<=address and address+32<=p['p_vaddr']+p['p_filesz']]
    require(len(parts)==1,'USB audio capability must be immutable and file backed.')
    offset=address-parts[0]['p_vaddr']
    require(struct.unpack('<4Q',parts[0].data()[offset:offset+32])==(0x4153373341554431,1,14,16000),'Native USB audio capability differs.')


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
    workload=struct.unpack_from('<I',raw,400)[0];require(workload in (0,1,2,3,4,6,8,10,12,14) and (workload not in (4,6,8,10,12,14) or struct.unpack_from('<Q',raw)[0]==0x4153373342535332),'Unsupported fixed workload.')
    topology=struct.unpack_from('<I',raw,404)[0]
    require(topology in (0,1) and (not topology or workload in (6,8,10,12)),'Unsupported fixed USB topology.')
    # Common native invariants (apps/board/admission.c), including templates
    # before preparation fills their image identity. Evidence identifies facts;
    # these wire checks do not establish the referenced physical conditions.
    require(all(any(raw[o:o+32]) for o in (80,216,248,280)) and raw[80:112]==raw[280:312] and
            all(struct.unpack_from('<4Q',raw,184)),'Missing or conflicting board authority.')
    host_workload=workload<2 or workload in (4,6,8,10,12,14)
    owners=struct.unpack_from('<4Q',raw,456)[:4 if host_workload else 2]
    require(all(owners) and len(set(owners))==len(owners) and not any(raw[582:588]) and
            struct.unpack_from('<2I',raw,512)==(3300000,3300000) and all(struct.unpack_from('<3I',raw,500)),
            'Invalid board owners, reserved fields, supplies or fabric.')
    if not host_workload:
        require(not any(raw[408:432]+raw[472:488]+raw[528:532]+raw[536:564]+raw[570:582]),
                'A workload without USB host tasks carries host authority or supplies.')
    if not workload&1:
        require(not any(raw[352:400]+raw[432:456]),'A workload without radio carries radio state.')
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

# The base deadline expression appears in the overflow guard, the diagnostic
# deadline and the expiry; every other launcher anchor appears once.
LAUNCHER_ANCHOR_COUNTS={'20*A733_SMP_COUNTER_HZ':3}

def adapt_launcher(text,changes,counts=LAUNCHER_ANCHOR_COUNTS):
    """Apply ordered edits to the shared supervised launcher source.

    Each anchor must occur exactly as often as expected. A missed edit would
    otherwise build a launcher whose contract every image admission refuses.
    """
    for old,new in changes.items():
        require(text.count(old)==counts.get(old,1),'Shared supervised launcher anchor changed: '+old)
        text=text.replace(old,new)
    return text

def build_launcher(output,clang,lld):
    shutil.copy2(ROOT/'apps/board/launch.c',output/'launch.c')
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text()
    (output/'base-supervised-launch.c').write_text(text)
    text=adapt_launcher(text,{'0x4153373353554c33':'0x4153373342534c32','REQUEST[1]==3':'REQUEST[1]==2','CONTRACT->version==3':'CONTRACT->version==2',
        'CONTRACT->reserved[0] && !CONTRACT->reserved[1]':'CONTRACT->reserved[0]==A733_BOARD_SESSION_ADDRESS && CONTRACT->reserved[1]==sizeof(struct a733_board_session)',
        '20*A733_SMP_COUNTER_HZ':'A733_BOARD_SUPERVISED_SECONDS*A733_SMP_COUNTER_HZ'})
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

def input_path(item, cache):
    """Snapshot a direct input object or a supplied file without process records."""
    require(isinstance(item, dict), 'Device input must be an object.')
    if set(item) == {'path', 'sha256'}:
        path = Path(item['path'])
        if str(path) not in cache:
            cache[str(path)] = path.read_bytes()
        require(hashlib.sha256(cache[str(path)]).hexdigest() == item['sha256'],
                'Device input changed before use.')
        return path
    value = {key: data for key, data in item.items() if key not in ('path', 'sha256')}
    raw = json.dumps(value, sort_keys=True, separators=(',', ':')).encode()
    identity = hashlib.sha256(raw).hexdigest()
    path = Path('/direct-input') / identity
    cache[str(path)] = raw
    item.update(path=str(path), sha256=identity)
    return path


unique_directory = module('board_host_evidence', ROOT/'tools/host_evidence.py').unique_directory


def snapshot_files(prepared):
    """Select the bytes consumed by launch, native I/O, and direct safety checks."""
    fixed={'image.elf','launch.bin','request.bin','contract.bin','result-zero.bin','report-zero.bin',
           'session.bin','source-session.bin','session-template.bin','admission.json','operation.bin','upload.bin','manifest.bin',
           'input.jpg','input.h264','input.h265','input.nv12','input.argb','input.bggr12',
           'input.pcm','reference.yu12','oracle.yuv','frame0.bin','frame1.bin'}
    fixed.update(item['name'] for item in prepared.get('segments',[]))
    fixed.update(prepared.get('_used_evidence',[]))
    if prepared.get('profile'):fixed.add(prepared['profile'])
    payload={'.bin','.pcm','.s16le','.nv12','.yu12','.yuv','.jpg','.jpeg','.h264','.argb','.bggr12','.hevc','.h265'}
    return {name:identity for name,identity in prepared['files'].items()
            if name in fixed or (name.startswith(('frame-','model-','firmware-'))
                                  and Path(name).suffix in payload)}


def input_cache(bundle, prepared):
    """Read supplied safety inputs only when the current validator consumes them."""
    references=prepared.get('entry_evidence',{})
    prepared['_used_evidence']=[]
    class Inputs(dict):
        def __init__(self):
            super().__init__();self.snapshots={}
        def __contains__(self,key):
            return dict.__contains__(self,key) or key in references
        def __getitem__(self,key):
            if not dict.__contains__(self,key):
                item=references[key];name=item['name']
                require(Path(name).name==name,'Invalid device input filename.')
                raw=(Path(bundle)/name).read_bytes();identity=hashlib.sha256(raw).hexdigest()
                require(identity==item['sha256']==prepared['files'][name],
                        'Device input changed before use.')
                dict.__setitem__(self,key,raw)
                prepared['_used_evidence'].append(name);self.snapshots[name]=raw
            return dict.__getitem__(self,key)
    return Inputs()


def validate_admission(record,image_hash,template_hash,cache=None,frozen=False,execution_profile="board-supervised-v2"):
    cache = {} if cache is None else cache
    require(isinstance(record, dict) and record.get('identity') and record.get('port'),
            'Explicit target identity and USB port required.')
    require(type(record.get('reservation')) is int and 0 < record['reservation'] < 2**64,
            'Native memory reservation identity required.')
    identity, port = record['identity'], record['port']
    def evidence(item):
        path = input_path(item, cache)
        return cache[str(path)]
    if record.get('superspeed') is not None:
        ss=json.loads(evidence(record['superspeed']))
        require(ss.get('identity')==identity and ss.get('port')==port and
            type(ss.get('qualified_source_ma')) is int and ss['qualified_source_ma']>=900,
            'Actual SuperSpeed physical/source admission differs.')
        topology=ss.get('topology','direct');protocol=ss.get('root_protocol','super-gen1')
        device_class=ss.get('device_class','msc')
        require(device_class in ('msc','hid-keyboard'),'Unsupported fixed SuperSpeed device class.')
        required={'typec24-comb0-fitted-route','dcdc2-comb0-core-stable-800mv','cldo5-comb0-analog-stable-1800mv',
            'comb0-rtc-reference-and-common-clocks-stable','comb0-dp-remote-writers-excluded',
            (('reviewed-typec24-usb2-companion-cable-and-hid-keyboard' if device_class=='hid-keyboard' else 'reviewed-typec24-usb2-companion-cable-and-bot-device') if protocol=='usb2-companion' else ('reviewed-passive-gen1-cable-and-hid-keyboard' if device_class=='hid-keyboard' else 'reviewed-passive-gen1-cable-and-bot-device')),'source-switch-discharge-and-reverse-blocking-qualified'}
        require(topology in ('direct','self-powered-hub') and protocol in ('super-gen1','usb2-companion'),'USB topology differs.')
        if ss.get('schema')=='a733-usb3-gen1-physical-entry-v1':
            require(topology=='direct' and protocol=='super-gen1','Legacy authority admits only direct SuperSpeed.')
        else:
            required.add('reviewed-root-protocol-and-leaf-binding')
            if topology=='self-powered-hub':
                required.update({'hub-local-supply-qualified','hub-downstream-power-qualified','hub-single-leaf-and-no-nested-hub'})
                require(type(ss.get('hub_ports')) is int and 1<=ss['hub_ports']<=7,
                        'Qualified self-powered hub supply and bounded ports required.')
                for ref in ss.get('hub_supply_evidence',[]):evidence(ref)
        require(set(ss.get('conditions',{}))==required and all(v is True for v in ss['conditions'].values()),
            'SuperSpeed route/power/exclusion evidence missing.')
        for ref in ss.get('evidence',[]):evidence(ref)
    return cache

def superspeed_source_check(record,cache,session):
    workload=struct.unpack_from('<I',session,400)[0]
    if workload not in (6,8,10,12):return
    ref=record.get('superspeed')
    require(ref is not None,'Actual SuperSpeed physical admission required.')
    fact=json.loads(cache[ref['path']]);capacity=fact.get('qualified_source_ma')
    require(fact.get('device_class','msc')==('hid-keyboard' if workload in (8,10) else 'msc'),'SuperSpeed class authority differs from session.')
    require(fact.get('topology','direct')==('self-powered-hub' if struct.unpack_from('<I',session,404)[0] else 'direct'),'USB topology authority differs from session.')
    require(fact.get('root_protocol','super-gen1')==('usb2-companion' if workload in (10,12) else 'super-gen1'),'Root protocol authority differs from session.')
    require(type(capacity) is int and 900<=struct.unpack_from('<I',session,552)[0]<=capacity,
            'Session source capacity exceeds actual SuperSpeed qualification.')

def prepare(image,manifest_path,admission_path,session_template,output,clang,lld,profile=None,payloads=()):
    image=Path(image);output=Path(output);manifest_path=Path(manifest_path) if manifest_path is not None else None;admission_path=Path(admission_path);session_template=Path(session_template)
    image_data=image.read_bytes();manifest_data=manifest_path.read_bytes() if manifest_path is not None else b'{}';admission_data=admission_path.read_bytes()
    template_data=session_template.read_bytes();profile_data=Path(profile).read_bytes() if profile is not None else None
    payload_snapshots=[(dict(item),Path(item['path']).read_bytes()) for item in payloads]
    image_hash=hashlib.sha256(image_data).hexdigest();template_hash=hashlib.sha256(template_data).hexdigest()
    manifest={}
    execution_profile=image_manifest_check(manifest,image_data)
    admission=json.loads(admission_data)
    raw=bytearray(template_data);workload=session_check(raw,usb_reference_modes=(2,) if execution_profile=='board-single-core-supervised-v1' else (1,))
    require(workload!=4 or (execution_profile=='board-supervised-v2'),'Combined USB image capability required.')
    require(workload!=6 or (execution_profile=='board-supervised-v2'),'SuperSpeed image capability required.')
    require(workload!=8 or (execution_profile=='board-supervised-v2'),'SuperSpeed keyboard image capability required.')
    if workload==14:
        require(execution_profile=='board-supervised-v2','USB audio requires the supervised board image.')
        usb_audio_image_check(image_data)
    if workload==8:usb3_hid_image_check(image_data)
    if workload in (10,12):
        require(execution_profile=='board-supervised-v2','Companion-root image profile required.')
        usb3_topology_image_check(image_data)
    evidence_cache=validate_admission(admission,image_hash,template_hash,execution_profile=execution_profile)
    if workload in (6,8) and struct.unpack_from('<I',raw,404)[0]:
        usb3_topology_image_check(image_data)
    superspeed_source_check(admission,evidence_cache,raw)

    epoch,generation,lease=(secrets.randbits(64) or 1 for _ in range(3))
    struct.pack_into('<3Q',raw,24,epoch,generation,lease)
    output=unique_directory(output);build_launcher(output,clang,lld)
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
    for name,expected in snapshot_files(p).items():
        require(Path(name).name==name and digest(bundle/name)==expected,'Prepared board input changed.');sizes[name]=(bundle/name).stat().st_size
    require(digest(bundle/'image.elf')==p['image_sha256'] and digest(bundle/'launch.bin')==p['launcher_sha256'],'Image/launcher changed.')
    cache=input_cache(bundle,p)
    None
    admitted=json.loads((bundle/'admission.json').read_text())
    manifest={}
    execution_profile=image_manifest_check(manifest,(bundle/'image.elf').read_bytes())
    require(p.get('execution_profile','board-supervised-v2')==execution_profile,'Board execution profile changed.')
    validate_admission(admitted,p['image_sha256'],digest(bundle/'session-template.bin'),cache,True,execution_profile)
    require(p['workload']!=4 or (execution_profile=='board-supervised-v2'),'Combined USB image capability required.')
    require(p['workload'] not in (6,8,10,12) or admitted.get('superspeed') is not None,'Actual SuperSpeed physical admission required.')
    require(p['workload']!=6 or (execution_profile=='board-supervised-v2'),'SuperSpeed image capability required.')

    require(p['workload']!=8 or (execution_profile=='board-supervised-v2'),'SuperSpeed keyboard image capability required.')
    raw=(bundle/'session.bin').read_bytes();require(session_check(raw,usb_reference_modes=(2,) if execution_profile=='board-single-core-supervised-v1' else (1,))==p['workload'],'Workload changed.')
    if p['workload']==14:
        require(execution_profile=='board-supervised-v2','USB audio requires the supervised board image.')
        usb_audio_image_check((bundle/'image.elf').read_bytes())
    if p['workload']==8:usb3_hid_image_check((bundle/'image.elf').read_bytes())
    if p['workload'] in (10,12):
        require(execution_profile=='board-supervised-v2','Companion-root image profile required.')
        usb3_topology_image_check((bundle/'image.elf').read_bytes())
    if p['workload'] in (6,8) and struct.unpack_from('<I',raw,404)[0]:
        usb3_topology_image_check((bundle/'image.elf').read_bytes())
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
    p['_input_files']=snapshot_files(p);p['_sizes']=sizes;p['_prepared_sha256']=hashlib.sha256(metadata).hexdigest();return p

if __name__=='__main__':
    a=argparse.ArgumentParser(description=__doc__)
    for name in ('image','admission','session-template','output','clang','lld'):a.add_argument('--'+name,type=Path,required=True)
    a.add_argument('--manifest',type=Path)
    a.add_argument('--profile',type=Path);a.add_argument('--payloads',type=Path)
    x=a.parse_args();prepare(x.image,x.manifest,x.admission,x.session_template,x.output,x.clang,x.lld,x.profile,json.loads(x.payloads.read_text()) if x.payloads else ())
