#!/usr/bin/env python3
"""Prepare or execute one admitted version-2 device-memory launch.

The watchdog expires after four seconds. No feed, disarm or physical
qualification is provided by this tool. NOLOAD pools are never uploaded.
"""
import argparse
import fcntl
import hashlib
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
import importlib.util
ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location('device_launch_base', ROOT/'tools/check-a733-smp.py')
base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(base)
require, digest, artifact, save_json = base.require, base.digest, base.artifact, base.save_json
module, sync_file = base.module, base.sync_file
launch_once, observe_refusal, stop_after_failure = base.launch_once, base.observe_refusal, base.stop_after_failure
validate_qualification = base.validate_qualification
FEL_HASH, ROM_HASH, CONTRACT = base.FEL_HASH, base.ROM_HASH, base.CONTRACT
REQUEST_MAGIC = 0x41533733444c4332
TARGET = 'sunstone-a733-smp-cached-devices'
RANGES = [[0x41000000,0x42200000],[0x48000000,0x78000000],[0x78000000,0x79000000]]
POOL_SYMBOLS = {'__device_memory_profile':1, '__device_pool_start':0x48000000,
                '__device_pool_end':0x78000000, '__uncached_pool_start':0x78000000,
                '__uncached_pool_end':0x79000000}
CONDITIONS = {'dram-initialized', 'dual-pas-dram', 'dirty-alias-exclusion',
              'exclusive-cpus-memory-dma', 'remote-writers-excluded',
              'source-backed-cpu-dsu-transition', 'physical-counter-continuity'}
PREREQUISITES = {'independent-watchdog-rom-return', 'stable-protected-fel',
                 'device-memory-session-v2', 'device-launch-software-v2'}

