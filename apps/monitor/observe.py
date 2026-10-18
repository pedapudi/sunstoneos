#!/usr/bin/env python3
"""Validate copied asynchronous thermal and raw GPADC monitoring snapshots."""
import hashlib
import importlib.util
import struct
from pathlib import Path
spec=importlib.util.spec_from_file_location('monitor_observe_prepare',Path(__file__).with_name('prepare.py'))
preparation=importlib.util.module_from_spec(spec);spec.loader.exec_module(preparation)
def decode(frame,session,previous=None):
 preparation.session_check(session)
 if len(frame)<128:raise ValueError('Truncated monitor frame')
 magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',frame)
 if (magic,version)!=(0x314e4f4d,1) or status not in range(6):raise ValueError('Wrong monitor frame')
 if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or frame[32:64]!=hashlib.sha256(session).digest():raise ValueError('Monitor session differs')
 if frame[96:128]!=bytes(32):raise ValueError('Reserved monitor bytes differ')
 if status!=2:
  if count or len(frame)!=128 or frame[64:96]!=bytes(32):raise ValueError('Non-snapshot carries monitor output')
  return dict(status=status,bytes=0)
 p=frame[128:]
 if count!=192 or len(p)!=192 or frame[64:96]!=hashlib.sha256(p).digest():raise ValueError('Monitor output hash/extent differs')
 sequence,time,thermal_sets,adc_sets=struct.unpack_from('<4Q',p)
 tt=struct.unpack_from('<5Q',p,32);at=struct.unpack_from('<4Q',p,72)
 temperatures=struct.unpack_from('<5i',p,104)
 ts,ads,uv,parent,state=struct.unpack_from('<5I',p,124)
 tc=struct.unpack_from('<5H',p,144);ac=struct.unpack_from('<4H',p,154)
 factory=struct.unpack_from('<H',p,162)[0];factory_codes=struct.unpack_from('<5H',p,164);corrections=struct.unpack_from('<5H',p,174)
 events=tuple(p[184:187])
 if not all((sequence,time,thermal_sets,adc_sets,ts,ads)) or state!=1 or parent!=0x81000000 or uv!=struct.unpack_from('<I',session,776)[0]:raise ValueError('Invalid monitor progress/supply profile')
 if not all(0<x<=time for x in (*tt,*at)) or not all(0<x<=4095 for x in tc) or not all(x<=4095 for x in ac):raise ValueError('Invalid monitor codes/times')
 expected=tuple((x-2822)*-62 if x>1769 else (x-2835)*-59 for x in tc)
 if temperatures!=expected or not 0<factory<4095 or not all(0<x<4095 for x in factory_codes) or not all(x<=4095 for x in corrections) or any(x&~31 for x in events) or p[187:]!=bytes(5):raise ValueError('Invalid thermal calibration/temperature/events')
 result=dict(status=2,bytes=192,generation=sequence,observed_us=time,thermal_sets=thermal_sets,adc_sets=adc_sets,
  thermal_times=list(tt),adc_times=list(at),thermal_codes=list(tc),adc_raw_codes=list(ac),thermal_millicelsius=list(temperatures),
  thermal_sequence=ts,adc_sequence=ads,cldo3_programmed_uv=uv,events=list(events),
  calibration=dict(factory_tenth_c=factory,factory_codes=list(factory_codes),corrections=list(corrections)))
 if previous:
  if sequence<=previous['generation'] or time<previous['observed_us'] or result['calibration']!=previous['calibration']:raise ValueError('Monitor generation/calibration regressed')
  for kind in ('thermal','adc'):
   sets=result[kind+'_sets'];old=previous[kind+'_sets'];times=result[kind+'_times'];before=previous[kind+'_times']
   if sets<old or any(a<b for a,b in zip(times,before)) or (sets>old and not all(a>b for a,b in zip(times,before))):raise ValueError('Monitor set freshness regressed')
   if sets==old and (times!=before or result[kind+'_sequence']!=previous[kind+'_sequence']):raise ValueError('Monitor set changed without progress')
  if any((a&b)!=b for a,b in zip(events,previous['events'])):raise ValueError('Monitor event observations regressed')
 return result
