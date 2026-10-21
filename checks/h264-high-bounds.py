#!/usr/bin/env python3
"""Focused malformed/admission refusals through the actual sanitized syntax producer."""
import argparse,hashlib,json,os,re,subprocess
from pathlib import Path
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(exist_ok=False);raw=(a.evidence/'fullhd/qualification/frame-000.h264').read_bytes();starts=list(re.finditer(b'\0\0\0\1',raw));assert len(starts)==3;nal=starts[-1].end();changed=bytearray(raw);changed[nal]=(changed[nal]&~31)|1;forbidden=bytearray(raw);forbidden[4]|=128
 cases={'forbidden-header':[bytes(forbidden)],'truncated-SPS':[raw[:starts[1].start()-2]],'non-IDR-first':[bytes(changed)],'extra-NAL':[raw+b'\0\0\0\1\x09\xf0'],'over-capacity':[raw+bytes(2097153-len(raw))]};reports=[]
 for name,inputs in cases.items():
  paths=[]
  for i,data in enumerate(inputs):path=a.output/f'{name}-{i}.h264';path.write_bytes(data);paths.append(path)
  command=[str(a.evidence/'producer'),*map(str,paths)];r=subprocess.run(command,capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1'});assert r.returncode in (3,4) and 'runtime error:' not in r.stderr and 'ERROR: AddressSanitizer' not in r.stderr;(a.output/f'{name}.stderr').write_text(r.stderr);reports.append(dict(case=name,returncode=r.returncode,command=command))
 # Actual four-reference weighted producer fixture exceeds the independently
 # admitted four active entries, despite its four-picture DPB fitting storage.
 paths=sorted((a.evidence/'hd4/qualification').glob('frame-*.h264'));assert len(paths)==12;r=subprocess.run([str(a.evidence/'producer'),*map(str,paths)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1'});assert r.returncode==4 and 'runtime error:' not in r.stderr and 'ERROR: AddressSanitizer' not in r.stderr;reports.append(dict(case='four-refs-five-weighted-active-entries-refused',returncode=r.returncode))
 # A 1920x1088 level-4.0 clip with three references needs four Y/chroma/MV
 # slots: the producer admits it and places every picture in slots 0..3.
 paths=sorted((a.evidence/'fullhd3/qualification').glob('frame-*.h264'));assert len(paths)==12;r=subprocess.run([str(a.evidence/'producer'),*map(str,paths)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1'});assert r.returncode==0 and 'runtime error:' not in r.stderr and 'ERROR: AddressSanitizer' not in r.stderr
 plans=json.loads(r.stdout);assert all(x['width']==1920 and x['height']==1088 and x['refs']==3 for x in plans) and {x['slot'] for x in plans}=={0,1,2,3} and max(len(x['list0'])+len(x['list1']) for x in plans)>=3;reports.append(dict(case='fullhd-three-references-admitted',returncode=0,slots=4))
 (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,producer_sha256=hashlib.sha256((a.evidence/'producer').read_bytes()).hexdigest(),cases=reports,scope='Actual ASan/UBSan-built standalone syntax/DPB producer rejects focused bounded input errors. No device entropy execution, fuzz campaign or physical claim.'),indent=2)+'\n');print('PASS bounded High syntax/admission refusals and full-HD three-reference admission')
if __name__=='__main__':main()
