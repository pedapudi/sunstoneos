#!/usr/bin/env python3
"""Check thermal scheduled-slot lateness, missing samples and both capture paths."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
from harness import refused
ROOT=Path(__file__).resolve().parents[1]
CHECK_INPUTS=['apps/os/thermal-report.py','apps/os/cpu-return-thermal.py','apps/os/thermal-report.h','include/FreeRTOSConfig.h']
def load(name,path):
 spec=importlib.util.spec_from_file_location(name,path);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
report=load('report_cadence',ROOT/'apps/os/thermal-report.py')
returned=load('return_cadence',ROOT/'apps/os/cpu-return-thermal.py')
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
assert '#define configTICK_RATE_HZ 1000' in (ROOT/'include/FreeRTOSConfig.h').read_text()
def fixture(errors=(2,4,2,3),version=2):
 period=report.PERIOD_US if version==2 else 100000
 origin=1000000;times=[origin+i*period+error for i,error in enumerate(errors)]
 rows=[dict(time_us=t,millicelsius=[40000]*5,age_us=[0]*5,sequence=i+1,seen=31,stale=0,level=0,sensor=0) for i,t in enumerate(times)]
 return dict(magic=report.MAGIC,version=version,epoch=11,generation=12,recognition_generation=7,begin_us=origin+10,end_us=times[-1]+10,overflow=0,cpu_critical_millicelsius=70000,row_bytes=56,recognition_ok=1,scheduled_first_us=origin,readings=rows)
def encode(h):
 head=report.HEADER_BYTES if h['version']==2 else report.LEGACY_HEADER_BYTES
 h={**h,'rows':len(h['readings']),'bytes':head+len(h['readings'])*report.ROW_BYTES}
 raw=struct.pack('<4I5Q4I',*(h[k] for k in report.FIELDS))
 if h['version']==2:raw+=struct.pack('<Q',h['scheduled_first_us'])
 for row in h['readings']:raw+=struct.pack('<Q5i6I4B',row['time_us'],*row['millicelsius'],*row['age_us'],row['sequence'],row['seen'],row['stale'],row['level'],row['sensor'])
 return raw
def classify(h):return report.classify(report.decode(encode(h),11,12))
cases=[]
h=fixture();assert classify(h)=='complete';assert h['readings'][1]['time_us']-h['readings'][0]['time_us']==125002;assert h['readings'][2]['time_us']-h['readings'][1]['time_us']==124998
cases.append('opposite-jitter-does-not-imply-missed-sample')
for delay,expected in ((report.LATENESS_US,'complete'),(report.LATENESS_US+1,'incomplete'),(-1,'incomplete')):
 x=fixture((0,delay,0));assert classify(x)==expected
cases.append('late-inclusive-budget-and-early-sample-boundaries')
x=fixture();x['readings'].pop(1);assert classify(x)=='incomplete';cases.append('missing-scheduled-slot')
x=fixture(tuple(i*500 for i in range(6)));assert classify(x)=='incomplete';cases.append('accumulated-drift-exceeds-budget')
for offset,expected in ((report.LATENESS_US,'complete'),(report.LATENESS_US+1,'incomplete')):
 x=fixture((0,0));x['end_us']=x['scheduled_first_us']+len(x['readings'])*report.PERIOD_US+offset;assert classify(x)==expected
cases.append('tail-deadline-budget')
x=fixture();x['begin_us']=x['scheduled_first_us']+report.PERIOD_US+report.LATENESS_US+1;assert classify(x)=='incomplete';cases.append('missing-start-coverage')
for change,expected in ((lambda x:x.update(overflow=1),'overflow'),(lambda x:x.update(recognition_ok=0),'recognition-failed'),(lambda x:x['readings'][1].update(level=2),'thermal-stop'),(lambda x:x['readings'][1].update(stale=1),'incomplete'),(lambda x:x['readings'][1].update(age_us=[1000001]*5),'incomplete')):
 x=fixture();change(x);assert classify(x)==expected
cases.append('overflow-failure-critical-and-freshness-rules')
# Both transport paths receive the same real header bytes and must classify alike.
for version in (1,2):
 x=fixture(version=version);raw=encode(x)
 def exchange(request):
  op,unused,count,offset,expected=struct.unpack('<BBHIQ',request);assert op==7 and expected in (0,7)
  body=raw[offset:offset+count]
  return struct.pack('<8I2Q16x',0x424d494d,1,1,len(body),len(raw),offset,0,7,11,12)+body
 usb=report.decode(report.capture(exchange,11,12),11,12)
 head=report.HEADER_BYTES if version==2 else report.LEGACY_HEADER_BYTES
 w=raw[:head]+struct.pack('<3I4x',0,1,1)
 r=raw[head:]+bytes((report.MAX_ROWS-len(x['readings']))*report.ROW_BYTES)+struct.pack('<2I',len(x['readings']),0)+struct.pack('<Q',2000000)
 layout={'window':dict(bytes=len(w),members=dict(header=0,first=head,collecting=head+4,ready=head+8)),'readings':dict(bytes=len(r),members=dict(rows=0,count=report.MAX_ROWS*report.ROW_BYTES,overflow=report.MAX_ROWS*report.ROW_BYTES+4,next=report.MAX_ROWS*report.ROW_BYTES+8))}
 native=returned.decode(layout,{'thermal-window':w,'thermal-readings':r},11,12)
 assert report.classify(usb)==native['classification']=='complete'
 cases.append('usb-and-returned-version-'+str(version))
 bad=bytearray(raw);struct.pack_into('<I',bad,4,99);refused(lambda:report.decode(bad,11,12),'layout differs')
(a.output/'result.json').write_text(json.dumps(dict(status='PASS',cases=cases,period_us=report.PERIOD_US,lateness_us=report.LATENESS_US,model='Actual decoders with modeled schedule, jitter, missing rows and USB frames'),indent=2)+'\n')
print('PASS thermal report cadence:',len(cases),'cases')
