#!/usr/bin/env python3
"""Decode copied raw board-monitor observations; no voltage or board-ID inference."""
import hashlib,importlib.util,struct
from pathlib import Path
spec=importlib.util.spec_from_file_location('adc_observe_prepare',Path(__file__).with_name('prepare.py'))
preparation=importlib.util.module_from_spec(spec);spec.loader.exec_module(preparation)
def decode(frame,session):
 preparation.session_check(session)
 if len(frame)<128:raise ValueError('Truncated ADC frame')
 magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',frame)
 if (magic,version)!=(0x31434441,1) or status not in (0,1,2):raise ValueError('Wrong ADC frame')
 if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or frame[32:64]!=hashlib.sha256(session).digest():raise ValueError('ADC session differs')
 if frame[96:128]!=bytes(32):raise ValueError('Reserved ADC frame bytes differ')
 if status!=1:
  if count or len(frame)!=128 or frame[64:96]!=bytes(32):raise ValueError('Non-success carries ADC output')
  return dict(status=status,bytes=0)
 lradc=struct.unpack_from('<Q',session,600)[0]==2
 payload=frame[128:]
 if count!=160 or len(payload)!=160 or frame[64:96]!=hashlib.sha256(payload).digest():raise ValueError('ADC output hash/extent differs')
 uv,selector,reference,divider_rate,sets,mask,r0,r1=struct.unpack_from('<8I',payload)
 if (uv,selector)!=(struct.unpack_from('<I',session,776)[0],session[769]) or (reference,divider_rate,sets,mask,r0,r1)!=((2,3,2,1,0,0) if lradc else (1,1000,2,15,0,0)):raise ValueError('ADC selection/supply metadata differs')
 records=[]
 for group in range(2):
  channels=[]
  for i in range(1 if lradc else 4):
   time,code,channel,reserved,sequence=struct.unpack_from('<QHBBI',payload,32+64*group+16*i)
   if not time or code>(63 if lradc else 4095) or channel!=i or reserved or not sequence:raise ValueError('Invalid ADC raw record')
   if group and (time<=records[0][i]['observed_us'] or sequence<=records[0][i]['sequence']):raise ValueError('ADC set is not a later fresh observation')
   channels.append(dict(channel=i,observed_us=time,sequence=sequence,raw_code=code))
  if lradc and payload[48+64*group:96+64*group]!=bytes(48):raise ValueError('Unused LRADC report slots differ')
  records.append(channels)
 result=dict(status=1,bytes=160,copied_verified=True,cldo3_programmed_uv=uv,observations=records)
 if lradc:result.update(controller='LRADC0',sample_selector=3,calibrated_voltage=False)
 else:result['internal_reference_selected']=True
 return result
