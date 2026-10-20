#!/usr/bin/env python3
"""Check the actual USB capture reader with packet-boundary ZLPs and a deadline."""
import argparse,importlib.util,json
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('bundle','frame','output'):p.add_argument('--'+n,type=Path,required=True)
 a=p.parse_args();a.output.mkdir(exist_ok=False);spec=importlib.util.spec_from_file_location('encoder_zlp',ROOT/'tools/check-a733-jpeg-encode-supervised.py');h=importlib.util.module_from_spec(spec);spec.loader.exec_module(h)
 session=(a.bundle/'session.bin').read_bytes();upload=(a.bundle/'upload.bin').read_bytes();raw=bytearray(a.frame.read_bytes());raw[16:32]=session[24:40];frame=bytes(raw);reports=[]
 for case in ('intermediate-zlp','only-zlp-deadline'):
  out=a.output/case;out.mkdir();now=[100.];record={};reads=[];writes=[]
  class Device:
   def write(self,endpoint,data,timeout):writes.append(bytes(data));return len(data)
   def read(self,endpoint,n,timeout):
    reads.append(n)
    if case=='only-zlp-deadline' or len(reads)%2:return b''
    first=sum(len(x) for x in chunks);part=frame[first:first+min(n,47)];chunks.append(part);return part
  def clock():now[0]+=.01;return now[0]
  chunks=[]
  original_capture=h.capture
  def capture(*args,**kw):kw['now']=clock;return original_capture(*args,**kw)
  with patch.object(h.time,'monotonic',clock),patch.object(h,'capture',capture):
   try:result=h.capture_device(Device(),lambda:None,130.,session,out,record,lambda:None,upload=upload)
   except ValueError:
    assert case=='only-zlp-deadline' and not any(len(x)==41 and x[0]==1 for x in writes)
   else:
    assert case=='intermediate-zlp' and result['status']==1 and writes[-1]==h.decoder.ack(frame,session)
  assert any(x['bytes']==0 for x in record['jpeg_fragments'])
  reports.append(dict(case=case,empty_fragments=sum(x['bytes']==0 for x in record['jpeg_fragments']),ack_sent=record.get('copied_output_ack_sent',False)))
 (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,cases=reports,scope='Actual private capture_device USB reader accepts intermediate ZLPs, retains fragments and remains deadline bounded; no USB handle or hardware.'),indent=2)+'\n');print('PASS intermediate ZLP and bounded empty-read refusal')
if __name__=='__main__':main()
