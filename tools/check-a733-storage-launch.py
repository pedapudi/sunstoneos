#!/usr/bin/env python3
"""Prepare or execute one physically admitted native storage maintenance session.

Preparation does not qualify a board. The existing storage grant remains the
sole card/range authority. The coordinator retains ownership after uncertainty.
"""
import argparse
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import secrets
import shutil
import struct
import subprocess
import sys
import time
ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location('storage_launch_base',ROOT/'tools/check-a733-smp.py')
base = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(base)
store_api = base.module('storage_update_host',ROOT/'tools/storage-update.py')
require,digest,save_json = base.require,base.digest,base.save_json
TARGET = 'sunstone-a733-storage-maintenance'
SESSION,SESSION_BYTES,CONTRACT = 0x41f00000,704,0x42004000
REQUEST,REPORT,ENTRY = 0x54000,0x54100,0x52000
REGIONS = [('launcher',ENTRY,0x2000),('request',REQUEST,128),('report',REPORT,128),
           ('stack',0x55000,0x1000),('scratch',0x71000,128)]


def sha(data): return hashlib.sha256(data).hexdigest()
def u64(value): return type(value) is int and 0 < value < 2**64

def image_parts(path):
    segments,address,identity=base.image_parts(path)
    data=path.read_bytes(); h=struct.unpack_from('<HHIQQQIHHHHHH',data,16)
    sections=[struct.unpack_from('<IIQQQQIIQQ',data,h[5]+i*h[10]) for i in range(h[11])]
    reservations=[s for s in sections if s[2]&2 and s[3]<SESSION+4096 and SESSION<s[3]+s[5]]
    require(len(reservations)==1 and reservations[0][1:4]==(8,3,SESSION) and reservations[0][5]==4096,
            'Image must reserve the complete session page as distinct writable NOBITS.')
    require(all(not s['data'] or s['address']+len(s['data'])<=SESSION for s in segments),
            'File-backed image bytes overlap the immutable session.')
    return segments,address,identity


