#!/usr/bin/env python3
"""Prepare or launch one protected cached SMP observer and durably capture its result."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import secrets
import shutil
import sqlite3
import struct
import subprocess
import sys
import time
import fcntl
ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location('smp_observe_base', ROOT / 'tools/check-a733-smp.py')
base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(base)  # Library import; the old tool's main guard stays inactive.
require, digest, artifact, save_json = base.require, base.digest, base.artifact, base.save_json
image_parts, module, sync_file = base.image_parts, base.module, base.sync_file
CONTRACT = base.CONTRACT
REQUEST_MAGIC = 0x415337334f4c4331
MESSAGE_MAGIC, FRAME_MAGIC = 0x53534f53, 0x415337334f425331
PRODUCT, SERIAL = 'sunstoneos cached SMP observer', 'sunstoneos-a733-smp-observe'
PREREQUISITES = {'independent-watchdog-rom-return', 'stable-protected-fel',
                 'ddr-reservation', 'smp-observer-platform', 'smp-observer-software'}

def request_bytes(build_address, build_id, epoch, generation, lease):
    require(len(build_id) == 32 and all(type(x) is int and 0 < x < 2**64 for x in (epoch, generation, lease)), 'Invalid launch identity.')
    request = struct.pack('<8Q', REQUEST_MAGIC, 2, 128, epoch, generation, generation ^ (2**64-1), lease, build_address) + build_id + bytes(32)
    contract = struct.pack('<6Q', 0, 1, 224, epoch, generation, generation ^ (2**64-1)) + build_id
    contract += struct.pack('<18Q', 0, 0, 0, 0, lease, 0x41, 1, 2, 0x5aa5a55a, 0xfa50392f, 0x20, 0, *([0]*6))
    require(len(request) == 128 and len(contract) == 224, 'Internal ABI mismatch.')
    return request, contract + bytes(4096 - len(contract))


def prepare(image, manifest_path, output, clang, lld):
    manifest=json.loads(manifest_path.read_text())
    require(manifest.get('target')=='a733-smp-observe' and manifest.get('protocol_version')==1 and
            manifest.get('final_elf')==digest(image), 'Image differs from reviewed observer manifest.')
    require('#ifndef A733_SMP_CONTRACT_MAGIC' in (ROOT/'platform/a733-smp-cached/boot.h').read_text(), 'Guarded observer contract header missing.')
    segments, build_address, build_id = image_parts(image)
    output.mkdir(parents=True, exist_ok=False)
    for name in ('launch.S', 'launch.c', 'launch.ld'):
        shutil.copy2(ROOT / 'platform/a733-smp-observe' / name, output / name)
    shutil.copy2(ROOT/'platform/a733-smp-cached/boot.h',output/'boot.h')
    commands = []
    flags = ['-DA733_SMP_CONTRACT_MAGIC=0x415337334f424331ULL', '--target=arm-none-eabi', '-march=armv8-a', '-marm', '-mfloat-abi=soft', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-Os', '-g', '-Wall', '-Wextra', '-Werror', '-I' + str(output)]
    def run(command):
        commands.append([str(x) for x in command]); result = subprocess.run(command, capture_output=True, text=True)
        with (output / 'build.log').open('a') as stream: stream.write(result.stdout + result.stderr)
        save_json(output / 'commands.json', commands); require(result.returncode == 0, 'Launcher build failed; inspect build.log.')
    for source, target in [('launch.S', 'entry.o'), ('launch.c', 'launch.o')]: run([clang, *flags, '-c', output / source, '-o', output / target])
    run([lld, '-T', output / 'launch.ld', output / 'entry.o', output / 'launch.o', '-o', output / 'launch.elf'])
    run(['/usr/lib/llvm-21/bin/llvm-objcopy', '-O', 'binary', output / 'launch.elf', output / 'launch.bin'])
    require(0 < (output / 'launch.bin').stat().st_size <= 0xc00, 'Launcher exceeds SRAM reservation.')
    shutil.copy2(image, output / 'image.elf'); shutil.copy2(manifest_path, output / 'image-input-manifest.json')
    payloads = []
    for index, segment in enumerate(segments):
        if segment['data']:
            name = f'segment-{index}.bin'; (output / name).write_bytes(segment['data'])
            payloads.append({'name': name, 'address': segment['address']})
    epoch, generation, lease = (secrets.randbits(64) or 1 for _ in range(3))
    request, contract = request_bytes(build_address, build_id, epoch, generation, lease)
    for name, data in [('request.bin', request), ('contract.bin', contract), ('result-zero.bin', bytes(16384)), ('report-zero.bin', bytes(128))]: (output / name).write_bytes(data)
    record = {'schema': 'a733-smp-observe-bundle-v1', 'image_sha256': digest(image), 'launcher_sha256': digest(output / 'launch.bin'), 'build_id': build_id.hex(),
              'epoch': epoch, 'generation': generation, 'lease': lease, 'segments': payloads,
              'reservations': [{k:v for k,v in s.items() if k != 'data'} for s in segments],
              'files': {f.name: digest(f) for f in output.iterdir() if f.is_file()}, 'physical_access': False}
    save_json(output / 'prepared.json', record)
    return record


def validate_bundle(bundle):
    prepared = json.loads((bundle / 'prepared.json').read_text())
    require(prepared.get('schema') == 'a733-smp-observe-bundle-v1' and prepared.get('physical_access') is False, 'Unsupported prepared bundle.')
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
    prepared['_sizes'] = sizes
    return prepared


def request(sequence, prepared, challenge):
    require(type(sequence) is int and 0 < sequence < 2**32 and len(challenge) == 32 and any(challenge), 'Invalid observer request.')
    return struct.pack('<4I2Q', MESSAGE_MAGIC, sequence, 0x31, 48, prepared['epoch'], prepared['generation']) + challenge


def response(frame, sequence, prepared, challenge):
    """Validate the entire fixed frame before interpreting any reported success."""
    require(len(frame) == 528, 'Incomplete or oversized observer response.')
    require(struct.unpack_from('<4I', frame) == (MESSAGE_MAGIC, sequence, 0x80000031, 512), 'Response header/session sequence differs.')
    p = frame[16:]
    require(hashlib.sha256(p[:480]).digest() == p[480:], 'Observer frame digest differs.')
    magic, version, size, status, epoch, generation, lease, start, end, deadline, mask = struct.unpack_from('<QII8Q', p)
    require((magic, version, size) == (FRAME_MAGIC, 1, 512) and status in (0, 1), 'Unsupported observer frame.')
    require((epoch, generation, lease) == (prepared['epoch'], prepared['generation'], prepared['lease']) and
            p[80:112] == challenge and p[112:144] == bytes.fromhex(prepared['build_id']), 'Stale or mismatched observer identity.')
    require(p[144:160] == bytes(16) and deadline >= 48000000, 'Invalid observer reserved fields/deadline.')
    rows = [list(struct.unpack_from('<5Q', p, 160 + 40 * c)) for c in range(8)]
    if status:
        require(start == end == mask == 0 and not any(p[160:480]), 'Pending frame claims partial completion.')
    else:
        require(deadline - 48000000 <= start <= end < deadline and mask == 255, 'Observer completion/time mask differs.')
        require(all(r[0] == 32 and r[1] > 0 and r[2] > 0 and r[3] == 0 for r in rows) and rows[0][4] >= 32,
                'Eight-core diagnostic criteria are not satisfied.')
    return {'status':'pending' if status else 'success', 'epoch':epoch, 'generation':generation,
            'lease':lease, 'build_id':prepared['build_id'], 'start_counter':start, 'end_counter':end,
            'diagnostic_deadline':deadline, 'completed_mask':mask, 'cores':rows,
            'frame_sha256':hashlib.sha256(frame).hexdigest()}


def durable_frame(path, frame):
    """Commit exact received bytes before any success/recovery classification."""
    with path.open('xb') as stream:
        stream.write(frame); stream.flush(); os.fsync(stream.fileno())
    fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try: os.fsync(fd)
    finally: os.close(fd)


class CaptureWindowExpired(RuntimeError):
    """Local budget exhausted after no transfer or a complete valid reply."""


def capture(exchange, prepared, challenge, evidence, record, save, deadline,
            now=time.monotonic, pause=time.sleep):
    """Only successful pending replies permit another bounded query; no retries."""
    observed_deadline=None
    for sequence in range(1, 129):
        if now() >= deadline: raise CaptureWindowExpired('Observer capture deadline expired.')
        packet = request(sequence, prepared, challenge)
        frame = exchange(packet, min(500, max(1, int((deadline - now()) * 1000))))
        result = response(frame, sequence, prepared, challenge)
        require(observed_deadline is None or result['diagnostic_deadline']==observed_deadline, 'Observer deadline changed within a session.')
        observed_deadline=result['diagnostic_deadline']
        if now() >= deadline: raise CaptureWindowExpired('Valid observer response arrived after capture deadline.')
        path = evidence / f'observer-{sequence:02}.bin'
        durable_frame(path, frame)
        record.setdefault('observer_frames', []).append({'path':path.name, **result})
        if result['status'] == 'success':
            record['scheduler_result'] = 'observed-success'
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
        record['scheduler_result']='unobserved'
        save()
        return False
    return True


def validate_software(value, bundle):
    require(value.get('schema') == 'a733-smp-observer-software-v1' and value.get('physical_access') is False and
            value.get('composed_result_session') is True and value.get('launcher_instruction_equivalence') is True,
            'Observer software evidence is incomplete.')
    for key, name in [('prepared','prepared.json'), ('image','image.elf'), ('launcher','launch.bin')]:
        require(artifact(value[key]) == (bundle/name).resolve(), 'Software evidence covers different artifacts.')
    require(artifact(value['host_tool']) == Path(__file__).resolve() and
            artifact(value['base_host_tool']) == Path(base.__file__).resolve(), 'Software evidence covers different host tools.')
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
                         ('ddr-reservation',ddr),('smp-observer-platform',platform)]:
            require(refs[key]==path.resolve(), 'Queue prerequisite identity differs: '+key)
        validate_software(json.loads(refs['smp-observer-software'].read_text()),bundle)
        require({(bundle/n).resolve() for n in ('prepared.json','image.elf','launch.bin')} <=
                {artifact(a) for a in submission['artifacts']}, 'Queue artifacts do not cover prepared image.')
    finally: db.close()


def validate_platform(value, prepared, identity, port):
    require(value.get('schema')=='a733-smp-observer-platform-v1' and value.get('identity')==identity and
            value.get('port')==port and value.get('image_sha256')==prepared['image_sha256'], 'Observer platform admission differs.')
    required={'source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram',
              'exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership',
              'usb-device-mapping','physical-counter-continuity'}
    require(set(value.get('conditions',{}))==required, 'Observer entry conditions are incomplete.')
    for item in value['conditions'].values(): artifact(item)
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
    record['complete']=record.get('scheduler_result')=='observed-success' and record.get('fresh_rom_response') is True
    save()

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__); sub=parser.add_subparsers(dest='operation',required=True)
    prep=sub.add_parser('prepare')
    for name in ('image','image-manifest','output'): prep.add_argument('--'+name,type=Path,required=True)
    prep.add_argument('--clang',default='clang-21')
    prep.add_argument('--lld',type=Path,default=Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    live=sub.add_parser('launch')
    for name in ('bundle','fel','qualification','input-evidence','ddr-contract','platform-contract','evidence','database'):
        live.add_argument('--'+name,type=Path,required=True)
    live.add_argument('--claim',required=True); live.add_argument('--owner',required=True)
    live.add_argument('--confirm-bridge-stable',action='store_true',required=True)
    args=parser.parse_args(argv)
    if args.operation=='prepare':
        manifest=json.loads(args.image_manifest.read_text())
        require(manifest.get('target')=='a733-smp-observe' and manifest.get('protocol_version')==1, 'Reviewed observer image manifest required.')
        result=prepare(args.image,args.image_manifest,args.output.resolve(),args.clang,args.lld)
        print(json.dumps(result,indent=2)); return 0
    bundle=args.bundle.resolve(); prepared=validate_bundle(bundle)
    manifest=json.loads((bundle/'image-input-manifest.json').read_text())
    require(manifest.get('target')=='a733-smp-observe' and manifest.get('protocol_version')==1 and
            manifest.get('final_elf')==prepared['image_sha256'], 'Bundle is not the reviewed observer image.')
    require(digest(args.fel)==base.FEL_HASH, 'FEL executable differs from reviewed bytes.')
    def ready(): claimed(args.database,args.claim,args.owner,bundle,args.qualification,args.input_evidence,args.ddr_contract,args.platform_contract)
    ready()
    qualification=json.loads(args.qualification.read_text()); protection=json.loads(args.input_evidence.read_text())
    ddr=json.loads(args.ddr_contract.read_text()); platform=json.loads(args.platform_contract.read_text())
    args.evidence.mkdir(parents=True,exist_ok=False)
    for label,path in [('host-tool.py',Path(__file__)),('base-host-tool.py',Path(base.__file__)),
                       ('qualification.json',args.qualification),('input-evidence.json',args.input_evidence),
                       ('ddr-contract.json',args.ddr_contract),('platform-contract.json',args.platform_contract),
                       ('prepared.json',bundle/'prepared.json')]: shutil.copy2(path,args.evidence/label)
    access=module('observe_usb_access',ROOT/'tools/usb-access.py')
    sys.path.insert(0,str(Path.home()/'git/mimus/tools')); import usb_guard
    usb_guard.check_health()
    record={'complete':False,'transactions':[],'claim':args.claim,'owner':args.owner,
            'bundle_sha256':digest(bundle/'prepared.json'),'launch_attempted':False,'scheduler_result':'unobserved'}
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
                            if revision=='0101':
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
                                require(device is not None and device.bcdDevice==0x0101, 'Observer handle differs.')
                                def same():
                                    require(not failed and access.identity(node)==identity,'Observer USB instance changed; no more requests.')
                                    require((port/'bcdDevice').read_text().strip()=='0101','Observer revision changed.')
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
                                        return bytes(device.read(0x81,528,timeout=min(timeout,remaining)))
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
            with usb_guard.reenumeration('sunstoneos-a733','cached SMP observer and independent watchdog ROM return',2):
                launch_once(transaction,observe,bundle,prepared,args.evidence,record,save,ready)
        except BaseException as error:
            record['transport_failed']=failed
            base.stop_after_failure(record,save,
                lambda:access.quarantine_instance(node,'Observer attempt stopped; no requests or automatic retry.',identity),error)
            raise
    print('Diagnostic: '+record['scheduler_result']+'. Fresh ROM response: '+str(record.get('fresh_rom_response',False))+'. Queue claim remains with the coordinator.')
    return 0 if record['complete'] else 2


if __name__=='__main__':
    try: sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error,struct.error) as error: sys.exit(f'Stopped: {error}')
