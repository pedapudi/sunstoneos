#!/usr/bin/env python3
"""Prepare, validate or launch the protected NPU image and capture its USB result."""
import argparse
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
import secrets
import shutil
import sqlite3
import struct
import subprocess
import sys
import time
import fcntl
ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location('smp_observe_base', ROOT / 'tools/check-a733-smp.py')
base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(base)  # Library import; the old tool's main guard stays inactive.
require, digest, artifact, save_json = base.require, base.digest, base.artifact, base.save_json
image_parts, module, sync_file = base.image_parts, base.module, base.sync_file
observer = base.module('npu_reused_observer', ROOT / 'tools/check-a733-smp-observe.py')
CONTRACT = base.CONTRACT
REQUEST_MAGIC = 0x415337334f4c4331
MESSAGE_MAGIC, FRAME_MAGIC = 0x53534f53, 0x415337334e505531
PRODUCT, SERIAL = 'sunstoneos NPU convolution observer', 'sunstoneos-a733-npu-convolution'
PREREQUISITES = {'independent-watchdog-rom-return', 'stable-protected-fel',
                 'ddr-reservation', 'npu-convolution-platform', 'npu-convolution-software'}

request_bytes = observer.request_bytes


CATALOGUE=json.loads((ROOT/'tools/npu-models.json').read_text())['models']
ARTIFACTS={name:(CATALOGUE[0][name]['bytes'],CATALOGUE[0][name]['sha256']) for name in ('model','input','reference')}

def selected_artifacts(prepared):
    name=prepared.get('model_name','conv8')
    selected=next((v for v in CATALOGUE if v['name']==name),None)
    require(selected is not None,'Unknown reviewed model.')
    return {key:(selected[key]['bytes'],selected[key]['sha256']) for key in ('model','input','reference')}

def image_profile(image):
    from elftools.elf.elffile import ELFFile
    with image.open('rb') as stream:
        elf=ELFFile(stream)
        symbols={s.name:s for s in elf.get_section_by_name('.symtab').iter_symbols()}
        def data(name,size):
            symbol=symbols[name];require(symbol['st_size']>=size,'Immutable record is too small.')
            section=elf.get_section(symbol['st_shndx'])
            require(not section['sh_flags']&1,'Admission record is writable.')
            offset=symbol['st_value']-section['sh_addr']
            value=section.data()[offset:offset+size]
            require(len(value)==size,'Truncated immutable record.');return value
        values=struct.unpack('<14Q',data('a733_npu_application_boot',112))
        keys=('epoch','generation','lease','npu_evidence','command_admission','usb_evidence',
              'pmic_evidence','pin_evidence','npu_owner','pmic_owner','usb_owner','bridge_owner',
              'npu_established','usb_established')
        profile=dict(zip(keys,values))
        symbol=symbols['a733_npu_application_model']
        model=data('a733_npu_application_model',symbol['st_size'])
        selected=next((v for v in CATALOGUE if v['model']['bytes']==len(model) and
            v['model']['sha256']==hashlib.sha256(model).hexdigest()),None)
        require(selected is not None,'Model is absent from reviewed catalogue.')
        profile['model_name']=selected['name']
        hashes=[]
        for name,(size,expected) in selected_artifacts(profile).items():
            value=data('a733_npu_application_'+name,size)
            require(hashlib.sha256(value).hexdigest()==expected,'Fixed NPU artifact differs: '+name)
            hashes.append(bytes.fromhex(expected))
        require(data('a733_npu_application_artifact_hashes',96)==b''.join(hashes),'Artifact digest table differs.')
        return profile


