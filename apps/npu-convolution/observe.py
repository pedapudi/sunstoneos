#!/usr/bin/env python3
"""The NPU observer protocol: request and response frames, bounded capture and
the platform admission predicate, used by the supervised NPU preparation and
host tools. The compact NPU image that this tool once prepared and launched is
retired; its operations are refused."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import time
ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location('smp_observe_base', ROOT / 'tools/check-a733-smp.py')
base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(base)  # Library import; the old tool's main guard stays inactive.
require, artifact = base.require, base.artifact
observer = base.module('npu_reused_observer', ROOT / 'tools/check-a733-smp-observe.py')
REQUEST_MAGIC = 0x415337334f4c4331
MESSAGE_MAGIC, FRAME_MAGIC = 0x53534f53, 0x415337334e505531
PRODUCT, SERIAL = 'sunstoneos NPU convolution observer', 'sunstoneos-a733-npu-convolution'


CATALOGUE=json.loads((ROOT/'tools/npu-models.json').read_text())['models']
ARTIFACTS={name:(CATALOGUE[0][name]['bytes'],CATALOGUE[0][name]['sha256']) for name in ('model','input','reference')}

def selected_artifacts(prepared):
    name=prepared.get('model_name','conv8')
    selected=next((v for v in CATALOGUE if v['name']==name),None)
    require(selected is not None,'Unknown reviewed model.')
    return {key:(selected[key]['bytes'],selected[key]['sha256']) for key in ('model','input','reference')}

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


def main(argv=None):
    """The compact image is retired; its preparation and launch are refused."""
    argparse.ArgumentParser(description=__doc__).parse_args(argv)
    sys.exit('The compact NPU image is retired: prepare the supervised image with '
             'apps/npu-convolution/prepare-supervised.py and launch it with tools/check-a733-npu-supervised.py.')


if __name__=='__main__':main()