def image_parts(path):
    """Parse the small required ELF64 subset without loading executable code."""
    data = path.read_bytes()
    require(len(data) >= 64 and data[:7] == b'\x7fELF\x02\x01\x01', 'Expected little-endian ELF64.')
    header = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
    kind, machine, version, entry, phoff, shoff, _, ehsize, phsize, phnum, shsize, shnum, _ = header
    require((kind, machine, version, entry, ehsize, phsize, shsize) == (2, 183, 1, 0x41000000, 64, 56, 64), 'ELF contract differs.')
    require(0 < phnum < 64 and 0 < shnum < 4096 and phoff + phnum * phsize <= len(data) and shoff + shnum * shsize <= len(data), 'Invalid ELF tables.')
    segments = []
    for i in range(phnum):
        ptype, flags, offset, address, physical, filesz, memsz, alignment = struct.unpack_from('<IIQQQQQQ', data, phoff + i * phsize)
        if ptype != 1: continue
        require(address == physical and address < address + memsz < 2**64 and
                filesz <= memsz and offset + filesz <= len(data) and flags & 3 != 3, 'Invalid load segment.')
        ordinary = 0x41000000 <= address < address + memsz <= 0x42200000
        pool = (address, address+memsz) in {(0x48000000,0x78000000), (0x78000000,0x79000000), (0x48000000,0x79000000)}
        require(ordinary or (pool and filesz == 0 and flags == 6), 'Segment is outside the exact reservation profile.')
        require(not (flags & 1) or address + memsz <= 0x41200000, 'Code exceeds reserved block.')
        require(not filesz or address + filesz <= 0x42000000, 'Image file bytes overlap result or contract.')
        require(all(address + memsz <= other['address'] or other['address'] + other['memory_bytes'] <= address for other in segments), 'ELF load allocations overlap.')
        segments.append({'address': address, 'memory_bytes': memsz, 'data': data[offset:offset + filesz]})
    require(any(s['address'] <= entry < s['address'] + len(s['data']) for s in segments), 'Entry has no file bytes.')
    symbols = {}
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shsize) for i in range(shnum)]
    for section in sections:
        if section[1] != 2: continue
        offset, size, link, entsize = section[4], section[5], section[6], section[9]
        require(link < shnum and entsize == 24 and size % 24 == 0 and offset + size <= len(data), 'Malformed symbol table.')
        strings = sections[link]; require(strings[4] + strings[5] <= len(data), 'Malformed string table.')
        names = data[strings[4]:strings[4] + strings[5]]
        for pos in range(offset, offset + size, 24):
            name, info, other, index, value, length = struct.unpack_from('<IBBHQQ', data, pos)
            require(name < len(names), 'Invalid symbol name.')
            end = names.find(b'\0', name); require(end >= 0, 'Unterminated symbol name.')
            label = names[name:end].decode('ascii', errors='replace')
            if label in {'a733_smp_build_id', '__a733_smp_result', '__a733_smp_contract', '__a733_smp_contract_end'} | set(POOL_SYMBOLS):
                require(label not in symbols, 'Duplicate image contract symbol.'); symbols[label] = (value, length)
    require(symbols.get('__a733_smp_result', (0,))[0] == 0x42000000 and
            symbols.get('__a733_smp_contract', (0,))[0] == CONTRACT and
            symbols.get('__a733_smp_contract_end', (0,))[0] == CONTRACT + 4096, 'Missing linker reservations.')
    require(all(symbols.get(name, (None,))[0] == value for name,value in POOL_SYMBOLS.items()),
            'ELF device layout symbols differ.')
    pool_sections = [(s[3],s[5]) for s in sections if s[2] & 2 and s[3] >= 0x48000000]
    require(sorted(pool_sections) == [(0x48000000,0x30000000),(0x78000000,0x1000000)],
            'ELF must reserve both complete pools as distinct sections.')
    for s in sections:
        if s[2] & 2 and s[3] >= 0x48000000:
            require(s[1] == 8 and s[2] == 3, 'Device pool must be writable non-executable NOBITS.')
    pools = sorted((s['address'],s['address']+s['memory_bytes']) for s in segments if s['address'] >= 0x48000000)
    require(pools in [[(0x48000000,0x79000000)],[(0x48000000,0x78000000),(0x78000000,0x79000000)]],
            'ELF pool reservations are incomplete.')
    address, size = symbols.get('a733_smp_build_id', (0, 0))
    require(size == 32 and 0x41000000 <= address <= 0x411fffe0, 'Invalid image build identity.')
    for segment in segments:
        offset = address - segment['address']
        if 0 <= offset <= len(segment['data']) - 32:
            return segments, address, segment['data'][offset:offset + 32]
    raise ValueError('Build identity is not file-backed.')


def request_bytes(build_address, build_id, epoch, generation, lease, reservation):
    require(len(build_id)==32 and all(type(x) is int and 0 < x < 2**64
            for x in (epoch,generation,lease,reservation)), 'Invalid launch identity.')
    request = struct.pack('<8Q',REQUEST_MAGIC,2,128,epoch,generation,generation^(2**64-1),lease,build_address)
    request += build_id + struct.pack('<4Q',1,reservation,0,0)
    contract = struct.pack('<6Q',0,2,224,epoch,generation,generation^(2**64-1)) + build_id
    contract += struct.pack('<18Q',0,0,0,0,lease,0x41,1,2,0x5aa5a55a,0xfa50392f,0x20,0,
                            1,reservation,0x48000000,0x78000000,0x78000000,0x79000000)
    require(len(request)==128 and len(contract)==224, 'Internal wire size mismatch.')
    return request, contract + bytes(4096-len(contract))


def validate_image_manifest(value, image):
    require(value.get('target')==TARGET and value.get('contract_version')==2 and
            value.get('execution_profile')=='bounded-device-diagnostic-v2' and
            value.get('image_kind')=='runtime' and value.get('final_elf')==digest(image),
            'A matching runtime image manifest is required.')

