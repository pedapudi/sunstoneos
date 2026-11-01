#!/usr/bin/env python3
"""Prepare or launch one supervised RAM-only all-core observer; recovery is manual."""
import argparse
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
import fcntl
ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location('smp_observe_base', ROOT / 'tools/check-a733-smp.py')
base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(base)  # Library import; the old tool's main guard stays inactive.
require, digest, artifact, save_json = base.require, base.digest, base.artifact, base.save_json
image_parts, module, sync_file = base.image_parts, base.module, base.sync_file
observer=base.module('supervised_observer',ROOT/'tools/check-a733-smp-observe.py')
CONTRACT = base.CONTRACT
REQUEST_MAGIC = 0x4153373353554c33
MESSAGE_MAGIC, FRAME_MAGIC = 0x53534f53, 0x415337334f425331
PRODUCT, SERIAL = 'sunstoneos cached SMP observer', 'sunstoneos-a733-smp-observe'
WINDOW_TICKS = 20 * 24000000
CONTRACT_MAGIC = 0x4153373353554333

def request_bytes(build_address, build_id, epoch, generation, lease, reservation, admission_digest):
    require(len(build_id) == 32 and all(type(x) is int and 0 < x < 2**64 for x in (epoch, generation, lease)), 'Invalid launch identity.')
    require(type(reservation) is int and 0 < reservation < 2**64 and len(admission_digest)==32 and any(admission_digest), 'Native reservation and session digest required.')
    request = struct.pack('<8Q', REQUEST_MAGIC, 3, 128, epoch, generation, generation ^ (2**64-1), lease, build_address) + build_id + admission_digest
    contract = struct.pack('<6Q', 0, 3, 224, epoch, generation, generation ^ (2**64-1)) + build_id
    contract += struct.pack('<14Q', 0, 0, 0, 0, lease, 0, 0, 0, 0, 0, 0, 0, reservation, 0) + admission_digest
    require(len(request) == 128 and len(contract) == 224, 'Internal ABI mismatch.')
    return request, contract + bytes(4096 - len(contract))