def prepare(image, manifest_path, output, launcher_bundle, refusal=False):
    manifest=json.loads(manifest_path.read_text())
    require(manifest.get('target')=='a733-npu-convolution' and manifest.get('protocol_version') in (1,2) and
            manifest.get('final_elf')==digest(image),'Image differs from reviewed NPU manifest.')
    prior=observer.validate_bundle(launcher_bundle)
    for name in ('launch.S','launch.c','launch.ld'):
        require((launcher_bundle/name).read_bytes()==(ROOT/'platform/a733-smp-observe'/name).read_bytes(),
                'Preserved launcher source differs; review its compatibility before use.')
    # The shared header documents v2 in comments; compact declarations remain v1.
    def contract_tokens(path):
        return re.sub(r'/\*.*?\*/|//[^\n]*','',path.read_text(),flags=re.S).split()
    require(contract_tokens(launcher_bundle/'boot.h')==contract_tokens(ROOT/'platform/a733-smp-cached/boot.h'),
            'Preserved launcher contract declarations differ.')
    profile=image_profile(image)
    require(all(profile[k] for k in ('epoch','generation','lease')),'Image needs fixed nonzero session identity even for refusal preparation.')
    if not refusal:
        require(profile['usb_established']==15 and profile['npu_established']==127 and
            all(profile[k] for k in ('npu_evidence','command_admission','usb_evidence','pmic_evidence','pin_evidence')) and
            all(profile[k] for k in ('npu_owner','pmic_owner','usb_owner','bridge_owner')) and
            len({profile[k] for k in ('npu_owner','pmic_owner','usb_owner','bridge_owner')})==4,
            'NPU/PMIC/pin/USB image records refuse admission; only --refusal preparation is available.')
    segments,address,build_id=image_parts(image)
    output.mkdir(parents=True,exist_ok=False)
    shutil.copy2(image,output/'image.elf');shutil.copy2(manifest_path,output/'image-input-manifest.json')
    for name in ('launch.bin','launch.S','launch.c','launch.ld','boot.h'):
        shutil.copy2(launcher_bundle/name,output/name)
    payloads=[]
    for index,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{index}.bin';(output/name).write_bytes(segment['data'])
            payloads.append({'name':name,'address':segment['address']})
    request,contract=request_bytes(address,build_id,profile['epoch'],profile['generation'],profile['lease'])
    for name,data in [('request.bin',request),('contract.bin',contract),('result-zero.bin',bytes(16384)),('report-zero.bin',bytes(128))]:
        (output/name).write_bytes(data)
    record={'schema':'a733-npu-convolution-bundle-v1','image_sha256':digest(image),
        'launcher_sha256':prior['launcher_sha256'],'build_id':build_id.hex(),**profile,
        'refusal_only':refusal,'segments':payloads,
        'launcher_provenance':{'prepared':str(launcher_bundle/'prepared.json'),'sha256':digest(launcher_bundle/'prepared.json')},
        'reservations':[{k:v for k,v in segment.items() if k!='data'} for segment in segments],
        'files':{f.name:digest(f) for f in output.iterdir() if f.is_file()},'physical_access':False}
    save_json(output/'prepared.json',record);return record


def validate_bundle(bundle):
    prepared = json.loads((bundle / 'prepared.json').read_text())
    require(prepared.get('schema') == 'a733-npu-convolution-bundle-v1' and prepared.get('physical_access') is False, 'Unsupported prepared bundle.')
    sizes = {}
    for name, expected in prepared['files'].items():
        require(Path(name).name == name, 'Invalid prepared filename.')
        data = (bundle / name).read_bytes()
        require(hashlib.sha256(data).hexdigest() == expected, 'Prepared input changed.')
        sizes[name] = len(data)
    segments, address, identity = image_parts(bundle / 'image.elf')
    expected_segments = []
    for index, segment in enumerate(segments):
        if segment['data']:
            name = f'segment-{index}.bin'
            require((bundle / name).read_bytes() == segment['data'], 'Segment bytes differ from ELF.')
            expected_segments.append({'name': name, 'address': segment['address']})
    require(prepared['segments'] == expected_segments and prepared['image_sha256'] == digest(bundle / 'image.elf') and
            prepared['build_id'] == identity.hex(), 'Prepared ELF metadata differs.')
    request, contract = request_bytes(address, identity, prepared['epoch'], prepared['generation'], prepared['lease'])
    for name, expected in [('request.bin', request), ('contract.bin', contract), ('result-zero.bin', bytes(16384)), ('report-zero.bin', bytes(128))]:
        require((bundle / name).read_bytes() == expected, 'Prepared launch input differs: ' + name)
    require(0 < (bundle / 'launch.bin').stat().st_size <= 0xc00 and prepared['launcher_sha256'] == digest(bundle / 'launch.bin'), 'Invalid launcher identity or size.')
    require(all(prepared.get(k,'conv8' if k=='model_name' else None)==v for k,v in image_profile(bundle/'image.elf').items()),'Prepared admission profile differs from ELF.')
    prepared['_sizes'] = sizes
    return prepared