def prepare(image, manifest_path, output, clang, lld, reservation):
    validate_image_manifest(json.loads(manifest_path.read_text()),image)
    require(type(reservation) is int and 0 < reservation < 2**64, 'A reservation identity is required for binding; preparation does not qualify it.')
    segments, build_address, build_id = image_parts(image)
    require(json.loads(manifest_path.read_text()).get('build_id')==build_id.hex(),
            'Manifest build identity differs from the linked image.')
    output.mkdir(parents=True, exist_ok=False)
    for name in ('launch.S', 'launch.c', 'launch.ld', 'contract.h'):
        shutil.copy2(ROOT / 'platform/a733-device-launch' / name, output / name)
    commands = []
    flags = ['--target=arm-none-eabi', '-march=armv8-a', '-marm', '-mfloat-abi=soft', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-Os', '-g', '-Wall', '-Wextra', '-Werror', '-I' + str(output)]
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
    request, contract = request_bytes(build_address, build_id, epoch, generation, lease, reservation)
    for name, data in [('request.bin', request), ('contract.bin', contract), ('result-zero.bin', bytes(16384)), ('report-zero.bin', bytes(128))]: (output / name).write_bytes(data)
    record = {'schema': 'a733-device-launch-bundle-v2', 'reservation':reservation, 'image_sha256': digest(image), 'launcher_sha256': digest(output / 'launch.bin'), 'build_id': build_id.hex(),
              'epoch': epoch, 'generation': generation, 'lease': lease, 'segments': payloads,
              'reservations': [{k:v for k,v in s.items() if k != 'data'} for s in segments],
              'files': {f.name: digest(f) for f in output.iterdir() if f.is_file()}, 'base_host_sha256':digest(Path(base.__file__)), 'physical_access': False}
    save_json(output / 'prepared.json', record)
    return record


def validate_bundle(bundle):
    prepared = json.loads((bundle / 'prepared.json').read_text())
    require(prepared.get('schema') == 'a733-device-launch-bundle-v2' and prepared.get('physical_access') is False, 'Unsupported prepared bundle.')
    validate_image_manifest(json.loads((bundle/'image-input-manifest.json').read_text()),bundle/'image.elf')
    sizes = {}
    for name, expected in prepared['files'].items():
        require(Path(name).name == name, 'Invalid prepared filename.')
        data = (bundle / name).read_bytes()
        require(hashlib.sha256(data).hexdigest() == expected, 'Prepared input changed.')
        sizes[name] = len(data)
    segments, address, identity = image_parts(bundle / 'image.elf')
    require(json.loads((bundle/'image-input-manifest.json').read_text()).get('build_id')==identity.hex(),
            'Bundle manifest build identity differs from the linked image.')
    expected_segments = []
    for index, segment in enumerate(segments):
        if segment['data']:
            name = f'segment-{index}.bin'
            require((bundle / name).read_bytes() == segment['data'], 'Segment bytes differ from ELF.')
            expected_segments.append({'name': name, 'address': segment['address']})
    require(prepared['segments'] == expected_segments and prepared['image_sha256'] == digest(bundle / 'image.elf') and
            prepared['build_id'] == identity.hex(), 'Prepared ELF metadata differs.')
    require(prepared['reservations']==[{k:v for k,v in s.items() if k!='data'} for s in segments],
            'Prepared memory reservations differ from the ELF.')
    request, contract = request_bytes(address, identity, prepared['epoch'], prepared['generation'], prepared['lease'], prepared['reservation'])
    for name, expected in [('request.bin', request), ('contract.bin', contract), ('result-zero.bin', bytes(16384)), ('report-zero.bin', bytes(128))]:
        require((bundle / name).read_bytes() == expected, 'Prepared launch input differs: ' + name)
    require(0 < (bundle / 'launch.bin').stat().st_size <= 0xc00 and prepared['launcher_sha256'] == digest(bundle / 'launch.bin'), 'Invalid launcher identity or size.')
    require(prepared.get('base_host_sha256')==digest(Path(base.__file__)), 'Shared transfer helper changed.')
    prepared['_sizes'] = sizes
    return prepared


def session_binding(prepared, identity, port, sysfs):
    return {'target':{'soc':'00001903','port':port,'sysfs':sysfs,'instance':identity},
            'image_sha256':prepared['image_sha256'],'build_id':prepared['build_id'],
            'epoch':prepared['epoch'],'generation':prepared['generation'],'boot_lease':prepared['lease'],
            'layout':1,'reservation':prepared['reservation'],'ranges':RANGES}


def read_record(reference):
    # Hash the same bytes that are decoded; a changed path cannot supply another
    # record between a separate hash read and JSON read.
    path = artifact(reference)
    data = path.read_bytes()
    require(hashlib.sha256(data).hexdigest()==reference['sha256'], 'Evidence changed while reading.')
    return json.loads(data)


def validate_memory(value, prepared, identity, port, sysfs):
    binding = session_binding(prepared,identity,port,sysfs)
    require(value.get('schema')=='a733-device-memory-session-v2' and value.get('binding')==binding and
            value.get('physical_qualification') is True and value.get('ownership_retained') is True,
            'Current physical memory reservation does not match this launch.')
    require(set(value.get('conditions',{}))==CONDITIONS, 'Memory/CPU admission evidence is incomplete.')
    for name, reference in value['conditions'].items():
        condition=read_record(reference)
        require(condition.get('schema')=='a733-device-memory-condition-v2' and
                condition.get('condition')==name and condition.get('binding')==binding and
                condition.get('physical_access') is True and condition.get('established') is True,
                'Physical condition is stale, unqualified or covers another reservation: '+name)
        observations=condition.get('observations')
        require(isinstance(observations,list) and observations, 'Physical observations are missing: '+name)
        for item in observations: artifact(item)
    return binding


def validate_software(value,bundle):
    require(value.get('schema')=='a733-device-launch-software-v2' and value.get('host_protocol') is True and
            value.get('instruction_model') is True and value.get('physical_access') is False,
            'New launcher software evidence is missing.')
    for key,name in [('prepared','prepared.json'),('image','image.elf'),('launcher','launch.bin')]:
        require(artifact(value[key]).resolve()==(bundle/name).resolve(), 'Software evidence differs from bundle.')
    for key,path in [('host_tool',Path(__file__)),('base_host_tool',Path(base.__file__))]:
        require(digest(artifact(value[key]))==digest(path), 'Software evidence covers another host implementation.')
    require(value.get('evidence'), 'Focused launcher evidence is missing.')
    for item in value['evidence']: artifact(item)


def claimed(database,test_id,owner,bundle,qualification,input_evidence,memory):
    db=sqlite3.connect(database.resolve().as_uri()+'?mode=ro',uri=True);db.row_factory=sqlite3.Row
    try:
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0], 'Board queue is halted.')
        row=db.execute('SELECT * FROM tests WHERE id=?',(test_id,)).fetchone()
        require(row is not None and row['state']=='claimed' and row['owner']==owner, 'Matching coordinator claim required.')
        submission,review=json.loads(row['submission']),json.loads(row['review'])
        require(set(submission['prerequisites'])==PREREQUISITES and set(review['prerequisites'])==PREREQUISITES,
                'Reviewed prerequisite set differs.')
        refs={name:artifact(item).resolve() for name,item in review['prerequisites'].items()}
        for name,path in [('independent-watchdog-rom-return',qualification),('stable-protected-fel',input_evidence),
                          ('device-memory-session-v2',memory)]:
            require(refs[name]==path.resolve(), 'Qualification is not the reviewed record.')
        validate_software(read_record(review['prerequisites']['device-launch-software-v2']),bundle)
        require({(bundle/n).resolve() for n in ('prepared.json','image.elf','launch.bin')} <=
                {artifact(item).resolve() for item in submission['artifacts']}, 'Queue lacks exact bundle artifacts.')
        return {name:read_record(reference) for name,reference in review['prerequisites'].items()}
    finally: db.close()


