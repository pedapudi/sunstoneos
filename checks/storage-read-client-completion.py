#!/usr/bin/env python3
"""Check host completion against preserved actual native SD serialization frames."""
import argparse,importlib.util,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
spec=importlib.util.spec_from_file_location('storage_completion_host',ROOT/'tools/check-a733-storage-read-supervised.py');host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
raw=(a.evidence/'host/MODEL-NOT-PHYSICAL-bundle/session.bin').read_bytes()
frames=[]
for name in ('idle-frame.bin','pending-frame.bin','native-frame.bin'):
    frame=bytearray((a.evidence/'native'/name).read_bytes());struct.pack_into('<2Q',frame,16,*struct.unpack_from('<2Q',raw,24));frames.append(frame)

def frame(index,status,reads):
    data=bytearray(frames[index]);struct.pack_into('<2I',data,272,status,reads);return bytes(data)

def run(name,sequence,success,expected_calls):
    out=a.output/name;out.mkdir(parents=True);record={};calls=[];clock=[0.]
    def exchange(packet,timeout):calls.append(packet);return sequence[len(calls)-1]
    def pause(seconds):clock[0]+=max(seconds,.001)
    try:
        host.capture(exchange,raw,out,record,lambda:None,.1,now=lambda:clock[0],pause=pause,ranges=((19,2),))
        assert success
    except (ValueError,RuntimeError,host.supervisor.CaptureWindowExpired):
        assert not success
    assert len(calls)==expected_calls,(name,len(calls))
    assert (record.get('storage_read_result')=='observed-success')==success
    if success:assert record['storage_read_application_reads']==2
    return dict(case=name,calls=len(calls),result=record.get('storage_read_result'))
a.output.mkdir(parents=True,exist_ok=False)
results=[
 run('delayed-native-completion',[frame(0,0,0),frame(2,0,0),frame(0,0,1),frame(0,1,2)],True,4),
 run('native-failure-before-usb',[frame(0,2,0)],False,1),
 run('native-failure-after-usb',[frame(0,0,0),frame(2,0,0),frame(0,2,1)],False,3),
 run('invalid-native-success-count',[frame(0,0,0),frame(2,1,1)],False,2),
 run('native-deadline',[frame(0,0,0),frame(2,0,0)]+[frame(0,0,1)]*4,False,5)]
(a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,source_frames='Preserved actual serializer output; only session epoch/generation and native completion fields adapted.',cases=results),indent=2)+'\n')
print('PASS: delayed native completion, failure, invalid count and bounded timeout.')