def request(sequence, prepared, challenge):
    require(type(sequence) is int and 0 < sequence < 2**32 and len(challenge) == 32 and any(challenge), 'Invalid observer request.')
    return struct.pack('<4I2Q', MESSAGE_MAGIC, sequence, 0x33, 48, prepared['epoch'], prepared['generation']) + challenge


def response(frame, sequence, prepared, challenge):
    """Validate the entire fixed frame before interpreting any reported success."""
    artifacts=selected_artifacts(prepared)
    require(len(frame) in (528,1040), 'Incomplete or oversized observer response.')
    size=len(frame)-16
    require(struct.unpack_from('<4I', frame) == (MESSAGE_MAGIC, sequence, 0x80000033, size), 'Response header/session sequence differs.')
    p = frame[16:]
    digest_at=size-32
    require(hashlib.sha256(p[:digest_at]).digest() == p[digest_at:], 'Observer frame digest differs.')
    magic, version, wire_size, status, epoch, generation, lease, start, end, deadline, job = struct.unpack_from('<QII8Q', p)
    require((magic, version, wire_size) in ((FRAME_MAGIC,1,512),(FRAME_MAGIC,2,1024)) and wire_size==size and status in (0,1,2,3), 'Unsupported observer frame.')
    require((epoch, generation, lease) == (prepared['epoch'], prepared['generation'], prepared['lease']) and
            p[80:112] == challenge and p[112:144] == bytes.fromhex(prepared['build_id']), 'Stale or mismatched observer identity.')
    controller,npu_evidence,command_admission,usb_evidence,armed=struct.unpack_from('<5Q',p,144)
    require(controller==0x03600000 and (npu_evidence,command_admission,usb_evidence)==
        tuple(prepared[k] for k in ('npu_evidence','command_admission','usb_evidence')),
        'NPU profile differs from the reviewed image.')
    require(p[184:280]==b''.join(bytes.fromhex(h) for _,h in artifacts.values()),'Reviewed artifact identities differ.')
    if version==1:
        require(prepared.get('model_name','conv8')=='conv8','Legacy frame cannot publish selected model.')
        output_bytes,failure=struct.unpack_from('<2Q',p,384)
        output=p[280:352];output_digest=p[352:384]
        require(not any(p[400:480]),'Invalid reserved fields.')
        first_match=True
    else:
        output_bytes,failure,jobs,previous=struct.unpack_from('<4Q',p,824)
        output=p[280:280+output_bytes];output_digest=p[792:824]
        require(output_bytes<=512 and not any(p[280+output_bytes:792]) and not any(p[888:992]),'Invalid output padding/reserved fields.')
        require((jobs==2 and previous and previous!=job) if output_bytes else jobs==previous==0 and not any(p[856:888]),'Incomplete or repeated inference tickets.')
        first_match=p[856:888].hex()==artifacts['reference'][1]
    require(armed<deadline,'Invalid frame bounds.')
    if output_bytes:
        require(output_bytes==artifacts['reference'][0] and job and armed<=start<=end<deadline and
                hashlib.sha256(output).digest()==output_digest,'Invalid completed output.')
        matched=output_digest.hex()==artifacts['reference'][1] and first_match
        require(status==3 or status==0 and matched or status==2 and not matched,'Result comparison differs.')
    else:
        require(start==end==job==0 and not any(output) and not any(output_digest) and status in (1,3),'Incomplete frame claims a result.')
    require((status==3)==bool(failure) and (status!=1 or output_bytes==0),'Fault/status publication differs.')
    return {'status':('success','pending','mismatch','fault')[status],'epoch':epoch,'generation':generation,
        'lease':lease,'build_id':prepared['build_id'],'start_counter':start,'end_counter':end,
        'diagnostic_deadline':deadline,'armed_at':armed,'job':job,'failure':failure,
        'output_hex':output.hex() if output_bytes else None,'output_sha256':output_digest.hex() if output_bytes else None,
        'controller':controller,'frame_sha256':hashlib.sha256(frame).hexdigest()}


durable_frame = observer.durable_frame


class CaptureWindowExpired(RuntimeError):
    """Local budget exhausted after no transfer or a complete valid reply."""


