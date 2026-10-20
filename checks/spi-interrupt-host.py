#!/usr/bin/env python3
"""Focused version-2 SPI notification, copied-output and polling ABI checks."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m

def run(bundle,frame_path,old_session,old_frame,out):
 out.mkdir(parents=True,exist_ok=False);host=module('spi_irq_capture',ROOT/'apps/spi/capture.py');host.preparation.validate_bundle(bundle)
 raw=(bundle/'session.bin').read_bytes();frame=bytearray(frame_path.read_bytes());frame[16:32]=raw[24:40];frame[32:64]=hashlib.sha256(raw).digest();frame=bytes(frame)
 info=host.decoder.decode(frame,raw);assert info['interrupt_notifications']==1
 for count in (0,33):
  bad=bytearray(frame);struct.pack_into('<I',bad,128+28,count);bad[64:96]=hashlib.sha256(bad[128:]).digest()
  try:host.decoder.decode(bytes(bad),raw)
  except ValueError:pass
  else:raise AssertionError('Invalid IRQ count accepted')
 # Reuse the historical polling artifact; do not replay its native tests.
 old=host.decoder.decode(old_frame.read_bytes(),old_session.read_bytes());assert old['interrupt_notifications']==0
 record={};host.capture(lambda *a:frame,raw,out,record,lambda:None,60,now=lambda:1.)
 assert record['spi_result']=='observed-success' and (out/'output.bin').read_bytes()==frame[128:]
 (out/'result.json').write_text(json.dumps(dict(status='pass',cases=['irq-count-and-refusals','existing-polling-frame-compatibility','durable-version2-capture'],physical_access=False),indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('bundle','frame','old-session','old-frame','output'):p.add_argument('--'+name,type=Path,required=True)
 a=p.parse_args();run(a.bundle,a.frame,a.old_session,a.old_frame,a.output)