def prepare(image, manifest_path, admission_path, output, clang, lld):
    require('#ifndef A733_SMP_CONTRACT_MAGIC' in (ROOT/'platform/a733-smp-cached/boot.h').read_text(), 'Guarded observer contract header missing.')
    segments, build_address, build_id = image_parts(image)
    admission = {'reservation': secrets.randbits(64) or 1}
    admission_digest = secrets.token_bytes(32)
    output = base.artifact_directory(output)
    for name in ('launch.S', 'launch.c', 'launch.ld'):
        source=ROOT/('platform/a733-smp-supervised' if name=='launch.c' else 'platform/a733-smp-observe')/name
        shutil.copy2(source, output / name)
    shutil.copy2(ROOT/'platform/a733-smp-cached/boot.h',output/'boot.h')
    commands = []
    flags = ['-DA733_SMP_CONTRACT_MAGIC=0x4153373353554333ULL', '--target=arm-none-eabi', '-march=armv8-a', '-marm', '-mfloat-abi=soft', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-Os', '-g', '-Wall', '-Wextra', '-Werror', '-I' + str(output)]
    def run(command):
        commands.append([str(x) for x in command]); result = subprocess.run(command, capture_output=True, text=True)
        with (output / 'build.log').open('a') as stream: stream.write(result.stdout + result.stderr)
        save_json(output / 'commands.json', commands); require(result.returncode == 0, 'Launcher build failed; inspect build.log.')
    for source, target in [('launch.S', 'entry.o'), ('launch.c', 'launch.o')]: run([clang, *flags, '-c', output / source, '-o', output / target])
    run([lld, '-T', output / 'launch.ld', output / 'entry.o', output / 'launch.o', '-o', output / 'launch.elf'])
    run(['/usr/lib/llvm-21/bin/llvm-objcopy', '-O', 'binary', output / 'launch.elf', output / 'launch.bin'])
    require(0 < (output / 'launch.bin').stat().st_size <= 0xc00, 'Launcher exceeds SRAM reservation.')
    shutil.copy2(image, output / 'image.elf')
    payloads = []
    for index, segment in enumerate(segments):
        if segment['data']:
            name = f'segment-{index}.bin'; (output / name).write_bytes(segment['data'])
            payloads.append({'name': name, 'address': segment['address']})
    epoch, generation, lease = (secrets.randbits(64) or 1 for _ in range(3))
    request, contract = request_bytes(build_address, build_id, epoch, generation, lease, admission['reservation'], admission_digest)
    for name, data in [('request.bin', request), ('contract.bin', contract), ('result-zero.bin', bytes(16384)), ('report-zero.bin', bytes(128))]: (output / name).write_bytes(data)
    record = {'reservation':admission['reservation'], 'admission_sha256':admission_digest.hex(), 'schema': 'a733-smp-supervised-bundle-v1', 'image_sha256': digest(image), 'launcher_sha256': digest(output / 'launch.bin'), 'build_id': build_id.hex(),
              'epoch': epoch, 'generation': generation, 'lease': lease, 'segments': payloads,
              'reservations': [{k:v for k,v in s.items() if k != 'data'} for s in segments],
              'files': {f.name: digest(f) for f in output.iterdir() if f.is_file()}, 'physical_access': False, 'output': str(output)}
    save_json(output / 'prepared.json', record)
    return record


def validate_bundle(bundle):
    metadata = (bundle / 'prepared.json').read_bytes()
    prepared = json.loads(metadata)
    require(prepared.get('schema') == 'a733-smp-supervised-bundle-v1' and prepared.get('physical_access') is False, 'Unsupported prepared bundle.')
    sizes = {}; frozen = {}
    required_files = {'image.elf', 'launch.bin', 'request.bin', 'contract.bin', 'result-zero.bin', 'report-zero.bin'} | {item['name'] for item in prepared['segments']}
    require(required_files <= prepared['files'].keys(), 'Native transfer inputs are incomplete.')
    for name in required_files:
        expected = prepared['files'][name]
        require(Path(name).name == name, 'Invalid prepared filename.')
        data = (bundle / name).read_bytes()
        require(hashlib.sha256(data).hexdigest() == expected, 'Prepared input changed.')
        sizes[name] = len(data); frozen[name] = data
    segments, address, identity = image_parts(bundle / 'image.elf')
    expected_segments = []
    for index, segment in enumerate(segments):
        if segment['data']:
            name = f'segment-{index}.bin'
            require((bundle / name).read_bytes() == segment['data'], 'Segment bytes differ from ELF.')
            expected_segments.append({'name': name, 'address': segment['address']})
    require(prepared['segments'] == expected_segments and prepared['image_sha256'] == digest(bundle / 'image.elf') and
            prepared['build_id'] == identity.hex(), 'Prepared ELF metadata differs.')
    request, contract = request_bytes(address, identity, prepared['epoch'], prepared['generation'], prepared['lease'], prepared['reservation'], bytes.fromhex(prepared['admission_sha256']))
    for name, expected in [('request.bin', request), ('contract.bin', contract), ('result-zero.bin', bytes(16384)), ('report-zero.bin', bytes(128))]:
        require((bundle / name).read_bytes() == expected, 'Prepared launch input differs: ' + name)
    require(0 < (bundle / 'launch.bin').stat().st_size <= 0xc00 and prepared['launcher_sha256'] == digest(bundle / 'launch.bin'), 'Invalid launcher identity or size.')
    prepared['_sizes'] = sizes
    base.require_upload_bounds(prepared)
    prepared['_frozen'] = frozen
    prepared['_prepared_sha256'] = hashlib.sha256(metadata).hexdigest()
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
    require(p[144:160] == bytes(16) and deadline >= WINDOW_TICKS, 'Invalid observer reserved fields/deadline.')
    rows = [list(struct.unpack_from('<5Q', p, 160 + 40 * c)) for c in range(8)]
    if status:
        require(start == end == mask == 0 and not any(p[160:480]), 'Pending frame claims partial completion.')
    else:
        require(deadline - WINDOW_TICKS <= start <= end < deadline and mask == 255, 'Observer completion/time mask differs.')
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
    for sequence in range(1, 769):
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










def validate_admission(value, image_hash, identity=None, port=None):
    """Validate native session fields, accepting optional recorded target fields.

    The reservation is sent to native code. Historical DDR, survey, recovery and
    records are optional metadata. The live session checks its target
    and reserved result memory before upload.
    """
    require(type(value.get('reservation')) is int and 0 < value['reservation'] < 2**64,
            'Invalid native reservation identity.')
    if 'image_sha256' in value:
        require(value['image_sha256'] == image_hash, 'Native session image differs.')
    if identity is not None and 'identity' in value:
        require(value['identity'] == identity, 'Native session covers another USB instance.')
    if port is not None and 'port' in value:
        require(value['port'] == port, 'Native session covers another port.')




def device_software_sources(sources):
    """A device tool's own source files plus this tool, the base tool and the observer tool."""
    paths=dict(sources)
    paths.update({'supervised-host-tool.py':Path(__file__),
                  'base-host-tool.py':Path(base.__file__),
                  'observer-host-tool.py':Path(observer.__file__)})
    return paths






def launch_once(transaction,observe,bundle,prepared,evidence,record,save,ready,*,result_key="scheduler_result"):
    require(not record.get('launch_attempted'), 'One supervised attempt only.')
    base.preload(transaction,bundle,prepared,evidence)
    require(int(transaction('readl','0x02050014').strip(),16)&1==0, 'Watchdog enabled before launch.')
    ready()
    record.update(execution_intent={'entry':0x52000,'epoch':prepared['epoch'],'generation':prepared['generation'],
                  'native_elapsed_limit_ticks':24000,'recovery':'manual-usb-reconnect'},
                  backup_sha256=prepared['_backup_hashes'].copy(),launch_attempted=True)
    save();started=time.monotonic();transaction('exe','0x52000');observe(started)
    record['launch_protocol_complete']=True
    record['complete']=record.get(result_key)=='observed-success'
    record['manual_reconnect_required']=not record.get('clean_refusal',False)
    save()


def probe_clean_refusal(transaction,evidence,prepared,record,save,*,same_instance,transport_failed):
    require(same_instance and not transport_failed, 'Refusal probe requires the unchanged original instance and no transport failure.')
    require(not record.get('refusal_probe_attempted'), 'One refusal report probe only.')
    record['refusal_probe_attempted']=True;save()
    # The helper admits only a nonce-bound pre-RMR refusal, checks the disabled
    # watchdog and verifies complete SRAM restoration. Any error propagates.
    base.observe_refusal(transaction,evidence,prepared,record,save)
    record.update(clean_refusal=True,manual_reconnect_required=False,rom_usable=True)
    save()


def finish_after_observation(record,save,quarantine,hold=base.hold_ownership):
    if record.get('clean_refusal'):
        require(record.get('refusal_probe_attempted') and record.get('sram_restored') and
                record.get('rom_usable') and not record.get('transport_failed'),
                'Clean refusal requires a verified report and actual SRAM restoration.')
        record['ownership_release_permitted']=True;save()
        return
    retain_after_capture(record,save,quarantine,hold)


def retain_after_capture(record,save,quarantine,hold=base.hold_ownership):
    # A frozen snapshot does not stop CPUs or restore ROM. Reuse the existing
    # coordinator hold on every post-entry outcome, including observed success.
    # hold() never returns, so report the durable result before quarantine.
    try:
        record['manual_reconnect_required']=True
        save()
        try:
            print('Diagnostic: '+str(record.get(record.get('result_key'),'unobserved'))+
                  '. Manual reconnection required.',flush=True)
        except OSError: pass  # A closed console must not skip quarantine.
        quarantine()
    finally:
        hold()


def run_session(args,bundle,prepared,ready,validate_entry,capture_device,*,
                product,serial,bcd_device,capture_seconds,result_key,source_files=(),
                description='Supervised native application; manual reconnection after capture'):
    """Shared protected transport for the observer and native board application.

    Callers validate their image geometry and native session inputs.
    The capture callback consumes only the newly enumerated native instance.
    Every uncertain post-entry outcome retains the physical lock.
    """
    capture_limit={'application_result':179,'accelerator_result':19,'gpu_display_result':19,'hdmi_audio_result':19,'g2d_result':19,'displayport_result':19,'deinterlace_result':19,'isp0_result':19}.get(result_key,59)
    require(0<capture_seconds<=capture_limit and result_key in ('scheduler_result','board_result','npu_result','gpu_result','gpu_transport_result','gpu_display_result','hdmi_audio_result','g2d_result','displayport_result','deinterlace_result','isp0_result','accelerator_result','i2s_result','jpeg_result','jpeg_encode_result','h264_encode_result','hevc_result','h264_result','dsi_result','bluetooth_result','camera_result','application_result','application_board_result','crypto_result','thermal_result','rtc_result','pcie_result','storage_read_result','spi_result','uart_result','pwm_result','adc_result','monitor_result'),
            'Unsupported supervised capture bounds.')
    ready()
    args.evidence = base.output_directory(args.evidence)
    for label,path in [('host-tool.py',Path(__file__)),('base-host-tool.py',Path(base.__file__)),
                       ('observer-host-tool.py',Path(observer.__file__)),
                       ('prepared.json',bundle/'prepared.json'), *source_files]: shutil.copy2(path,args.evidence/label)
    access=module('observe_usb_access',ROOT/'tools/usb-access.py')
    sys.path.insert(0,str(ROOT/'tools')); import usb_guard
    usb_guard.check_health()
    record={'complete':False,'transactions':[],
            'bundle_sha256':digest(bundle/'prepared.json'),'launch_attempted':False,
            'result_key':result_key,result_key:'unobserved'}
    def save(): save_json(args.evidence/'session.json',record)
    with access.board_lock() as node:
        access.require_session(node); access.require_rom(node); identity=access.identity(node)
        port=access.target(node); port_config={'port':port.name,'sysfs':str(port.resolve())}; failed=False
        validate_entry(identity,port.name)
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
                result=base.fel_request(args.fel,node,arguments,entry,pass_fds)
            except (OSError,ValueError,subprocess.TimeoutExpired) as error:
                failed=True;entry['failure']=str(error);save();raise
            save();return result.stdout
        def observe(started):
            nonlocal node,identity,failed
            old=identity; deadline=started+capture_seconds; original_stayed=True
            # Enumeration is passive. A sole nonce-bound report probe is allowed
            # only if this window ends with the original instance unchanged.
            while time.monotonic()<deadline:
                candidate,current=base.present_instance(access,port_config)
                if candidate is None or current!=old:original_stayed=False
                if candidate is not None and current!=old and os.access(candidate,os.R_OK|os.W_OK):
                    access.require_session(candidate)
                    require((port/'bcdDevice').read_text().strip()==f'{bcd_device:04x}' and
                            (port/'product').read_text().strip()==product and
                            (port/'serial').read_text().strip()==serial, 'Unexpected target service.')
                    node,identity=candidate,current
                    import usb.core
                    device=usb.core.find(idVendor=0x1f3a,idProduct=0xefe8,bus=int(node.parent.name),address=int(node.name))
                    require(device is not None and device.bcdDevice==bcd_device, 'Observer handle differs.')
                    def same():
                        require(not failed and access.identity(node)==identity, 'Observer instance changed; stop.')
                        require((port/'bcdDevice').read_text().strip()==f'{bcd_device:04x}', 'Observer revision changed.')
                    try:
                        same();device.default_timeout=min(500,max(1,int((deadline-time.monotonic())*1000)))
                        device.set_configuration(1)
                        record['observer_identity']=identity;save()
                        capture_device(device,same,deadline,record,save)
                        return
                    except BaseException:
                        failed=True;raise
                time.sleep(.01)
            candidate,current=base.present_instance(access,port_config)
            same_original=original_stayed and candidate is not None and current==old
            if same_original and not failed:
                access.require_session(candidate);access.require_rom(candidate)
                probe_clean_refusal(transaction,args.evidence,prepared,record,save,
                    same_instance=True,transport_failed=failed)
                return
            raise CaptureWindowExpired('No native observer within bounded capture window; reconnect manually.')
        try:
            require('soc=00001903(A733)' in transaction('version'),'Target is not A733.')
            with usb_guard.reenumeration('sunstoneos-a733',description,1):
                base.verify_dram(transaction,args.evidence,record,save)
                launch_once(transaction,observe,bundle,prepared,args.evidence,record,save,ready,result_key=result_key)
                finish_after_observation(record,save,lambda:access.quarantine_instance(node,
                    'Supervised result captured; manual reconnection and coordinator recovery review required.',identity))
        except BaseException as error:
            record['transport_failed']=failed
            base.stop_after_failure(record,save,
                lambda:access.quarantine_instance(node,'Observer attempt stopped; no requests or automatic retry.',identity),error)
            raise
    # In operation only a clean refusal returns, with status 2: every other
    # outcome holds ownership in retain_after_capture or stop_after_failure.
    # Offline fixtures inject returning holds and also use the status below.
    if record.get('clean_refusal'):
        print('Clean pre-entry refusal '+str(record['refusal_reason'])+'. SRAM restored; ROM usable.')
    return 0 if record['complete'] else 2


def capture_observer_device(device,same,deadline,prepared,evidence,record,save):
    challenge=secrets.token_bytes(32);record['challenge']=challenge.hex();save()
    def exchange(packet,timeout):
        same();require(device.write(0x02,packet,timeout=timeout)==len(packet),'Incomplete request.')
        same();remaining=int((deadline-time.monotonic())*1000)
        require(remaining>0,'Capture deadline expired after write.')
        return bytes(device.read(0x81,528,timeout=min(timeout,remaining)))
    return capture(exchange,prepared,challenge,evidence,record,save,deadline)


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__); sub=parser.add_subparsers(dest='operation',required=True)
    prep=sub.add_parser('prepare')
    for name in ('image','output'): prep.add_argument('--'+name,type=Path,required=True)
    for name in ('image-manifest','admission'): prep.add_argument('--'+name,type=Path,help=argparse.SUPPRESS)
    prep.add_argument('--clang',default='clang-21')
    prep.add_argument('--lld',type=Path,default=Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    live=sub.add_parser('launch')
    for name in ('bundle','fel','evidence'):
        live.add_argument('--'+name,type=Path,required=True)
    live.add_argument('--supervised-manual-reconnect',action='store_true',required=True)
    args=parser.parse_args(argv)
    if args.operation=='prepare':
        result=prepare(args.image,args.image_manifest,args.admission,args.output.resolve(),args.clang,args.lld)
        print(json.dumps(result,indent=2)); return 0
    bundle=args.bundle.resolve(); prepared=validate_bundle(bundle)
    return run_session(args,bundle,prepared,
        lambda:None, lambda identity,port:None,
        lambda device,same,deadline,record,save:capture_observer_device(
            device,same,deadline,prepared,args.evidence,record,save),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0101,capture_seconds=19,
        result_key='scheduler_result')


if __name__=='__main__':
    try: sys.exit(main())
    except (OSError,ValueError,RuntimeError,struct.error) as error: sys.exit(f'Stopped: {error}')