def capture(exchange, prepared, challenge, evidence, record, save, deadline,
            now=time.monotonic, pause=time.sleep):
    """Only successful pending replies permit another bounded query; no retries."""
    observed_identity=None
    for sequence in range(1, 129):
        if now() >= deadline: raise CaptureWindowExpired('Observer capture deadline expired.')
        packet = request(sequence, prepared, challenge)
        frame = exchange(packet, min(500, max(1, int((deadline - now()) * 1000))))
        result = response(frame, sequence, prepared, challenge)
        identity=(result['armed_at'],result['diagnostic_deadline'])
        require(observed_identity is None or identity==observed_identity,'NPU boot timing changed within a session.')
        observed_identity=identity
        if now() >= deadline: raise CaptureWindowExpired('Valid observer response arrived after capture deadline.')
        path = evidence / f'observer-{sequence:02}.bin'
        durable_frame(path, frame)
        record.setdefault('observer_frames', []).append({'path':path.name, **result})
        if result['status'] != 'pending':
            record['npu_result'] = 'observed-'+result['status']
            record['durable_result'] = {'path':path.name, 'sha256':result['frame_sha256']}
            save(); return result
        save()
        pause(min(.025, max(0, deadline - now())))
    raise CaptureWindowExpired('Observer exhausted its bounded pending-query budget.')


def capture_until_recovery(exchange, prepared, challenge, evidence, record, save, deadline,
                           now=time.monotonic, pause=time.sleep):
    try:
        capture(exchange,prepared,challenge,evidence,record,save,deadline,now,pause)
    except CaptureWindowExpired as error:
        record['capture_limit']=str(error)
        record['npu_result']='unobserved'
        save()
        return False
    return True


def validate_software(value, bundle):
    require(value.get('schema') == 'a733-npu-convolution-software-v1' and value.get('physical_access') is False and
            value.get('composed_npu_result_session') is True and value.get('launcher_instruction_equivalence') is True,
            'Observer software evidence is incomplete.')
    for key, name in [('prepared','prepared.json'), ('image','image.elf'), ('launcher','launch.bin')]:
        require(artifact(value[key]) == (bundle/name).resolve(), 'Software evidence covers different artifacts.')
    require(artifact(value['host_tool']) == Path(__file__).resolve() and
            artifact(value['base_host_tool']) == Path(base.__file__).resolve() and
            artifact(value['observer_helper']) == Path(observer.__file__).resolve(), 'Software evidence covers different host tools.')
    require(value.get('evidence'), 'Observer evidence is missing.')
    for item in value['evidence']: artifact(item)


def claimed(database, test_id, owner, bundle, qualification, protection, ddr, platform):
    db = sqlite3.connect(database.resolve().as_uri() + '?mode=ro', uri=True); db.row_factory=sqlite3.Row
    try:
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0], 'Board queue is halted.')
        row=db.execute('SELECT * FROM tests WHERE id=?',(test_id,)).fetchone()
        require(row is not None and row['state']=='claimed' and row['owner']==owner, 'Coordinator queue claim required.')
        submission,review=json.loads(row['submission']),json.loads(row['review'])
        require(set(submission['prerequisites'])==PREREQUISITES and set(review['prerequisites'])==PREREQUISITES, 'Queue prerequisites differ.')
        refs={k:artifact(v) for k,v in review['prerequisites'].items()}
        for key,path in [('independent-watchdog-rom-return',qualification),('stable-protected-fel',protection),
                         ('ddr-reservation',ddr),('npu-convolution-platform',platform)]:
            require(refs[key]==path.resolve(), 'Queue prerequisite identity differs: '+key)
        validate_software(json.loads(refs['npu-convolution-software'].read_text()),bundle)
        require({(bundle/n).resolve() for n in ('prepared.json','image.elf','launch.bin')} <=
                {artifact(a) for a in submission['artifacts']}, 'Queue artifacts do not cover prepared image.')
    finally: db.close()


