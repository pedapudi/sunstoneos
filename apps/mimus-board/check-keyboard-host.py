#!/usr/bin/env python3
"""Check keyboard capture and production preparation with explicitly modeled facts.

No USB, queue or launcher execution occurs. The actual compiled image identity
and native-produced trigger frame are reused. Platform, DDR, electrical facts,
keyboard/MSC attachment, scheduling and transport are software fixture inputs.
"""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
p=argparse.ArgumentParser(description=__doc__)
for name in ('image','native-trigger','output','clang','lld'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
host=module('keyboard_host',ROOT/'apps/mimus-board/host.py');prep=module('keyboard_prepare',ROOT/'apps/mimus-board/prepare.py')
trigger=a.native_trigger.read_bytes()[64:];assert host.decode_trigger(trigger)['attachment_generation']==23
text=b'FORMATTED There was silence.\n';sector=bytes(range(256))*2
results={}
def capture(epoch=19,generation=23,keyboard=True,payload=trigger,version=2):
    calls=[]
    def exchange(packet):
        op,zero,count,offset,reserved=struct.unpack('<BBHIQ',packet);assert not zero and not reserved
        calls.append(op);body=text[offset:offset+count] if op==1 else sector if op==2 else payload if op==3 else b''
        return struct.pack('<8I2Q16x',0x424d494d,version,1,len(body),len(text),offset,1,op,epoch,generation)+body
    value=host.capture(exchange,epoch,generation,100,now=lambda:1,keyboard=keyboard)
    assert value['transcript']==text and value['sector']==sector and calls[-1]==0 and 3 in calls
    return value
assert capture()['trigger']['bytes']==trigger;results['actual_native_trigger_decoded']=True
for name,offset,blob in [('stale-generation',16,struct.pack('<Q',24)),('sequence',80,struct.pack('<Q',999)),('nonneutral',40,b'\x01'),('nonenter',90,b'\x04'),('protocol',27,b'\x02'),('mode',30,b'\x01'),('reserved',127,b'\x01')]:
    changed=bytearray(trigger);changed[offset:offset+len(blob)]=blob
    try:capture(payload=bytes(changed))
    except ValueError:pass
    else:raise AssertionError(name)
    results[name+'_refused']=True
for name,options in [('legacy-reader',dict(keyboard=False)),('legacy-frame',dict(version=1))]:
    try:capture(**options)
    except ValueError:pass
    else:raise AssertionError(name)
    results[name+'_refused']=True
# Construct actual-image-bound, explicitly nonphysical producer inputs.
fixture=a.output/'fixture';fixture.mkdir()
ih=hashlib.sha256(a.image.read_bytes()).hexdigest();_,_,identity=prep.board.base.image_parts(a.image)
raw=bytearray((Path.home()/'.cache/sunstoneos/evidence/board-supervised-20261017/batch1/session-fixture.bin').read_bytes());assert len(raw)==592
raw[48:80]=identity;struct.pack_into('<I',raw,400,4)
target=dict(identity='mimus-keyboard-software-model',port='no-physical-port',image_sha256=ih)
def record(name,value):
    data=json.dumps(value,sort_keys=True).encode();path=(fixture/name).resolve();path.write_bytes(data)
    return {'path':str(path),'sha256':hashlib.sha256(data).hexdigest()}
facts=record('software-only.json',dict(**target,physical_access=False,purpose='Modeled facts only. No connected keyboard, hub, disk or board admission.'))
for offset in (80,216,248,280):raw[offset:offset+32]=bytes.fromhex(facts['sha256'])
conditions={'source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity'}
platform=record('platform.json',dict(**target,schema='a733-smp-observer-platform-v1',entry_conditions_established=True,physical_coherence_result='under-test',conditions={k:facts for k in sorted(conditions)}))
def memory(name,ranges):return record(name,dict(**target,schema='a733-ddr-reservation-v1',exclusive=True,published_cache_disabled=True,remote_writers_excluded=True,ranges=ranges,evidence=[facts]))
template=fixture/'session-template.bin';template.write_bytes(raw)
admission=dict(**target,schema='a733-board-supervised-entry-v2',reservation=1,session_template_sha256=hashlib.sha256(raw).hexdigest(),
    ddr=memory('ddr.json',[[0x41000000,0x42200000]]),mimus_ddr=memory('mimus-ddr.json',prep.RANGES),platform=platform,
    whole_board_power_removal=True,no_resource_reuse_before_power_removal=True,recovery=dict(mode='manual-usb-reconnect',demonstrated_fel=True,explicit_reconnect_before_resumption=True,preserve_microsd=True,one_attempt=True,evidence=[facts]))
path=fixture/'admission.json';path.write_text(json.dumps(admission,indent=2)+'\n')
assets=[Path.home()/'.cache/sunstoneos/inputs/mimus-bench'/name for name in ('base.mimus','overlay.mimus')]+[Path('/home/sunil/data/asr/device/replay_devother/4515-11057-0027.wav')]
bundle=a.output/'bundle';manifest=a.image.parent/'mimus-board-application/image-manifest.json'
prep.prepare(a.image,manifest,path,template,assets,bundle,str(a.clang),str(a.lld));prep.validate_bundle(bundle)
# Actual bundle entry chooses v2 capture from its frozen workload, before transport.
session=(bundle/'board/session.bin').read_bytes();epoch,generation=struct.unpack_from('<2Q',session,24)
payload=bytearray(trigger);struct.pack_into('<Q',payload,16,generation);calls=[]
def exchange(packet):
    op,_,count,offset,_=struct.unpack('<BBHIQ',packet);calls.append(op)
    body=text[offset:offset+count] if op==1 else sector if op==2 else bytes(payload) if op==3 else b''
    return struct.pack('<8I2Q16x',0x424d494d,2,1,len(body),len(text),offset,1,op,epoch,generation)+body
assert host.capture_bundle(exchange,bundle,100,now=lambda:1)['trigger']['attachment_generation']==generation and 3 in calls
results['actual_prepare_and_bundle_capture_entry']=True
# Fragmented reads and transport refusal remain exercised through real adapter.
class Device:
    def write(self,ep,data,timeout):self.data=bytearray(exchange(data));return len(data)
    def read(self,ep,size,timeout):n=min(13,len(self.data),size);data=self.data[:n];del self.data[:n];return data
retained=[];adapter=host.device_exchange(Device(),lambda:None,100,retained.append,now=lambda:1)
assert host.capture(adapter,epoch,generation,100,now=lambda:1,keyboard=True)['trigger']['bytes']==bytes(payload)
results['fragmented_transport_and_final_live_query']=True
attempts=[]
def failed(packet):attempts.append(packet);raise OSError('Modeled transport failure')
try:host.capture(failed,19,23,100,now=lambda:1,keyboard=True)
except OSError:pass
else:raise AssertionError('Failure hidden')
assert len(attempts)==1;results['transport_no_retry']=True
results['physical_access']=False;results['boundaries']=__doc__
(a.output/'result.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results))