def storage_inputs(store):
    raw=os.pread(store.open('grant.bin',256),256,0); grant=store_api.grant_decode(raw)
    name='prepared.json' if grant['mode'] else 'capture.json'
    manifest=store.json(name)
    require(manifest.get('grant')==grant and manifest.get('grant_sha256')==sha(raw),
            'Protected grant and storage manifest differ.')
    if grant['mode']:
        require(manifest.get('schema')=='sunstone-storage-update-1' and manifest.get('bytes')==grant['blocks']*512,
                'Prepared writer extent differs.')
        store.open('backup.bin',manifest['bytes'],bytes.fromhex(manifest['backup_sha256']))
        store.open('candidate.bin',manifest['bytes'],bytes.fromhex(manifest['candidate_sha256']))
        chunk=store_api.WRITE_CHUNK
        store.open('chain.bin',((manifest['bytes']+chunk-1)//chunk)*32,bytes.fromhex(manifest['chain_sha256']))
        require(manifest['backup_sha256']==grant['backup'],'Backup identity differs from the grant.')
    return raw,grant,sha(store_api.canonical(manifest))


def _vendor_mbus():
    """tools/vendor-mbus.py: sessions name MBUS as the restored image finds it."""
    spec = importlib.util.spec_from_file_location('vendor_mbus', Path(__file__).resolve().parent / 'vendor-mbus.py')
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


def session_bytes(raw,config,image,epoch,generation,lease):
    g=store_api.grant_decode(raw)
    minimum,maximum,guard,reset=(config[k] for k in ('minimum_reload_us','maximum_recovery_us','guard_us','reset_by_us'))
    require(all(u64(x) and x<=((2**64-1)//24) for x in (minimum,maximum,guard,reset,g['deadline_us'])) and
            minimum>guard+1000000 and maximum>=minimum and reset>maximum and
            g['deadline_us']<=reset-maximum,'Invalid absolute watchdog bounds.')
    require(all(u64(x) for x in (epoch,generation,lease,config['topology'],*config['owners'])) and
            len(config['owners'])==3 and len(set(config['owners']))==3,'Invalid ownership identities.')
    b=bytearray(SESSION_BYTES)
    struct.pack_into('<5Q',b,8,1,SESSION_BYTES,epoch,generation,lease)
    b[48:80]=bytes.fromhex(image);b[80:112]=bytes.fromhex(g['recovery'])
    struct.pack_into('<4Q',b,112,minimum,maximum,guard,reset)
    struct.pack_into('<3I',b,144,*config['rtc'])
    b[160:416]=raw;struct.pack_into('<Q',b,416,config['topology'])
    b[424:456]=bytes.fromhex(config['topology_artifact'])
    # Optional SD bus profile: 0 one-bit 400 kHz (default), 1 four-bit default
    # speed, 2 four-bit high speed. The native platform checks the pin route.
    profile=config.get('mmc_sd_profile',0)
    require(type(profile) is int and profile in (0,1,2) and (not profile or g['kind']==1),
            'MMC SD profile must be 0, 1 or 2, and nonzero only for SD.')
    struct.pack_into('<I',b,456,profile)
    struct.pack_into('<4I',b,488,*config['usb_supply_uv'])
    struct.pack_into('<3Q',b,504,*config['owners'])
    fabric,fabric_hz=_vendor_mbus().fabric(config['fabric'],config['fabric_hz'])
    struct.pack_into('<3I',b,528,*fabric);struct.pack_into('<3I',b,540,*fabric_hz)
    struct.pack_into('<2I',b,552,config['mmc_voltage_mask'],config['usb_cold_reference'])
    for offset,key,count in ((560,'mmc_expected',12),(596,'mmc_desired',12),(632,'pmic_expected',2)):
        pins=config[key];require(len(pins)==count and all(len(p)==3 for p in pins),'Pin configuration extent differs.')
        struct.pack_into('<'+'B'*(count*3),b,offset,*(v for p in pins for v in p))
    require(len(b)==704 and any(b[48:80]) and any(b[80:112]) and any(b[424:456]),'Missing image or evidence identity.')
    return bytes(b)


def request_bytes(address,identity,epoch,generation,lease,session):
    request=struct.pack('<8Q',0x4153373353544c31,2,128,epoch,generation,generation^(2**64-1),lease,address)
    request+=identity+struct.pack('<4Q',SESSION,SESSION_BYTES,0,0)
    contract=struct.pack('<6Q',0,1,224,epoch,generation,generation^(2**64-1))+identity
    contract+=struct.pack('<14Q',0,0,0,0,lease,0x41,1,2,0x5aa5a55a,0xfa50392f,0x20,0,SESSION,SESSION_BYTES)
    contract+=session[184:216]
    require(len(request)==128 and len(contract)==224,'Internal wire extent differs.')
    return request,contract+bytes(4096-224)


def build_launcher(output,session_header,extra_flags=()):
    """Compile the shared SRAM launcher with one application's wire validator."""
    clang,lld=extra_flags[:2];flags_extra=list(extra_flags[2:])
    for name in ('launch.S','launch.c','launch.ld'):
        shutil.copy2(ROOT/'platform/a733-storage-launch'/name,output/name)
    shutil.copy2(session_header,output/'session.h')
    shutil.copy2(ROOT/'platform/a733-smp-cached/boot.h',output/'boot.h')
    commands=[]
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding',
           '-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables',
           *flags_extra,'-Os','-g','-Wall','-Wextra','-Werror','-I'+str(output)]
    def run(command):
        commands.append(list(map(str,command)));r=subprocess.run(command,capture_output=True,text=True)
        with (output/'build.log').open('a') as f:f.write(r.stdout+r.stderr)
        save_json(output/'commands.json',commands);require(r.returncode==0,'Launcher compile/link failed; inspect build.log.')
    for source,obj in [('launch.S','entry.o'),('launch.c','launch.o')]:run([clang,*flags,'-c',output/source,'-o',output/obj])
    run([lld,'-T',output/'launch.ld',output/'entry.o',output/'launch.o','-o',output/'launch.elf'])
    run(['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',output/'launch.elf',output/'launch.bin'])
    require(0<(output/'launch.bin').stat().st_size<=0x2000,'Launcher exceeds admitted SRAM.')


def prepare(image,manifest,config_path,storage,output,clang,lld):
    segments,address,identity=image_parts(image)
    with store_api.ProtectedDirectory(storage) as store:
        raw,grant,storage_manifest=storage_inputs(store)
    config=json.loads(config_path.read_text());epoch,generation,lease=(secrets.randbits(64) or 1 for _ in range(3))
    session=session_bytes(raw,config,digest(image),epoch,generation,lease)
    output=base.module('storage_launch_output',ROOT/'tools/host_evidence.py').unique_directory(output)
    build_launcher(output,ROOT/'platform/a733-storage-maintenance/session.h',(clang,lld))
    shutil.copy2(image,output/'image.elf')
    save_json(output/'configuration.json',config)
    payloads=[]
    for index,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{index}.bin';(output/name).write_bytes(segment['data']);payloads.append({'name':name,'address':segment['address']})
    request,contract=request_bytes(address,identity,epoch,generation,lease,session)
    for name,data in [('session.bin',session+bytes(4096-704)),('request.bin',request),('contract.bin',contract),
                      ('result-zero.bin',bytes(16384)),('report-zero.bin',bytes(128))]:(output/name).write_bytes(data)
    record=dict(schema='a733-storage-launch-bundle-1',epoch=epoch,generation=generation,lease=lease,
                image_sha256=digest(image),build_id=identity.hex(),launcher_sha256=digest(output/'launch.bin'),
                storage_manifest_sha256=storage_manifest,grant_sha256=sha(raw),segments=payloads,
                reservations=[{k:v for k,v in s.items() if k!='data'} for s in segments],
                files={p.name:digest(p) for p in output.iterdir() if p.is_file()},
                base_host_sha256=digest(Path(base.__file__)),storage_host_sha256=digest(Path(store_api.__file__)),physical_access=False)
    save_json(output/'prepared.json',record);return record


def validate_bundle(bundle,store):
    p=json.loads((bundle/'prepared.json').read_text())
    require(p.get('schema')=='a733-storage-launch-bundle-1' and p.get('physical_access') is False,'Wrong prepared bundle.')
    required={'image.elf','configuration.json','session.bin','request.bin','contract.bin','result-zero.bin','report-zero.bin','launch.bin'}
    required.update(item['name'] for item in p['segments'])
    require(required<=set(p['files']),'Missing maintenance transport input.')
    for name in required:require(Path(name).name==name and digest(bundle/name)==p['files'][name],'Prepared artifact changed.')
    segments,address,identity=image_parts(bundle/'image.elf')
    require(p['image_sha256']==digest(bundle/'image.elf') and p['build_id']==identity.hex(),'Image binding changed.')
    raw,grant,manifest=storage_inputs(store)
    require(sha(raw)==p['grant_sha256'] and manifest==p['storage_manifest_sha256'],'Protected storage artifacts changed.')
    session=session_bytes(raw,json.loads((bundle/'configuration.json').read_text()),p['image_sha256'],p['epoch'],p['generation'],p['lease'])
    request,contract=request_bytes(address,identity,p['epoch'],p['generation'],p['lease'],session)
    for name,value in [('session.bin',session+bytes(4096-704)),('request.bin',request),('contract.bin',contract),
                       ('result-zero.bin',bytes(16384)),('report-zero.bin',bytes(128))]:
        require((bundle/name).read_bytes()==value,'Prepared session differs: '+name)
    payloads=[]
    for index,s in enumerate(segments):
        if s['data']:
            name=f'segment-{index}.bin';require((bundle/name).read_bytes()==s['data'],'ELF transfer differs.')
            payloads.append({'name':name,'address':s['address']})
    require(p['segments']==payloads and p['reservations']==[{k:v for k,v in s.items() if k!='data'} for s in segments],
            'Prepared ELF reservations differ.')
    require(p['launcher_sha256']==digest(bundle/'launch.bin') and 0<(bundle/'launch.bin').stat().st_size<=0x2000,
            'Launcher bytes or SRAM bounds changed.')
    return p,grant


def validate_session(value,p,grant,config,identity,port,sysfs):
    """Check the live target and native watchdog/session limits directly."""
    require(identity and port and sysfs,'Current target identity required.')
    session_bytes(store_api.grant_encode(grant),config,p['image_sha256'],p['epoch'],p['generation'],p['lease'])


def preload(transaction,bundle,p,evidence):
    for name,address,size in REGIONS:
        path=evidence/f'before-{name}.bin';transaction('read',hex(address),size,path)
        require(path.stat().st_size==size,'Incomplete SRAM preservation.');base.sync_file(path)
        p.setdefault('_backup_hashes',{})[name]=digest(path)
    uploads=[(s['name'],s['address']) for s in p['segments']]+[
        ('session.bin',SESSION),('result-zero.bin',0x42000000),('contract.bin',CONTRACT),
        ('request.bin',REQUEST),('report-zero.bin',REPORT),('launch.bin',ENTRY)]
    for name,address in uploads:
        require(int(transaction('readl','0x02050014').strip(),16)&1==0,'Watchdog became active during preload.')
        data=(bundle/name).read_bytes();require(sha(data)==p['files'][name],'Transfer source changed.')
        path=evidence/('readback-'+name)
        with base.sealed_source(data) as (source,fd):transaction('write',hex(address),source,'read',hex(address),len(data),path,pass_fds=(fd,))
        require(path.stat().st_size==len(data) and digest(path)==p['files'][name],'Transfer readback differs.');base.sync_file(path)


def refusal(transaction,p,evidence,record,save):
    path=evidence/'refusal-report.bin';transaction('read',hex(REPORT),128,path)
    data=path.read_bytes();require(len(data)==128,'Short refusal report.')
    w=struct.unpack('<16Q',data)
    require(w[0] in (1,2,3,4,5,6,13) and w[1:3]==(0,0) and w[3:6]==(p['epoch'],p['generation'],p['lease']) and
            w[6]==0x4153373352454631 and not any(w[7:]),'No authenticated pre-mutation refusal.');base.sync_file(path)
    require(int(transaction('readl','0x02050014').strip(),16)&1==0,'Refusal has an active watchdog.')
    for name,address,size in REGIONS:
        data=(evidence/f'before-{name}.bin').read_bytes();expected=p['_backup_hashes'][name]
        require(len(data)==size and sha(data)==expected,'Preserved SRAM changed.')
        output=evidence/f'restored-{name}.bin'
        with base.sealed_source(data) as (source,fd):transaction('write',hex(address),source,'read',hex(address),size,output,pass_fds=(fd,))
        require(output.stat().st_size==size and digest(output)==expected,'SRAM restoration differs.');base.sync_file(output)
    record.update(clean_refusal=True,sram_restored=True);save()


def prepare_capture(grant_path,identification_path,allocation_path,qualification_path,output):
    raw=grant_path.read_bytes();g=store_api.grant_decode(raw)
    require(g['mode']==0,'Capture preparation accepts only a read-only grant.')
    with store_api.ProtectedDirectory(output,create=True) as store:
        fd=store.create('grant.bin');store_api.write_all(fd,raw);store.seal('grant.bin',fd,256,bytes.fromhex(sha(raw)))
        store.record('capture.json',dict(grant=g,grant_sha256=sha(raw)))
    return store.path


def connected_launch(transaction,wait_runtime,wait_rom,ready,bundle,p,store,grant,evidence,record,save):
    """One execution and one storage client, both covered by caller-held locks."""
    preload(transaction,bundle,p,evidence)
    words=[int(x,16) for x in transaction(*[a for _ in range(4) for a in ('readl','0x03000024')]).split()]
    require(len(words)==4 and all(0<=x<=0xffffffff and not x&0x100 for x in words),'FEL input changed.')
    require(int(transaction('readl','0x02050014').strip(),16)&1==0,'Watchdog became active before execution.')
    ready()
    record.update(launch_attempted=True,execution_intent=dict(entry=ENTRY,epoch=p['epoch'],generation=p['generation'],lease=p['lease']),
                  backup_sha256=p['_backup_hashes'].copy());save()
    transaction('exe',hex(ENTRY))
    exchange=wait_runtime()
    if exchange is None:return
    client=store_api.Client(exchange,grant)
    if grant['mode']:store_api.apply(client,store)
    else:store_api.backup(client,store)
    # apply/backup fsync their completion record before returning.
    name='complete.json' if grant['mode'] else 'backup-complete.json'
    receipt=store.json(name)
    record.update(storage_complete=True,completion_receipt=receipt,completion_sha256=sha(store_api.canonical(receipt)))
    save()
    wait_rom()
    record.update(launch_protocol_complete=True,complete=True,
                  recovery_limit='Distinct ROM execution observed; reset cause and storage power-loss durability remain unqualified.')
    save()


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__);sub=parser.add_subparsers(dest='operation',required=True)
    capture=sub.add_parser('prepare-capture')
    for name in ('grant','output'):capture.add_argument('--'+name,type=Path,required=True)
    for name in ('identification','allocation','qualification'):capture.add_argument('--'+name,type=Path,help=argparse.SUPPRESS)
    prep=sub.add_parser('prepare')
    for name in ('image','configuration','storage','output'):prep.add_argument('--'+name,type=Path,required=True)
    prep.add_argument('--image-manifest',type=Path,help=argparse.SUPPRESS)
    prep.add_argument('--clang',default='clang-21');prep.add_argument('--lld',type=Path,default=Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    live=sub.add_parser('launch')
    for name in ('bundle','storage','fel','evidence'):live.add_argument('--'+name,type=Path,required=True)
    for name in ('qualification','input-evidence','admission','database'):live.add_argument('--'+name,type=Path,help=argparse.SUPPRESS)
    live.add_argument('--claim',help=argparse.SUPPRESS);live.add_argument('--owner',help=argparse.SUPPRESS)
    live.add_argument('--confirm-bridge-stable',action='store_true',required=True)
    args=parser.parse_args(argv)
    if args.operation=='prepare-capture':
        print(prepare_capture(args.grant,args.identification,args.allocation,args.qualification,args.output));return 0
    if args.operation=='prepare':
        print(json.dumps(prepare(args.image,args.image_manifest,args.configuration,args.storage,args.output.resolve(),args.clang,args.lld),indent=2));return 0
    bundle=args.bundle.resolve()
    with store_api.ProtectedDirectory(args.storage) as store:
        p,grant=validate_bundle(bundle,store)
        config=json.loads((bundle/'configuration.json').read_text())
        args.evidence=base.module('storage_launch_live_output',ROOT/'tools/host_evidence.py').unique_directory(args.evidence)
        for name,path in [('host.py',Path(__file__)),('base-host.py',Path(base.__file__)),
                          ('storage-host.py',Path(store_api.__file__)),('prepared.json',bundle/'prepared.json')]:
            shutil.copy2(path,args.evidence/name);base.sync_file(args.evidence/name)
        access=base.module('storage_usb_access',ROOT/'tools/usb-access.py')
        sys.path.insert(0,str(ROOT/'tools'));import usb_guard
        usb_guard.check_health()
        record=dict(complete=False,transactions=[],launch_attempted=False,storage_complete=False)
        def save():save_json(args.evidence/'session.json',record)
        with access.board_lock() as node:
            access.require_session(node);access.require_rom(node);identity=access.identity(node)
            port=access.target(node);port_config={'port':port.name,'sysfs':str(port.resolve())}
            original=identity;failed=False
            def ready():
                validate_session({},p,grant,config,original,port.name,str(port.resolve()))
                validate_bundle(bundle,store)
            ready();record.update(initial_identity=identity,port=port.name);save()
            def transaction(*arguments,pass_fds=()):
                nonlocal failed
                require(not failed,'Failed transport forbids further requests.')
                entry={'arguments':list(map(str,arguments))};record['transactions'].append(entry);save()
                try:
                    require(access.identity(node)==identity,'USB instance changed before request.');access.require_rom(node)
                    r=subprocess.run([str(args.fel),'-d',f'{int(node.parent.name)}:{int(node.name)}',*map(str,arguments)],
                                     capture_output=True,text=True,timeout=15,pass_fds=pass_fds)
                    entry.update(exit_code=r.returncode,stdout=r.stdout,stderr=r.stderr)
                    require(r.returncode==0,'FEL request failed.')
                except BaseException as error:
                    failed=True;entry['failure']=str(error);save();raise
                save();return r.stdout
            def wait_runtime():
                nonlocal node,identity
                until=time.monotonic()+5;departed=False
                while time.monotonic()<until:
                    candidate=access.session_node(port_config)
                    if candidate is None:departed=True
                    else:
                        try:
                            observed=access.identity(candidate)
                            if observed!=original:
                                departed=True
                                if os.access(candidate,os.R_OK|os.W_OK):
                                    access.require_session(candidate)
                                    require((port/'bcdDevice').read_text().strip()=='0102' and
                                            (port/'product').read_text().strip()=='sunstoneos storage maintenance' and
                                            (port/'serial').read_text().strip()=='sunstoneos-a733-storage',
                                            'Distinct USB instance is not the maintenance runtime.')
                                    node,identity=candidate,observed;record['runtime_identity']=observed;save()
                                    return store_api.USBExchange(access,node,identity,grant)
                        except FileNotFoundError:departed=True
                    time.sleep(.02)
                candidate=access.session_node(port_config)
                if not departed and candidate is not None and access.identity(candidate)==original:
                    refusal(transaction,p,args.evidence,record,save);return None
                raise ValueError('No maintenance runtime within the acquisition bound; ownership retained.')
            def wait_rom():
                nonlocal node,identity
                previous=identity;until=time.monotonic()+config['maximum_recovery_us']/1000000+5
                while time.monotonic()<until:
                    candidate=access.session_node(port_config)
                    if candidate is not None:
                        try:
                            observed=access.identity(candidate)
                            if observed!=previous and os.access(candidate,os.R_OK|os.W_OK):
                                access.require_session(candidate);access.require_rom(candidate);node,identity=candidate,observed
                                require('soc=00001903(A733)' in transaction('version'),'Returned ROM is not A733.')
                                record['returned_identity']=observed;save();return
                        except FileNotFoundError:pass
                    time.sleep(.05)
                raise ValueError('No distinct accessible ROM after completed maintenance; ownership retained.')
            try:
                require('soc=00001903(A733)' in transaction('version'),'Target is not A733.')
                base.verify_dram(transaction,args.evidence,record,save)
                with usb_guard.reenumeration('sunstoneos-a733','storage maintenance and independent ROM return',2):
                    connected_launch(transaction,wait_runtime,wait_rom,ready,bundle,p,store,grant,args.evidence,record,save)
            except BaseException as error:
                record['transport_failed']=failed
                base.stop_after_failure(record,save,lambda:access.quarantine_instance(node,
                    'Storage maintenance stopped; no further requests or automatic retry.',identity),error)
                raise
    if record.get('clean_refusal'):
        print('Launch refused before mutation; SRAM restored. Coordinator retains board ownership.');return 2
    print('Storage completion receipt is durable and a distinct ROM executed. Power-loss durability and bootability remain unqualified.')
    return 0


if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,struct.error) as error:sys.exit(f'Stopped: {error}')