def validate_platform(value, prepared, identity, port):
    require(value.get('schema')=='a733-npu-convolution-platform-v1' and value.get('identity')==identity and
            value.get('port')==port and value.get('image_sha256')==prepared['image_sha256'], 'Observer platform admission differs.')
    required={'source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram',
              'exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership',
              'usb-device-mapping','physical-counter-continuity','npu-startup-sram-confinement-drain-ownership','pmic-bus-rail-pin-ownership'}
    if prepared.get('execution_profile')=='npu-single-core-supervised-v1':
        required.remove('source-backed-cpu-dsu-transition')
        required.update({'primary-cache-transition-and-secondary-off','rom-usb0-pio-takeover',
            'usb0-irq188-189-190-disabled-inactive-clear','usb-supplies-stable-and-writers-excluded'})
    require(set(value.get('conditions',{}))==required, 'Observer entry conditions are incomplete.')
    for item in value['conditions'].values(): artifact(item)
    require(value.get('usb_evidence')==prepared['usb_evidence'] and value.get('npu_evidence')==prepared['npu_evidence'] and
            value.get('command_admission')==prepared['command_admission'] and
            value.get('pmic_evidence')==prepared['pmic_evidence'] and value.get('pin_evidence')==prepared['pin_evidence'] and
            value.get('epoch')==prepared['epoch'] and value.get('generation')==prepared['generation'] and
            value.get('lease')==prepared['lease'],'Platform evidence does not bind immutable NPU/PMIC/pin/USB admission.')
    require(value.get('entry_conditions_established') is True and value.get('physical_coherence_result')=='under-test',
            'Platform record must establish entry conditions without claiming the experiment already passed.')