def archive_conditions(value,evidence):
    # Preserve exactly the reviewed physical evidence, without producing a
    # qualifying statement or converting modeled observations into one.
    folder=evidence/'physical-inputs';folder.mkdir(exist_ok=True)
    for reference in value['conditions'].values():
        condition=read_record(reference)
        for item in [reference,*condition['observations']]:
            path=artifact(item); data=path.read_bytes()
            require(hashlib.sha256(data).hexdigest()==item['sha256'], 'Evidence changed during archival.')
            output=folder/item['sha256']
            if not output.exists(): output.write_bytes(data);sync_file(output)
    fd=os.open(folder,os.O_RDONLY|os.O_DIRECTORY)
    try:os.fsync(fd)
    finally:os.close(fd)

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__); sub = parser.add_subparsers(dest='operation',required=True)
    prep = sub.add_parser('prepare'); prep.add_argument('--image',type=Path,required=True); prep.add_argument('--image-manifest',type=Path,required=True); prep.add_argument('--output',type=Path,required=True)
    prep.add_argument('--reservation',type=lambda s:int(s,0),required=True)
    prep.add_argument('--clang',default='clang-21'); prep.add_argument('--lld',type=Path,default=Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    live = sub.add_parser('launch')
    for name in ('bundle','fel','qualification','input-evidence','memory-admission','evidence','database'): live.add_argument('--'+name,type=Path,required=True)
    live.add_argument('--claim',required=True); live.add_argument('--owner',required=True); live.add_argument('--confirm-bridge-stable',action='store_true',required=True)
    args = parser.parse_args(argv)
    if args.operation == 'prepare':
        result=prepare(args.image,args.image_manifest,args.output.resolve(),args.clang,args.lld,args.reservation); print(json.dumps(result,indent=2)); return 0
    bundle=args.bundle.resolve(); prepared=validate_bundle(bundle)
    require(digest(args.fel)==FEL_HASH, 'FEL executable differs from reviewed bytes.')
    claimed(args.database,args.claim,args.owner,bundle,args.qualification,args.input_evidence,args.memory_admission)
    qualification=json.loads(args.qualification.read_text()); input_record=json.loads(args.input_evidence.read_text()); memory=json.loads(args.memory_admission.read_text())
    args.evidence.mkdir(parents=True,exist_ok=False)
    for label, path in [('host-tool.py', Path(__file__)), ('qualification.json', args.qualification), ('input-evidence.json', args.input_evidence), ('memory-admission.json', args.memory_admission), ('base-host-tool.py',Path(base.__file__)), ('prepared.json', bundle/'prepared.json')]:
        shutil.copy2(path,args.evidence/label)
    access=module('smp_usb_access',ROOT/'tools/usb-access.py')
    sys.path.insert(0,str(Path.home()/'git/mimus/tools')); import usb_guard
    usb_guard.check_health()
    record={'complete':False,'transactions':[],'claim':args.claim,'owner':args.owner,'bundle_sha256':digest(bundle/'prepared.json'),'launch_attempted':False}
    def save(): save_json(args.evidence/'session.json',record)
    with open('/tmp/mimus-board.lock','a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        node=access.discover(); access.require_session(node); access.require_rom(node); identity=access.identity(node)
        port=access.target(node); port_config={'port':port.name,'sysfs':str(port.resolve())}; failed=False
        validate_qualification(qualification,port.name,str(port.resolve()))
        require(input_record.get('complete') is True and input_record.get('asserted') is True and input_record.get('workspace_restored') is True and input_record.get('identity')==identity and input_record.get('port')==port.name and input_record.get('rom_sha256')==ROM_HASH, 'Stable low-input evidence does not cover this live instance.')
        def ready():
            reviewed=claimed(args.database,args.claim,args.owner,bundle,args.qualification,args.input_evidence,args.memory_admission)
            validate_qualification(reviewed['independent-watchdog-rom-return'],port.name,str(port.resolve()))
            protection=reviewed['stable-protected-fel']
            require(protection==input_record, 'Protected FEL record changed.')
            current=reviewed['device-memory-session-v2']
            require(current==memory, 'Memory admission changed.')
            validate_memory(current,prepared,identity,port.name,str(port.resolve()))
        ready()
        archive_conditions(memory,args.evidence)
        record.update(initial_node=str(node),initial_identity=identity,port=port.name, host_tool_sha256=digest(Path(__file__)), usb_access_sha256=digest(ROOT/'tools/usb-access.py'), usb_guard_sha256=digest(Path(usb_guard.__file__))); save()
        def transaction(*arguments, pass_fds=()):
            nonlocal failed
            require(not failed,'Transport failure forbids all further requests.')
            entry={'arguments':[str(a) for a in arguments]}
            if pass_fds:
                entry['sealed_sources'] = [{'fd':fd,'bytes':os.fstat(fd).st_size,'sha256':hashlib.sha256(os.pread(fd,os.fstat(fd).st_size,0)).hexdigest(),'seals':fcntl.fcntl(fd,fcntl.F_GET_SEALS)} for fd in pass_fds]
            record['transactions'].append(entry); save()
            try:
                require(access.identity(node)==identity,'USB identity changed before a request.')
                access.require_rom(node)
                result=subprocess.run([str(args.fel),'-d',f'{int(node.parent.name)}:{int(node.name)}',*map(str,arguments)],capture_output=True,text=True,timeout=15,pass_fds=pass_fds)
                entry.update(exit_code=result.returncode,stdout=result.stdout,stderr=result.stderr)
                require(result.returncode==0,'FEL request failed.')
            except (OSError,ValueError,subprocess.TimeoutExpired) as error:
                failed=True; entry['failure']=str(error); save(); raise
            save(); return result.stdout
        def wait_return():
            nonlocal node,identity
            old=identity; deadline=time.monotonic()+20; departure=False
            while time.monotonic()<deadline:
                candidate=access.session_node(port_config)
                if candidate is None:
                    departure=True
                else:
                    try:
                        new=access.identity(candidate)
                        if new!=old: departure=True
                        if new!=old and os.access(candidate,os.R_OK|os.W_OK):
                            access.require_session(candidate); access.require_rom(candidate); node,identity=candidate,new
                            record.update(returned_node=str(node),returned_identity=identity); save()
                            require('soc=00001903(A733)' in transaction('version'),'Returned ROM is not A733.'); return
                    except FileNotFoundError: departure=True
                time.sleep(.05)
            candidate=access.session_node(port_config)
            if not departure and candidate is not None and access.identity(candidate)==old and os.access(candidate,os.R_OK|os.W_OK):
                access.require_session(candidate); access.require_rom(candidate)
                return observe_refusal(transaction,args.evidence,prepared,record,save)
            raise ValueError('No distinct accessible ROM instance in 20 seconds; ownership remains held.')
        try:
            require('soc=00001903(A733)' in transaction('version'),'Target is not A733.')
            with usb_guard.reenumeration('sunstoneos-a733','version-2 device-memory launch and watchdog ROM return',2):
                launch_once(transaction,wait_return,bundle,prepared,args.evidence,record,save,
                    ready)
        except BaseException as error:
            record['transport_failed'] = failed
            stop_after_failure(record,save,
                lambda: access.quarantine_instance(node,'Device-memory launch stopped; no further device communication or automatic retry.',identity),error)
            raise
    if record.get('clean_refusal'):
        print('Original ROM reported a clean launch refusal; SRAM was restored. Scheduler result remains unobserved. Queue claim remains with the coordinator.')
    else:
        print('A new accessible ROM instance was observed. Scheduler result remains unobserved; DDR must be reinitialized. Queue claim remains with the coordinator.')
    return 2


if __name__=='__main__':
    try: sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error,struct.error) as error: sys.exit(f'Stopped: {error}')