def launch_once(transaction, observe, bundle, prepared, evidence, record, save, ready):
    base.preload(transaction,bundle,prepared,evidence)
    words=[int(x,16) for x in transaction(*[a for _ in range(4) for a in ('readl','0x03000024')]).split()]
    require(len(words)==4 and all(0<=x<=0xffffffff and not x&0x100 for x in words), 'Physical FEL protection is not consistently low.')
    require(int(transaction('readl','0x02050014').strip(),16)&1==0, 'Watchdog enabled before launch.')
    ready()
    record.update(execution_intent={'entry':0x52000,'epoch':prepared['epoch'],'generation':prepared['generation'],
                  'native_elapsed_limit_ticks':24000},backup_sha256=prepared['_backup_hashes'].copy(),launch_attempted=True)
    save()
    started=time.monotonic()
    transaction('exe','0x52000')
    observe(started)
    record['launch_protocol_complete']=True
    record['complete']=record.get('npu_result')=='observed-success' and record.get('fresh_rom_response') is True
    save()

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__); sub=parser.add_subparsers(dest='operation',required=True)
    prep=sub.add_parser('prepare')
    for name in ('image','image-manifest','output'): prep.add_argument('--'+name,type=Path,required=True)
    prep.add_argument('--launcher-bundle',type=Path,required=True)
    prep.add_argument('--refusal',action='store_true',help='Offline refusal bundle; launch remains prohibited')
    validate=sub.add_parser('validate');validate.add_argument('--bundle',type=Path,required=True)
    live=sub.add_parser('launch')
    for name in ('bundle','fel','qualification','input-evidence','ddr-contract','platform-contract','evidence','database'):
        live.add_argument('--'+name,type=Path,required=True)
    live.add_argument('--claim',required=True); live.add_argument('--owner',required=True)
    live.add_argument('--confirm-bridge-stable',action='store_true',required=True)
    args=parser.parse_args(argv)
    if args.operation=='prepare':
        manifest=json.loads(args.image_manifest.read_text())
        require(manifest.get('target')=='a733-npu-convolution' and manifest.get('protocol_version') in (1,2), 'Reviewed observer image manifest required.')
        result=prepare(args.image,args.image_manifest,args.output.resolve(),args.launcher_bundle.resolve(),args.refusal)
        print(json.dumps(result,indent=2)); return 0
    bundle=args.bundle.resolve(); prepared=validate_bundle(bundle)
    if args.operation=='validate':
        print(json.dumps({k:v for k,v in prepared.items() if not k.startswith('_')},indent=2));return 0
    require(prepared.get('refusal_only') is False,'Offline refusal bundle cannot be launched.')
    manifest=json.loads((bundle/'image-input-manifest.json').read_text())
    require(manifest.get('target')=='a733-npu-convolution' and manifest.get('protocol_version') in (1,2) and
            manifest.get('final_elf')==prepared['image_sha256'], 'Bundle is not the reviewed observer image.')
    require(digest(args.fel)==base.FEL_HASH, 'FEL executable differs from reviewed bytes.')
    def ready(): claimed(args.database,args.claim,args.owner,bundle,args.qualification,args.input_evidence,args.ddr_contract,args.platform_contract)
    ready()
    qualification=json.loads(args.qualification.read_text()); protection=json.loads(args.input_evidence.read_text())
    ddr=json.loads(args.ddr_contract.read_text()); platform=json.loads(args.platform_contract.read_text())
    args.evidence.mkdir(parents=True,exist_ok=False)
    for label,path in [('host-tool.py',Path(__file__)),('base-host-tool.py',Path(base.__file__)),
                       ('observer-helper.py',Path(observer.__file__)),
                       ('qualification.json',args.qualification),('input-evidence.json',args.input_evidence),
                       ('ddr-contract.json',args.ddr_contract),('platform-contract.json',args.platform_contract),
                       ('prepared.json',bundle/'prepared.json')]: shutil.copy2(path,args.evidence/label)
    access=module('observe_usb_access',ROOT/'tools/usb-access.py')
    sys.path.insert(0,str(Path.home()/'git/mimus/tools')); import usb_guard
    usb_guard.check_health()
    record={'complete':False,'transactions':[],'claim':args.claim,'owner':args.owner,
            'bundle_sha256':digest(bundle/'prepared.json'),'launch_attempted':False,'npu_result':'unobserved'}
    def save(): save_json(args.evidence/'session.json',record)
    with access.board_lock() as node:
        access.require_session(node); access.require_rom(node); identity=access.identity(node)
        port=access.target(node); port_config={'port':port.name,'sysfs':str(port.resolve())}; failed=False
        base.validate_qualification(qualification,port.name,str(port.resolve()))
        require(protection.get('complete') is True and protection.get('asserted') is True and
                protection.get('workspace_restored') is True and protection.get('identity')==identity and
                protection.get('port')==port.name and protection.get('rom_sha256')==base.ROM_HASH,
                'Protected low FEL input evidence does not cover this instance.')
        require(ddr.get('schema')=='a733-ddr-reservation-v1' and ddr.get('identity')==identity and
                ddr.get('port')==port.name and ddr.get('image_sha256')==prepared['image_sha256'] and
                ddr.get('exclusive') is True and ddr.get('published_cache_disabled') is True and
                ddr.get('remote_writers_excluded') is True and ddr.get('ranges')==[[0x41000000,0x42200000]],
                'DDR reservation/publication contract is incomplete.')
        require(ddr.get('evidence'), 'DDR initialization evidence is missing.')
        for item in ddr['evidence']: artifact(item)
        validate_platform(platform,prepared,identity,port.name)
        record.update(initial_node=str(node),initial_identity=identity,port=port.name,
                      host_tool_sha256=digest(Path(__file__)),base_host_tool_sha256=digest(Path(base.__file__)),
                      usb_access_sha256=digest(ROOT/'tools/usb-access.py'),usb_guard_sha256=digest(Path(usb_guard.__file__)))
        save()
        def transaction(*arguments,pass_fds=()):
            nonlocal failed
            require(not failed,'A transport failure forbids all further requests.')
            entry={'arguments':[str(x) for x in arguments]}
            if pass_fds:
                entry['sealed_sources']=[{'fd':fd,'bytes':os.fstat(fd).st_size,
                    'sha256':hashlib.sha256(os.pread(fd,os.fstat(fd).st_size,0)).hexdigest(),
                    'seals':fcntl.fcntl(fd,fcntl.F_GET_SEALS)} for fd in pass_fds]
            record['transactions'].append(entry); save()
            try:
                require(access.identity(node)==identity,'USB instance changed before FEL request.')
                access.require_rom(node)
                result=subprocess.run([str(args.fel),'-d',f'{int(node.parent.name)}:{int(node.name)}',*map(str,arguments)],
                                      capture_output=True,text=True,timeout=15,pass_fds=pass_fds)
                entry.update(exit_code=result.returncode,stdout=result.stdout,stderr=result.stderr)
                require(result.returncode==0,'FEL request failed.')
            except (OSError,ValueError,subprocess.TimeoutExpired) as error:
                failed=True;entry['failure']=str(error);save();raise
            save();return result.stdout
        def observe(started):
            nonlocal node,identity,failed
            old=identity; departure=False; observed=False; native_io=False; deadline=started+20
            challenge=secrets.token_bytes(32)
            record['challenge']=challenge.hex();save()
            while time.monotonic()<deadline:
                candidate=access.session_node(port_config)
                if candidate is None: departure=True
                else:
                    try:
                        current=access.identity(candidate)
                        if current!=old: departure=True
                        if current!=old and os.access(candidate,os.R_OK|os.W_OK):
                            access.require_session(candidate)
                            revision=(port/'bcdDevice').read_text().strip()
                            if revision=='0103':
                                if observed:
                                    require(current==identity,'Unexpected second observer instance.')
                                    time.sleep(.01);continue
                                require((port/'product').read_text().strip()==PRODUCT and
                                        (port/'serial').read_text().strip()==SERIAL, 'Observer descriptors differ.')
                                node,identity=candidate,current
                                if time.monotonic()>=started+3.6:
                                    record['capture_limit']='Observer appeared after capture deadline.'
                                    observed=True;save();continue
                                native_io=True
                                import usb.core
                                device=usb.core.find(idVendor=0x1f3a,idProduct=0xefe8,
                                    bus=int(node.parent.name),address=int(node.name))
                                require(device is not None and device.bcdDevice==0x0103, 'Observer handle differs.')
                                def same():
                                    require(not failed and access.identity(node)==identity,'Observer USB instance changed; no more requests.')
                                    require((port/'bcdDevice').read_text().strip()=='0103','Observer revision changed.')
                                same();device.default_timeout=min(500,max(1,int((started+3.6-time.monotonic())*1000)))
                                device.set_configuration(1)
                                record['observer_identity']=identity;save()
                                def exchange(packet,timeout):
                                    nonlocal failed
                                    try:
                                        same()
                                        require(device.write(0x02,packet,timeout=timeout)==len(packet),'Observer request was incomplete.')
                                        same()
                                        remaining=int((started+3.6-time.monotonic())*1000)
                                        require(remaining>0,'Observer deadline expired after request write.')
                                        return bytes(device.read(0x81,1040,timeout=min(timeout,remaining)))
                                    except Exception:
                                        failed=True;raise
                                capture_until_recovery(exchange,prepared,challenge,args.evidence,record,save,started+3.6)
                                observed=True;native_io=False
                            elif revision=='02b3':
                                node,identity=candidate,current;access.require_rom(node)
                                require('soc=00001903(A733)' in transaction('version'),'Returned ROM is not A733.')
                                record.update(returned_node=str(node),returned_identity=identity,fresh_rom_response=True,
                                              recovery_limit='A fresh ROM response does not independently establish watchdog causation.')
                                save();return
                            else: raise ValueError('Unexpected service at the protected target port.')
                    except FileNotFoundError:
                        if native_io:
                            failed=True;raise
                        departure=True
                    except Exception:
                        if native_io: failed=True
                        raise
                time.sleep(.01)
            candidate=access.session_node(port_config)
            if not departure and candidate is not None and access.identity(candidate)==old and os.access(candidate,os.R_OK|os.W_OK):
                access.require_session(candidate);access.require_rom(candidate)
                base.observe_refusal(transaction,args.evidence,prepared,record,save)
                record['clean_refusal']=True;save();return
            raise ValueError('No fresh accessible ROM response within recovery observation bound.')
        try:
            require('soc=00001903(A733)' in transaction('version'),'Target is not A733.')
            with usb_guard.reenumeration('sunstoneos-a733','NPU convolution observer and independent watchdog ROM return',2):
                launch_once(transaction,observe,bundle,prepared,args.evidence,record,save,ready)
        except BaseException as error:
            record['transport_failed']=failed
            base.stop_after_failure(record,save,
                lambda:access.quarantine_instance(node,'Observer attempt stopped; no requests or automatic retry.',identity),error)
            raise
    print('NPU convolution: '+record['npu_result']+'. Fresh ROM response: '+str(record.get('fresh_rom_response',False))+'. Queue claim remains with the coordinator.')
    return 0 if record['complete'] else 2


if __name__=='__main__':
    try: sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error,struct.error) as error: sys.exit(f'Stopped: {error}')
